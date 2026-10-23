#include "search_caption.hpp"
#include "language.hpp"

#include <commctrl.h>
#include <windowsx.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int Close = 3601;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// All windows and keyboard focus belong to a private, never-displayed desktop.
class TestDesktop {
public:
    TestDesktop() {
        previous_ = GetThreadDesktop(GetCurrentThreadId());
        const auto name = L"Glint.Caption.Tests." + std::to_wstring(GetCurrentProcessId());
        desktop_ = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        require(desktop_ && SetThreadDesktop(desktop_), "create private test desktop");
    }
    ~TestDesktop() { SetThreadDesktop(previous_); CloseDesktop(desktop_); }
private:
    HDESK previous_ = nullptr;
    HDESK desktop_ = nullptr;
};

class TestWindow {
public:
    glint::SearchCaption caption;
    HWND window = nullptr;
    int closes = 0;

    TestWindow() {
        WNDCLASSW type{};
        type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"Glint.Caption.TestWindow";
        RegisterClassW(&type);
        window = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"Glint 1.2.3",
            WS_POPUP | WS_BORDER | WS_SYSMENU | WS_CLIPCHILDREN, 10, 10, 700, 400,
            nullptr, nullptr, type.hInstance, this);
        require(window != nullptr, "create caption test window");
        require(caption.create(window, Close, L"Glint 1.2.3"), "create usable caption close button");
        caption.layout(698, 96);
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }
    ~TestWindow() { if (window) DestroyWindow(window); }

    static LRESULT CALLBACK procedure(HWND native, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<TestWindow*>(GetWindowLongPtrW(native, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<TestWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(native, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(native, message, wparam, lparam);
        if (message == WM_NCHITTEST) {
            const auto native_hit = DefWindowProcW(native, message, wparam, lparam);
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(native, &point);
            return native_hit == HTCLIENT && self->caption.is_drag_point(point) ? HTCAPTION : native_hit;
        }
        if (message == WM_ENABLE) self->caption.set_enabled(wparam != FALSE);
        if (message == WM_COMMAND && LOWORD(wparam) == Close && HIWORD(wparam) == BN_CLICKED && IsWindowEnabled(native)) {
            SendMessageW(native, WM_CLOSE, 0, 0); return 0;
        }
        if (message == WM_CLOSE) { ++self->closes; ShowWindow(native, SW_HIDE); return 0; }
        return DefWindowProcW(native, message, wparam, lparam);
    }
};

RECT bounds(HWND window, HWND parent) {
    RECT rectangle{}; GetWindowRect(window, &rectangle);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rectangle), 2);
    return rectangle;
}

LRESULT hit(HWND window, POINT point) {
    ClientToScreen(window, &point);
    return SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
}

std::vector<COLORREF> render(HWND button) {
    RECT rectangle{}; GetClientRect(button, &rectangle);
    HDC dc = CreateCompatibleDC(nullptr);
    HDC screen = GetDC(nullptr);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, rectangle.right, rectangle.bottom);
    ReleaseDC(nullptr, screen);
    require(dc && bitmap, "create offscreen close button render target");
    const auto old = SelectObject(dc, bitmap);
    SendMessageW(button, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    std::vector<COLORREF> pixels;
    for (int y = 0; y < rectangle.bottom; ++y)
        for (int x = 0; x < rectangle.right; ++x) pixels.push_back(GetPixel(dc, x, y));
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
    return pixels;
}

void check_native_button_and_language() {
    TestWindow test;
    const HWND button = test.caption.close_button();
    wchar_t name[32]{}; GetClassNameW(button, name, 32);
    require(wcscmp(name, L"Button") == 0, "close retains native Button accessibility and keyboard semantics");
    require((GetWindowLongPtrW(button, GWL_STYLE) & WS_TABSTOP) != 0, "close is in keyboard tab order");
    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        10, 50, 100, 20, test.window, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(GetNextDlgTabItem(test.window, edit, FALSE) == button, "Tab traversal reaches close button");
    for (const auto language : {glint::i18n::Language::English, glint::i18n::Language::Chinese}) {
        glint::i18n::current_language = language;
        test.caption.refresh_language();
        GetWindowTextW(button, name, 32);
        require(std::wstring(name) == glint::i18n::text(L"Close", L"关闭"), "accessible close name follows current language");
    }
    glint::i18n::current_language = glint::i18n::Language::English;
}

