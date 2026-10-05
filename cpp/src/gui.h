// Общие части окон программы: тёмная тема, шрифты, DPI, кнопки, диалоги, настройки, окно хода работы.
#pragma once
#include <windows.h>
#include <commctrl.h>
#include "json.h"
#include "png.h"

namespace ui {
// ---------------------------------------------------------------- тема
constexpr COLORREF BG = RGB(0x1b, 0x1c, 0x1f), PANEL = RGB(0x24, 0x26, 0x2b), FIELD = RGB(0x2d, 0x30, 0x36), LINE = RGB(0x3a, 0x3d, 0x44);
constexpr COLORREF FG = RGB(0xe3, 0xe3, 0xe3), MUTED = RGB(0xa0, 0xa3, 0xa8), DIM = RGB(0x7a, 0x7d, 0x83);
constexpr COLORREF ACCENT = RGB(0xff, 0xa3, 0x1a), ACCENT2 = RGB(0xff, 0xbe, 0x5c), SEL = RGB(0x5a, 0x3d, 0x10);
extern HBRUSH brBG, brPANEL, brFIELD;
COLORREF service_colour(const std::string &kind);

void init(HINSTANCE inst);
HINSTANCE inst();
int dpi(HWND h);
int S(HWND h, int v);                       // масштаб под DPI окна
HFONT font(HWND h, int pt = 10, bool bold = false, const wchar_t *face = L"Segoe UI");
void dark_window(HWND h);                   // тёмный заголовок
std::wstring W(const std::string &s);
std::string N(const std::wstring &s);

// ---------------------------------------------------------------- элементы
enum BtnStyle { BTN_NORMAL, BTN_ACCENT, BTN_BIG, BTN_LINK, BTN_CHECK, BTN_COLOUR };
HWND button(HWND parent, int id, const std::string &text, BtnStyle st = BTN_NORMAL, COLORREF colour = 0);
HWND label(HWND parent, const std::string &text, int id = 0, DWORD extra = 0);
HWND edit(HWND parent, int id, const std::string &text = "", DWORD extra = 0);
HWND listview(HWND parent, int id, const std::vector<std::pair<std::string, int>> &cols);
void set_text(HWND h, const std::string &s);
std::string get_text(HWND h);
bool checked(HWND h);
void set_checked(HWND h, bool v);
// обработать WM_DRAWITEM для кнопок; true — нарисовано
bool draw_item(LPARAM lp);
// WM_CTLCOLOR*: цвета для подписей и полей (muted — для подписей с id из набора)
LRESULT ctl_colour(UINT msg, WPARAM wp, LPARAM lp);
void set_label_colour(HWND h, COLORREF c);
void lv_set(HWND lv, int row, int col, const std::string &s);
LRESULT lv_custom_draw(LPARAM lp, const std::function<COLORREF(int row)> &text_colour = nullptr);

// ---------------------------------------------------------------- рисование
void blit(HDC dc, const Image &im, int x, int y, int w, int h, bool smooth = false);
void fill(HDC dc, RECT r, COLORREF c);
void text_out(HDC dc, int x, int y, const std::string &s, COLORREF c, HFONT f, UINT align = 0);

// ---------------------------------------------------------------- диалоги
std::string open_file(HWND owner, const std::string &title, const std::vector<std::pair<std::string, std::string>> &filters);
std::string save_file(HWND owner, const std::string &title, const std::string &def_name, const std::string &ext);
std::string pick_folder(HWND owner, const std::string &title, const std::string &start = "");
int ask(HWND owner, const std::string &title, const std::string &text, UINT flags);   // MessageBox
bool input(HWND owner, const std::string &title, const std::string &prompt, std::string &value);
// один из вариантов (кнопки); -1 — отмена. check — подпись флажка (пусто — без него), remember — его значение
int choose(HWND owner, const std::string &title, const std::string &prompt, const std::vector<std::string> &opts, int def,
           const std::string &check = "", bool *remember = nullptr);
void open_path(const std::string &p);        // ShellExecute

// ---------------------------------------------------------------- настройки
std::string state_path();
Json load_state();
void save_state(const Json &j);
std::vector<Json> recent_list();
void recent_add(const std::string &path, const std::string &kind);

// ---------------------------------------------------------------- фоновая работа
// work выполняется в рабочем потоке; on_done(ok) — в потоке окна после завершения
void run_task(HWND owner, const std::string &title, std::function<void(Progress &)> work, std::function<void(bool ok)> on_done);

// окно-класс с WndProc в виде std::function
struct Window {
    HWND hwnd = nullptr;
    virtual ~Window() {}
    virtual LRESULT proc(UINT msg, WPARAM wp, LPARAM lp);
    HWND create(const std::wstring &cls, const std::string &title, DWORD style, int w, int h, HWND parent = nullptr, DWORD ex = 0);
    static LRESULT CALLBACK thunk(HWND h, UINT m, WPARAM w, LPARAM l);
    static void register_class(const std::wstring &cls, HBRUSH bg = nullptr);
};
// функция в главный поток (из любого потока)
void post_ui(std::function<void()> fn);
extern HWND g_main;
constexpr UINT WM_APP_CALL = WM_APP + 1;
}
