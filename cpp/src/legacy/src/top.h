#pragma once
#include "util.h"

// навигация TOP и служебные шестнадцатеричные страницы из потока .t42; 0 — ничего нет
int top_report(const std::string &t42, const std::string &out_txt, std::string &summary);
