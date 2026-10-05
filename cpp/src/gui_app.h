// Окна программы, которые открывает главное окно.
#pragma once
#include "gui.h"

namespace ui {
struct StartActions {
    std::function<void()> open_vbi, open_stream, open_project;
    std::function<void(const Json &)> open_recent;
};
HWND start_screen(HWND parent, const StartActions &a);       // дочернее окно во всю клиентскую область
void recording_window(HWND owner, const Json &report, std::function<void(const Json &)> opener);
void sign_window(HWND owner, const std::string &folder);
void amol_window(HWND owner, const std::string &json);      // AMOL: часы передачи, код источника, биты пакета
void xds_window(HWND owner, const std::string &json);       // XDS: станция, часы эфира, передача
void starsight_window(HWND owner, const std::string &json); // StarSight: пакеты программы передач
void nabts_window(HWND owner, const std::string &t33);
void set_button_style(HWND h, BtnStyle st);
// столбчатая диаграмма (карта качества); hit — индекс столбика под точкой (или -1)
void bar_chart(HDC dc, RECT r, const std::vector<double> &vals, const std::vector<std::string> &labels, HWND ref);
int bar_hit(RECT r, size_t n, POINT p, HWND ref);
// тёмная строка меню (недокументированные сообщения UAH); true — обработано
bool dark_menubar(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT &res);
}
