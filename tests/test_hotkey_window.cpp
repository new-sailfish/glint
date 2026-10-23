#include "hotkey_window.hpp"
#include "language.hpp"

#include <commctrl.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int Recorder = 3101, Error = 3102, Defaults = 3103;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
HWND settings_window() {
    HWND candidate = nullptr;
    for (;;) {
        candidate = FindWindowExW(nullptr, candidate, L"Glint.HotkeyWindow", nullptr);
        if (!candidate) break;
        DWORD process = 0;
        if (GetWindowThreadProcessId(candidate, &process) == GetCurrentThreadId() && process == GetCurrentProcessId())
            return candidate;
    }
    throw std::runtime_error("settings window belongs to this test");
}
std::wstring text(HWND window) {
    std::wstring value(GetWindowTextLengthW(window) + 1, L'\0');
    GetWindowTextW(window, value.data(), static_cast<int>(value.size()));
    value.resize(value.size() - 1);
    return value;
}
class Keyboard {
public:
    Keyboard() { require(GetKeyboardState(original_) != FALSE, "snapshot thread keyboard state"); }
    ~Keyboard() { SetKeyboardState(original_); }
    void set(UINT modifiers) {
        BYTE keys[256]{};
        if (modifiers & MOD_CONTROL) keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        if (modifiers & MOD_ALT) keys[VK_MENU] = keys[VK_LMENU] = 0x80;
        if (modifiers & MOD_SHIFT) keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
        if (modifiers & MOD_WIN) keys[VK_LWIN] = 0x80;
        require(SetKeyboardState(keys) != FALSE, "set test thread keyboard state");
    }
private:
    BYTE original_[256]{};
};
void key(glint::HotkeyWindow& window, UINT key_code, UINT modifiers = 0, bool system = false) {
    Keyboard keyboard;
    keyboard.set(modifiers);
    const HWND recorder = GetDlgItem(settings_window(), Recorder);
    SetFocus(recorder);
    MSG message{};
    message.hwnd = recorder;
    message.message = system ? WM_SYSKEYDOWN : WM_KEYDOWN;
    message.wParam = key_code;
    require(window.translate(message), "settings handles keyboard input");
}
void click(int control) { SendMessageW(GetDlgItem(settings_window(), control), BM_CLICK, 0, 0); }
void check_layout(HWND native, UINT dpi) {
    RECT frame{0, 0, MulDiv(460, dpi, 96), MulDiv(320, dpi, 96)};
    AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(native, GWL_STYLE)), FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(native, GWL_EXSTYLE)), dpi);
    SendMessageW(native, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&frame));
    RECT client{}; GetClientRect(native, &client);
    std::vector<RECT> occupied;
    for (HWND child = GetWindow(native, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT bounds{}; GetWindowRect(child, &bounds);
        MapWindowPoints(nullptr, native, reinterpret_cast<POINT*>(&bounds), 2);
        require(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom,
            "settings control fits its viewport at each DPI");
        for (const auto& previous : occupied) {
            RECT overlap{};
            require(!IntersectRect(&overlap, &bounds, &previous), "settings controls do not overlap");
        }
        occupied.push_back(bounds);
        wchar_t kind[32]{}; GetClassNameW(child, kind, 32);
        const auto value = text(child);
        if (wcscmp(kind, L"Static") == 0 && !value.empty()) {
            HDC dc = GetDC(child);
            const auto previous = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0)));
            RECT needed{0, 0, bounds.right - bounds.left, 0};
            DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &needed, DT_CALCRECT | DT_WORDBREAK);
            SelectObject(dc, previous); ReleaseDC(child, dc);
            require(needed.bottom <= bounds.bottom - bounds.top, "settings text including long conflicts must not clip");
        }
    }
}
void check_capture_and_cancel(HWND owner) {
    int saves = 0;
    glint::hotkey::Binding saved{};
    glint::HotkeyWindow window(owner, [&](glint::hotkey::Binding next, std::wstring&) {
        ++saves; saved = next; return true;
    });
    const glint::hotkey::Binding initial{MOD_CONTROL | MOD_SHIFT, 'J'};
    window.show(initial);
    require(window.visible(), "show native hotkey settings");
    require(text(GetDlgItem(settings_window(), Recorder)) == glint::hotkey::name(initial), "show current binding");
    key(window, 'K', MOD_CONTROL | MOD_ALT);
    require(text(GetDlgItem(settings_window(), Recorder)) == L"Ctrl+Alt+K", "record arbitrary key combination");
    require(saves == 0, "recording does not commit settings");
    click(IDOK);
    require(saves == 1 && saved == glint::hotkey::Binding{MOD_CONTROL | MOD_ALT, 'K'}, "save recorded combination once");
    require(!window.visible(), "successful save closes settings");
    window.show(initial);
    key(window, 'M', MOD_ALT, true);
    require(text(GetDlgItem(settings_window(), Recorder)) == L"Alt+M", "capture system key messages for Alt");
    key(window, VK_ESCAPE);
    require(!window.visible() && saves == 1, "Escape cancels without committing");
    window.show(initial);
    require(text(GetDlgItem(settings_window(), Recorder)) == glint::hotkey::name(initial), "reopen discards cancelled draft");
    key(window, 'G', MOD_WIN | MOD_SHIFT);
    require(text(GetDlgItem(settings_window(), Recorder)) == L"Shift+Win+G", "record Windows modifier");
    click(IDCANCEL);
    require(!window.visible() && saves == 1, "Cancel preserves current binding");
}
void check_invalid_defaults_and_conflict(HWND owner) {
    int saves = 0;
    bool allow = false;
    glint::hotkey::Binding saved{};
    glint::HotkeyWindow window(owner, [&](glint::hotkey::Binding next, std::wstring& error) {
        ++saves; saved = next; error = L"这个组合已被其他程序使用，请换一个。"; return allow;
    });
    window.show({MOD_CONTROL, 'J'});
    key(window, 'A');
    require(!IsWindowEnabled(GetDlgItem(settings_window(), IDOK)), "unmodified typing cannot become global hotkey");
    require(!text(GetDlgItem(settings_window(), Error)).empty(), "invalid shortcut has inline guidance");
    click(IDOK);
    require(saves == 0 && window.visible(), "invalid shortcut is never applied");
    key(window, VK_CONTROL, MOD_CONTROL);
    require(!IsWindowEnabled(GetDlgItem(settings_window(), IDOK)), "modifier alone cannot be saved");
    key(window, VK_F12, MOD_CONTROL);
    require(!IsWindowEnabled(GetDlgItem(settings_window(), IDOK)), "reserved F12 cannot be saved");
    click(Defaults);
    require(saves == 0 && IsWindowEnabled(GetDlgItem(settings_window(), IDOK)), "restore default only edits draft");
    require(text(GetDlgItem(settings_window(), Recorder)) == L"Ctrl+Shift+Space", "default preview");
    click(IDOK);
    require(saves == 1 && window.visible(), "failed registration keeps settings open");
    require(text(GetDlgItem(settings_window(), Error)).find(L"其他程序") != std::wstring::npos, "surface registration conflict inline");
    allow = true;
    key(window, 'L', MOD_CONTROL | MOD_ALT);
    require(text(GetDlgItem(settings_window(), Error)).empty(), "new recording clears stale conflict");
    key(window, VK_RETURN);
    require(saves == 2 && saved == glint::hotkey::Binding{MOD_CONTROL | MOD_ALT, 'L'}, "Enter saves the recorded shortcut");
    require(!window.visible(), "Enter closes after successful save");
}
void check_recording_existing_registration(HWND owner) {
    int saves = 0;
    glint::HotkeyWindow window(owner, [&](glint::hotkey::Binding, std::wstring&) { ++saves; return true; });
    const glint::hotkey::Binding current{MOD_CONTROL | MOD_SHIFT, VK_SPACE};
    window.show(current);
    key(window, VK_CONTROL, MOD_CONTROL);
    key(window, VK_SHIFT, MOD_CONTROL | MOD_SHIFT);
    // RegisterHotKey routes the current combination to the owner as WM_HOTKEY,
    // instead of sending its main-key WM_KEYDOWN to the recording edit.
    window.record_registered(current);
    require(text(GetDlgItem(settings_window(), Recorder)) == L"Ctrl+Shift+Space", "record the current global binding from its notification");
    require(IsWindowEnabled(GetDlgItem(settings_window(), IDOK)), "current binding remains saveable");
    SetFocus(GetDlgItem(settings_window(), IDCANCEL));
    window.record_registered({MOD_ALT, 'Q'});
    require(text(GetDlgItem(settings_window(), Recorder)) == L"Ctrl+Shift+Space", "notification outside recorder does not overwrite draft");
    require(saves == 0, "global notification alone never applies settings");
    click(IDCANCEL);
}
void check_keyboard_traversal(HWND owner) {
    glint::HotkeyWindow window(owner, [](glint::hotkey::Binding, std::wstring&) { return true; });
    window.show(glint::hotkey::kDefault);
    const HWND native = settings_window(), recorder = GetDlgItem(native, Recorder);
    key(window, VK_TAB);
    require(GetFocus() != recorder && IsChild(native, GetFocus()), "Tab exits recorder for native keyboard traversal");
    require(window.visible(), "Tab does not save or dismiss");
    key(window, VK_SHIFT, MOD_SHIFT);
    key(window, VK_TAB, MOD_SHIFT);
    require(GetFocus() != recorder && IsChild(native, GetFocus()), "Shift+Tab exits recorder backwards");
    require(text(recorder) == L"Ctrl+Shift+Space" && IsWindowEnabled(GetDlgItem(native, IDOK)),
        "Shift+Tab must preserve the recorded shortcut and Save state");
    click(IDCANCEL);
    MSG unrelated{}; unrelated.hwnd = owner; unrelated.message = WM_KEYDOWN; unrelated.wParam = 'Q';
    require(!window.translate(unrelated), "hidden settings do not consume other window input");
}
void check_conflict_layout(HWND owner) {
    glint::HotkeyWindow window(owner, [](glint::hotkey::Binding, std::wstring& error) {
        error = L"快捷键 Ctrl+Alt+Shift+Win+Browser Favorites 已被其他程序或 Windows 占用，请换一个组合。";
        return false;
    });
    window.show(glint::hotkey::kDefault);
    click(IDOK);
    for (const UINT dpi : {96u, 144u, 192u}) check_layout(settings_window(), dpi);
    key(window, 'A', MOD_SHIFT);
    for (const UINT dpi : {96u, 144u, 192u}) check_layout(settings_window(), dpi);
    key(window, VK_F12);
    for (const UINT dpi : {96u, 144u, 192u}) check_layout(settings_window(), dpi);
    click(IDCANCEL);
}
void check_language_refresh(HWND owner) {
    glint::HotkeyWindow window(owner, [](glint::hotkey::Binding, std::wstring&) { return true; });
    window.show(glint::hotkey::kDefault);
    key(window, 'J', MOD_CONTROL | MOD_ALT);
    glint::i18n::current_language = glint::i18n::Language::English;
    window.refresh_language();
    require(text(settings_window()) == L"Glint · Keyboard shortcut", "language refresh updates the window title");
    require(text(GetDlgItem(settings_window(), IDOK)) == L"&Save", "language refresh updates action labels");
    require(text(GetDlgItem(settings_window(), Recorder)) == L"Ctrl+Alt+J", "language refresh preserves an unsaved shortcut");
    for (const UINT dpi : {96u, 144u, 192u}) check_layout(settings_window(), dpi);
    key(window, VK_F12);
    require(text(GetDlgItem(settings_window(), Error)).find(L"reserved") != std::wstring::npos,
        "validation guidance follows the selected language");
    glint::i18n::current_language = glint::i18n::Language::Chinese;
    window.refresh_language();
    require(text(GetDlgItem(settings_window(), Error)).find(L"保留") != std::wstring::npos,
        "refresh translates current validation without clearing it");
    require(!IsWindowEnabled(GetDlgItem(settings_window(), IDOK)), "refresh preserves invalid draft state");
    click(IDCANCEL);
}
} // namespace

int main() {
    try {
        glint::i18n::current_language = glint::i18n::Language::Chinese;
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        HWND owner = CreateWindowExW(0, L"STATIC", L"Glint hotkey settings test owner", WS_OVERLAPPED, 0, 0, 100, 100,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(owner != nullptr, "create test-owned owner");
        check_capture_and_cancel(owner);
        check_invalid_defaults_and_conflict(owner);
        check_recording_existing_registration(owner);
        check_keyboard_traversal(owner);
        check_conflict_layout(owner);
        check_language_refresh(owner);
        DestroyWindow(owner);
        std::cout << "Hotkey window tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
