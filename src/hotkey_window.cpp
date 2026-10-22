#include "hotkey_window.hpp"
#include "ui_theme.hpp"

#include <commctrl.h>

#include <algorithm>
#include <utility>

namespace glint {
namespace {
constexpr wchar_t kClass[] = L"Glint.HotkeyWindow";
constexpr int Recorder = 3101, Error = 3102, Defaults = 3103;
constexpr int kWidth = 460, kHeight = 320;
UINT modifiers() {
    UINT value = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) value |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) value |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) value |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) value |= MOD_WIN;
    return value;
}
bool modifier_key(UINT key) {
    return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
        key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
        key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT || key == VK_LWIN || key == VK_RWIN;
}
} // namespace

HotkeyWindow::HotkeyWindow(HWND owner, Apply apply) : owner_(owner), apply_(std::move(apply)) {}
HotkeyWindow::~HotkeyWindow() {
    if (window_) DestroyWindow(window_);
    if (font_) DeleteObject(font_);
    if (heading_font_) DeleteObject(heading_font_);
}
bool HotkeyWindow::visible() const { return window_ && IsWindowVisible(window_); }
void HotkeyWindow::paint(HDC dc) const {
    ui::paint_background(window_, dc);
    const auto px = [&](int value) { return MulDiv(value, dpi_, 96); };
    RECT logo{px(20), px(16), px(52), px(48)};
    ui::paint_logo(dc, logo);
    RECT client{}; GetClientRect(window_, &client);
    RECT separator{px(20), px(264), client.right - px(20), px(265)};
    HBRUSH brush = CreateSolidBrush(ui::line());
    FillRect(dc, &separator, brush); DeleteObject(brush);
}
void HotkeyWindow::show(hotkey::Binding current) {
    if (visible()) { SetForegroundWindow(window_); SetFocus(recorder_); return; }
    if (!window_ && !create()) {
        MessageBoxW(owner_, L"无法打开快捷键设置，请稍后重试。", L"Glint", MB_OK | MB_ICONERROR);
        return;
    }
    draft_ = current;
    recording_modifiers_ = false;
    update_draft();
    ShowWindow(window_, SW_SHOWNORMAL);
    SetForegroundWindow(window_);
    SetFocus(recorder_);
    SendMessageW(recorder_, EM_SETSEL, 0, -1);
}
bool HotkeyWindow::create() {
    WNDCLASSEXW klass{sizeof(klass)};
    klass.lpfnWndProc = window_proc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = ui::icon();
    klass.hIconSm = ui::icon();
    klass.hbrBackground = ui::background_brush();
    klass.lpszClassName = kClass;
    if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    dpi_ = owner_ ? GetDpiForWindow(owner_) : GetDpiForSystem();
    if (!dpi_) dpi_ = 96;
    RECT frame{0, 0, MulDiv(kWidth, dpi_, 96), MulDiv(kHeight, dpi_, 96)};
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, dpi_);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = frame.right - frame.left, height = frame.bottom - frame.top;
    const int x = monitor.rcWork.left + ((monitor.rcWork.right - monitor.rcWork.left) - width) / 2;
    const int y = monitor.rcWork.top + ((monitor.rcWork.bottom - monitor.rcWork.top) - height) / 2;
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, kClass, L"Glint · 快捷键设置", style,
        x, y, width, height, owner_, nullptr, klass.hInstance, this);
    if (!window_) return false;
    const auto add = [&](const wchar_t* type, const wchar_t* caption, DWORD control_style, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, type, caption, WS_CHILD | WS_VISIBLE | control_style, 0, 0, 0, 0,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), klass.hInstance, nullptr);
    };
    heading_ = add(L"STATIC", L"设置搜索快捷键", 0, 0);
    instruction_ = add(L"STATIC", L"点击输入框，然后按下想使用的组合键。", 0, 0);
    label_ = add(L"STATIC", L"唤起搜索(&K)", 0, 0);
    recorder_ = add(L"EDIT", L"", WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL, Recorder, WS_EX_CLIENTEDGE);
    hint_ = add(L"STATIC", L"普通键须含 Ctrl/Alt/Win，可加 Shift；功能键可单用。\r\nTab 切换输入框和按钮，Enter 保存，Esc 放弃修改。", 0, 0);
    error_ = add(L"STATIC", L"", 0, Error);
    defaults_ = add(L"BUTTON", L"恢复默认(&D)", WS_TABSTOP | BS_PUSHBUTTON, Defaults);
    save_ = add(L"BUTTON", L"保存(&S)", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    cancel_ = add(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
    if (!heading_ || !instruction_ || !label_ || !recorder_ || !hint_ || !error_ || !defaults_ || !save_ || !cancel_) {
        DestroyWindow(window_); return false;
    }
    ui::style_edit(recorder_);
    ui::style_button(defaults_);
    ui::style_button(save_, true);
    ui::style_button(cancel_);
    update_fonts();
    layout();
    return true;
}
void HotkeyWindow::update_fonts() {
    LOGFONTW description{};
    description.lfHeight = -MulDiv(10, dpi_, 72);
    description.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(description.lfFaceName, L"Microsoft YaHei UI");
    HFONT next = CreateFontIndirectW(&description);
    description.lfHeight = -MulDiv(15, dpi_, 72);
    description.lfWeight = FW_SEMIBOLD;
    HFONT next_heading = CreateFontIndirectW(&description);
    if (next) {
        for (const auto control : {instruction_, label_, recorder_, hint_, error_, defaults_, save_, cancel_})
            if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        if (font_) DeleteObject(font_);
        font_ = next;
    }
    if (next_heading) {
        if (heading_) SendMessageW(heading_, WM_SETFONT, reinterpret_cast<WPARAM>(next_heading), TRUE);
        if (heading_font_) DeleteObject(heading_font_);
        heading_font_ = next_heading;
    }
}
void HotkeyWindow::layout() {
    if (!window_ || !recorder_) return;
    RECT client{}; GetClientRect(window_, &client);
    const auto px = [&](int value) { return MulDiv(value, dpi_, 96); };
    const int content = (std::max)(px(160), static_cast<int>(client.right) - px(40));
    MoveWindow(heading_, px(64), px(16), content - px(44), px(32), TRUE);
    MoveWindow(instruction_, px(20), px(52), content, px(24), TRUE);
    MoveWindow(label_, px(20), px(86), content, px(24), TRUE);
    MoveWindow(recorder_, px(20), px(114), content, px(32), TRUE);
    MoveWindow(hint_, px(20), px(156), content, px(44), TRUE);
    MoveWindow(error_, px(20), px(202), content, px(58), TRUE);
    MoveWindow(defaults_, px(20), px(278), px(110), px(30), TRUE);
    MoveWindow(save_, client.right - px(188), px(278), px(80), px(30), TRUE);
    MoveWindow(cancel_, client.right - px(100), px(278), px(80), px(30), TRUE);
    InvalidateRect(window_, nullptr, FALSE);
}
void HotkeyWindow::update_draft() {
    std::wstring caption;
    if (draft_.key) {
        caption = hotkey::name(draft_);
        if (caption.empty()) {
            if (draft_.key >= VK_F1 && draft_.key <= VK_F24) caption = L"F" + std::to_wstring(draft_.key - VK_F1 + 1);
            else if ((draft_.key >= 'A' && draft_.key <= 'Z') || (draft_.key >= '0' && draft_.key <= '9'))
                caption.assign(1, static_cast<wchar_t>(draft_.key));
            else caption = L"请重新按下组合键";
        }
    }
    else {
        if (draft_.modifiers & MOD_CONTROL) caption += L"Ctrl+";
        if (draft_.modifiers & MOD_ALT) caption += L"Alt+";
        if (draft_.modifiers & MOD_SHIFT) caption += L"Shift+";
        if (draft_.modifiers & MOD_WIN) caption += L"Win+";
        caption += L"…";
    }
    SetWindowTextW(recorder_, caption.c_str());
    const bool valid = hotkey::valid(draft_);
    EnableWindow(save_, valid);
    SetWindowTextW(error_, valid ? L"" : draft_.key == VK_F12 ? L"F12 为系统保留键，请换一个组合。" :
        L"普通键须含 Ctrl、Alt 或 Win，可叠加 Shift；也可使用 F1–F24（F12 除外）。");
}
void HotkeyWindow::record(UINT key, bool down) {
    if (modifier_key(key)) {
        if (down && !recording_modifiers_) { before_modifiers_ = draft_; recording_modifiers_ = true; }
        if (down || draft_.key == 0) { draft_ = {modifiers(), 0}; update_draft(); }
    } else if (down) {
        recording_modifiers_ = false;
        draft_ = {modifiers(), key};
        update_draft();
    }
}
void HotkeyWindow::record_registered(hotkey::Binding binding) {
    if (!visible() || GetFocus() != recorder_ || !hotkey::valid(binding)) return;
    draft_ = binding;
    recording_modifiers_ = false;
    update_draft();
}
void HotkeyWindow::save() {
    if (!hotkey::valid(draft_)) return;
    std::wstring error;
    if (!apply_ || !apply_(draft_, error)) {
        SetWindowTextW(error_, error.empty() ? L"无法保存快捷键，请重试。" : error.c_str());
        return;
    }
    dismiss();
}
void HotkeyWindow::dismiss() {
    ShowWindow(window_, SW_HIDE);
    if (IsWindowVisible(owner_)) SetForegroundWindow(owner_);
}
bool HotkeyWindow::translate(MSG& message) {
    if (!visible() || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) return false;
    if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
        const UINT key = static_cast<UINT>(message.wParam), held = modifiers();
        if (!held && key == VK_ESCAPE) { dismiss(); return true; }
        if (!held && key == VK_RETURN) { save(); return true; }
        if (message.hwnd == recorder_ && key == VK_TAB && !(held & (MOD_CONTROL | MOD_ALT | MOD_WIN)) && recording_modifiers_) {
            draft_ = before_modifiers_; recording_modifiers_ = false; update_draft();
        }
        if (message.hwnd == recorder_ && !(key == VK_TAB && !(held & (MOD_CONTROL | MOD_ALT | MOD_WIN)))) {
            record(key, true); return true;
        }
    }
    if (message.hwnd == recorder_) {
        if (message.message == WM_KEYUP || message.message == WM_SYSKEYUP) {
            record(static_cast<UINT>(message.wParam), false); return true;
        }
        if (message.message == WM_CHAR || message.message == WM_SYSCHAR) return true;
    }
    return IsDialogMessageW(window_, &message) != FALSE;
}
LRESULT CALLBACK HotkeyWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<HotkeyWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<HotkeyWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_ERASEBKGND:
        ui::paint_background(hwnd, reinterpret_cast<HDC>(wparam)); return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        self->paint(dc);
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_PRINTCLIENT: self->paint(reinterpret_cast<HDC>(wparam)); return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) { self->save(); return 0; }
        if (LOWORD(wparam) == IDCANCEL) { self->dismiss(); return 0; }
        if (LOWORD(wparam) == Defaults) {
            self->draft_ = hotkey::kDefault; self->recording_modifiers_ = false;
            self->update_draft(); SetFocus(self->recorder_); return 0;
        }
        break;
    case WM_CLOSE: self->dismiss(); return 0;
    case WM_SETFOCUS: if (self->recorder_) SetFocus(self->recorder_); return 0;
    case WM_SIZE: self->layout(); return 0;
    case WM_DPICHANGED: {
        self->dpi_ = HIWORD(wparam);
        self->update_fonts();
        const auto* frame = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, frame->left, frame->top, frame->right - frame->left, frame->bottom - frame->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        self->layout(); return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        const auto child = reinterpret_cast<HWND>(lparam);
        if (child == self->recorder_) return ui::control_color(dc, ui::accent(), true);
        const COLORREF color = child == self->error_ ? ui::error() : child == self->label_ ? ui::accent()
            : child == self->hint_ || child == self->instruction_ ? ui::muted() : ui::ink();
        return ui::control_color(dc, color);
    }
    case WM_CTLCOLOREDIT:
        return ui::control_color(reinterpret_cast<HDC>(wparam), ui::accent(), true);
    case WM_CTLCOLORBTN:
        return ui::control_color(reinterpret_cast<HDC>(wparam), ui::ink());
    case WM_DESTROY: self->window_ = nullptr; self->recorder_ = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace glint
