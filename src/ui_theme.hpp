#pragma once

#include <windows.h>
#include <commctrl.h>
#include <algorithm>

namespace glint::ui {

// Shared visual tokens.  These intentionally stay close to the native Windows
// surface so the interface remains calm at every DPI scale.
constexpr COLORREF background() { return RGB(245, 248, 253); }
constexpr COLORREF surface() { return RGB(255, 255, 255); }
constexpr COLORREF ink() { return RGB(34, 48, 67); }
constexpr COLORREF muted() { return RGB(83, 101, 123); }
constexpr COLORREF line() { return RGB(215, 225, 236); }
constexpr COLORREF accent() { return RGB(44, 97, 189); }
constexpr COLORREF accent_hover() { return RGB(35, 79, 161); }
constexpr COLORREF wash() { return RGB(234, 241, 255); }
constexpr COLORREF success() { return RGB(35, 124, 86); }
constexpr COLORREF warning() { return RGB(165, 108, 24); }
constexpr COLORREF error() { return RGB(165, 42, 42); }

inline HBRUSH background_brush() {
    static HBRUSH brush = CreateSolidBrush(background());
    return brush;
}
inline HBRUSH surface_brush() {
    static HBRUSH brush = CreateSolidBrush(surface());
    return brush;
}
inline HBRUSH wash_brush() {
    static HBRUSH brush = CreateSolidBrush(wash());
    return brush;
}

inline HICON icon(HINSTANCE instance = GetModuleHandleW(nullptr)) {
    if (const auto result = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(201), IMAGE_ICON,
            0, 0, LR_DEFAULTSIZE | LR_SHARED)); result) return result;
    return LoadIconW(nullptr, IDI_APPLICATION);
}

inline LRESULT control_color(HDC dc, COLORREF color = ink(), bool on_surface = false) {
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    SetBkColor(dc, on_surface ? surface() : background());
    return reinterpret_cast<LRESULT>(on_surface ? surface_brush() : background_brush());
}

inline void paint_background(HWND window, HDC dc) {
    RECT bounds{}; GetClientRect(window, &bounds);
    FillRect(dc, &bounds, background_brush());
}

inline void paint_logo(HDC dc, RECT bounds) {
    POINT corners[] = {{bounds.left, bounds.top}, {bounds.right, bounds.bottom}};
    LPtoDP(dc, corners, 2);
    bounds = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
    const int saved = SaveDC(dc);
    SetWindowOrgEx(dc, 0, 0, nullptr);
    SetViewportOrgEx(dc, 0, 0, nullptr);
    SetMapMode(dc, MM_TEXT);
    const auto mark = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(201),
        IMAGE_ICON, bounds.right - bounds.left, bounds.bottom - bounds.top, LR_SHARED));
    if (mark) {
        DrawIconEx(dc, bounds.left, bounds.top, mark, bounds.right - bounds.left, bounds.bottom - bounds.top,
            0, nullptr, DI_NORMAL);
        RestoreDC(dc, saved);
        return;
    }
    SetMapMode(dc, MM_ANISOTROPIC);
    SetWindowExtEx(dc, 100, 100, nullptr);
    SetViewportOrgEx(dc, bounds.left, bounds.top, nullptr);
    SetViewportExtEx(dc, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr);
    SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH blue = CreateSolidBrush(accent()), ice = CreateSolidBrush(RGB(220, 232, 255));
    HBRUSH dot = CreateSolidBrush(RGB(169, 198, 255));
    SelectObject(dc, blue); RoundRect(dc, 0, 0, 100, 100, 25, 25);
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    POINT star[] = {{50,8},{61,39},{92,50},{61,61},{50,92},{39,61},{8,50},{39,39}};
    Polygon(dc, star, 8);
    SelectObject(dc, ice);
    POINT center[] = {{50,28},{56,44},{73,50},{56,56},{50,73},{44,56},{28,50},{44,44}};
    Polygon(dc, center, 8);
    SelectObject(dc, dot); Ellipse(dc, 68,18,82,32);
    RestoreDC(dc, saved);
    DeleteObject(blue); DeleteObject(ice); DeleteObject(dot);
}

