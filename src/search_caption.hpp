#pragma once

#include "language.hpp"
#include "ui_theme.hpp"

#include <string>

namespace glint {

class SearchCaption {
public:
    SearchCaption() = default;
    ~SearchCaption() { destroy(); }
    SearchCaption(const SearchCaption&) = delete;
    SearchCaption& operator=(const SearchCaption&) = delete;

    bool create(HWND parent, int close_id, const wchar_t* title) {
        destroy();
        parent_ = parent;
        title_ = title ? title : L"";
        close_ = CreateWindowExW(0, L"BUTTON", close_name(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(close_id)), GetModuleHandleW(nullptr), nullptr);
        if (!close_ || !SetWindowSubclass(close_, button_proc, 1, reinterpret_cast<DWORD_PTR>(this))) {
            destroy(); return false;
        }
        set_enabled(IsWindowEnabled(parent) != FALSE);
        return true;
    }

    static int height(UINT dpi) { return MulDiv(32, dpi ? dpi : 96, 96); }

    void layout(int client_width, UINT dpi) {
        width_ = (std::max)(0, client_width);
        dpi_ = dpi ? dpi : 96;
        const int right = (std::max)(0, width_ - px(6));
        close_bounds_ = {(std::max)(0, right - px(32)), px(3), right, px(3) + px(26)};
        if (close_) SetWindowPos(close_, nullptr, close_bounds_.left, close_bounds_.top,
            close_bounds_.right - close_bounds_.left, close_bounds_.bottom - close_bounds_.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        if (parent_) InvalidateRect(parent_, nullptr, FALSE);
    }

    void paint(HDC dc, HFONT title_font) const {
        const int saved = SaveDC(dc);
        const bool contrast = ui::high_contrast();
        const COLORREF background = contrast ? GetSysColor(COLOR_WINDOW) : ui::background();
        const COLORREF foreground = contrast ? GetSysColor(COLOR_WINDOWTEXT) : ui::ink();
        RECT band{0, 0, width_, height(dpi_)};
        const HBRUSH brush = CreateSolidBrush(background);
        FillRect(dc, &band, brush); DeleteObject(brush);
        if (title_font) SelectObject(dc, title_font);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, foreground);
        RECT title{px(12), 0, (std::max<LONG>)(px(12), close_bounds_.left - px(8)), height(dpi_)};
        DrawTextW(dc, title_.c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        RestoreDC(dc, saved);
    }

    bool is_drag_point(POINT point) const {
        const RECT band{0, 0, width_, height(dpi_)};
        return PtInRect(&band, point) && !PtInRect(&close_bounds_, point);
    }

    HWND close_button() const { return close_; }

    void refresh_language() {
        if (close_) SetWindowTextW(close_, close_name());
    }

    void set_enabled(bool enabled) {
        if (!enabled) hover_ = false;
        if (close_) { EnableWindow(close_, enabled); InvalidateRect(close_, nullptr, FALSE); }
    }

    void destroy() {
        if (close_) DestroyWindow(close_);
        close_ = nullptr; parent_ = nullptr; hover_ = false;
    }

private:
    HWND parent_ = nullptr;
    HWND close_ = nullptr;
    UINT dpi_ = 96;
    int width_ = 0;
    RECT close_bounds_{};
    std::wstring title_;
    bool hover_ = false;

    int px(int value) const { return MulDiv(value, dpi_, 96); }
    static const wchar_t* close_name() { return i18n::text(L"Close", L"关闭"); }

    void paint_button(HWND window, HDC dc) const {
        const int saved = SaveDC(dc);
        RECT bounds{}; GetClientRect(window, &bounds);
        const bool contrast = ui::high_contrast();
        const bool enabled = IsWindowEnabled(window) && IsWindowEnabled(parent_);
        const bool pressed = (SendMessageW(window, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
        const bool highlighted = enabled && (pressed || hover_);
        const COLORREF base = contrast ? GetSysColor(COLOR_WINDOW) : ui::background();
        const COLORREF fill = highlighted ? (contrast ? GetSysColor(COLOR_HIGHLIGHT)
            : pressed ? RGB(160, 32, 23) : RGB(196, 43, 28)) : base;
        const COLORREF ink = !enabled ? GetSysColor(COLOR_GRAYTEXT)
            : highlighted ? (contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(255, 255, 255))
            : contrast ? GetSysColor(COLOR_WINDOWTEXT) : ui::ink();
        HBRUSH brush = CreateSolidBrush(fill);
        FillRect(dc, &bounds, brush); DeleteObject(brush);
        HPEN pen = CreatePen(PS_SOLID, (std::max)(1, px(2)), ink);
        SelectObject(dc, pen);
        const int center_x = bounds.right / 2, center_y = bounds.bottom / 2;
        const int radius = px(5);
        MoveToEx(dc, center_x - radius, center_y - radius, nullptr);
        LineTo(dc, center_x + radius, center_y + radius);
        MoveToEx(dc, center_x + radius, center_y - radius, nullptr);
        LineTo(dc, center_x - radius, center_y + radius);
        if (GetFocus() == window && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            RECT focus = bounds; InflateRect(&focus, -px(3), -px(3));
            SetTextColor(dc, ink); SetBkColor(dc, fill); DrawFocusRect(dc, &focus);
        }
        RestoreDC(dc, saved); DeleteObject(pen);
    }

    static LRESULT CALLBACK button_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR id, DWORD_PTR data) {
        auto* self = reinterpret_cast<SearchCaption*>(data);
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT paint{};
            HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wparam);
            self->paint_button(window, dc);
            if (message == WM_PAINT) EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_MOUSEMOVE && !self->hover_) {
            self->hover_ = true;
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking); InvalidateRect(window, nullptr, FALSE);
        }
        if (message == WM_MOUSELEAVE || message == WM_CANCELMODE) {
            self->hover_ = false; InvalidateRect(window, nullptr, FALSE);
        }
        if (message == WM_NCDESTROY) {
            self->close_ = nullptr; self->hover_ = false;
            RemoveWindowSubclass(window, button_proc, id);
            return DefSubclassProc(window, message, wparam, lparam);
        }
        const auto result = DefSubclassProc(window, message, wparam, lparam);
        if (message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
            message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_KEYDOWN ||
            message == WM_KEYUP || message == BM_SETSTATE || message == WM_SETTEXT ||
            message == WM_UPDATEUISTATE || message == WM_THEMECHANGED || message == WM_SYSCOLORCHANGE)
            InvalidateRect(window, nullptr, FALSE);
        return result;
    }
};

} // namespace glint
