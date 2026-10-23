#include "update_window.hpp"
#include "language.hpp"
#include <commctrl.h>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS}; InitCommonControlsEx(&controls);
    try {
        int checks = 0, installs = 0, toggles = 0;
        glint::UpdateWindow window(nullptr, [&] { ++checks; }, [&] { ++installs; }, [&](bool) { ++toggles; });
        glint::update::Snapshot state; state.current_version = "0.0.0";
        for (const auto language : {glint::i18n::Language::English, glint::i18n::Language::Chinese}) {
            glint::i18n::current_language = language;
            state.state = glint::update::State::Checking; window.show(state);
            HWND native = FindWindowW(L"Glint.UpdateWindow", nullptr);
            require(native && window.visible(), "update window opened");
            require(!IsWindowEnabled(GetDlgItem(native, 4101)), "checking prevents duplicate check");
            require(!IsWindowEnabled(GetDlgItem(native, 4102)), "unchecked bytes cannot install");
            state.state = glint::update::State::Ready; state.target_version = "0.1.9"; window.refresh(state);
            require(IsWindowEnabled(GetDlgItem(native, 4102)), "verified ready update can install");
            SendMessageW(GetDlgItem(native, 4102), BM_CLICK, 0, 0);
            for (const UINT dpi : {96u, 144u, 192u}) {
                RECT bounds{0, 0, MulDiv(560, dpi, 96), MulDiv(420, dpi, 96)};
                AdjustWindowRectExForDpi(&bounds, static_cast<DWORD>(GetWindowLongPtrW(native, GWL_STYLE)), FALSE,
                    static_cast<DWORD>(GetWindowLongPtrW(native, GWL_EXSTYLE)), dpi);
                SendMessageW(native, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&bounds));
                RECT client{}; GetClientRect(native, &client);
                for (HWND child = GetWindow(native, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                    RECT rect{}; GetWindowRect(child, &rect); MapWindowPoints(nullptr, native, reinterpret_cast<POINT*>(&rect), 2);
                    require(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom,
                        "update controls fit at every DPI");
                }
            }
            state.state = glint::update::State::Error; state.error = std::wstring(800, L'x'); window.refresh(state);
            require(IsWindowEnabled(GetDlgItem(native, 4101)), "errors permit explicit retry");
            SendMessageW(GetDlgItem(native, 4101), BM_CLICK, 0, 0);
            SendMessageW(GetDlgItem(native, 4103), BM_CLICK, 0, 0);
            SendMessageW(native, WM_CLOSE, 0, 0);
            require(!window.visible(), "close hides updates without authorizing installation");
        }
        require(installs == 2 && checks == 2 && toggles == 2, "only explicit buttons dispatch actions");
        std::cout << "Update window checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
