#pragma once

#include <string>
#include <vector>

namespace lingxi {

// 符号搜索：按拼音/中文/英文关键词过滤符号表，返回命中的符号串。
// 空查询返回空表；大小写不敏感；空格分隔多词 AND；整词命中排在子串命中前
// （"mao"：猫/🐱 整词在前，"maohao 冒号"子串在后）；结果上限 96（面板 12 行）。
// 数据与逻辑自包含（symsearch.cpp），符号面板搜索行与 engine_test 单测共用。
std::vector<std::wstring> SymbolSearch(const std::wstring& query);

} // namespace lingxi
