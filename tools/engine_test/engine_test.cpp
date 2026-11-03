//
// 灵犀输入法预测引擎单元测试。
//
// 直接链接 ui/engine/ngram.cpp + pinyin.cpp（不经 TSF/IPC/LingxiUI），对
// CreateDefaultPredictor() 做纯进程内断言：英文前缀补全、带上下文补全、
// 中文高频接续、空上下文行为与预测延迟（平均 < 5ms）；
// M5 追加拼音段：CreatePinyinPredictor() 的音节切分/词链/残段/延迟。
//
// 数据定位：ngram.cpp 用 GetModuleFileName(NULL) 拼 <exe目录>\data\*.txt。
// 本测试 exe 输出在 bin\enginetest\，而词表随主构建放在 bin\Debug\data，
// 因此首次运行若 <exe目录>\data 缺词表，自动从 <exe目录>\..\Debug\data 拷贝。
//
#include "predictor.h"
#include "calcnum.h"
#include "uradical.h"
#include "../../ui/symsearch.h"
#include "../../common/proto.h"
#include "../../common/punct.h"
#include "../../common/log.h"
#include "../../common/settings.h"
#include "../../common/shuangpin.h"
#include "../../common/pipe_util.h"

#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <io.h>
#include <fcntl.h>
#include <ctime>

static int g_pass = 0;
static int g_fail = 0;

static void Report(bool ok, const char* name, const wchar_t* detail = L"")
{
    wprintf(L"[%s] %hs %s\n", ok ? L"PASS" : L"FAIL", name, detail);
    if (ok) g_pass++; else g_fail++;
}

// exe 所在目录（不带尾反斜杠）
static std::wstring ExeDir()
{
    wchar_t path[MAX_PATH] = L"";
    if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0) return L".";
    std::wstring s(path);
    size_t pos = s.find_last_of(L'\\');
    return (pos == std::wstring::npos) ? L"." : s.substr(0, pos);
}