void check_dpi_layout_and_drag_regions() {
    TestWindow test;
    for (const UINT dpi : {96u, 144u, 192u}) {
        const int width = MulDiv(700, dpi, 96);
        SetWindowPos(test.window, nullptr, 0, 0, width + 8, MulDiv(400, dpi, 96), SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
        test.caption.layout(width, dpi);
        const RECT close = bounds(test.caption.close_button(), test.window);
        require(width - close.right == MulDiv(6, dpi, 96), "close button has six-DIP right inset");
        require(close.right - close.left == MulDiv(32, dpi, 96), "close hit target scales to 32 DIP");
        require(close.bottom - close.top == MulDiv(26, dpi, 96), "close hit target scales to 26 DIP");
        require(close.top >= 0 && close.bottom <= glint::SearchCaption::height(dpi), "close stays inside caption at each DPI");
        const POINT center{(close.left + close.right) / 2, (close.top + close.bottom) / 2};
        require(hit(test.window, center) == HTCLIENT, "close does not initiate title drag");
        require(ChildWindowFromPoint(test.window, center) == test.caption.close_button(), "full close hit target belongs to native button");
        require(hit(test.window, {MulDiv(20, dpi, 96), MulDiv(16, dpi, 96)}) == HTCAPTION, "title area keeps native dragging");
        require(hit(test.window, {MulDiv(20, dpi, 96), glint::SearchCaption::height(dpi) + 1}) == HTCLIENT,
            "search content is outside title drag area");
    }
}

void check_activation_and_disabled_owner() {
    TestWindow test;
    const HWND button = test.caption.close_button();
    SendMessageW(test.window, WM_NCACTIVATE, TRUE, 0);
    const auto active = render(button);
    const COLORREF normal_ink = glint::ui::high_contrast() ? GetSysColor(COLOR_WINDOWTEXT) : glint::ui::ink();
    require(std::count(active.begin(), active.end(), normal_ink) >= 12, "close paints a contrasting X");
    SendMessageW(test.window, WM_NCACTIVATE, FALSE, 0);
    require(IsWindowEnabled(button) && active == render(button), "inactive close stays enabled with identical contrast");
    SendMessageW(button, BM_CLICK, 0, 0);
    require(test.closes == 1 && !IsWindowVisible(test.window), "inactive close sends normal close notification and can hide");
    ShowWindow(test.window, SW_SHOWNOACTIVATE);
    SetFocus(button);
    require(GetFocus() == button, "close can receive keyboard focus on test desktop");
    SendMessageW(button, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageW(button, WM_KEYUP, VK_SPACE, 0);
    require(test.closes == 2, "Space activates native focused close button");
    ShowWindow(test.window, SW_SHOWNOACTIVATE);
    EnableWindow(test.window, FALSE);
    require(!IsWindowEnabled(button), "modal-disabled owner disables close button");
    SendMessageW(button, BM_CLICK, 0, 0);
    require(test.closes == 2, "modal-disabled owner ignores close activation");
    EnableWindow(test.window, TRUE);
    require(IsWindowEnabled(button), "close is restored after modal owner is enabled");
    test.caption.destroy();
    require(!IsWindow(button), "caption cleanup destroys its native button");
    require(test.caption.create(test.window, Close, L"Glint 1.2.3"), "caption can be recreated after cleanup");
}

void check_hover_pressed_and_focus() {
    TestWindow test;
    const HWND button = test.caption.close_button();
    const auto ordinary = render(button);
    SendMessageW(button, WM_MOUSEMOVE, 0, MAKELPARAM(16, 13));
    const auto hovered = render(button);
    require(hovered != ordinary, "hover visibly highlights close");
    const COLORREF highlight_ink = glint::ui::high_contrast() ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(255, 255, 255);
    require(std::count(hovered.begin(), hovered.end(), highlight_ink) >= 12, "hover keeps X legible");
    SendMessageW(button, BM_SETSTATE, TRUE, 0);
    const auto pressed = render(button);
    require(glint::ui::high_contrast() || pressed != hovered, "press deepens hover feedback");
    SendMessageW(button, BM_SETSTATE, FALSE, 0);
    SendMessageW(button, WM_MOUSELEAVE, 0, 0);
    require(render(button) == ordinary, "leaving returns to calm enabled appearance");
    SetFocus(button);
    SendMessageW(button, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    require(render(button) != ordinary, "keyboard focus has visible indicator");
    DestroyWindow(test.window); test.window = nullptr;
    require(test.caption.close_button() == nullptr, "parent destruction clears child handle safely");
}
} // namespace

int main() {
    try {
        TestDesktop desktop;
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
        require(InitCommonControlsEx(&controls) != FALSE, "initialize native controls");
        check_native_button_and_language();
        check_dpi_layout_and_drag_regions();
        check_activation_and_disabled_owner();
        check_hover_pressed_and_focus();
        std::cout << "Search caption tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