inline bool high_contrast() {
    HIGHCONTRASTW mode{sizeof(mode)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(mode), &mode, 0) && (mode.dwFlags & HCF_HIGHCONTRASTON);
}

inline LRESULT CALLBACK button_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR id, DWORD_PTR primary) {
    constexpr wchar_t hover_key[] = L"Glint.ButtonHover";
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        if (high_contrast()) return DefSubclassProc(window, message, wparam, lparam);
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wparam);
        const int saved = SaveDC(dc);
        RECT bounds{}; GetClientRect(window, &bounds);
        FillRect(dc, &bounds, background_brush());
        const bool enabled = IsWindowEnabled(window) != FALSE;
        const bool pressed = (SendMessageW(window, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
        const bool hover = GetPropW(window, hover_key) != nullptr;
        const UINT dpi = GetDpiForWindow(window);
        const auto px = [dpi](int value) { return MulDiv(value, dpi ? dpi : 96, 96); };
        const COLORREF fill = !enabled ? RGB(233,238,246) : primary ? (hover || pressed ? accent_hover() : accent())
            : pressed || hover ? wash() : surface();
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, px(1), primary && enabled ? accent() : hover ? accent() : line());
        SelectObject(dc, brush); SelectObject(dc, pen);
        RoundRect(dc, 0, 0, bounds.right, bounds.bottom, px(8), px(8));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, !enabled ? muted() : primary ? surface() : ink());
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        if (font) SelectObject(dc, font);
        wchar_t text[256]{}; GetWindowTextW(window, text, 256);
        RECT content = bounds; InflateRect(&content, -px(5), -px(2));
        UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
        if (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) format |= DT_HIDEPREFIX;
        DrawTextW(dc, text, -1, &content, format);
        if (GetFocus() == window && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            RECT focus = bounds; InflateRect(&focus, -px(4), -px(4)); DrawFocusRect(dc, &focus);
        }
        RestoreDC(dc, saved); DeleteObject(brush); DeleteObject(pen);
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_MOUSEMOVE && !GetPropW(window, hover_key)) {
        SetPropW(window, hover_key, reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
        InvalidateRect(window, nullptr, FALSE);
    }
    if (message == WM_MOUSELEAVE) { RemovePropW(window, hover_key); InvalidateRect(window, nullptr, FALSE); }
    if (message == WM_NCDESTROY) {
        RemovePropW(window, hover_key); RemoveWindowSubclass(window, button_proc, id);
        return DefSubclassProc(window, message, wparam, lparam);
    }
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_KEYDOWN ||
        message == WM_KEYUP || message == BM_SETSTATE || message == WM_SETTEXT || message == WM_UPDATEUISTATE)
        InvalidateRect(window, nullptr, FALSE);
    return result;
}

inline void style_button(HWND window, bool primary = false) {
    if (window) SetWindowSubclass(window, button_proc, 1, primary ? 1 : 0);
}

inline LRESULT CALLBACK edit_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                  UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCPAINT && !high_contrast()) {
        HDC dc = GetWindowDC(window);
        RECT bounds{}; GetWindowRect(window, &bounds); OffsetRect(&bounds, -bounds.left, -bounds.top);
        const int saved = SaveDC(dc);
        RECT client{}; GetClientRect(window, &client);
        MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
        RECT screen{}; GetWindowRect(window, &screen); OffsetRect(&client, -screen.left, -screen.top);
        ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
        FillRect(dc, &bounds, surface_brush());
        HBRUSH brush = CreateSolidBrush(GetFocus() == window ? accent() : line());
        FrameRect(dc, &bounds, brush); DeleteObject(brush);
        RestoreDC(dc, saved); ReleaseDC(window, dc);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, edit_proc, id);
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
        RedrawWindow(window, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
    return result;
}

inline void style_edit(HWND window) {
    if (!window) return;
    const int margin = MulDiv(8, GetDpiForWindow(window), 96);
    SendMessageW(window, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(margin, margin));
    SetWindowSubclass(window, edit_proc, 2, 0);
}

} // namespace glint::ui