// 保证 <exe目录>\data\{en_words,zh_words,zh_pinyin}.txt 就位（首次运行从 bin\Debug\data 拷贝）
static bool EnsureWordlists()
{
    std::wstring exeDir = ExeDir();
    std::wstring dstDir = exeDir + L"\\data";
    std::wstring marker = dstDir + L"\\en_words.txt";
    std::wstring markerPy = dstDir + L"\\zh_pinyin.txt";
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(markerPy.c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;

    std::wstring srcDir = exeDir + L"\\..\\Debug\\data";
    CreateDirectoryW(dstDir.c_str(), NULL);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((srcDir + L"\\*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::wstring from = srcDir + L"\\" + fd.cFileName;
            std::wstring to = dstDir + L"\\" + fd.cFileName;
            CopyFileW(from.c_str(), to.c_str(), FALSE);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW(markerPy.c_str()) != INVALID_FILE_ATTRIBUTES;
}

struct LatencyStats
{
    double avgMs = 0.0;
    double medianMs = 0.0; // 中位数：对后台进程偶发抢占比平均值鲁棒
    double maxMs = 0.0;
};

// 共享机器上后台进程（其他代理/视频/IDE）会偶发抢占，单次测量可能被拉高。
// 这里：多次预热 → 逐次记录 → 断言用中位数（p50），平均值/最大值仅作参考。
static LatencyStats MeasureLatency(lingxi::IPredictor* p, const std::wstring& ctx, int runs)
{
    LatencyStats st;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    auto qpc = [&]() -> LONGLONG {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return c.QuadPart;
    };
    for (int i = 0; i < 5; i++) p->Predict(ctx); // 预热（冷路径/缓存）
    // 测量期临时提升线程优先级，隔离共享机器上其他进程（其他代理/视频/IDE）
    // 的 CPU 抢占——否则性能数字测的是"系统有多忙"而非引擎本身。
    HANDLE th = GetCurrentThread();
    int oldPri = GetThreadPriority(th);
    SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
    std::vector<double> samples;
    samples.reserve(runs);
    LONGLONG t0 = qpc();
    for (int i = 0; i < runs; i++)
    {
        LONGLONG c0 = qpc();
        p->Predict(ctx);
        samples.push_back((double)(qpc() - c0) * 1000.0 / (double)freq.QuadPart);
    }
    st.avgMs = (double)(qpc() - t0) * 1000.0 / (double)freq.QuadPart / (double)runs;
    SetThreadPriority(th, oldPri);
    std::sort(samples.begin(), samples.end());
    st.medianMs = samples[samples.size() / 2];
    st.maxMs = samples.back();
    return st;
}

// B4 测试替身：固定候选的假预测器（ChainPredictor 路由单测）
class FakePredictor : public lingxi::IPredictor
{
public:
    explicit FakePredictor(std::vector<std::wstring> cands) : _cands(std::move(cands)) {}
    lingxi::Prediction Predict(const std::wstring&) override
    {
        lingxi::Prediction p;
        p.cands = _cands;
        return p;
    }
private:
    std::vector<std::wstring> _cands;
};

static bool AllAscii(const std::vector<std::wstring>& cands)
{
    for (const auto& c : cands)
        for (wchar_t ch : c)
            if ((unsigned short)ch >= 0x80) return false;
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    // 强杀台架子进程模式（C1-c）：只做 Learn 连发（每次 KickPersist 异步原子
    // 落盘），由父进程随机时刻 TerminateProcess，检验 userdict.txt 在任意腰斩
    // 点都是完整旧态或完整新态（tmp+rename 不变量的真进程证据）。
    // 必须在沙盒清词典之前分叉：子进程继承父进程已设好的 APPDATA 沙盒。
    if (argc > 1 && _wcsicmp(argv[1], L"--ud-churn") == 0)
    {
        lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
        if (!py) return 2;
        static const wchar_t b26[] = L"abcdefghijklmnopqrstuvwxyz";
        for (int i = 0; i < 200000; i++)
        {
            wchar_t key[16];
            key[0] = L'c'; key[1] = L'h';
            int v = i % 456976; // 26^4
            for (int p = 3; p >= 0; p--) { key[2 + p] = b26[v % 26]; v /= 26; }
            key[6] = 0;
            wchar_t word[16];
            _snwprintf_s(word, _TRUNCATE, L"测%06d", i % 1000000);
            py->Learn(key, word);
            Sleep(15);
        }
        delete py;
        return 0;
    }

    // L5：stdout 切 UTF-16 文本模式——C locale 下 wprintf 遇中文停止转换，
    // 汇总行"=== 结果:"后整段截断（PASS/FAIL 计数不落盘）；宽文本模式中文
    // 与计数完整（控制台直出 UTF-16，重定向文件为 UTF-16LE，编辑器可读）
    _setmode(_fileno(stdout), _O_U16TEXT);
    lingxi::InitLogging();

    // 设置沙盒（2026-10-29 实证事故：测试直写真实 HKCU，一次半程退出把用户
    // 全部能力位刷 0——输入法直接瘫成直通）。注册表与词典同权隔离：无 NS 则自设
    // "test"，并清沙盒键回到结构默认（=各用例断言基线）。真实 HKCU 从此不共轨。
    {
        wchar_t nsv[8] = L"";
        DWORD nl = GetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", nsv, 6);
        if (nl == 0 || nl >= 6) { 
            nsv[0] = L't'; nsv[1] = L'e'; nsv[2] = L's'; nsv[3] = L't'; nsv[4] = 0;
            SetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", nsv);
        }
        std::wstring sandk = L"Software\\LingxiIme_";
        sandk += nsv;
        RegDeleteTreeW(HKEY_CURRENT_USER, sandk.c_str());
    }
    // —— 用户词典沙盒：Learn 用例会把"脏/喵喵"等测试词写进 %APPDATA%\LingxiIme\
    // userdict.txt（真实用户的学习数据）；反过来说，上一次运行的残留又会污染本次
    // 断言（如 nihao 被测试词置顶 → P4b 口语首选翻车）。把 APPDATA 重定向到
    // exe 旁 userdata\ 并在每次运行前清空 userdict——测试自我隔离且幂等。
    {
        std::wstring sandbox = ExeDir() + L"\\userdata";
        CreateDirectoryW(sandbox.c_str(), NULL);
        std::wstring lx = sandbox + L"\\LingxiIme";
        CreateDirectoryW(lx.c_str(), NULL);
        std::wstring ud = lx + L"\\userdict.txt";
        DeleteFileW(ud.c_str());
        SetEnvironmentVariableW(L"APPDATA", sandbox.c_str());
    }
    if (EnsureWordlists())
        wprintf(L"[INFO] wordlists staged under <exeDir>\\data\n");
    else
        wprintf(L"[WARN] wordlists not staged into <exeDir>\\data; dict tests will fail\n");

    lingxi::IPredictor* p = lingxi::CreateDefaultPredictor();
    if (!p) { Report(false, "CreateDefaultPredictor"); return 1; }

    // 1. 英文前缀补全：候选非空，且 replaceLen 必须等于已输入前缀长度
    {
        lingxi::Prediction r = p->Predict(L"mor");
        wchar_t detail[160];
        bool ok = !r.cands.empty() && r.replaceLen == 3;
        _snwprintf_s(detail, _TRUNCATE, L"cands=%zu first='%ls' replaceLen=%u",
                     r.cands.size(),
                     r.cands.empty() ? L"" : r.cands[0].c_str(), r.replaceLen);
        Report(ok, "en prefix predict: mor", detail);
    }

    // 2. 带上下文补全："good inte" 的首候选应以 inte 开头（不区分大小写）
    {
        lingxi::Prediction r = p->Predict(L"good inte");
        wchar_t detail[160];
        bool ok = !r.cands.empty() && _wcsnicmp(r.cands[0].c_str(), L"inte", 4) == 0;
        _snwprintf_s(detail, _TRUNCATE, L"first='%ls'",
                     r.cands.empty() ? L"" : r.cands[0].c_str());
        Report(ok, "en completion: good inte", detail);
    }

    // 3. 中文上下文：M3 词频占位已退役（2026-10-29「候选污染」修复）——
    //    不再返回"了/是/在/和…"高频字垃圾列表；中文续写由 LLM 独占
    {
        lingxi::Prediction r = p->Predict(L"今天天气不错");
        wchar_t detail[160];
        _snwprintf_s(detail, _TRUNCATE, L"cands=%zu first='%ls'",
                     r.cands.size(),
                     r.cands.empty() ? L"" : r.cands[0].c_str());
        Report(r.cands.empty(), "zh continuation: placeholder junk retired", detail);
    }

    // 4. 空上下文：不产生任何候选
    {
        lingxi::Prediction r = p->Predict(L"");
        Report(r.cands.empty(), "empty context: no candidates");
    }

    // 5. 延迟统计：三个上下文各跑 50 次，平均 < 5ms
    {
        const wchar_t* ctxs[] = { L"hello wor", L"今天", L"international" };
        bool allOk = true;
        for (const wchar_t* ctx : ctxs)
        {
            LatencyStats st = MeasureLatency(p, ctx, 50);
            wchar_t line[192];
            _snwprintf_s(line, _TRUNCATE,
                         L"ctx='%ls' median=%.3fms avg=%.3fms max=%.0fms",
                         ctx, st.medianMs, st.avgMs, st.maxMs);
            wprintf(L"[INFO] latency %ls\n", line);
            if (st.medianMs >= 5.0) allOk = false;
        }
        Report(allOk, "latency median < 5ms (50 runs x 3 contexts)");
    }

    delete p;

    // ===== M5 拼音转换引擎（CreatePinyinPredictor）=====
    // 数据：zh_pinyin.txt（EnsureWordlists 按 *.txt 通配拷贝，含本文件）。
    {
        ULONGLONG t0 = GetTickCount64();
        lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
        ULONGLONG loadMs = GetTickCount64() - t0;
        wprintf(L"[INFO] pinyin dict load: %llu ms\n", loadMs);
        {
            PROCESS_MEMORY_COUNTERS pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
                wprintf(L"[INFO] pinyin engine working set: %u KB\n",
                        (unsigned)(pmc.WorkingSetSize / 1024));
        }
        if (!py)
        {
            Report(false, "CreatePinyinPredictor");
        }
        else
        {
            // 整句质量回归：核心长串的首选必须是语义正确的句子（防止词频/cost
            // 模型改动导致同音词回退，如"世界"→"石碣"、"想"→"向"）。
            // 另有一批扩展句（extra）只写诊断文件、不记分：它们暴露的是纯
            // unigram 模型的上下文消歧上限，留待 R3 LLM 整句重排统一解决，
            // 不做逐词穷举。
            {
                struct SCase { const wchar_t* in; const wchar_t* top; };
                // 核心：必须全通过，纳入回归
                const SCase kSentCore[] = {
                    {L"shijie", L"世界"},
                    {L"woshi", L"我是"},
                    {L"zhenmei", L"真美"},
                    {L"shiyixia", L"试一下"},
                    {L"zhegeshijiezhenmei", L"这个世界真美"},
                    {L"tamenzaikaihui", L"他们在开会"},
                    {L"woshiyigexuesheng", L"我是一个学生"},
                    {L"wojintianxiangqugongyuankaihui", L"我今天想去公园开会"},
                    {L"nichifanlema", L"你吃饭了吗"},
                    {L"womingtianyaoqubeijing", L"我明天要去北京"},
                    {L"tashiwodehaopengyou", L"他是我的好朋友"},
                    {L"womenyiqiqukandianying", L"我们一起去看电影"},
                    {L"zhegewentihenjiandan", L"这个问题很简单"},
                    {L"wofeichangxihuanzhegedifang", L"我非常喜欢这个地方"},
                    {L"nizaiganshenme", L"你在干什么"},
                    {L"woxianghuijiashuijiao", L"我想回家睡觉"},
                    {L"jintiantianqizhenhao", L"今天天气真好"},
                    {L"wobumingbainideyisi", L"我不明白你的意思"},
                    {L"qingbangwoyixia", L"请帮我一下"},
                    {L"woelexiangchifan", L"我饿了想吃饭"},
                    {L"tazaixuexiaodushu", L"他在学校读书"},
                    {L"wohengaoxingrenshini", L"我很高兴认识你"},
                    {L"zhesishenmedongxi", L"这是什么东西"},
                    {L"womenquchifanba", L"我们去吃饭吧"},
                };
                // 扩展：仅诊断，不记分（R3 待办）
                const SCase kSentExtra[] = {
                    {L"woxihuanchipingguo", L"我喜欢吃苹果"},
                    {L"tazaigongsishangban", L"他在公司上班"},
                    {L"woyaoquchaoshimaidongxi", L"我要去超市买东西"},
                    {L"zhegedianyinghenhaokan", L"这个电影很好看"},
                    {L"wobuhuishuoyingyu", L"我不会说英语"},
                    {L"womenxuexiaohenda", L"我们学校很大"},
                    {L"qinggeiwoyibeishui", L"请给我一杯水"},
                    {L"wozailushangduchele", L"我在路上堵车了"},
                    {L"tadalanqiuhenlihai", L"他打篮球很厉害"},
                    {L"woyongshoujidadianhua", L"我用手机打电话"},
                    {L"jintianwanshangyouyueliang", L"今天晚上有月亮"},
                    {L"wojiazhuzaihaibian", L"我家住在海边"},
                    {L"tashiwonvpengyou", L"她是我女朋友"},
                    {L"wozaiwangshangmaidongxi", L"我在网上买东西"},
                    {L"zhegezizenmexie", L"这个字怎么写"},
                    {L"womeitianzaoshangpaobu", L"我每天早上跑步"},
                    {L"taqizixingcheshangban", L"他骑自行车上班"},
                    {L"wozaicantingchifan", L"我在餐厅吃饭"},
                    {L"zhegehaizihencongming", L"这个孩子很聪明"},
                    {L"woxiangheyibeikafei", L"我想喝一杯咖啡"},
                };
                wchar_t sfile[MAX_PATH] = L"";
                wchar_t tdir[MAX_PATH] = L"";
                if (GetTempPathW(MAX_PATH, tdir))
                    _snwprintf_s(sfile, _TRUNCATE, L"%lslingxi_sent.txt", tdir);
                HANDLE hsf = CreateFileW(sfile, GENERIC_WRITE, 0, NULL,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                auto runGroup = [&](const SCase* arr, size_t cnt, bool assert)
                {
                    for (size_t i = 0; i < cnt; i++)
                    {
                        const SCase& c = arr[i];
                        lingxi::Prediction r = py->Predict(c.in);
                        std::wstring line = std::wstring(c.in) +
                            (assert ? L" [CORE]" : L" [extra]") + L" ->";
                        for (size_t k = 0; k < r.cands.size() && k < 3; k++)
                            line += L"  [" + std::to_wstring(k) + L"]" + r.cands[k];
                        line += L"\r\n";
                        DWORD wr2 = 0;
                        WriteFile(hsf, line.data(), (DWORD)(line.size() * 2), &wr2, NULL);
                        if (!assert) continue;
                        bool ok = !r.cands.empty() && r.cands[0] == c.top;
                        wchar_t detail[160];
                        _snwprintf_s(detail, _TRUNCATE,
                                     L"'%ls' top='%ls' got='%ls'",
                                     c.in, c.top,
                                     r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                        Report(ok, "sentence top is contextually correct", detail);
                    }
                };
                runGroup(kSentCore, sizeof(kSentCore) / sizeof(SCase), true);
                runGroup(kSentExtra, sizeof(kSentExtra) / sizeof(SCase), false);
                CloseHandle(hsf);
                wprintf(L"[SENT] sentence results written to %ls\n", sfile);
            }

            // P1. "jintianqi" → 存在以"今"开头的候选
            {
                lingxi::Prediction r = py->Predict(L"jintianqi");
                size_t hit = r.cands.size();
                for (size_t k = 0; k < r.cands.size(); k++)
                {
                    if (r.cands[k].compare(0, 1, L"今") == 0) { hit = k; break; }
                }
                wchar_t detail[192];
                _snwprintf_s(detail, _TRUNCATE, L"first='%ls' cands=%zu 今*@%zu",
                             r.cands.empty() ? L"" : r.cands[0].c_str(), r.cands.size(),
                             (hit == r.cands.size()) ? (size_t)-1 : hit);
                Report(hit != r.cands.size(), "pinyin: jintianqi -> 今*", detail);
            }

            // P2. "ni hao"（带空格，需过滤）→ 含"你"开头候选（"你好"高频）
            {
                lingxi::Prediction r = py->Predict(L"ni hao");
                bool ok = false;
                for (const auto& c : r.cands)
                {
                    if (c.compare(0, 1, L"你") == 0) { ok = true; break; }
                }
                wchar_t detail[192];
                _snwprintf_s(detail, _TRUNCATE, L"first='%ls' cands=%zu",
                             r.cands.empty() ? L"" : r.cands[0].c_str(), r.cands.size());
                Report(ok, "pinyin: ni hao -> 你*", detail);
            }

            // P3. 末尾不完整音节："jintianq" 非空
            {
                lingxi::Prediction r = py->Predict(L"jintianq");
                wchar_t detail[192];
                _snwprintf_s(detail, _TRUNCATE, L"first='%ls' cands=%zu",
                             r.cands.empty() ? L"" : r.cands[0].c_str(), r.cands.size());
                Report(!r.cands.empty(), "pinyin: jintianq (partial tail) non-empty", detail);
            }

            // P4. 非法串："xyzxyz" 为空且不崩溃
            {
                lingxi::Prediction r = py->Predict(L"xyzxyz");
                wchar_t detail[64];
                _snwprintf_s(detail, _TRUNCATE, L"cands=%zu", r.cands.size());
                Report(r.cands.empty(), "pinyin: xyzxyz -> empty", detail);
            }

            // P4c. 小鹤双拼：开启 Shuangpin 后，"jbtm"(jin-tian) 含"今天"
            {
                lingxi::Settings s0 = lingxi::LoadSettings();
                lingxi::Settings s1 = s0;
                s1.shuangpin = true;
                lingxi::SaveSettings(s1);
                lingxi::Prediction r = py->Predict(L"jbtm");
                bool ok = false;
                for (const auto& c : r.cands)
                    if (c.compare(0, 2, L"今天") == 0) { ok = true; break; }
                wchar_t detail[192];
                _snwprintf_s(detail, _TRUNCATE, L"first='%ls' cands=%zu",
                             r.cands.empty() ? L"" : r.cands[0].c_str(),
                             r.cands.size());
                Report(ok, "pinyin: shuangpin jbtm -> 今天*", detail);
                lingxi::SaveSettings(s0); // 恢复，避免污染后续测试
            }

            // P4c2. 双拼 üe 走词典拼写 lve/nve：vjlt(zhan-lve) 含"战略"
            // （回归：T 键曾一律转 "ue"，与词典 lve 拼写错位 → 高频词出错误词链）
            {
                lingxi::Settings s0 = lingxi::LoadSettings();
                lingxi::Settings s1 = s0;
                s1.shuangpin = true;
                lingxi::SaveSettings(s1);
                lingxi::Prediction r = py->Predict(L"vjlt");
                bool ok = false;
                for (const auto& c : r.cands)
                    if (c.find(L"战略") != std::wstring::npos) { ok = true; break; }
                wchar_t detail[192];
                _snwprintf_s(detail, _TRUNCATE, L"first='%ls' cands=%zu",
                             r.cands.empty() ? L"" : r.cands[0].c_str(),
                             r.cands.size());
                Report(ok, "pinyin: shuangpin vjlt -> *战略*", detail);
                lingxi::SaveSettings(s0);
            }

            // P4d. 繁体输出：开启 Traditional 后，"diannao" 首候选为"電腦"
            {
                lingxi::Settings s0 = lingxi::LoadSettings();
                lingxi::Settings s1 = s0;
                s1.traditionalOutput = true;
                lingxi::SaveSettings(s1);
                lingxi::Prediction r = py->Predict(L"diannao");
                bool ok = !r.cands.empty() && r.cands[0] == L"電腦";
                wchar_t detail[192];
                _snwprintf_s(detail, _TRUNCATE, L"first='%ls' cands=%zu",
                             r.cands.empty() ? L"" : r.cands[0].c_str(),
                             r.cands.size());
                Report(ok, "pinyin: traditional diannao -> 電腦", detail);
                lingxi::SaveSettings(s0); // 恢复，避免污染后续测试
            }

            // P4b. 高频口语词（口语覆盖层）：首候选必须精确命中
            {
                struct Case { const wchar_t* in; const wchar_t* expect; };
                const Case cases[] = {
                    { L"haode",      L"好的" },
                    { L"haoma",      L"好吗" },
                    { L"haoba",      L"好吧" },
                    { L"shide",      L"是的" },
                    { L"nihao",      L"你好" },
                    { L"nihaoma",    L"你好吗" },
                    { L"xiexie",     L"谢谢" },
                    { L"zaijian",    L"再见" },
                    { L"bukeqi",     L"不客气" },
                    { L"duibuqi",    L"对不起" },
                    { L"meiguanxi",  L"没关系" },
                    { L"meiwenti",   L"没问题" },
                    { L"zhidaole",   L"知道了" },
                    { L"mingbaile",  L"明白了" },
                    { L"zaoshanghao", L"早上好" },
                    { L"wanshanghao", L"晚上好" },
                    { L"xinniankuaile", L"新年快乐" },
                    { L"shengrikuaile", L"生日快乐" },
                };
                for (const auto& cs : cases)
                {
                    lingxi::Prediction r = py->Predict(cs.in);
                    bool ok = !r.cands.empty() && r.cands[0] == cs.expect;
                    wchar_t detail[160];
                    _snwprintf_s(detail, _TRUNCATE, L"'%ls' -> first='%ls' expect='%ls'",
                                 cs.in, r.cands.empty() ? L"" : r.cands[0].c_str(), cs.expect);
                    Report(ok, "pinyin spoken first-match", detail);
                }
                // P4b+. 多音单字读音补全（数据管线 fix_readings 产物的行为验证）：
                //  1) 口语读音单输入唯一命中；2) 任何登记读音下该字必须出现在候选中
                //     （书面音常更挤不进首选——"shui"首选是"水"，属正确行为）。
                {
                    struct PolyCase { const wchar_t* in; const wchar_t* mustContain; };
                    const PolyCase polys[] = {
                        { L"shei", L"谁" },        // 口语：谁=首选
                        { L"xie",  L"血" },        // 血 白读 xie(些/写 同音,包含即对)
                        { L"shai", L"色" },        // 色 白读 shai
                        { L"liao", L"了" },        // 了 文读 liao
                        { L"dei",  L"得" },        // 得 口语 dei
                        { L"huan", L"还" },        // 还 归还 huan
                        { L"chang", L"长" },        // 长 双读音(chang 与 zhang 都可)
                        { L"hang", L"行" },        // 行 hang(银行) vs xing
                        { L"di",   L"的" },        // 的 dí(的确)
                    };
                    for (const auto& pc : polys)
                    {
                        lingxi::Prediction r = py->Predict(pc.in);
                        bool found = false;
                        for (const auto& c : r.cands)
                            if (!c.empty() && c[0] == pc.mustContain[0] && c.size() == 1)
                                { found = true; break; }
                        wchar_t detail[160];
                        _snwprintf_s(detail, _TRUNCATE, L"'%ls' 含'%ls'=%d (cands=%zu first='%ls')",
                                     pc.in, pc.mustContain, found, r.cands.size(),
                                     r.cands.empty() ? L"" : r.cands[0].c_str());
                        Report(found, "polyphonic single-char reading reachable", detail);
                    }
                    // 她/它/他 与 谁 这类高频基础字绝不允许从任何标准读音消失：
                    {
                        lingxi::Prediction r = py->Predict(L"shei");
                        bool firstOk = !r.cands.empty() && r.cands[0] == L"谁";
                        Report(firstOk, "polyphonic: shei top is 谁", L"");
                    }
                }
            }

            // P4e. 现代词层（modern_terms band 注入修复）：技术词整词/词链可达。
            //      用户报告 "zidongbuquan" 推荐只剩"自动+单字不全"——补全 rank 畸形
            //      +自动补全整词缺失，注入后须精确命中整词。
            {
                struct MCase { const wchar_t* in; const wchar_t* expect; };
                const MCase ms[] = {
                    { L"zidongbuquan", L"自动补全" },
                    { L"tishici", L"提示词" },
                    { L"damoxing", L"大模型" },
                    { L"yanzhengma", L"验证码" },
                    { L"shurufa", L"输入法" },
                };
                for (const auto& m : ms)
                {
                    lingxi::Prediction r = py->Predict(m.in);
                    bool found = false;
                    for (const auto& c : r.cands)
                        if (c == m.expect) { found = true; break; }
                    wchar_t detail[192];
                    _snwprintf_s(detail, _TRUNCATE, L"%ls 含 %ls=%d (cands=%zu first=%ls)",
                                 m.in, m.expect, found, r.cands.size(),
                                 r.cands.empty() ? L"" : r.cands[0].c_str());
                    Report(found, "modern term layer reachable", detail);
                }
            }

            // P4b++ 变体质量闸（用户走查发现首页被单字替换垃圾变体刷屏）：
            // jinwantianshanghenhaode 的首页不得出现「进/金/近/浸/禁+万…天上…」类低质变体。
            {
                lingxi::Prediction r = py->Predict(L"jinwantianshanghenhaode");
                bool dirty = false;
                const wchar_t* junk[] = { L"进万", L"金丸", L"今万", L"近万", L"浸完", L"禁完" };
                size_t head = std::min<size_t>(r.cands.size(), lingxi::kPageSize);
                for (size_t k = 0; k < head; k++)
                    for (const wchar_t* j : junk)
                        if (r.cands[k].size() > 6 && r.cands[k].compare(0, 2, j) == 0)
                            dirty = true;
                bool topOk = !r.cands.empty() && r.cands[0].compare(0, 2, L"今晚") == 0;
                wchar_t detail[160];
                _snwprintf_s(detail, _TRUNCATE, L"top1=%ls dirty=%d",
                             r.cands.empty() ? L"" : r.cands[0].c_str(), dirty);
                Report(!dirty && topOk, "pinyin variant quality gate (no junk on page1)", detail);
            }

            // P4c. 超长混排 + 末尾残段：>10 音节的长串不应崩溃且首选是连贯整句；
            //      末尾拖一个未打全的音节（残段）时，仍要产出覆盖该残段的候选。
            //      这正是 Prune 旧实现（完整/残段同池抢 K_POS 名额）被挤掉的路径：
            //      旧实现在 "nichifanlemawohenb" 上候选塌到 27 个（残段池被完整池
            //      吃光配额），分池修复后回到上限 60。碎切分拼接闸（tier>0 只出
            //      词典词）落地后垃圾墙不再填充上限：健康基线 23（首选"你吃饭了
            //      吗我很吧"，残段 b→吧/不/把/被/并/便/扁 全在，词全真实），
            //      阈值 40 是垃圾墙时代的计数，按无垃圾基线校准为 20。
            {
                struct LCase { const wchar_t* in; size_t minLen; size_t minCands; };
                const LCase longs[] = {
                    { L"womenjintianxiangqugongyuanyiqidakaishui", 8, 5 },  // 12 音节长句
                    { L"zhegewentihenyijiandanwomenyijingtaolunduo", 7, 5 }, // 长句混排
                    { L"nichifanlemawohenb", 4, 20 },                         // 长句 + 残段（垃圾墙清除后基线 23）
                };
                for (const auto& lc : longs)
                {
                    lingxi::Prediction r = py->Predict(lc.in);
                    bool hasLongCand = false;
                    for (const auto& c : r.cands)
                        if (c.size() >= lc.minLen) { hasLongCand = true; break; }
                    if (lc.minCands > 0 && r.cands.size() < lc.minCands) hasLongCand = false;
                    bool ok = !r.cands.empty() && hasLongCand;
                    wchar_t detail[256];
                    _snwprintf_s(detail, _TRUNCATE, L"'%ls…' cands=%zu first='%ls'",
                                 lc.in, r.cands.size(),
                                 r.cands.empty() ? L"" : r.cands[0].c_str());
                    Report(ok, "pinyin long mixed + trailing partial survives", detail);
                }
            }

            // P5. 延迟：中位数 < 10ms（多次预热，逐次采样，取中位数以隔离
            // 共享机器上后台进程的偶发抢占）。
            {
                LatencyStats st = MeasureLatency(py, L"jintianqi", 50);
                wchar_t line[192];
                _snwprintf_s(line, _TRUNCATE,
                             L"ctx='jintianqi' median=%.3fms avg=%.3fms max=%.0fms",
                             st.medianMs, st.avgMs, st.maxMs);
                wprintf(L"[INFO] latency %ls\n", line);
                Report(st.medianMs < 10.0, "pinyin latency median < 10ms (50 runs)", line);
            }

            // P5v. v 模式（数字大写/计算器，ui/engine/calcnum.cpp）：引擎在
            // 词典处理前拦截 v 组合（IsCalcQuery），产出确定性候选
            // （大写/小写/金额 或 计算结果/中文读法），cover=整串消耗。
            {
                auto has = [](const std::vector<std::wstring>& v, const wchar_t* s) {
                    return std::find(v.begin(), v.end(), std::wstring(s)) != v.end();
                };
                // P5v-1. 触发口径：v/V + calc 字符算；裸 v/字母串/算式无前缀不算
                Report(lingxi::IsCalcQuery(L"v1") && lingxi::IsCalcQuery(L"V9") &&
                       lingxi::IsCalcQuery(L"v(1+2)") && lingxi::IsCalcQuery(L"v.5"),
                       "calc IsCalcQuery accepts v+calc char");
                Report(!lingxi::IsCalcQuery(L"v") && !lingxi::IsCalcQuery(L"va") &&
                       !lingxi::IsCalcQuery(L"niha") && !lingxi::IsCalcQuery(L"1+2") &&
                       !lingxi::IsCalcQuery(L""),
                       "calc IsCalcQuery rejects non-calc");
                // P5v-2. 纯数字：大写/小写/金额三连（顺序即优先级）
                {
                    std::vector<std::wstring> c = lingxi::CalcCandidates(L"v123");
                    Report(c.size() == 3 && c[0] == L"壹佰贰拾叁" &&
                           c[1] == L"一百二十三" && c[2] == L"壹佰贰拾叁元整",
                           "calc v123 -> upper/lower/rmb ordered");
                }
                // P5v-3. 零折叠：整组零+低位组前导零合并为一个零（壹亿零伍）
                Report(has(lingxi::CalcCandidates(L"v100000005"), L"壹亿零伍") &&
                       has(lingxi::CalcCandidates(L"v10010"), L"壹万零壹拾"),
                       "calc zero folding (100000005/10010)");
                // P5v-4. 小写"一十"归约（10→十）；前导零剥（007→柒）
                Report(has(lingxi::CalcCandidates(L"v10"), L"十") &&
                       lingxi::CalcCandidates(L"v007")[0] == L"柒" &&
                       has(lingxi::CalcCandidates(L"v007"), L"柒元整"),
                       "calc lower yishi fold + leading zeros");
                // P5v-5. 金额规则矩阵：00→整 / 30→角 / 03→零分 / 03 零元→叁分 / 34→角分
                Report(has(lingxi::CalcCandidates(L"v12.00"), L"壹拾贰元整") &&
                       has(lingxi::CalcCandidates(L"v12.30"), L"壹拾贰元叁角") &&
                       has(lingxi::CalcCandidates(L"v12.03"), L"壹拾贰元零叁分") &&
                       has(lingxi::CalcCandidates(L"v0.03"), L"叁分") &&
                       has(lingxi::CalcCandidates(L"v12.34"), L"壹拾贰元叁角肆分") &&
                       has(lingxi::CalcCandidates(L"v0.5"), L"伍角"),
                       "calc rmb jiao/fen matrix");
                // P5v-6. 全零：零/零元整（大小写同形去重）
                {
                    std::vector<std::wstring> c = lingxi::CalcCandidates(L"v0");
                    Report(!c.empty() && c[0] == L"零" && has(c, L"零元整"),
                           "calc v0 -> ling/ling yuan zheng");
                }
                // P5v-7. 计算器：+ * 括号负数小数；结果/大写/小写三连
                {
                    std::vector<std::wstring> c = lingxi::CalcCandidates(L"v(1+2)*3");
                    Report(c.size() >= 3 && c[0] == L"9" && c[1] == L"玖" && c[2] == L"九",
                           "calc expr (1+2)*3 -> 9/jiu");
                }
                Report(lingxi::CalcCandidates(L"v10/4")[0] == L"2.5" &&
                       has(lingxi::CalcCandidates(L"v10/4"), L"贰点伍"),
                       "calc expr 10/4 -> 2.5 decimal");
                Report(lingxi::CalcCandidates(L"v2-5")[0] == L"-3" &&
                       has(lingxi::CalcCandidates(L"v2-5"), L"负叁"),
                       "calc expr negative result -> fu prefix");
                // P5v-8. 非法输入不产候选：除零/语法错/超界（17 位）
                Report(lingxi::CalcCandidates(L"v1/0").empty() &&
                       lingxi::CalcCandidates(L"v1..2").empty() &&
                       lingxi::CalcCandidates(L"v99999999999999999").empty(),
                       "calc illegal inputs -> no candidates");
                // P5v-9. 引擎集成：Predict 拦截在词典前，cover=整串、replaceLen=0
                {
                    lingxi::Prediction r = py->Predict(L"v1+2");
                    bool coverFull = r.cover.size() == r.cands.size() && !r.cands.empty();
                    for (uint32_t cv : r.cover) if (cv != 4) coverFull = false;
                    Report(!r.cands.empty() && r.cands[0] == L"3" && r.replaceLen == 0 &&
                           coverFull,
                           "calc engine Predict intercept + full cover");
                }
                // P5v-10. Learn 拒收：v 组合不进自学习（防词典污染）
                py->Learn(L"v123", L"XXX污染");
                Report(py->Predict(L"v123").cands[0] == L"壹佰贰拾叁",
                       "calc Learn rejected, dict not polluted");
                // P5v-11. L11 复探针（audit §八-5 销账依据）：一元/括号共用深度闸
                // 64（calcnum.cpp ParseFactor）落地后，20 万连发负号（修复前实测
                // 0xC00000FD 栈溢出）必须被闸拒=空候选且进程存活——若栈溢出回潮，
                // 本用例会直接崩掉 engine_test，门禁即红，无需人工判读。
                {
                    std::wstring deep = std::wstring(L"v") + std::wstring(200000, L'-') + L"1";
                    bool gatedEmpty = lingxi::CalcCandidates(deep).empty();
                    // 浅层 sanity：闸内连负号语义不变（-1→-1，--1→1）
                    std::vector<std::wstring> c1 = lingxi::CalcCandidates(L"v-1");
                    std::vector<std::wstring> c2 = lingxi::CalcCandidates(L"v--1");
                    Report(gatedEmpty && !c1.empty() && c1[0] == L"-1" &&
                           !c2.empty() && c2[0] == L"1",
                           "calc 200k unary-minus probe: depth-gated empty, no overflow");
                }
            }

            // P5s. 符号搜索（ui/symsearch.cpp，符号面板搜索行共用）：
            // 整词>子串排序、多词 AND、拼音/中文/英文检索、空查询空结果。
            {
                auto has = [](const std::vector<std::wstring>& v, const wchar_t* s) {
                    return std::find(v.begin(), v.end(), std::wstring(s)) != v.end();
                };
                // P5s-1. 整词命中排前："mao" 首选 🐱（整词），冒号（子串）其后
                {
                    std::vector<std::wstring> r = lingxi::SymbolSearch(L"mao");
                    Report(!r.empty() && r[0] == L"🐱" && has(r, L"：") && has(r, L"🦉"),
                           "symsearch mao -> cat first, colon substring later");
                }
                // P5s-2. 拼音整词："douhao" 精确命中逗号
                {
                    std::vector<std::wstring> r = lingxi::SymbolSearch(L"douhao");
                    Report(!r.empty() && r[0] == L"，",
                           "symsearch douhao -> comma");
                }
                // P5s-3. 多词 AND："mao tou ying" 只剩猫头鹰
                {
                    std::vector<std::wstring> r = lingxi::SymbolSearch(L"mao tou ying");
                    Report(r.size() == 1 && r[0] == L"🦉",
                           "symsearch multi-token AND narrows to owl");
                }
                // P5s-4. 中文关键词："爱心" 首选红心；"颜文字" 全量 24 条以 (^_^) 开头
                {
                    std::vector<std::wstring> r = lingxi::SymbolSearch(L"爱心");
                    Report(!r.empty() && r[0] == L"\u2764\uFE0F",
                           "symsearch chinese kw aixin -> red heart");
                }
                {
                    std::vector<std::wstring> r = lingxi::SymbolSearch(L"yanwenzi");
                    Report(r.size() == 24 && r[0] == L"(^_^)",
                           "symsearch yanwenzi -> 24 kaomoji");
                }
                // P5s-5. 空查询空结果；进度条可达
                Report(lingxi::SymbolSearch(L"").empty(),
                       "symsearch empty query -> empty");
                Report(has(lingxi::SymbolSearch(L"jindutiao"), L"█"),
                       "symsearch jindutiao -> progress block");
                // P5s-6. 分类可检索性（Tier1-4）：每类有类关键词，按类查询命中数不低于阈值
                {
                    auto cnt = [](const wchar_t* q) { return lingxi::SymbolSearch(q).size(); };
                    Report(cnt(L"shuxue") >= 30,     "symsearch cat math: shuxue>=30");
                    Report(cnt(L"huobi") >= 16,      "symsearch cat currency: huobi>=16");
                    Report(cnt(L"xvhao") >= 20,      "symsearch cat index: xvhao>=20");
                    Report(cnt(L"zhuyin") >= 30,     "symsearch cat zhuyin: zhuyin>=30");
                    Report(cnt(L"shengdiao") >= 24,  "symsearch cat tone: shengdiao>=24");
                    Report(cnt(L"daxie") >= 24,      "symsearch cat greek: daxie>=24");
                    Report(cnt(L"jiantou") >= 15,    "symsearch cat arrow: jiantou>=15");
                    Report(cnt(L"danwei") >= 28,     "symsearch cat unit: danwei>=28");
                }
            }

            // P6. 候选翻页数据有效性：长串应给出第二页且候选唯一、页边界正确
            {
                lingxi::Prediction r = py->Predict(L"jintianq");
                size_t N = r.cands.size();
                bool hasPage2 = N > lingxi::kPageSize;
                bool distinct = hasPage2 && !r.cands[0].empty() &&
                                !r.cands[lingxi::kPageSize].empty() &&
                                r.cands[0] != r.cands[lingxi::kPageSize];
                bool noDup = true;
                for (size_t i = 0; i < N; ++i)
                {
                    for (size_t j = i + 1; j < N; ++j)
                        if (r.cands[i] == r.cands[j]) { noDup = false; break; }
                    if (!noDup) break;
                }
                size_t lastStart = (N > 0) ? (N - 1) / lingxi::kPageSize * lingxi::kPageSize : 0;
                bool boundary = N > 0 && lastStart < N &&
                                lastStart + lingxi::kPageSize >= N;
                bool capped = N <= lingxi::kPredictMaxCandidates;

                wchar_t detail[128];
                _snwprintf_s(detail, _TRUNCATE, L"cands=%zu page2='%ls' pages=%zu",
                             N, hasPage2 ? r.cands[lingxi::kPageSize].c_str() : L"",
                             N ? (N + lingxi::kPageSize - 1) / lingxi::kPageSize : 0);
                Report(hasPage2 && distinct, "pinyin: >9 candidates & page2 distinct", detail);
                Report(noDup, "pinyin: paging candidates unique", detail);
                Report(boundary && capped, "pinyin: page boundary & cap", detail);
            }

            // P6c. B1 部分提交 cover：候选携带"消耗的原始拼音前缀字符数"。
            // "nihao"→"你" 消耗 "ni"(2)；"你好" 消耗全部(5)。
            {
                lingxi::Prediction r = py->Predict(L"nihao");
                bool parallel = r.cover.size() == r.cands.size();
                uint32_t coverNi = 0, coverNihao = 0;
                for (size_t k = 0; k < r.cands.size() && k < r.cover.size(); k++)
                {
                    if (r.cands[k] == L"你") coverNi = r.cover[k];
                    else if (r.cands[k] == L"你好") coverNihao = r.cover[k];
                }
                wchar_t detail[160];
                _snwprintf_s(detail, _TRUNCATE, L"你@%u 你好@%u cover=%zu cands=%zu",
                             coverNi, coverNihao, r.cover.size(), r.cands.size());
                Report(parallel && coverNi == 2, "cover: nihao '你' consumes 2 ('ni')", detail);
                Report(coverNihao == 5, "cover: nihao '你好' consumes 5 (all)", detail);
            }

            // P6c4. nihao 候选序取证："你"必须落在首页（≤9，deep_test T14 依赖）
            {
                lingxi::Prediction r = py->Predict(L"nihao");
                int idxNi = -1;
                for (size_t k = 0; k < r.cands.size(); k++)
                    if (r.cands[k] == L"你") { idxNi = (int)k; break; }
                wchar_t detail[96];
                _snwprintf_s(detail, _TRUNCATE, L"idxNi=%d cands=%zu", idxNi, r.cands.size());
                Report(idxNi >= 0 && idxNi < 9,
                       "cover: nihao '你' in first page (digit-reachable)", detail);
            }

            // P6c2. cover：长串部分词——"jintianqi"→"今天" 消耗 "jintian"(7)
            {
                lingxi::Prediction r = py->Predict(L"jintianqi");
                bool parallel = r.cover.size() == r.cands.size();
                uint32_t coverJt = 0;
                bool hasJt = false;
                for (size_t k = 0; k < r.cands.size() && k < r.cover.size(); k++)
                    if (r.cands[k] == L"今天") { hasJt = true; coverJt = r.cover[k]; break; }
                wchar_t detail[160];
                _snwprintf_s(detail, _TRUNCATE, L"今天@%u has=%d cover=%zu cands=%zu",
                             coverJt, (int)hasJt, r.cover.size(), r.cands.size());
                Report(parallel && hasJt && coverJt == 7,
                       "cover: jintianqi '今天' consumes 7 ('jintian')", detail);
            }

            // P6d. B3 零声母简写兜底：i/u 打头韵母（省略 y/w 的人类写法）
            // 此前一律 0 候选（词典无 i/u 打头音节）。补简写映射后应出对应
            // 高频字，且 cover 严格等于原始键数（rawLen 回放，非改写后音节
            // 长度——i→yi 若回放 text 长度 cover 会错报 2）。
            {
                struct B3Case { const wchar_t* input; const wchar_t* wa; const wchar_t* wb; uint32_t cover; };
                const B3Case b3[] = {
                    { L"i",   L"一", L"",   1 }, // i→yi
                    { L"u",   L"五", L"",   1 }, // u→wu
                    { L"in",  L"音", L"因", 2 }, // in→yin
                    { L"iu",  L"有", L"",   2 }, // iu→you
                    { L"ui",  L"为", L"位", 2 }, // ui→wei
                    { L"uai", L"外", L"歪", 3 }, // uai→wai（"un" 已被 U 模式拆字接管，见 P6u）
                    { L"uo",  L"我", L"",   2 }, // uo→wo
                    { L"ian", L"眼", L"言", 3 }, // ian→yan
                };
                for (const auto& cs : b3)
                {
                    lingxi::Prediction r = py->Predict(cs.input);
                    bool parallel = r.cover.size() == r.cands.size();
                    bool found = false;
                    uint32_t cv = 0;
                    for (size_t k = 0; k < r.cands.size() && k < r.cover.size(); k++)
                    {
                        if (r.cands[k] == cs.wa || (cs.wb[0] && r.cands[k] == cs.wb))
                        {
                            found = true;
                            cv = r.cover[k];
                            break;
                        }
                    }
                    wchar_t detail[128];
                    _snwprintf_s(detail, _TRUNCATE, L"'%ls' found=%d cover=%u cands=%zu",
                                 cs.input, (int)found, cv, r.cands.size());
                    Report(parallel && found && cv == cs.cover,
                           "zero-initial: i/u shorthand yields cands (B3)", detail);
                }
            }

            // P6u. U 模式拆字（GAP §1，ui/engine/uradical.cpp）：'u'+部件读音
            // → 不会读的字。表无前缀命中落回正常拼音（"ui"→为/"uo"→我保活）；
            // "un" 被接管（n 前缀命中 女/牛/鸟，B3 问/文让位——P6d 行已换 uai）。
            // 排序：精确命中 > 已拼出部件多者 > 表序；cover=整串；Learn 拒收。
            {
                // P6u-1. 触发矩阵：小写化、裸 u/非字母后缀/无命中均非
                {
                    bool t1 = lingxi::IsRadicalQuery(L"ujin");
                    bool t2 = lingxi::IsRadicalQuery(L"Ujin");
                    bool t3 = lingxi::IsRadicalQuery(L"un");
                    bool t4 = lingxi::IsRadicalQuery(L"ujinjinjin");
                    bool f0 = lingxi::IsRadicalQuery(L"");
                    bool f1 = lingxi::IsRadicalQuery(L"jin");
                    bool f2 = lingxi::IsRadicalQuery(L"u");
                    bool f3 = lingxi::IsRadicalQuery(L"u金");
                    bool f4 = lingxi::IsRadicalQuery(L"uzhzhzh");
                    Report(t1 && t2 && t3 && t4 && !f0 && !f1 && !f2 && !f3 && !f4,
                           "uradical: IsRadicalQuery trigger matrix");
                }
                // P6u-2. 部件字早出口：ujin → 金 top1，鑫（拼写中）在列
                {
                    lingxi::Prediction r = py->Predict(L"ujin");
                    bool hasXin = false;
                    for (const auto& c : r.cands) if (c == L"鑫") { hasXin = true; break; }
                    wchar_t detail[160];
                    _snwprintf_s(detail, _TRUNCATE, L"first='%ls' cands=%zu",
                                 r.cands.empty() ? L"" : r.cands[0].c_str(), r.cands.size());
                    Report(!r.cands.empty() && r.cands[0] == L"金" && hasXin,
                           "uradical: ujin -> 金 top1, 鑫 pending", detail);
                }
                // P6u-3. 精确命中整串消耗：ujinjinjin → 鑫 top1，cover 全 10、replaceLen=0
                {
                    lingxi::Prediction r = py->Predict(L"ujinjinjin");
                    bool coverFull = r.cover.size() == r.cands.size() && !r.cands.empty();
                    for (uint32_t cv : r.cover) if (cv != 10) coverFull = false;
                    Report(!r.cands.empty() && r.cands[0] == L"鑫" && r.replaceLen == 0 &&
                           coverFull,
                           "uradical: ujinjinjin -> 鑫 full cover");
                }
                // P6u-4/5/6. 叠字族精确命中：水三叠/牛三叠/火四叠
                {
                    struct UCase { const wchar_t* in; const wchar_t* want; };
                    const UCase uc[] = {
                        { L"ushuishuishui", L"淼" },
                        { L"uniuniuniu",   L"犇" },
                        { L"uhuohuohuohuo", L"燚" },
                    };
                    for (const auto& cs : uc)
                    {
                        lingxi::Prediction r = py->Predict(cs.in);
                        wchar_t detail[160];
                        _snwprintf_s(detail, _TRUNCATE, L"first='%ls' cands=%zu",
                                     r.cands.empty() ? L"" : r.cands[0].c_str(), r.cands.size());
                        Report(!r.cands.empty() && r.cands[0] == cs.want,
                               "uradical: stack family exact hit", detail);
                    }
                }
                // P6u-7. 名字用字：umingkong → 曌
                {
                    lingxi::Prediction r = py->Predict(L"umingkong");
                    wchar_t detail[160];
                    _snwprintf_s(detail, _TRUNCATE, L"first='%ls' cands=%zu",
                                 r.cands.empty() ? L"" : r.cands[0].c_str(), r.cands.size());
                    Report(!r.cands.empty() && r.cands[0] == L"曌",
                           "uradical: umingkong -> 曌", detail);
                }
                // P6u-8. 同串双候选（表序兜底）：ubaoyu → 宇+宝 并列
                {
                    lingxi::Prediction r = py->Predict(L"ubaoyu");
                    bool hasYu = false, hasBao = false;
                    for (const auto& c : r.cands)
                    {
                        if (c == L"宇") hasYu = true;
                        if (c == L"宝") hasBao = true;
                    }
                    Report(hasYu && hasBao,
                           "uradical: ubaoyu -> 宇+宝 both exact");
                }
                // P6u-9. 排序细则：精确>前缀（umumu → 林 top1，森 在列）
                {
                    lingxi::Prediction r = py->Predict(L"umumu");
                    bool hasSen = false;
                    for (const auto& c : r.cands) if (c == L"森") { hasSen = true; break; }
                    Report(!r.cands.empty() && r.cands[0] == L"林" && hasSen,
                           "uradical: umumu -> 林 exact before 森 pending");
                }
                // P6u-10. "un" 接管面：牛在列、问/文缺席（B3 让位实证）
                {
                    lingxi::Prediction r = py->Predict(L"un");
                    bool hasNiu = false, hasWen = false;
                    for (const auto& c : r.cands)
                    {
                        if (c == L"牛") hasNiu = true;
                        if (c == L"问" || c == L"文") hasWen = true;
                    }
                    wchar_t detail[160];
                    _snwprintf_s(detail, _TRUNCATE, L"cands=%zu niu=%d wen=%d",
                                 r.cands.size(), (int)hasNiu, (int)hasWen);
                    Report(!r.cands.empty() && hasNiu && !hasWen,
                           "uradical: un takeover (牛 in, 问/文 out)", detail);
                }
                // P6u-11. fall-through：ui→为、uo→我 保活（表无 a/o/i 打头部件读音）
                {
                    lingxi::Prediction r1 = py->Predict(L"ui");
                    lingxi::Prediction r2 = py->Predict(L"uo");
                    bool hasWei = false, hasWo = false;
                    for (const auto& c : r1.cands) if (c == L"为") { hasWei = true; break; }
                    for (const auto& c : r2.cands) if (c == L"我") { hasWo = true; break; }
                    Report(hasWei && hasWo,
                           "uradical: fall-through keeps B3 ui->wei uo->wo");
                }
                // P6u-12. 双拼门控：双拼下 'u' 是键位，U 模式不接管
                {
                    lingxi::Settings s0 = lingxi::LoadSettings();
                    lingxi::Settings s1 = s0;
                    s1.shuangpin = true;
                    lingxi::SaveSettings(s1);
                    lingxi::Prediction r = py->Predict(L"uniuniuniu");
                    bool hasBen = false;
                    for (const auto& c : r.cands) if (c == L"犇") { hasBen = true; break; }
                    wchar_t detail[160];
                    _snwprintf_s(detail, _TRUNCATE, L"first='%ls' cands=%zu",
                                 r.cands.empty() ? L"" : r.cands[0].c_str(), r.cands.size());
                    Report(!hasBen,
                           "uradical: shuangpin gate (u is a key, no takeover)", detail);
                    lingxi::SaveSettings(s0); // 恢复，避免污染后续测试
                }
                // P6u-13. Learn 拒收：U 键不进自学习（防词典污染）
                py->Learn(L"ujinjinjin", L"XX污染");
                Report(py->Predict(L"ujinjinjin").cands[0] == L"鑫",
                       "uradical: Learn rejected, table not polluted");
            }

            // P6e. 拼音隔音符（B6）：组合中撇号（xi'an）强制切分边界，
            // "西安"可出且消耗全部 5 字符；不加隔音的 xian 单音节词"先"仍在
            // （对照组）；隔音后跨边界的整串音节词不出现（xi'an 不出"先"）；
            // 弯撇号（U+2019）同样归一（只断言存在：非 ASCII 使 cover 降级 0，
            // 与"非 ASCII 字符 UTF-8 变长不下标映射"口径一致）。
            {
                struct SepCase { const wchar_t* input; const wchar_t* word; bool checkCover; uint32_t cover; };
                const SepCase sp[] = {
                    { L"xi'an",      L"西安", true,  5 },
                    { L"xi\u2019an", L"西安", false, 0 },
                };
                for (const auto& cs : sp)
                {
                    lingxi::Prediction r = py->Predict(cs.input);
                    bool parallel = r.cover.size() == r.cands.size();
                    bool found = false;
                    uint32_t cv = 0;
                    for (size_t k = 0; k < r.cands.size() && k < r.cover.size(); k++)
                        if (r.cands[k] == cs.word) { found = true; cv = r.cover[k]; break; }
                    wchar_t detail[160];
                    _snwprintf_s(detail, _TRUNCATE, L"'%ls' found=%d cover=%u cands=%zu",
                                 cs.input, (int)found, cv, r.cands.size());
                    bool ok = parallel && found && (!cs.checkCover || cv == cs.cover);
                    Report(ok, "separator: xi'an yields '西安' (B6)", detail);
                }
                {
                    // 对照组：无隔音 xian → 单音节词"先"仍在，且消耗整串 4 字符
                    lingxi::Prediction r = py->Predict(L"xian");
                    bool hasXian = false;
                    uint32_t cvXian = 0;
                    for (size_t k = 0; k < r.cands.size() && k < r.cover.size(); k++)
                        if (r.cands[k] == L"先") { hasXian = true; cvXian = r.cover[k]; break; }
                    wchar_t detail[128];
                    _snwprintf_s(detail, _TRUNCATE, L"has先=%d cover=%u cands=%zu",
                                 (int)hasXian, cvXian, r.cands.size());
                    Report(hasXian && cvXian == 4,
                           "separator: bare xian '先' consumes all 4", detail);
                }
                {
                    // 隔音边界阻断整串音节路径：xi'an 里"先"若在，只能是简拼
                    // 前缀解读（B1 部分消耗：只吃 'x' 一个字符，cover=1）——
                    // 不再是 xian 整串消耗 4。这正是隔音符的因果效果。
                    lingxi::Prediction r = py->Predict(L"xi'an");
                    int idxXian = -1;
                    uint32_t cvXian = 0;
                    for (size_t k = 0; k < r.cands.size() && k < r.cover.size(); k++)
                        if (r.cands[k] == L"先") { idxXian = (int)k; cvXian = r.cover[k]; break; }
                    bool ok = idxXian < 0 || cvXian == 1; // 不出现，或仅简拼前缀
                    wchar_t detail[128];
                    _snwprintf_s(detail, _TRUNCATE, L"先idx=%d cover=%u cands=%zu",
                                 idxXian, cvXian, r.cands.size());
                    Report(ok, "separator: xi'an '先' is prefix-only (cover 1, not 4)", detail);
                }
                {
                    // B6×B1 集成：xi'an 部分提交"西"（cover=2）后剩余 "'an"——
                    // 引擎必须把前导隔音边界随后续音节一并消耗（安 cover=3），
                    // 不在组合里留死撇号。
                    lingxi::Prediction r = py->Predict(L"'an");
                    bool parallel = r.cover.size() == r.cands.size();
                    int idxAn = -1;
                    uint32_t cvAn = 0;
                    for (size_t k = 0; k < r.cands.size() && k < r.cover.size(); k++)
                        if (r.cands[k] == L"安") { idxAn = (int)k; cvAn = r.cover[k]; break; }
                    wchar_t detail[128];
                    _snwprintf_s(detail, _TRUNCATE, L"安idx=%d cover=%u cands=%zu",
                                 idxAn, cvAn, r.cands.size());
                    Report(parallel && idxAn >= 0 && cvAn == 3,
                           "separator: leftover 'an consumes boundary too (cover 3)", detail);
                }
                {
                    // 音节数校验不误伤对照组：无隔音 xian 的双音节词"西安"仍从
                    // [xi][an] 路径命中（cover=4）——校验只拒跨隔音边界的撞键词。
                    lingxi::Prediction r = py->Predict(L"xian");
                    int idxXa = -1;
                    uint32_t cvXa = 0;
                    for (size_t k = 0; k < r.cands.size() && k < r.cover.size(); k++)
                        if (r.cands[k] == L"西安") { idxXa = (int)k; cvXa = r.cover[k]; break; }
                    wchar_t detail[128];
                    _snwprintf_s(detail, _TRUNCATE, L"西安idx=%d cover=%u cands=%zu",
                                 idxXa, cvXa, r.cands.size());
                    Report(idxXa >= 0 && cvXa == 4,
                           "separator: bare xian '西安' still matches (no over-reject)", detail);
                }
            }

            // P6c3. 双拼不提供 cover（键位→全拼不可逆推）：全 0 或空
            {
                lingxi::Settings s0 = lingxi::LoadSettings();
                lingxi::Settings s1 = s0;
                s1.shuangpin = true;
                lingxi::SaveSettings(s1);
                lingxi::Prediction r = py->Predict(L"jbtm");
                bool allZero = true;
                for (uint32_t cv : r.cover)
                    if (cv != 0) { allZero = false; break; }
                wchar_t detail[96];
                _snwprintf_s(detail, _TRUNCATE, L"cover=%zu cands=%zu",
                             r.cover.size(), r.cands.size());
                Report(allZero, "cover: shuangpin provides none (all zero)", detail);
                lingxi::SaveSettings(s0);
            }

            // P6b. 翻页页码纯函数 NextPageStart：边界/夹取（不依赖引擎数据）
            {
                struct Case { size_t total; uint32_t cur; int delta; uint32_t expect; };
                const Case cases[] = {
                    {0,  0,  +1, 0},   // 空
                    {1,  0,  +1, 0},   // 单页：下页不动
                    {1,  0,  -1, 0},   // 单页：上页不动
                    {9,  0,  +1, 0},   // 恰好一页
                    {10, 0,  +1, 9},   // 两页(末页1)
                    {10, 9,  +1, 9},   // 末页再向下：夹到末页
                    {10, 9,  -1, 0},   // 末页向上
                    {18, 0,  +1, 9},   // 恰好两页
                    {18, 9,  +1, 9},   // 第二页已到底
                    {19, 0,  +1, 9},
                    {19, 9,  +1, 18},  // 三页(末页1)
                    {19, 18, -1, 9},
                    {56, 0,  +1, 9},
                    {56, 9,  +1, 18},
                    {56, 36, +1, 45},
                    {56, 45, +1, 54},  // 56=6*9+2 → 7 页，末页起点 54(含 54、55)
                    {56, 54, +1, 54},  // 末页夹取
                    {56, 54, -1, 45},
                };
                for (const auto& cs : cases)
                {
                    uint32_t got = lingxi::NextPageStart(cs.total, cs.cur, cs.delta);
                    wchar_t detail[128];
                    _snwprintf_s(detail, _TRUNCATE, L"total=%zu cur=%u d=%+d -> %u (expect %u)",
                                 cs.total, cs.cur, cs.delta, got, cs.expect);
                    Report(got == cs.expect, "NextPageStart paging boundary", detail);
                }
            }

            // P7. 中文标点映射（纯函数，不依赖引擎）
            {
                struct PCase { WPARAM vk; bool shift; int n; wchar_t first; };
                const PCase cases[] = {
                    {VK_OEM_PERIOD, false, 1, L'\u3002'}, // . → 。
                    {VK_OEM_PERIOD, true,  1, L'\u300B'}, // Shift+. → 》
                    {VK_OEM_COMMA,  false, 1, L'\uFF0C'}, // , → ，
                    {VK_OEM_COMMA,  true,  1, L'\u300A'}, // Shift+, → 《
                    {VK_OEM_2,      false, 1, L'\u3001'}, // / → 、
                    {VK_OEM_2,      true,  1, L'\uFF1F'}, // Shift+/ → ？
                    {VK_OEM_1,      false, 1, L'\uFF1B'}, // ; → ；
                    {VK_OEM_1,      true,  1, L'\uFF1A'}, // Shift+; → ：
                    {VK_OEM_5,      false, 1, L'\u3001'}, // \ → 、
                    {VK_OEM_3,      false, 1, L'\u00B7'}, // ` → ·
                    {VK_OEM_4,      false, 1, L'\u3010'}, // [ → 【
                    {VK_OEM_6,      false, 1, L'\u3011'}, // ] → 】
                    {L'1',          false, 0, 0},         // 数字 1 不映射（选词）
                    {L'1',          true,  1, L'\uFF01'}, // Shift+1 → ！
                    {L'9',          true,  1, L'\uFF08'}, // Shift+9 → （
                    {L'0',          true,  1, L'\uFF09'}, // Shift+0 → ）
                    {L'4',          true,  1, L'\uFFE5'}, // Shift+4 → ￥
                    {L'6',          true,  2, L'\u2026'}, // Shift+6 → ……
                    {VK_OEM_MINUS,  false, 0, 0},         // 无 Shift - 保留（翻页）
                    {VK_OEM_MINUS,  true,  2, L'\u2014'}, // Shift+- → ——
                    {VK_OEM_PLUS,   false, 0, 0},         // 无 Shift = 保留（翻页）
                    {VK_OEM_PLUS,   true,  1, L'\uFF0B'}, // Shift+= → ＋
                };
                for (const auto& cs : cases)
                {
                    wchar_t out[3] = {0,0,0};
                    bool dq = false, sq = false;
                    int n = lingxi::MapChinesePunct(cs.vk, cs.shift, dq, sq, out);
                    wchar_t detail[96];
                    _snwprintf_s(detail, _TRUNCATE, L"vk=0x%X shift=%d n=%d first=U+%04X",
                                 (unsigned)cs.vk, (int)cs.shift, n, (unsigned)out[0]);
                    Report(n == cs.n && (n == 0 || out[0] == cs.first),
                           "Chinese punct mapping", detail);
                }
                // 成对引号：依次 “ ” ‘ ’
                {
                    wchar_t out[3];
                    bool dq = false, sq = false;
                    lingxi::MapChinesePunct(VK_OEM_7, true, dq, sq, out);
                    bool o1 = out[0] == L'\u201C'; // “
                    lingxi::MapChinesePunct(VK_OEM_7, true, dq, sq, out);
                    bool o2 = out[0] == L'\u201D'; // ”
                    lingxi::MapChinesePunct(VK_OEM_7, false, dq, sq, out);
                    bool o3 = out[0] == L'\u2018'; // ‘
                    lingxi::MapChinesePunct(VK_OEM_7, false, dq, sq, out);
                    bool o4 = out[0] == L'\u2019'; // ’
                    Report(o1 && o2 && o3 && o4, "Chinese paired quotes “ ” ‘ ’",
                           o1 && o2 && o3 && o4 ? L"OK" : L"mismatch");
                }
            }

            // P8. 模糊音（z/zh、c/ch、s/sh、n/l、前/后鼻音互通；精确优先）
            {
                // 打“模糊”的音，候选中应能找到正确字（容错命中）
                struct FCase { const wchar_t* in; const wchar_t* expect; };
                const FCase hits[] = {
                    {L"zang", L"张"},   // 平舌 zang -> 翘舌 zhang
                    {L"zhang", L"脏"},  // 反向 zhang -> zang
                    {L"si", L"是"},     // 平舌 si -> 翘舌 shi
                    {L"shi", L"四"},    // 反向
                    {L"na", L"拉"},     // n -> l
                    {L"la", L"那"},     // l -> n
                    {L"jin", L"京"},    // 前鼻 jin -> 后鼻 jing
                    {L"jing", L"金"},   // 后鼻 jing -> 前鼻 jin
                    {L"chen", L"程"},   // chen -> cheng
                    {L"cheng", L"陈"},  // cheng -> chen
                    {L"xian", L"想"},   // xian -> xiang
                    {L"xiang", L"先"},  // xiang -> xian
                    {L"nan", L"兰"},    // n/l 互通
                    {L"lan", L"南"},
                    {L"zao", L"找"},    // 平舌 zao -> 翘舌 zhao
                    {L"zhao", L"早"},
                };
                auto hasCand = [](const lingxi::Prediction& r, const std::wstring& w) {
                    for (const auto& c : r.cands)
                        if (c == w) return true;
                    return false;
                };
                for (const auto& c : hits)
                {
                    lingxi::Prediction r = py->Predict(c.in);
                    bool ok = hasCand(r, c.expect);
                    wchar_t detail[128];
                    _snwprintf_s(detail, _TRUNCATE,
                                 L"fuzzy '%ls' should include '%ls' (got %zu)",
                                 c.in, c.expect, r.cands.size());
                    Report(ok, "fuzzy pinyin tolerant hit", detail);
                }
                // 精确优先：打规范音时，高频精确字仍是首选，不被模糊字抢占
                {
                    struct ECase { const wchar_t* in; const wchar_t* top; };
                    const ECase tops[] = {
                        {L"zhang", L"长"},
                        {L"shi", L"是"},
                        {L"jing", L"经"},
                        {L"xiang", L"想"},
                    };
                    for (const auto& c : tops)
                    {
                        lingxi::Prediction r = py->Predict(c.in);
                        bool ok = !r.cands.empty() && r.cands[0] == c.top;
                        wchar_t detail[128];
                        _snwprintf_s(detail, _TRUNCATE, L"exact '%ls' top='%ls' got='%ls'",
                                     c.in, c.top,
                                     r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                        Report(ok, "fuzzy pinyin exact-first preserved", detail);
                    }
                }
            }

            // P8b. 模糊音组细分开关（Settings.fuzzyGroups 位掩码）门控行为
            {
                lingxi::Settings s0 = lingxi::LoadSettings();
                lingxi::Settings sg = s0;
                sg.fuzzyPinyin = true;
                sg.fuzzyGroups = 1; // 仅 zh/z 组
                lingxi::SaveSettings(sg);   // 引擎每次 Predict 热读，组变更自动重建模糊索引
                {
                    lingxi::Prediction r = py->Predict(L"si");
                    bool hasShi = false;
                    for (const auto& c : r.cands) if (c == L"是") hasShi = true;
                    Report(!hasShi, "fuzzy group gating: sh/s off, si no longer hits 是", L"");
                }
                {
                    lingxi::Prediction r = py->Predict(L"zhang");
                    bool hasZang = false;
                    for (const auto& c : r.cands) if (c == L"脏") hasZang = true;
                    Report(hasZang, "fuzzy group gating: zh/z on, zhang still hits 脏", L"");
                }
                {
                    lingxi::Prediction r = py->Predict(L"jin");
                    bool hasJing = false;
                    for (const auto& c : r.cands) if (c == L"京") hasJing = true;
                    Report(!hasJing, "fuzzy group gating: nasal off, jin no longer hits 京", L"");
                }
                sg.fuzzyGroups = 0xFF; // 全组恢复
                lingxi::SaveSettings(sg);
                {
                    lingxi::Prediction r = py->Predict(L"si");
                    bool hasShi = false;
                    for (const auto& c : r.cands) if (c == L"是") hasShi = true;
                    Report(hasShi, "fuzzy group gating: all groups on, si hits 是 again", L"");
                }
                {
                    // l/r 组（bit6）× n/l 组（bit3）共存回归（审计轮 5 else-if 修复）：
                    // 修复前 l→r 分支被 bit3 的 l→n 分支遮挡——双组同开时 lang 只出
                    // nang 变体，r→l 却正常（单向不对称）；修复后须同时出 rang（让）
                    lingxi::Prediction r = py->Predict(L"lang");
                    bool hasRang = false;
                    for (const auto& c : r.cands) if (c == L"让") hasRang = true;
                    Report(hasRang, "fuzzy groups: l/r+n/l coexist, lang hits 让", L"");
                }
                {
                    // f/h 组（bit5）：关→"he" 不再模糊出"和"(fo? 反向)；开→"fei" 可出"费"旁证。
                    // 用 hu↔fu：关组时 hu 输入不得出"服"（fu 系），开组时应可。
                    sg.fuzzyGroups = 1; lingxi::SaveSettings(sg);
                    lingxi::Prediction r1 = py->Predict(L"fu");
                    bool noHu = true;
                    for (const auto& c : r1.cands) if (c == L"湖") noHu = false;
                    Report(noHu, "fuzzy group gating: f/h off, fu no longer hits 湖", L"");
                    sg.fuzzyGroups = 0xFF; lingxi::SaveSettings(sg);
                    lingxi::Prediction r2 = py->Predict(L"fu");
                    bool hasHu = false;
                    for (const auto& c : r2.cands) if (c == L"湖") hasHu = true;
                    Report(hasHu, "fuzzy group gating: f/h on, fu hits 湖", L"");
                }
                lingxi::SaveSettings(s0); // 还原沙盒基线
            }

            // P8c. 无痕模式 IncognitoActive（批次 G）：单键快读与 LoadSettings 一致
            {
                Report(!lingxi::IncognitoActive(),
                       "incognito: default false (key absent)", L"");
                lingxi::Settings s0 = lingxi::LoadSettings();
                lingxi::Settings s1 = s0;
                s1.incognito = true;
                lingxi::SaveSettings(s1);
                Report(lingxi::IncognitoActive(),
                       "incognito: IncognitoActive true after set", L"");
                lingxi::Settings s2 = lingxi::LoadSettings();
                Report(s2.incognito,
                       "incognito: LoadSettings roundtrip keeps flag", L"");
                s1.incognito = false;
                lingxi::SaveSettings(s1);
                Report(!lingxi::IncognitoActive(),
                       "incognito: IncognitoActive false after clear", L"");
                lingxi::SaveSettings(s0); // 还原沙盒基线
            }

            // P9. 自学习用户词典：选字后该词置顶（备份/恢复真实词典，不污染）
            {
                // 拼出 userdict 路径并备份现有内容
                wchar_t appdata[MAX_PATH] = {0};
                GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
                std::wstring upath = std::wstring(appdata) + L"\\LingxiIme\\userdict.txt";
                std::vector<uint8_t> backup;
                bool existed = false;
                HANDLE hb = CreateFileW(upath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hb != INVALID_HANDLE_VALUE)
                {
                    existed = true;
                    DWORD sz = GetFileSize(hb, NULL);
                    backup.resize(sz);
                    DWORD rd = 0;
                    ReadFile(hb, backup.data(), sz, &rd, NULL);
                    CloseHandle(hb);
                }
                DeleteFileW(upath.c_str()); // 从干净状态开始

                // 全新预测：zang 首选不是“脏”（模糊高频“长/张”在前）
                lingxi::Prediction r0 = py->Predict(L"zang");
                bool beforeNotZang = r0.cands.empty() || r0.cands[0] != L"脏";
                Report(beforeNotZang, "learn: before feedback top is not '脏'",
                       r0.cands.empty() ? L"<none>" : r0.cands[0].c_str());

                // 用户两次选“脏”（模拟偏好）
                py->Learn(L"zang", L"脏");
                py->Learn(L"zang", L"脏");

                // 再打 zang：“脏”应置顶
                lingxi::Prediction r1 = py->Predict(L"zang");
                bool promoted = !r1.cands.empty() && r1.cands[0] == L"脏";
                Report(promoted, "learn: after feedback '脏' promoted to top",
                       r1.cands.empty() ? L"<none>" : r1.cands[0].c_str());

                // 持久化：新建引擎实例（重新加载词典）后“脏”仍置顶。
                // 落盘已后台化（Learn 即返回、detached 线程写盘）——轮询
                // userdict 文件等异步写完成（测试数据只有"脏\tzang\t2"，
                // 查 ASCII "zang" 即可），超时 2s 后照常创建实例让断言自己说话。
                for (int wait = 0; wait < 200; ++wait)
                {
                    HANDLE hf = CreateFileW(upath.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hf != INVALID_HANDLE_VALUE)
                    {
                        DWORD sz = GetFileSize(hf, NULL);
                        std::vector<uint8_t> buf(sz ? sz : 1);
                        DWORD rd = 0;
                        ReadFile(hf, buf.data(), sz, &rd, NULL);
                        CloseHandle(hf);
                        std::string txt(reinterpret_cast<char*>(buf.data()), rd);
                        if (txt.find("\tzang\t") != std::string::npos) break;
                    }
                    Sleep(10);
                }
                {
                    lingxi::IPredictor* py2 = lingxi::CreatePinyinPredictor();
                    lingxi::Prediction r2 = py2->Predict(L"zang");
                    bool persisted = !r2.cands.empty() && r2.cands[0] == L"脏";
                    Report(persisted, "learn: preference persisted across reload",
                           r2.cands.empty() ? L"<none>" : r2.cands[0].c_str());
                    delete py2;
                }

                // 真正新词（不在基础库）：学习后应能整串重现
                py->Learn(L"miaomiao", L"喵喵");
                lingxi::Prediction r3 = py->Predict(L"miaomiao");
                bool newWordAppears = false;
                for (const auto& c : r3.cands)
                    if (c == L"喵喵") { newWordAppears = true; break; }
                Report(newWordAppears, "learn: brand-new word '喵喵' appears",
                       r3.cands.empty() ? L"<none>" : r3.cands[0].c_str());

                // 简拼：输入 mm 应能调出"喵喵"（用户词支持缩写）
                lingxi::Prediction r4 = py->Predict(L"mm");
                bool abbreviated = false;
                for (const auto& c : r4.cands)
                    if (c == L"喵喵") { abbreviated = true; break; }
                Report(abbreviated, "learn: abbreviated 'mm' -> '喵喵'",
                       r4.cands.empty() ? L"<none>" : r4.cands[0].c_str());

                // 恢复原有词典（无则删除测试文件）。喵喵学习的后台落盘必须先
                // 落地，否则恢复备份后会被迟到的异步写覆盖成测试数据。
                for (int wait = 0; wait < 200; ++wait)
                {
                    HANDLE hf = CreateFileW(upath.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hf != INVALID_HANDLE_VALUE)
                    {
                        DWORD sz = GetFileSize(hf, NULL);
                        std::vector<uint8_t> buf(sz ? sz : 1);
                        DWORD rd = 0;
                        ReadFile(hf, buf.data(), sz, &rd, NULL);
                        CloseHandle(hf);
                        std::string txt(reinterpret_cast<char*>(buf.data()), rd);
                        if (txt.find("\tmiaomiao\t") != std::string::npos) break;
                    }
                    Sleep(10);
                }
                if (existed)
                {
                    HANDLE hw = CreateFileW(upath.c_str(), GENERIC_WRITE, 0, NULL,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hw != INVALID_HANDLE_VALUE)
                    {
                        DWORD wr = 0;
                        WriteFile(hw, backup.data(), (DWORD)backup.size(), &wr, NULL);
                        CloseHandle(hw);
                    }
                }
                else DeleteFileW(upath.c_str());
            }

            // P10. R4 拼音纠错：常见误拼（韵母全写/gn 颠倒/丢 g）改写后仍出词
            {
                struct TypoCase { const wchar_t* input; const wchar_t* expect; };
                const TypoCase kTypos[] = {
                    // ① 韵母全写 → 缩写（liou→liu、shuei→shui、chuen→chun）
                    { L"niou", L"牛" }, { L"shuei", L"水" }, { L"chuen", L"春" },
                    // ② gn → ng 颠倒（shegn→sheng、magn→mang）
                    { L"shegn", L"生" }, { L"magn", L"忙" },
                    // ③ 丢 g 补全（zhon→zhong、din→ding、son→song）
                    { L"zhon", L"中" }, { L"din", L"定" }, { L"son", L"送" },
                };
                for (const auto& tc : kTypos)
                {
                    lingxi::Prediction r = py->Predict(tc.input);
                    bool found = false;
                    for (const auto& w : r.cands)
                        if (w.find(tc.expect) != std::wstring::npos) { found = true; break; }
                    wchar_t detail[160];
                    _snwprintf_s(detail, _TRUNCATE, L"'%ls' 含'%ls'=%d (cands=%zu first='%ls')",
                                 tc.input, tc.expect, found ? 1 : 0, r.cands.size(),
                                 r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                    Report(found, "R4 typo correction", detail);
                }
                // 边界安全：纠错不得劫持合法切分——ren 的首选仍是"人"
                {
                    lingxi::Prediction r = py->Predict(L"ren");
                    bool ok = !r.cands.empty() && r.cands[0].find(L"人") != std::wstring::npos;
                    wchar_t detail[160];
                    _snwprintf_s(detail, _TRUNCATE, L"first='%ls'",
                                 r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                    Report(ok, "R4 valid syllable not hijacked", detail);
                }
            }

            // P11. R3 日期时间直出：rq/sj/xq/rqsj 整串触发注入置顶
            {
                struct DtCase { const wchar_t* input; const wchar_t* must1; const wchar_t* must2; };
                const DtCase kDt[] = {
                    { L"rq", L"年", L"月" },   // 2026年10月29日
                    { L"sj", L":", L":" },     // 21:45
                    { L"xq", L"星", L"期" },   // 星期四
                    { L"rqsj", L"年", L":" },  // 2026年10月29日 21:45
                };
                for (const auto& dc : kDt)
                {
                    lingxi::Prediction r = py->Predict(dc.input);
                    bool ok = !r.cands.empty() &&
                              r.cands[0].find(dc.must1) != std::wstring::npos &&
                              r.cands[0].find(dc.must2) != std::wstring::npos;
                    wchar_t detail[160];
                    _snwprintf_s(detail, _TRUNCATE, L"'%ls' first='%ls'",
                                 dc.input, r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                    Report(ok, "R3 datetime trigger", detail);
                }
                // 非触发不误伤：继续输入 rqa 首选不再是日期
                {
                    lingxi::Prediction r = py->Predict(L"rqa");
                    bool ok = r.cands.empty() || r.cands[0].find(L"年") == std::wstring::npos;
                    Report(ok, "R3 no hijack on longer input",
                           r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                }
                // 格式多样性：rq 至少含 - 或 / 数字格式备选
                {
                    lingxi::Prediction r = py->Predict(L"rq");
                    bool alt = false;
                    for (const auto& w : r.cands)
                        if (w.find(L"-") != std::wstring::npos ||
                            w.find(L"/") != std::wstring::npos) { alt = true; break; }
                    Report(alt, "R3 rq alt formats", L"");
                }
                // R7b 扩展格式：rq 大写日期（〇）、sj 中文时分、xq 礼拜、rqsj 日期+星期
                {
                    struct ExtCase { const wchar_t* input; const wchar_t* must; const wchar_t* name; };
                    const ExtCase kExt[] = {
                        { L"rq", L"〇", L"rq uppercase cn date" },
                        { L"sj", L"时", L"sj chinese hms" },
                        { L"xq", L"礼拜", L"xq libai alias" },
                        { L"rqsj", L"星期", L"rqsj date+weekday" },
                    };
                    for (const auto& ec : kExt)
                    {
                        lingxi::Prediction r = py->Predict(ec.input);
                        bool hit = false;
                        for (const auto& w : r.cands)
                            if (w.find(ec.must) != std::wstring::npos) { hit = true; break; }
                        Report(hit, "R3 extended formats", ec.name);
                    }
                }
            }

            // P5d. Tier1-5：农历表查锚点回归 + rq 无年份形态 + mt/ht/nl 触发注入
            {
                struct LAnchor { int y, m, d; const wchar_t* expect; };
                const LAnchor kLunar[] = {
                    { 1900,  1, 31, L"正月初一" },   // 表锚点（农历 1900 年首日）
                    { 2023,  1, 22, L"正月初一" },   // 2023 春节
                    { 2024,  2, 10, L"正月初一" },   // 2024 春节
                    { 2025,  1, 29, L"正月初一" },   // 2025 春节
                    { 2025,  7, 25, L"闰六月初一" }, // 2025 闰六月起
                    { 2026,  2, 17, L"正月初一" },   // 2026 春节
                    { 2027,  2,  6, L"正月初一" },   // 2027 春节
                    { 2024,  9, 17, L"八月十五" },   // 2024 中秋
                    { 2026,  9, 25, L"八月十五" },   // 2026 中秋
                };
                for (const auto& a : kLunar)
                {
                    std::wstring got = lingxi::LunarDateStr(a.y, a.m, a.d);
                    bool ok = got == a.expect;
                    wchar_t detail[160];
                    _snwprintf_s(detail, _TRUNCATE, L"%d-%02d-%02d got='%ls' want='%ls'",
                                 a.y, a.m, a.d, got.c_str(), a.expect);
                    Report(ok, "P5d lunar anchor", detail);
                }
                Report(lingxi::LunarDateStr(1899, 12, 31).empty() &&
                       lingxi::LunarDateStr(2101, 1, 1).empty(),
                       "P5d lunar range guard", L"");
                {
                    struct GAnchor { int y; const wchar_t* gz; };
                    const GAnchor kG[] = { { 2024, L"甲辰" }, { 2025, L"乙巳" },
                                           { 2026, L"丙午" }, { 2027, L"丁未" } };
                    for (const auto& g : kG)
                    {
                        std::wstring got = lingxi::LunarGanzhiYear(g.y);
                        bool ok = got == g.gz;
                        wchar_t detail[120];
                        _snwprintf_s(detail, _TRUNCATE, L"%d got='%ls' want='%ls'",
                                     g.y, got.c_str(), g.gz);
                        Report(ok, "P5d ganzhi anchor", detail);
                    }
                }
                // 触发注入（期望值以本地时间动态计算，避免硬编码随日失效）
                SYSTEMTIME st5{};
                GetLocalTime(&st5);
                {
                    lingxi::Prediction r = py->Predict(L"rq");
                    wchar_t want[48];
                    _snwprintf_s(want, _TRUNCATE, L"%u月%u日", st5.wMonth, st5.wDay);
                    bool hit = false;
                    for (const auto& w : r.cands)
                        if (w == want) { hit = true; break; }
                    Report(hit, "P5d rq yearless form", want);
                }
                {
                    FILETIME ft5{};
                    SYSTEMTIME n5{};
                    SystemTimeToFileTime(&st5, &ft5);
                    ULONGLONG u5 = ((ULONGLONG)ft5.dwHighDateTime << 32) | ft5.dwLowDateTime;
                    u5 += 864000000000ULL;
                    ft5.dwLowDateTime = (DWORD)u5;
                    ft5.dwHighDateTime = (DWORD)(u5 >> 32);
                    FileTimeToSystemTime(&ft5, &n5);
                    lingxi::Prediction r = py->Predict(L"mt");
                    wchar_t want[48];
                    _snwprintf_s(want, _TRUNCATE, L"%u年%02u月%02u日",
                                 n5.wYear, n5.wMonth, n5.wDay);
                    bool hit = false;
                    for (const auto& w : r.cands)
                        if (w == want) { hit = true; break; }
                    Report(hit, "P5d mt tomorrow date", want);
                }
                {
                    FILETIME ft5{};
                    SYSTEMTIME n5{};
                    SystemTimeToFileTime(&st5, &ft5);
                    ULONGLONG u5 = ((ULONGLONG)ft5.dwHighDateTime << 32) | ft5.dwLowDateTime;
                    u5 += 2ULL * 864000000000ULL;
                    ft5.dwLowDateTime = (DWORD)u5;
                    ft5.dwHighDateTime = (DWORD)(u5 >> 32);
                    FileTimeToSystemTime(&ft5, &n5);
                    lingxi::Prediction r = py->Predict(L"ht");
                    wchar_t want[48];
                    _snwprintf_s(want, _TRUNCATE, L"%u年%02u月%02u日",
                                 n5.wYear, n5.wMonth, n5.wDay);
                    bool hit = false;
                    for (const auto& w : r.cands)
                        if (w == want) { hit = true; break; }
                    Report(hit, "P5d ht day-after date", want);
                }
                {
                    lingxi::Prediction r = py->Predict(L"nl");
                    std::wstring wantS = L"农历" +
                        lingxi::LunarDateStr(st5.wYear, st5.wMonth, st5.wDay);
                    bool hit = false;
                    for (const auto& w : r.cands)
                        if (w == wantS) { hit = true; break; }
                    wchar_t detail[160];
                    _snwprintf_s(detail, _TRUNCATE, L"want='%ls'", wantS.c_str());
                    Report(hit, "P5d nl lunar today", detail);
                }
                // 非触发不误伤：mtt 不注入日期（首选不含"年"）
                {
                    lingxi::Prediction r = py->Predict(L"mtt");
                    bool ok = r.cands.empty() ||
                              r.cands[0].find(L"年") == std::wstring::npos;
                    Report(ok, "P5d mt no hijack",
                           r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                }
            }

            // P12. R2 自定义短语：缩写整串命中注入置顶，热载增删生效
            {
                wchar_t appdata[MAX_PATH] = {0};
                GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
                std::wstring dir = std::wstring(appdata) + L"\\LingxiIme";
                CreateDirectoryW(dir.c_str(), NULL);
                std::wstring ppath = dir + L"\\phrases.txt";
                std::vector<uint8_t> backup;
                bool existed = false;
                HANDLE hb = CreateFileW(ppath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hb != INVALID_HANDLE_VALUE)
                {
                    existed = true;
                    DWORD sz = GetFileSize(hb, NULL);
                    backup.resize(sz);
                    DWORD rd = 0;
                    ReadFile(hb, backup.data(), sz, &rd, NULL);
                    CloseHandle(hb);
                }

                // 写测试短语（UTF-8 带 BOM）：addr→地址长句；sj→收件人一行
                auto wToUtf8 = [](const std::wstring& w) {
                    if (w.empty()) return std::string();
                    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                                NULL, 0, NULL, NULL);
                    std::string s(n, 0);
                    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                        s.data(), n, NULL, NULL);
                    return s;
                };
                const std::wstring kAddr = L"某省某市某区幸福路88号";
                std::string content = "\xEF\xBB\xBF" +
                    wToUtf8(L"addr\t" + kAddr + L"\r\n") +
                    wToUtf8(L"sj\t收件人：灵犀（13800000000）\r\n");
                {
                    HANDLE hw = CreateFileW(ppath.c_str(), GENERIC_WRITE, 0, NULL,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    DWORD wr = 0;
                    WriteFile(hw, content.data(), (DWORD)content.size(), &wr, NULL);
                    CloseHandle(hw);
                }
                py->ReloadUserDict();

                // 整串命中：addr 首选=长句
                {
                    lingxi::Prediction r = py->Predict(L"addr");
                    bool ok = !r.cands.empty() && r.cands[0] == kAddr;
                    wchar_t detail[192];
                    _snwprintf_s(detail, _TRUNCATE, L"first='%ls'",
                                 r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                    Report(ok, "R2 phrase exact trigger", detail);
                }
                // 大小写归一：ADDR 同样命中
                {
                    lingxi::Prediction r = py->Predict(L"ADDR");
                    bool ok = !r.cands.empty() && r.cands[0] == kAddr;
                    Report(ok, "R2 phrase case-insensitive",
                           r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                }
                // 与 R3 共存：sj=时间触发 + 短语注入（短语在时间之上）
                {
                    lingxi::Prediction r = py->Predict(L"sj");
                    bool ok = !r.cands.empty() && r.cands[0] == L"收件人：灵犀（13800000000）" &&
                              r.cands.size() > 1 && r.cands[1].find(L":") != std::wstring::npos;
                    wchar_t detail[192];
                    _snwprintf_s(detail, _TRUNCATE, L"first='%ls' second='%ls'",
                                 r.cands.empty() ? L"<none>" : r.cands[0].c_str(),
                                 r.cands.size() > 1 ? r.cands[1].c_str() : L"<none>");
                    Report(ok, "R2 phrase above R3 datetime", detail);
                }
                // 非整串不命中：addrx 首选不是短语
                {
                    lingxi::Prediction r = py->Predict(L"addrx");
                    bool ok = r.cands.empty() || r.cands[0] != kAddr;
                    Report(ok, "R2 phrase no hijack on longer input",
                           r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                }
                // 热载移除：删文件+重载后不再注入
                DeleteFileW(ppath.c_str());
                py->ReloadUserDict();
                {
                    lingxi::Prediction r = py->Predict(L"addr");
                    bool gone = true;
                    for (const auto& w : r.cands)
                        if (w == kAddr) { gone = false; break; }
                    Report(gone, "R2 phrase hot-unload",
                           r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                }
                // 恢复原文件（无则保持删除）
                if (existed)
                {
                    HANDLE hw = CreateFileW(ppath.c_str(), GENERIC_WRITE, 0, NULL,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    DWORD wr = 0;
                    WriteFile(hw, backup.data(), (DWORD)backup.size(), &wr, NULL);
                    CloseHandle(hw);
                }
            }

            // P13. R5 emoji 融合：命中词后紧跟 emoji（不夺位）
            {
                // 谢谢 → 🙏 紧随其后，谢谢仍第一
                lingxi::Prediction r = py->Predict(L"xiexie");
                {
                    bool ok = r.cands.size() >= 2 && r.cands[0] == L"谢谢" &&
                              r.cands[1] == L"🙏";
                    wchar_t detail[192];
                    _snwprintf_s(detail, _TRUNCATE, L"first='%ls' second='%ls'",
                                 r.cands.empty() ? L"<none>" : r.cands[0].c_str(),
                                 r.cands.size() > 1 ? r.cands[1].c_str() : L"<none>");
                    Report(ok, "R5 emoji follows word", detail);
                }
                // 无映射词不注入 emoji（nihao 页面干净）
                {
                    lingxi::Prediction n = py->Predict(L"nihao");
                    bool clean = true;
                    for (const auto& w : n.cands)
                        if (w == L"🙏" || w == L"😄") { clean = false; break; }
                    Report(clean, "R5 no emoji on unmapped input",
                           n.cands.empty() ? L"<none>" : n.cands[0].c_str());
                }
                // 哈哈 → 😄
                {
                    lingxi::Prediction h = py->Predict(L"haha");
                    bool found = false;
                    for (size_t k = 0; k + 1 < h.cands.size(); k++)
                        if (h.cands[k] == L"哈哈" && h.cands[k + 1] == L"😄")
                        { found = true; break; }
                    Report(found, "R5 emoji haha",
                           h.cands.empty() ? L"<none>" : h.cands[0].c_str());
                }
                // 批次 I 新词融合：问候/节日/生活（含 VS16 对显式转义）
                {
                    struct FusionCase { const wchar_t* input; const wchar_t* word; const wchar_t* emoji; };
                    const FusionCase cases[] = {
                        { L"wanan",       L"晚安",     L"🌙" },
                        { L"chunjie",     L"春节",     L"🏮" },
                        { L"hongbao",     L"红包",     L"🧧" },
                        { L"woaini",      L"我爱你",   L"\u2764\uFE0F" },
                        { L"zaoshanghao", L"早上好",   L"🌞" },
                    };
                    for (const auto& c : cases)
                    {
                        lingxi::Prediction p = py->Predict(c.input);
                        bool found = false;
                        for (size_t k = 0; k + 1 < p.cands.size(); k++)
                            if (p.cands[k] == c.word && p.cands[k + 1] == c.emoji)
                            { found = true; break; }
                        wchar_t detail[128];
                        _snwprintf_s(detail, _TRUNCATE, L"%ls -> %ls%ls",
                                     c.input, c.word, c.emoji);
                        Report(found, "R5 batch I fusion", detail);
                    }
                }
            }

            // P14. R6 屏蔽词：blocked.txt 热载过滤（Generate+自学习）+ 卸载恢复
            {
                wchar_t appdataB[MAX_PATH] = {0};
                GetEnvironmentVariableW(L"APPDATA", appdataB, MAX_PATH);
                std::wstring dirB = std::wstring(appdataB) + L"\\LingxiIme";
                CreateDirectoryW(dirB.c_str(), NULL);
                std::wstring bpath = dirB + L"\\blocked.txt";
                std::vector<uint8_t> backupB;
                bool existedB = false;
                HANDLE hb = CreateFileW(bpath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hb != INVALID_HANDLE_VALUE)
                {
                    existedB = true;
                    DWORD sz = GetFileSize(hb, NULL);
                    backupB.resize(sz);
                    DWORD rd = 0;
                    ReadFile(hb, backupB.data(), sz, &rd, NULL);
                    CloseHandle(hb);
                }

                // 写屏蔽词"谢谢"（UTF-8 BOM + E8B0A2E8B0A2）——选 spoken 高频词
                // 验证 Generate 输出被过滤；emoji 融合随触发词消失（同气连枝）
                const char kBlocked[] =
                    "\xEF\xBB\xBF\xE8\xB0\xA2\xE8\xB0\xA2\r\n";
                {
                    HANDLE hw = CreateFileW(bpath.c_str(), GENERIC_WRITE, 0, NULL,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    DWORD wr = 0;
                    WriteFile(hw, kBlocked, (DWORD)strlen(kBlocked), &wr, NULL);
                    CloseHandle(hw);
                }
                py->ReloadUserDict();
                {
                    lingxi::Prediction r = py->Predict(L"xiexie");
                    bool gone = true;
                    for (const auto& w : r.cands)
                        if (w == L"谢谢" || w == L"🙏") { gone = false; break; }
                    Report(gone, "R6 blocked word filtered",
                           r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                }
                // 删文件热卸载：谢谢回归首选、emoji 融合恢复
                DeleteFileW(bpath.c_str());
                py->ReloadUserDict();
                {
                    lingxi::Prediction r = py->Predict(L"xiexie");
                    bool back = r.cands.size() >= 2 && r.cands[0] == L"谢谢" &&
                                r.cands[1] == L"🙏";
                    Report(back, "R6 blocked hot-unload restores",
                           r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                }
                // 恢复原文件（无则保持删除）
                if (existedB)
                {
                    HANDLE hw = CreateFileW(bpath.c_str(), GENERIC_WRITE, 0, NULL,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    DWORD wr = 0;
                    WriteFile(hw, backupB.data(), (DWORD)backupB.size(), &wr, NULL);
                    CloseHandle(hw);
                }
            }

            // P15. 拟声音节罚：无元音音节（m/n/hm→呣/嗯/噷）作 FULL 单元加罚 5，
            // 不再压制 [z][m] 简拼链（zm/zn 垃圾拼装"在呣/在嗯"降 tier）；
            // 单独输入 m/n 时拟声字仍在候选中（tier 1），不因罚分消失
            {
                auto indexOf = [](const lingxi::Prediction& r, const wchar_t* w) -> int {
                    for (size_t k = 0; k < r.cands.size(); k++)
                        if (r.cands[k] == w) return (int)k;
                    return -1;
                };
                // zm：怎么进前三、字母在列，拼装垃圾"在呣"要么被挤出候选
                // （60 上限截断），要么必须排在怎么/字母之后。首选为更高频的
                // 合法 zm 简拼词（如"在吗"），不算垃圾
                {
                    lingxi::Prediction r = py->Predict(L"zm");
                    int iZm = indexOf(r, L"怎么");
                    int iZimu = indexOf(r, L"字母");
                    int iZai = indexOf(r, L"在呣");
                    bool orderOk = iZm >= 0 && iZm < 3 && iZimu >= 0 &&
                                   (iZai < 0 || (iZm < iZai && iZimu < iZai));
                    wchar_t detail[256];
                    _snwprintf_s(detail, _TRUNCATE, L"zm=%d zimu=%d zaimu=%d first='%ls'",
                                 iZm, iZimu, iZai,
                                 r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                    Report(orderOk, "P15 zm simple-initial beats interjection junk",
                           detail);
                }
                // zn：垃圾"在嗯"不得进前三
                {
                    lingxi::Prediction r = py->Predict(L"zn");
                    int iZai = indexOf(r, L"在嗯");
                    bool ok = iZai < 0 || iZai >= 3;
                    wchar_t detail[192];
                    _snwprintf_s(detail, _TRUNCATE, L"zaien=%d first='%ls'",
                                 iZai, r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                    Report(ok, "P15 zn no interjection junk in top3", detail);
                }
                // 单独 m/n：呣/嗯 仍在候选中（tier 1，罚分不除名）
                {
                    lingxi::Prediction rm = py->Predict(L"m");
                    lingxi::Prediction rn = py->Predict(L"n");
                    int iMu = indexOf(rm, L"呣");
                    int iEn = indexOf(rn, L"嗯");
                    wchar_t detail[192];
                    _snwprintf_s(detail, _TRUNCATE, L"mu=%d en=%d",
                                 iMu, iEn);
                    Report(iMu >= 0 && iEn >= 0, "P15 standalone m/n keep interjection chars",
                           detail);
                }
                // mm：妈妈（简拼整词）居首，拟声拼装"呣呣"不夺位
                {
                    lingxi::Prediction r = py->Predict(L"mm");
                    int iMama = indexOf(r, L"妈妈");
                    int iMMu = indexOf(r, L"呣呣");
                    bool ok = iMama >= 0 && iMama < 5 && (iMMu < 0 || iMMu > iMama);
                    wchar_t detail[192];
                    _snwprintf_s(detail, _TRUNCATE, L"mama=%d mumu=%d first='%ls'",
                                 iMama, iMMu,
                                 r.cands.empty() ? L"<none>" : r.cands[0].c_str());
                    Report(ok, "P15 mm mama beats interjection junk", detail);
                }
            }

            delete py;
        }
    }

    // ===== B4：续写回显裁剪 + 上下文感知降级链 =====
    {
        // E1. StripContinuationEcho：回显形态矩阵
        {
            struct ECase { const wchar_t* cont; const wchar_t* tail; const wchar_t* expect; };
            const ECase cases[] = {
                // 基础部分重叠：以上文尾部开头 → 裁掉重叠（旧版已覆盖的形态）
                {L"天气不错，气温晴", L"今天天气", L"不错，气温晴"},
                // 标点差形态：上文尾部带标点而输出不带（旧版漏掉的回显形态）
                {L"天气不错", L"今天天气不错，", L""},
                // 整段复读 ×2：循环剥干净（旧版只剥一轮，残余仍是复读）
                {L"今天天气不错今天天气不错", L"今天天气不错", L""},
                // 整段复读 + 真续写：只剥复读部分
                {L"今天天气不错今天更冷", L"今天天气不错", L"今天更冷"},
                // 无回显：保持原样
                {L"晴，气温不错", L"今天天气", L"晴，气温不错"},
                // 叠词保护：部分重叠只剥一轮，保留"很好"
                {L"很好很好", L"干得很好", L"很好"},
                // k≥2 阈值留缝：单字重叠不裁（回归即文档）
                {L"好，再见", L"天气好", L"好，再见"},
                // 前导标点回显："，天气"式也算复读
                {L"，天气不错", L"今天天气", L"不错"},
            };
            for (const auto& cs : cases)
            {
                std::wstring got = lingxi::StripContinuationEcho(cs.cont, cs.tail);
                bool ok = got == cs.expect;
                wchar_t detail[192];
                _snwprintf_s(detail, _TRUNCATE, L"'%ls' tail='%ls' -> '%ls' (expect '%ls')",
                             cs.cont, cs.tail, got.c_str(), cs.expect);
                Report(ok, "echo strip", detail);
            }
        }

        // E2. 链路由：纯 ASCII 上下文 → 词表优先（不出 LLM 中文候选）
        {
            std::vector<lingxi::IPredictor*> chain;
            chain.push_back(new FakePredictor({L"中文词"}));
            chain.push_back(new FakePredictor({L"hello there"}));
            lingxi::IPredictor* c = lingxi::CreateChainPredictor(std::move(chain));
            lingxi::Prediction r = c->Predict(L"Today is a nice ");
            bool ok = !r.cands.empty() && r.cands[0] == L"hello there";
            wchar_t detail[160];
            _snwprintf_s(detail, _TRUNCATE, L"first='%ls'",
                         r.cands.empty() ? L"" : r.cands[0].c_str());
            Report(ok, "chain: pure-ascii ctx prefers wordlist", detail);
            delete c;
        }

        // E3. 中文上下文：LLM 优先（链序保持，不被 ASCII 过滤误伤）
        {
            std::vector<lingxi::IPredictor*> chain;
            chain.push_back(new FakePredictor({L"中文词"}));
            chain.push_back(new FakePredictor({L"hello there"}));
            lingxi::IPredictor* c = lingxi::CreateChainPredictor(std::move(chain));
            lingxi::Prediction r = c->Predict(L"你好，今天");
            bool ok = !r.cands.empty() && r.cands[0] == L"中文词";
            wchar_t detail[160];
            _snwprintf_s(detail, _TRUNCATE, L"first='%ls'",
                         r.cands.empty() ? L"" : r.cands[0].c_str());
            Report(ok, "chain: cjk ctx keeps LLM first", detail);
            delete c;
        }

        // E4. 纯 ASCII + 词表空：LLM 兜底候选过滤为纯 ASCII
        {
            std::vector<lingxi::IPredictor*> chain;
            chain.push_back(new FakePredictor({L"中文词", L"sunny day", L"很好"}));
            chain.push_back(new FakePredictor({}));
            lingxi::IPredictor* c = lingxi::CreateChainPredictor(std::move(chain));
            lingxi::Prediction r = c->Predict(L"Today is a nice ");
            bool ok = r.cands.size() == 1 && r.cands[0] == L"sunny day";
            wchar_t detail[160];
            _snwprintf_s(detail, _TRUNCATE, L"cands=%zu first='%ls'",
                         r.cands.size(), r.cands.empty() ? L"" : r.cands[0].c_str());
            Report(ok, "chain: ascii filter drops cjk cands", detail);
            delete c;
        }

        // E5. 纯 ASCII + 词表空 + LLM 全中文：无候选（不出中文垃圾）
        {
            std::vector<lingxi::IPredictor*> chain;
            chain.push_back(new FakePredictor({L"中文词", L"很好"}));
            chain.push_back(new FakePredictor({}));
            lingxi::IPredictor* c = lingxi::CreateChainPredictor(std::move(chain));
            lingxi::Prediction r = c->Predict(L"Today is a nice ");
            wchar_t detail[96];
            _snwprintf_s(detail, _TRUNCATE, L"cands=%zu", r.cands.size());
            Report(r.cands.empty(), "chain: all-cjk LLM -> no candidates", detail);
            delete c;
        }

        // E6. 词中 + 真词表：inte 走词表补全（replaceLen 语义）
        {
            std::vector<lingxi::IPredictor*> chain;
            chain.push_back(new FakePredictor({L"中文词"}));
            chain.push_back(lingxi::CreateDefaultPredictor());
            lingxi::IPredictor* c = lingxi::CreateChainPredictor(std::move(chain));
            lingxi::Prediction r = c->Predict(L"inte");
            bool ok = !r.cands.empty() && _wcsnicmp(r.cands[0].c_str(), L"inte", 4) == 0 &&
                      r.replaceLen == 4 && AllAscii(r.cands);
            wchar_t detail[160];
            _snwprintf_s(detail, _TRUNCATE, L"first='%ls' replaceLen=%u",
                         r.cands.empty() ? L"" : r.cands[0].c_str(), r.replaceLen);
            Report(ok, "chain: midword uses wordlist completion", detail);
            delete c;
        }

        // E7. 真词表 + 纯 ASCII 词边界：候选为空或纯 ASCII（不出中文）
        {
            std::vector<lingxi::IPredictor*> chain;
            chain.push_back(new FakePredictor({L"中文词"}));
            chain.push_back(lingxi::CreateDefaultPredictor());
            lingxi::IPredictor* c = lingxi::CreateChainPredictor(std::move(chain));
            lingxi::Prediction r = c->Predict(L"Today is a nice ");
            bool ok = AllAscii(r.cands); // 空集也纯 ASCII
            wchar_t detail[160];
            _snwprintf_s(detail, _TRUNCATE, L"cands=%zu first='%ls'",
                         r.cands.size(), r.cands.empty() ? L"" : r.cands[0].c_str());
            Report(ok, "chain: real wordlist pure-ascii boundary", detail);
            delete c;
        }

        // E8. DedupCompletions：补全候选三重去重矩阵（2026-10-29「AI 补全大量重复」）
        {
            auto dedupCase = [](const std::vector<std::wstring>& in, const std::wstring& ctx,
                                const std::vector<std::wstring>& expect, const char* name)
            {
                std::vector<std::wstring> got = lingxi::DedupCompletions(in, ctx);
                bool ok = got == expect;
                std::wstring inj, gotj;
                for (size_t k = 0; k < in.size(); k++)
                {
                    if (k) inj += L"|";
                    inj += in[k];
                }
                for (size_t k = 0; k < got.size(); k++)
                {
                    if (k) gotj += L"|";
                    gotj += got[k];
                }
                wchar_t detail[256];
                _snwprintf_s(detail, _TRUNCATE, L"in[%ls] ctx='%ls' -> [%ls]",
                             inj.c_str(), ctx.c_str(), gotj.c_str());
                Report(ok, name, detail);
            };
            // ① 归一相等：同源首句/全句只差尾标点 → 保留首个
            dedupCase({ L"散心", L"散心。" }, L"", { L"散心" }, "dedup: norm-equal keeps first");
            dedupCase({ L"a text", L"a text." }, L"", { L"a text" }, "dedup: en norm-equal");
            // ② 前缀堆叠：全句是首句的延伸 → 保留较短（0.5B 长尾易循环）
            dedupCase({ L"晴，气温20℃-30℃", L"晴，气温20℃-30℃，湿度70%-80%。" }, L"",
                      { L"晴，气温20℃-30℃" }, "dedup: prefix stack keeps short");
            // ② 长在前短在后 → 换短
            dedupCase({ L"可以去公园散步，享受大自然。", L"可以去公园散步" }, L"",
                      { L"可以去公园散步" }, "dedup: reverse order swaps to short");
            // ② 单字前缀不合并（"天"不足以证明同源，③ 兜底）
            dedupCase({ L"天", L"天，你好" }, L"", { L"天", L"天，你好" },
                      "dedup: 1-char prefix not merged");
            // ③ 上文复读：整段复读已输入内容 → 剔除，保留真续写
            dedupCase({ L"天，今天天气真不错", L"可以去公园散步" },
                      L"今天天气真不错，我们一起去公园",
                      { L"可以去公园散步" }, "dedup: context loop dropped");
            // ③ 不误杀：真续写与上文仅零星重叠
            dedupCase({ L"好，天气真好，出门可以穿短袖短裤" }, L"今天天气真",
                      { L"好，天气真好，出门可以穿短袖短裤" }, "dedup: real continuation kept");
            // ③ 英文复读
            dedupCase({ L"nice day", L"for walking" }, L"have a nice day",
                      { L"for walking" }, "dedup: en context loop dropped");
            // 空/纯标点候选剔除
            dedupCase({ L"，", L"好" }, L"", { L"好" }, "dedup: punct-only dropped");
        }

        // E9. StageAssocCands：联想帧阶梯呈现（GAP §11 多段呈现，2026-11-02）
        {
            auto stageCase = [](const std::vector<std::wstring>& in,
                                const std::vector<std::wstring>& expect, const char* name)
            {
                lingxi::Prediction pred;
                pred.cands = in;
                lingxi::StageAssocCands(pred);
                bool ok = pred.cands == expect;
                std::wstring inj, gotj;
                for (size_t k = 0; k < in.size(); k++)
                {
                    if (k) inj += L"|";
                    inj += in[k];
                }
                for (size_t k = 0; k < pred.cands.size(); k++)
                {
                    if (k) gotj += L"|";
                    gotj += pred.cands[k];
                }
                wchar_t detail[256];
                _snwprintf_s(detail, _TRUNCATE, L"in[%ls] -> [%ls]", inj.c_str(), gotj.c_str());
                Report(ok, name, detail);
            };
            // 标准两级：首段含句读（词级颗粒）+ 全句原样
            stageCase({ L"晴，气温20℃-30℃" }, { L"晴，", L"晴，气温20℃-30℃" },
                      "stage: two-level seg+full");
            // 7 字首段（含句读 ≤8 门槛内）
            stageCase({ L"今天天气很好，我们去公园散步吧" },
                      { L"今天天气很好，", L"今天天气很好，我们去公园散步吧" },
                      "stage: 7-char seg staged");
            // 无句读 / 句读在末尾 / 剩余 1 字 / 句读在首位 / 首段超长（>8）→ 不拆
            stageCase({ L"你好" }, { L"你好" }, "stage: no stop unchanged");
            stageCase({ L"好。" }, { L"好。" }, "stage: trailing stop unchanged");
            stageCase({ L"晴，热" }, { L"晴，热" }, "stage: 1-char rest unchanged");
            stageCase({ L"，然后我们就出发" }, { L"，然后我们就出发" },
                      "stage: leading stop unchanged");
            stageCase({ L"今天天气非常好我们出去玩，一起去公园" },
                      { L"今天天气非常好我们出去玩，一起去公园" },
                      "stage: seg>8 unchanged");
            // 首段已在列（多路采样同源）→ 不重复插；c0 无句读 → 整批不动
            stageCase({ L"晴，气温20℃-30℃", L"晴，" }, { L"晴，气温20℃-30℃", L"晴，" },
                      "stage: existing seg no dup");
            stageCase({ L"晴，", L"晴，气温20℃-30℃" }, { L"晴，", L"晴，气温20℃-30℃" },
                      "stage: c0-no-split passthrough");
            // 多候选拆 c0，其余原样
            stageCase({ L"晴，气温20℃-30℃", L"晴，湿度70%" },
                      { L"晴，", L"晴，气温20℃-30℃", L"晴，湿度70%" },
                      "stage: only c0 staged");
            // 英文句读同集
            stageCase({ L"day. I hope you have a good one." },
                      { L"day.", L"day. I hope you have a good one." },
                      "stage: en stop same set");
            // cover/anno 原本平行 → 同步插头保持平行；原本空 → 保持空
            {
                lingxi::Prediction pred;
                pred.cands = { L"晴，气温20℃-30℃" };
                pred.cover = { 0 };
                pred.anno = { L"" };
                lingxi::StageAssocCands(pred);
                bool okArr = pred.cands.size() == 2 && pred.cover.size() == 2 &&
                             pred.anno.size() == 2 && pred.cover[0] == 0 && pred.anno[0].empty();
                lingxi::Prediction pred2;
                pred2.cands = { L"晴，气温20℃-30℃" }; // cover/anno 空 = 引擎未填 → 保持空
                lingxi::StageAssocCands(pred2);
                okArr = okArr && pred2.cands.size() == 2 && pred2.cover.empty() && pred2.anno.empty();
                Report(okArr, "stage: parallel arrays synced", L"cover/anno 平行不变量");
            }

            // ===== P10. L10 用户词典封顶淘汰边界（audit §七 L10 / 剧本 7c）=====
            // 规则（pinyin.cpp EvictUserDictKeysLocked）：kMaxUserDictKeys=20000、
            // batch=5%（1000 键）；仅"新键插入前"触发；候选=不含任何 manual 条目
            // 的键，按键内 count 总和最小优先淘汰；manual 键整体豁免；全 manual
            // 越限不淘汰。断言面：正好 20000 不触发 / 20001 触发 -1000+1 /
            // manual 豁免 / count 最小优先 / 并发落盘×淘汰收敛 / 逐行严格校验。
            // 本段在 B4 作用域，独立创建拼音引擎实例（M5 的 py 已随作用域结束）。
            {
                lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
                if (!py) Report(false, "L10 CreatePinyinPredictor");
                else
                {
                std::wstring udPath = ExeDir() + L"\\userdata\\LingxiIme\\userdict.txt";
                // 整读 userdict（落盘为原子写：任意时刻读到完整快照）→ 有效行数组
                auto readUdLines = [&]() -> std::vector<std::wstring> {
                    std::vector<std::wstring> out;
                    HANDLE h = CreateFileW(udPath.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (h == INVALID_HANDLE_VALUE) return out;
                    DWORD sz = GetFileSize(h, NULL), got = 0;
                    std::vector<char> buf;
                    if (sz != INVALID_FILE_SIZE && sz > 0)
                    {
                        buf.resize(sz);
                        if (!ReadFile(h, buf.data(), sz, &got, NULL)) got = 0;
                    }
                    CloseHandle(h);
                    if (!got) return out;
                    int w = MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)got, NULL, 0);
                    std::wstring text;
                    text.resize(w);
                    MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)got, &text[0], w);
                    size_t pos = 0;
                    while (pos < text.size())
                    {
                        size_t nl = text.find(L'\n', pos);
                        size_t end = (nl == std::wstring::npos) ? text.size() : nl;
                        if (end > pos)
                        {
                            std::wstring line = text.substr(pos, end - pos);
                            if (!line.empty() && line.back() == L'\r') line.pop_back();
                            if (!line.empty()) out.push_back(std::move(line));
                        }
                        if (nl == std::wstring::npos) break;
                        pos = nl + 1;
                    }
                    return out;
                };
                // 行格式严格校验：word\tkey\tcount[\t1 | \t0\tlast_used]；
                // 第 4 列 manual 写"1"（行尾），学习词写"0"+第 5 列 unix 秒（B 衰减）
                auto lineOk = [](const std::wstring& s) -> bool {
                    size_t t1 = s.find(L'\t');
                    if (t1 == std::wstring::npos || t1 == 0) return false;
                    size_t t2 = s.find(L'\t', t1 + 1);
                    if (t2 == std::wstring::npos || t2 == t1 + 1) return false;
                    size_t t3 = s.find(L'\t', t2 + 1);
                    size_t cEnd = (t3 == std::wstring::npos) ? s.size() : t3;
                    if (cEnd == t2 + 1) return false;
                    for (size_t i = t2 + 1; i < cEnd; i++)
                        if (s[i] < L'0' || s[i] > L'9') return false;
                    if (t3 == std::wstring::npos) return true;
                    if (s.size() <= t3 + 1) return false;
                    if (s[t3 + 1] == L'1') return s.size() == t3 + 2; // manual：行尾
                    if (s[t3 + 1] != L'0') return false;               // 第 4 列只许 0/1
                    size_t t4 = s.find(L'\t', t3 + 1);                  // "0"须跟第 5 列数字
                    if (t4 == std::wstring::npos || t4 + 1 >= s.size()) return false;
                    for (size_t i = t4 + 1; i < s.size(); i++)
                        if (s[i] < L'0' || s[i] > L'9') return false;
                    return true;
                };
                auto keyOf = [](const std::wstring& s) -> std::wstring {
                    size_t t1 = s.find(L'\t');
                    size_t t2 = s.find(L'\t', t1 + 1);
                    return (t1 == std::wstring::npos || t2 == std::wstring::npos)
                               ? std::wstring() : s.substr(t1 + 1, t2 - t1 - 1);
                };
                // 学习键空间："aa"+4 位 base26（456976 槽；Learn 不校验音节合法性）
                auto mkKey = [](int i) -> std::wstring {
                    wchar_t k[7] = L"aaaaaa";
                    k[2] = (wchar_t)(L'a' + (i / 17576) % 26);
                    k[3] = (wchar_t)(L'a' + (i / 676) % 26);
                    k[4] = (wchar_t)(L'a' + (i / 26) % 26);
                    k[5] = (wchar_t)(L'a' + i % 26);
                    return std::wstring(k, 6);
                };
                const int kCap = 20000, kBatch = 1000;
                py->ClearUserDict();

                // A1. 1 manual 键 + 19999 学习键 = 正好 20000：末次插入前 size=19999
                // <cap，全程不触发淘汰；循环中途抽查异步落盘快照的完整性（并发写盘）
                bool manOk = py->AddUserWord(L"手工词", L"zzzz", 1);
                bool midSnapOk = true;
                for (int i = 0; i < kCap - 1; i++)
                {
                    py->Learn(mkKey(i), L"学习词");
                    if (i == 10000)
                        for (const auto& l : readUdLines())
                            if (!lineOk(l)) { midSnapOk = false; break; }
                }
                py->FlushUserDict(); // 收敛在途 KickPersist
                std::vector<std::wstring> lines = readUdLines();
                bool fmtAll = midSnapOk && manOk;
                for (const auto& l : lines) if (!lineOk(l)) { fmtAll = false; break; }
                {
                    wchar_t det[128];
                    _snwprintf_s(det, _TRUNCATE, L"lines=%zu expect=%d midSnap=%d",
                                 lines.size(), kCap, midSnapOk ? 1 : 0);
                    Report(fmtAll && (int)lines.size() == kCap,
                           "L10 exactly-20000: no eviction, lines valid", det);
                }

                // A2. 前 5 键 count 抬到 101（Nudge 不触发淘汰）→ 再 Learn 第 20000
                // 个学习键：插入前 size=20000≥cap → 淘汰 1000（count 最小优先，
                // 高 count 键与 manual 键必存活）→ 插入 → 19001 键
                for (int i = 0; i < 5; i++) py->NudgeUserWord(L"学习词", mkKey(i), 100);
                py->Learn(mkKey(kCap - 1), L"学习词");
                py->FlushUserDict();
                lines = readUdLines();
                {
                    std::vector<std::wstring> keys;
                    keys.reserve(lines.size());
                    for (const auto& l : lines) keys.push_back(keyOf(l));
                    auto hasKey = [&](const std::wstring& k) {
                        return std::find(keys.begin(), keys.end(), k) != keys.end();
                    };
                    bool keepOk = hasKey(L"zzzz") && hasKey(mkKey(kCap - 1));
                    for (int i = 0; i < 5; i++) if (!hasKey(mkKey(i))) keepOk = false;
                    int learnedCnt = 0;
                    for (const auto& k : keys) if (k != L"zzzz") learnedCnt++;
                    wchar_t det[128];
                    _snwprintf_s(det, _TRUNCATE, L"lines=%zu expect=%d learned=%d keep=%d",
                                 lines.size(), kCap - kBatch + 1, learnedCnt,
                                 keepOk ? 1 : 0);
                    Report(fmtAll && (int)lines.size() == kCap - kBatch + 1 &&
                           learnedCnt == kCap - kBatch && keepOk,
                           "L10 20001st key: evict-1000, manual+high-count survive", det);
                }

                // B. 全 manual 越限：MergeUserDict 新键全 manual=true → 淘汰候选
                // 恒空 → 20001 键整体保留（键数可越上限），且每行带第 4 列"1"
                py->ClearUserDict();
                std::vector<lingxi::UserDictEntry> ents;
                ents.reserve(kCap + 1);
                for (int i = 0; i < kCap + 1; i++)
                {
                    lingxi::UserDictEntry e;
                    e.word = L"合并词";
                    e.key = mkKey(i);
                    e.count = 1;
                    ents.push_back(std::move(e));
                }
                bool mOk = py->MergeUserDict(ents);
                py->FlushUserDict();
                lines = readUdLines();
                bool allManual = mOk;
                for (const auto& l : lines)
                {
                    if (!lineOk(l) || l.size() < 2 ||
                        l[l.size() - 1] != L'1' || l[l.size() - 2] != L'\t')
                    { allManual = false; break; }
                }
                {
                    wchar_t det[128];
                    _snwprintf_s(det, _TRUNCATE, L"lines=%zu expect=%d",
                                 lines.size(), kCap + 1);
                    Report(allManual && (int)lines.size() == kCap + 1,
                           "L10 all-manual 20001: over-cap allowed, no eviction", det);
                }
                py->ClearUserDict(); // 复位沙盒词典，别把 2 万测试键留给后续用例
                delete py;
                }
            }
        }
    }

    // ===== B5. 词频衰减（裁决 2026-11-03①：查询时 score=count×2^(−age/H)，
    // manual 完全豁免，原始 count 永不改写；淘汰 Σscore 与排序同源）=====
    {
        std::wstring udPath5 = ExeDir() + L"\\userdata\\LingxiIme\\userdict.txt";
        auto writeUd5 = [&](const std::string& utf8) -> bool {
            HANDLE h = CreateFileW(udPath5.c_str(), GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) return false;
            DWORD w = 0;
            BOOL ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &w, NULL);
            CloseHandle(h);
            return ok && w == (DWORD)utf8.size();
        };
        auto readUd5 = [&]() -> std::string {
            HANDLE h = CreateFileW(udPath5.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) return std::string();
            DWORD sz = GetFileSize(h, NULL), got = 0;
            std::string s;
            if (sz != INVALID_FILE_SIZE && sz > 0)
            {
                s.resize(sz);
                if (!ReadFile(h, &s[0], sz, &got, NULL)) s.clear();
                else s.resize(got);
            }
            CloseHandle(h);
            return s;
        };
        const long long DAY = 86400LL;
        const long long now5 = (long long)std::time(nullptr);

        // B5-1 半衰期边界：旧热(10, age=H)→score 5 < 新温(6, age=0)→6 置顶；
        // decayEnabled=0 回到原始 count → 旧热置顶（可逆性）
        {
            std::string f = "oldhot\tgj\t10\t0\t" + std::to_string(now5 - 60 * DAY) +
                            "\nnewwarm\tgj\t6\t0\t" + std::to_string(now5) + "\n";
            bool ok = writeUd5(f);
            lingxi::IPredictor* py = ok ? lingxi::CreatePinyinPredictor() : nullptr;
            bool decayTop = false, offTop = false;
            if (py)
            {
                lingxi::Prediction r = py->Predict(L"gj");
                decayTop = !r.cands.empty() && r.cands[0] == L"newwarm";
                lingxi::Settings ss = lingxi::LoadSettings();
                ss.decayEnabled = 0;
                lingxi::SaveSettings(ss);
                r = py->Predict(L"gj");
                offTop = !r.cands.empty() && r.cands[0] == L"oldhot";
                ss = lingxi::LoadSettings();
                ss.decayEnabled = 1;
                lingxi::SaveSettings(ss);
                delete py;
            }
            Report(ok && decayTop && offTop, "B5-1 decay halflife: score-top, count-top when off");
        }

        // B5-2 时钟回拨：last_used 在未来 → age 钳 0（score=count），不得放大霸屏
        {
            std::string f = "futu10\tgj2\t10\t0\t" + std::to_string(now5 + 30 * DAY) +
                            "\nnorm100\tgj2\t100\t0\t" + std::to_string(now5) + "\n";
            bool ok = writeUd5(f);
            lingxi::IPredictor* py = ok ? lingxi::CreatePinyinPredictor() : nullptr;
            bool top = false;
            if (py)
            {
                lingxi::Prediction r = py->Predict(L"gj2");
                top = !r.cands.empty() && r.cands[0] == L"norm100";
                delete py;
            }
            Report(ok && top, "B5-2 clock rollback: future ts clamped, no inflation");
        }

        // B5-3 manual 豁免：手工(5, manual 不衰减) > 学习(6, age=2H→1.5) → 手工置顶
        {
            std::string f = "manuald\tmj\t5\t1\nlearned6\tmj\t6\t0\t" +
                            std::to_string(now5 - 120 * DAY) + "\n";
            bool ok = writeUd5(f);
            lingxi::IPredictor* py = ok ? lingxi::CreatePinyinPredictor() : nullptr;
            bool top = false;
            if (py)
            {
                lingxi::Prediction r = py->Predict(L"mj");
                top = !r.cands.empty() && r.cands[0] == L"manuald";
                delete py;
            }
            Report(ok && top, "B5-3 manual exempt: un-decayed 5 beats decayed 6");
        }

        // B5-4 淘汰 Σscore：20000 键（X=count2 极旧 score≈0.002，其余 count1 新鲜
        // score=1）→ Learn 新键触发封顶淘汰 1000 → X 必逐、新键在、总数 19001
        {
            std::string f;
            f.reserve((size_t)20000 * 30);
            f += "xword\taaaaaa\t2\t0\t" + std::to_string(now5 - 600 * DAY) + "\n";
            for (int i = 1; i < 20000; i++)
            {
                char k[7] = "aaaaaa";
                k[2] = (char)('a' + (i / 17576) % 26);
                k[3] = (char)('a' + (i / 676) % 26);
                k[4] = (char)('a' + (i / 26) % 26);
                k[5] = (char)('a' + i % 26);
                std::string key(k, 6);
                f += "kw" + key + "\t" + key + "\t1\t0\t" + std::to_string(now5) + "\n";
            }
            bool ok = writeUd5(f);
            lingxi::IPredictor* py = ok ? lingxi::CreatePinyinPredictor() : nullptr;
            bool evicted = false, countOk = false;
            if (py)
            {
                py->Learn(L"zzzzzz", L"newword");
                py->FlushUserDict();
                std::string s = readUd5();
                evicted = s.find("\taaaaaa\t") == std::string::npos &&
                          s.find("zzzzzz") != std::string::npos;
                int nl = 0;
                for (char c : s) if (c == '\n') nl++;
                countOk = nl == 20000 - 1000 + 1;
                delete py;
            }
            Report(ok && evicted && countOk, "B5-4 evict by sum-score: oldest-low evicted");
        }
        {
            lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
            if (py) { py->ClearUserDict(); delete py; } // 复位沙盒，不留 2 万测试键
        }
    }

    // ===== C2. 管道帧层：往返/半帧+对端消失/写向已消失对端/超时取消(H1路径) =====
    // 断言面：坏端点必须"有界返回 false"，不许挂死；H1 取消路径等取消落定后
    // 栈上 OVERLAPPED 才析构——本进程后续用例继续跑即栈未坏的行为证据。
    {
        wchar_t pipeNm[128];
        _snwprintf_s(pipeNm, _TRUNCATE, L"\\\\.\\pipe\\LingxiET-%u", GetCurrentProcessId());
        auto mkPair = [&](DWORD outBuf, bool srvOverlapped, HANDLE* pSrv, HANDLE* pCli) -> bool {
            DWORD open = PIPE_ACCESS_DUPLEX | (srvOverlapped ? FILE_FLAG_OVERLAPPED : 0);
            *pSrv = CreateNamedPipeW(pipeNm, open,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, outBuf, 4096, 0, NULL);
            if (*pSrv == INVALID_HANDLE_VALUE) return false;
            HANDLE c = CreateFileW(pipeNm, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (c == INVALID_HANDLE_VALUE) { CloseHandle(*pSrv); *pSrv = INVALID_HANDLE_VALUE; return false; }
            *pCli = c;
            return true;
        };
        // 1) 完整帧往返（sanity：两端都健康时读写正确）
        bool ok1 = false;
        { HANDLE s, c;
          if (mkPair(4096, true, &s, &c)) {
              std::vector<uint8_t> body(300, 0xAB), accum, rb; uint8_t t = 0;
              bool w = lingxi::WriteFrame(s, 0x7F, body.data(), (uint32_t)body.size(), 1000);
              bool r = lingxi::ReadFrame(c, NULL, accum, t, rb);
              ok1 = w && r && t == 0x7F && rb == body;
              CloseHandle(s); CloseHandle(c);
          } }
        Report(ok1, "C2 frame roundtrip");
        // 2) 半帧后服务端关闭 → 客户端 ReadFrame 有界 false（对端突然消失）
        bool ok2 = false;
        { HANDLE s, c;
          if (mkPair(4096, false, &s, &c)) {
              DWORD g = 0; const char hdr[5] = { 0x10, 0, 0, 0, 0x7F }; // 声明16字节只发5
              WriteFile(s, hdr, 5, &g, NULL);
              CloseHandle(s);                       // 半帧在途时服务端消失
              ULONGLONG t0 = GetTickCount64();
              std::vector<uint8_t> accum, body; uint8_t t = 0;
              bool r = lingxi::ReadFrame(c, NULL, accum, t, body);
              ok2 = !r && (GetTickCount64() - t0) < 2500;
              CloseHandle(c);
          } }
        Report(ok2, "C2 half-frame then peer gone: bounded false");
        // 3) 对端消失后写帧 → 有界 false（不挂死、不崩）
        bool ok3 = false;
        { HANDLE s, c;
          if (mkPair(4096, true, &s, &c)) {
              CloseHandle(c);                       // 客户端先没
              ULONGLONG t0 = GetTickCount64();
              std::vector<uint8_t> body(64, 1);
              bool w = lingxi::WriteFrame(s, 0x7F, body.data(), (uint32_t)body.size(), 200);
              ok3 = !w && (GetTickCount64() - t0) < 2500;
              CloseHandle(s);
          } }
        Report(ok3, "C2 write to vanished peer: bounded false");
        // 4) H1 取消路径：小缓冲(64B)无人读 → 4KB 帧写超时 → CancelIoEx 后
          //    必须等取消落定才返回（旧实现在此留栈破坏窗口）。超时分支不带
          //    hStop，走的正是 :106-108 的 CancelIoEx+GetOverlappedResult(TRUE)。
        bool ok4 = false;
        { HANDLE s, c;
          if (mkPair(64, true, &s, &c)) {
              ULONGLONG t0 = GetTickCount64();
              std::vector<uint8_t> body(4096, 0x5A);
              bool w = lingxi::WriteFrame(s, 0x7F, body.data(), (uint32_t)body.size(), 200);
              ok4 = !w && (GetTickCount64() - t0) < 2500;
              CloseHandle(s); CloseHandle(c);
          } }
        Report(ok4, "C2 write timeout cancel (H1): bounded false, stack intact");
    }

    // ===== C3. M2 非法 ns 隔离：读写永不落真实键，且与测试派生判定同口径 =====
    {
        wchar_t prevNs[64] = L"";
        GetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", prevNs, 64);
        auto regDword = [](const wchar_t* key, const wchar_t* name, DWORD& out) -> bool {
            DWORD v = 0, cb = sizeof(v), type = 0;
            if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, &type, &v, &cb) == ERROR_SUCCESS &&
                type == REG_DWORD) { out = v; return true; }
            return false;
        };
        DWORD seqBefore = 0; bool hadBefore = regDword(L"Software\\LingxiIme", L"SettingsSeq", seqBefore);
        SetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", L"bad ns!");   // 含空格与 ! = 非法 ns
        lingxi::SnapshotTestSpawned();
        bool spawned = lingxi::WasTestSpawned();                      // 非法 ns 仍算测试派生（同口径）
        lingxi::Settings s; s.candidateCount = 9;
        lingxi::SaveSettings(s);                                      // 必须整体落隔离键
        DWORD q = 0; bool quarantined = regDword(L"Software\\LingxiIme_NS_INVALID", L"CandidateCount", q) && q == 9;
        DWORD seqAfter = 0; bool hadAfter = regDword(L"Software\\LingxiIme", L"SettingsSeq", seqAfter);
        bool realUntouched = (hadBefore == hadAfter) && (!hadBefore || seqAfter == seqBefore);
        // 对照：合法 ns 走正常沙盒键
        SetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", L"m2ok");
        lingxi::SaveSettings(s);
        DWORD v2 = 0; bool sandboxed = regDword(L"Software\\LingxiIme_m2ok", L"CandidateCount", v2) && v2 == 9;
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\LingxiIme_m2ok");
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\LingxiIme_NS_INVALID");
        SetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", prevNs[0] ? prevNs : L"");
        lingxi::SnapshotTestSpawned();
        Report(spawned, "C3 invalid ns still test-spawned");
        Report(quarantined, "C3 invalid ns writes quarantine key");
        Report(realUntouched, "C3 invalid ns never touches real key");
        Report(sandboxed, "C3 valid ns writes sandbox key");

        // ① 候选数语义收口钉（2026-11-03）：kPageSize 是 DLL/UI 跨进程协议常量
        // （数字键 1-9 对应页内位置，改它=改帧语义，属 §六 待裁决）；
        // candidateCount 只作用于英文补全上限（pipeserver.cpp 裁剪处），默认必须为 9
        // （deep_test T12/T14 的"每页 9"口径以此为准）。
        Report(lingxi::kPageSize == 9, "PIN kPageSize protocol constant == 9");
        {
            lingxi::Settings d;
            Report(d.candidateCount == 9, "PIN default candidateCount == 9 (English cap only)");
        }

        // ③ 双拼方案表往返用例（每方案至少一条；键位依据见 common/shuangpin.h 头注）
        Report(lingxi::ShuangpinToQuanpin("ui", 0) == "shi",   "SP xiahe ui->shi");
        Report(lingxi::ShuangpinToQuanpin("vy", 0) == "zhun",  "SP xiahe vy->zhun");
        Report(lingxi::ShuangpinToQuanpin("eg", 0) == "eng",   "SP xiahe eg->eng");
        Report(lingxi::ShuangpinToQuanpin("aa", 0) == "a",     "SP xiahe aa->a");
        Report(lingxi::ShuangpinToQuanpin("lk", 0) == "ling",  "SP xiahe lk->ling");
        Report(lingxi::ShuangpinToQuanpin("ly", 1) == "ling",  "SP ziranma ly->ling");
        Report(lingxi::ShuangpinToQuanpin("vy", 1) == "zhuai", "SP ziranma vy->zhuai");
        Report(lingxi::ShuangpinToQuanpin("l;", 2) == "ling",  "SP ms l;->ling");
        Report(lingxi::ShuangpinToQuanpin("oa", 2) == "a",     "SP ms oa->a");
        Report(lingxi::ShuangpinToQuanpin("oh", 2) == "ang",   "SP ms oh->ang");
        Report(lingxi::ShuangpinToQuanpin("ny", 2) == "nv",    "SP ms ny->nv");
    }

    // 共用：词典字节读 + 逐行严格校验（word\tkey\tcount[\t1]，count 必须纯数字）
    auto readBytes = [](const std::wstring& p, std::string& out) -> bool {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD sz = GetFileSize(h, NULL); out.clear();
        if (sz == INVALID_FILE_SIZE) { CloseHandle(h); return false; }
        if (sz == 0) { CloseHandle(h); return true; }
        out.resize(sz); DWORD got = 0;
        BOOL r = ReadFile(h, &out[0], sz, &got, NULL);
        CloseHandle(h);
        return r != FALSE && got == sz;
    };
    auto udValidLines = [](const std::string& t) -> int {  // -1=存在非法行（腰斩/污染）
        int n = 0; size_t pos = 0;
        while (pos < t.size()) {
            size_t nl = t.find('\n', pos);
            size_t end = (nl == std::string::npos) ? t.size() : nl;
            std::string line = t.substr(pos, end - pos);
            pos = (nl == std::string::npos) ? t.size() : nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            size_t t1 = line.find('\t'); if (t1 == std::string::npos || t1 == 0) return -1;
            size_t t2 = line.find('\t', t1 + 1); if (t2 == std::string::npos || t2 == t1 + 1) return -1;
            size_t t3 = line.find('\t', t2 + 1);
            if (t3 != std::string::npos)
            {
                // B 衰减：第 4 列只许 0/1——manual"1"行尾；学习"0"须跟第 5 列 unix 秒
                if (line.size() <= t3 + 1) return -1;
                char c4 = line[t3 + 1];
                if (c4 == '1') { if (line.size() != t3 + 2) return -1; }
                else if (c4 == '0')
                {
                    size_t t4 = line.find('\t', t3 + 1);
                    if (t4 == std::string::npos || t4 + 1 >= line.size()) return -1;
                    for (size_t i = t4 + 1; i < line.size(); i++)
                        if (line[i] < '0' || line[i] > '9') return -1;
                }
                else return -1;
            }
            std::string cnt = (t3 == std::string::npos) ? line.substr(t2 + 1) : line.substr(t2 + 1, t3 - t2 - 1);
            if (cnt.empty()) return -1;
            for (char ch : cnt) if (ch < '0' || ch > '9') return -1;
            n++;
        }
        return n;
    };
    auto u8 = [](const wchar_t* w) -> std::string {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        std::string s(n > 1 ? n - 1 : 0, 0);
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
        return s;
    };
    std::wstring udPath = ExeDir() + L"\\userdata\\LingxiIme\\userdict.txt";
    std::wstring tmpPath = udPath + L".tmp";

    // ===== C4. 词典原子写断电态：陈旧 .tmp 不污染主文件；写失败保旧态+脏位重试 =====
    {
        lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
        if (!py) Report(false, "C4 CreatePinyinPredictor");
        else {
            py->Learn(L"ccak", L"原子一"); py->FlushUserDict();
            std::string f1; bool r1 = readBytes(udPath, f1);
            bool baseOk = r1 && udValidLines(f1) > 0 && f1.find(u8(L"原子一")) != std::string::npos;
            // 制造"断电中间态"：主文件完好 + .tmp 留垃圾半截
            { HANDLE h = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, NULL,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
              if (h != INVALID_HANDLE_VALUE) { DWORD g; const char junk[] = "PART\x01\x02TRUNC";
                  WriteFile(h, junk, (DWORD)sizeof(junk) - 1, &g, NULL); CloseHandle(h); } }
            delete py;
            py = lingxi::CreatePinyinPredictor();          // 重载：必须无视陈旧 .tmp
            std::string f2; readBytes(udPath, f2);
            bool staleIgnored = (f2 == f1);
            py->Learn(L"ccbk", L"原子二"); py->FlushUserDict();
            std::string f3; readBytes(udPath, f3);
            bool tmpConsumed = GetFileAttributesW(tmpPath.c_str()) == INVALID_FILE_ATTRIBUTES;
            bool grewOk = udValidLines(f3) > 0 && f3.find(u8(L"原子二")) != std::string::npos;
            // 写失败注入：排他占住 .tmp → persist 与同步 flush 都失败，主文件停旧完整态
            HANDLE hHold = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, NULL,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            bool held = hHold != INVALID_HANDLE_VALUE;
            py->Learn(L"cccb", L"原子三"); py->FlushUserDict();
            std::string f4; readBytes(udPath, f4);
            bool intact = held && (f4 == f3) && f4.find(u8(L"原子三")) == std::string::npos;
            CloseHandle(hHold);
            py->FlushUserDict();                            // 脏位重试：解除阻塞后补写成功
            std::string f5; readBytes(udPath, f5);
            bool retried = udValidLines(f5) > 0 && f5.find(u8(L"原子三")) != std::string::npos;
            delete py;
            Report(baseOk, "C4 baseline flush valid");
            Report(staleIgnored, "C4 stale .tmp ignored on reload");
            Report(tmpConsumed && grewOk, "C4 rename consumes .tmp");
            Report(intact, "C4 write-fail keeps old complete state");
            Report(retried, "C4 dirty retry succeeds after unblock");
            py = lingxi::CreatePinyinPredictor();
            if (py) { py->ClearUserDict(); delete py; }     // 复位，给 C1 干净起点
        }
    }

    // ===== C1. 停机三时序：正常收敛 / 落盘失败快回 / ownerDead 后僵尸不覆盖 =====
    // 注：真·2s 超时分支需磁盘挂死级 IO 停顿，台架不可构造；这里以"写失败
    // 快速返回"验证有界等待契约的两侧（耗时有上界、失败不污染），超时分支
    // 归 §八-7 观察项。
    {
        bool boundedA = false, validA = false;
        {
            lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
            if (py) {
                py->Learn(L"cdaa", L"停机一");
                ULONGLONG t0 = GetTickCount64();
                delete py;                                  // 在途 persist 必须收敛
                boundedA = (GetTickCount64() - t0) < 3000;
                std::string f; validA = readBytes(udPath, f) && udValidLines(f) > 0 &&
                                      f.find(u8(L"停机一")) != std::string::npos;
            }
        }
        Report(boundedA, "C1a normal shutdown bounded <3s");
        Report(validA, "C1a dict valid after normal shutdown");
        std::string fBefore; readBytes(udPath, fBefore);
        HANDLE hHold = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        bool boundedB = false, noZombie = false;
        if (hHold != INVALID_HANDLE_VALUE) {
            lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
            if (py) {
                py->Learn(L"cdbb", L"停机二");              // persist 失败（tmp 被占）
                ULONGLONG t0 = GetTickCount64();
                delete py;                                  // 停机尾部 flush 也失败 → 快回
                boundedB = (GetTickCount64() - t0) < 3000;
                CloseHandle(hHold);
                Sleep(500);                                 // 给僵尸 persist 线程窗口
                std::string fAfter; readBytes(udPath, fAfter);
                noZombie = (fAfter == fBefore) &&
                           fAfter.find(u8(L"停机二")) == std::string::npos; // ownerDead 生效
            } else CloseHandle(hHold);
        }
        Report(boundedB, "C1b shutdown with failing write bounded");
        Report(noZombie, "C1c ownerDead blocks zombie overwrite");
    }

    // ===== C1d. 强杀腰斩：子进程连发 Learn+异步落盘，父进程随机杀 ×5，词典恒完整 =====
    {
        wchar_t self[MAX_PATH] = L""; GetModuleFileNameW(NULL, self, MAX_PATH);
        std::wstring cmd = L"\""; cmd += self; cmd += L"\" --ud-churn";
        bool allValid = true; int killed = 0;
        for (int trial = 0; trial < 5; trial++)
        {
            STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
            if (!CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
            { allValid = false; break; }
            for (int w = 0; w < 150; w++) {            // 等首帧落盘（子进程冷载有秒级）
                if (GetFileAttributesW(udPath.c_str()) != INVALID_FILE_ATTRIBUTES) break;
                Sleep(100);
            }
            Sleep(150 + (DWORD)(GetTickCount64() % 500));   // 随机腰斩点
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 3000);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            std::string f; readBytes(udPath, f);
            if (udValidLines(f) < 0) { allValid = false; break; }
            killed++;
            DeleteFileW(udPath.c_str());              // 下轮从零开始，载词典耗时可控
        }
        Report(allValid && killed == 5, "C1d dict intact across 5 random kills");
    }

    // ===== M7. 畸形词典 fuzz（audit §三 M7 取证：zh_pinyin.txt 毒行 + userdict.txt 畸形行）=====
    // M7 修复=BuildIndexes badSyll 守卫（ui/engine/pinyin.cpp:873-879：任一心节为空→整词跳过）。
    // 取证：向运行词典副本注入畸形行→重载引擎→断言不崩（修复前 sy[0]/(*sylls[0])[0] 为 UB 读）、
    // 正常候选不变、毒词永不浮出；userdict 畸形行同口径优雅跳过（LoadUserDict 守卫链）。
    // 词典副本用后恢复/沙盒词典复位，不留污染给后续运行。
    {
        std::wstring pyData = ExeDir() + L"\\data\\zh_pinyin.txt";
        std::string orig; bool got = readBytes(pyData, orig);
        Report(got && !orig.empty(), "M7 baseline: staged zh_pinyin.txt readable");
        if (got && !orig.empty())
        {
            auto writeFile = [](const std::wstring& p, const std::string& data) -> bool {
                HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h == INVALID_HANDLE_VALUE) return false;
                DWORD g = 0;
                bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &g, NULL) != FALSE &&
                          g == (DWORD)data.size();
                CloseHandle(h);
                return ok;
            };
            // 动态基线首选（不硬编码词表假设）
            std::wstring baseFirst;
            {
                lingxi::IPredictor* py0 = lingxi::CreatePinyinPredictor();
                if (py0) {
                    lingxi::Prediction r0 = py0->Predict(L"jintian");
                    if (!r0.cands.empty()) baseFirst = r0.cands[0];
                    delete py0;
                }
            }
            // 毒行：前/双/尾空格（空音节）/空 py 字段/空词/缺 tab/64KB 超长音节/无尾换行
            std::string poison;
            poison += u8(L"毒甲") + "\t jin tian\n";
            poison += u8(L"毒乙") + "\tjin  tian\n";
            poison += u8(L"毒丙") + "\tjin tian \n";
            poison += u8(L"毒丁") + "\t\n";
            poison += std::string("\tjin tian\n");
            poison += u8(L"毒戊") + " no_tab_here\n";
            poison += u8(L"毒己") + "\t" + std::string(65536, 'a') + "\n";
            poison += u8(L"毒庚") + "\tjin  tian";   // 双空格 + 无尾换行
            static const wchar_t* kPoison[] = { L"毒甲", L"毒乙", L"毒丙", L"毒丁",
                L"毒戊", L"毒己", L"毒庚" };
            bool reloadOk = false, candsIntact = false, poisonGone = false, restoredStable = false;
            if (writeFile(pyData, orig + "\n" + poison))
            {
                lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
                reloadOk = py != NULL;
                if (py)
                {
                    lingxi::Prediction r = py->Predict(L"jintian");
                    candsIntact = !baseFirst.empty() && !r.cands.empty() && r.cands[0] == baseFirst;
                    poisonGone = true;
                    for (const wchar_t* q : { L"jintian", L"jin", L"tian" })
                    {
                        lingxi::Prediction rr = py->Predict(q);
                        for (const auto& c : rr.cands)
                            for (const wchar_t* pw : kPoison)
                                if (c == pw) poisonGone = false;
                    }
                    delete py;
                }
            }
            if (writeFile(pyData, orig))   // 恢复原字节，不给后续运行留毒
            {
                lingxi::IPredictor* py = lingxi::CreatePinyinPredictor();
                if (py)
                {
                    lingxi::Prediction r = py->Predict(L"jintian");
                    restoredStable = !baseFirst.empty() && !r.cands.empty() && r.cands[0] == baseFirst;
                    delete py;
                }
            }
            Report(reloadOk, "M7 malformed zh_pinyin reload: no crash");
            Report(candsIntact, "M7 normal candidates unchanged under poison dict");
            Report(poisonGone, "M7 poison entries never surface in candidates");
            Report(restoredStable, "M7 restored dict reload stable");

            // userdict.txt 畸形行（word\tkey\tcount 格式，LoadUserDict 守卫链）
            std::string ud;
            ud += u8(L"畸甲") + " no_tab\n";
            ud += std::string("\tnihao\t3\n");
            ud += u8(L"畸乙") + "\t\tnihao\t3\n";
            ud += u8(L"畸丙") + "\tnihao\tabc\n";
            ud += u8(L"畸丁") + "\tnihao\t0\n";
            ud += std::string(65536, 'x') + " no_tab_tail\n";
            ud += u8(L"畸戊") + "\tnihao\t5";          // 唯一合法行：无尾换行
            bool udNoCrash = false, udGraceful = false, udLastLineLoaded = false, udUntouched = false;
            std::string udBytes;
            if (writeFile(udPath, ud) && readBytes(udPath, udBytes))
            {
                lingxi::IPredictor* pyUd = lingxi::CreatePinyinPredictor();
                udNoCrash = pyUd != NULL;
                if (pyUd)
                {
                    lingxi::Prediction r = pyUd->Predict(L"nihao");
                    bool badPresent = false, lastPresent = false;
                    for (const auto& c : r.cands)
                    {
                        if (c == L"畸甲" || c == L"畸乙" || c == L"畸丙" || c == L"畸丁") badPresent = true;
                        if (c == L"畸戊") lastPresent = true;
                    }
                    udGraceful = !r.cands.empty() && !badPresent;
                    udLastLineLoaded = lastPresent;
                    std::string after;
                    udUntouched = readBytes(udPath, after) && after == udBytes;
                    pyUd->ClearUserDict();            // 复位沙盒词典
                    delete pyUd;
                }
            }
            Report(udNoCrash, "M7 malformed userdict reload: no crash");
            Report(udGraceful, "M7 malformed userdict: bad lines skipped, cands still out");
            Report(udLastLineLoaded, "M7 userdict valid last line w/o newline loaded");
            Report(udUntouched, "M7 userdict load does not rewrite file");
        }
    }

    // ===== M5-TTL. 外部直写陈旧窗口有界断言（audit §八-3 收口）=====
    // 契约：绕过 SaveSettings 的直写（reg.exe 类，不 bump SettingsSeq）由
    // kSetCacheTtlMs=500ms 兜底——窗口内缓存陈旧是设计行为，窗口后必反映新值。
    // 断言两侧都钉：①窗口内确走缓存（证明本测真的在压 TTL 路径而非 miss 直读）；
    // ②TTL 过期后 LoadSettings 必达新值（陈旧有界）。用后即删沙盒键。
    {
        wchar_t prevNs[64] = L"";
        GetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", prevNs, 64);
        SetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", L"ttl");   // 独立沙盒键
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\LingxiIme_ttl");
        lingxi::Settings s; s.candidateCount = 5;
        lingxi::SaveSettings(s);                                  // SaveSettings 路径：落 5 + bump seq
        lingxi::Settings warmed = lingxi::LoadSettings();         // 首读 miss→全量→填缓存（TTL 起点）
        bool wrote = false;
        { HKEY hk = nullptr;
          if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\LingxiIme_ttl", 0, KEY_WRITE, &hk) == ERROR_SUCCESS) {
              DWORD v = 7;
              wrote = RegSetValueExW(hk, L"CandidateCount", 0, REG_DWORD, (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS;
              RegCloseKey(hk);
          } }
        lingxi::Settings stale = lingxi::LoadSettings();          // 立即读：seq 未变+TTL 内→缓存命中
        Sleep(700);                                               // > kSetCacheTtlMs(500ms)
        lingxi::Settings fresh = lingxi::LoadSettings();          // TTL 过期→全量重读→必见 7
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\LingxiIme_ttl");
        SetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", prevNs[0] ? prevNs : L"");
        Report(warmed.candidateCount == 5 && wrote, "M5-TTL setup: saved=5, external write landed");
        Report(stale.candidateCount == 5, "M5-TTL stale served within window (cache active)");
        Report(fresh.candidateCount == 7, "M5-TTL staleness bounded: reload reflects direct write");
    }

    wprintf(L"=== 结果: PASS=%d FAIL=%d ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
