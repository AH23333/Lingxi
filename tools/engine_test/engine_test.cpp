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
#include "../../common/proto.h"
#include "../../common/punct.h"

#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <string>

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
    double maxMs = 0.0;
};

// GetTickCount64 粒度约 15.6ms：平均值按"runs 次总耗时 / runs"计算（精确），
// 最大值取单次差分（受计时粒度限制，仅作参考）。
static LatencyStats MeasureLatency(lingxi::IPredictor* p, const std::wstring& ctx, int runs)
{
    // 使用 QPC 高精度计时（GetTickCount64 精度约 15.6ms，无法测毫秒级）。
    LatencyStats st;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    auto qpc = [&]() -> LONGLONG {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return c.QuadPart;
    };
    p->Predict(ctx); // 预热（首调走冷路径）
    LONGLONG t0 = qpc();
    double maxMs = 0.0;
    for (int i = 0; i < runs; i++)
    {
        LONGLONG c0 = qpc();
        p->Predict(ctx);
        double ms = (double)(qpc() - c0) * 1000.0 / (double)freq.QuadPart;
        if (ms > maxMs) maxMs = ms;
    }
    st.avgMs = (double)(qpc() - t0) * 1000.0 / (double)freq.QuadPart / (double)runs;
    st.maxMs = maxMs;
    return st;
}

int wmain()
{
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

    // 3. 中文高频接续："今天天气不错" 应给出非空候选
    {
        lingxi::Prediction r = p->Predict(L"今天天气不错");
        wchar_t detail[160];
        _snwprintf_s(detail, _TRUNCATE, L"cands=%zu first='%ls'",
                     r.cands.size(),
                     r.cands.empty() ? L"" : r.cands[0].c_str());
        Report(!r.cands.empty(), "zh continuation: 今天天气不错", detail);
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
            _snwprintf_s(line, _TRUNCATE, L"ctx='%ls' avg=%.3fms max=%.0fms",
                         ctx, st.avgMs, st.maxMs);
            wprintf(L"[INFO] latency %ls\n", line);
            if (st.avgMs >= 5.0) allOk = false;
        }
        Report(allOk, "latency avg < 5ms (50 runs x 3 contexts)");
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
            }

            // P5. 延迟：平均 < 10ms（规格上限）
            {
                LatencyStats st = MeasureLatency(py, L"jintianqi", 50);
                wchar_t line[192];
                _snwprintf_s(line, _TRUNCATE, L"ctx='jintianqi' avg=%.3fms max=%.0fms",
                             st.avgMs, st.maxMs);
                wprintf(L"[INFO] latency %ls\n", line);
                Report(st.avgMs < 10.0, "pinyin latency avg < 10ms (50 runs)", line);
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
                        {L"xiang", L"向"},
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

                // 持久化：新建引擎实例（重新加载词典）后“脏”仍置顶
                {
                    lingxi::IPredictor* py2 = lingxi::CreatePinyinPredictor();
                    lingxi::Prediction r2 = py2->Predict(L"zang");
                    bool persisted = !r2.cands.empty() && r2.cands[0] == L"脏";
                    Report(persisted, "learn: preference persisted across reload",
                           r2.cands.empty() ? L"<none>" : r2.cands[0].c_str());
                    delete py2;
                }

                // 恢复原有词典（无则删除测试文件）
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

            delete py;
        }
    }

    wprintf(L"=== 结果: PASS=%d FAIL=%d ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
