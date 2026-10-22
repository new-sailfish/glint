#include "hotkey.hpp"

namespace glint::hotkey {
namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\Glint";
constexpr UINT kModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;

bool ordinary_key(UINT key) {
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
        (key >= VK_NUMPAD0 && key <= VK_DIVIDE) ||
        (key >= VK_OEM_1 && key <= VK_OEM_3) ||
        (key >= VK_OEM_4 && key <= VK_OEM_8) || key == VK_OEM_102 ||
        (key >= VK_BROWSER_BACK && key <= VK_LAUNCH_APP2)) return true;
    switch (key) {
    case VK_BACK: case VK_TAB: case VK_RETURN: case VK_PAUSE: case VK_CAPITAL:
    case VK_ESCAPE: case VK_SPACE: case VK_PRIOR: case VK_NEXT: case VK_END:
    case VK_HOME: case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_SELECT: case VK_PRINT: case VK_EXECUTE: case VK_SNAPSHOT:
    case VK_INSERT: case VK_DELETE: case VK_HELP: case VK_APPS:
    case VK_NUMLOCK: case VK_SCROLL:
        return true;
    default:
        return false;
    }
}

std::wstring key_name(UINT key) {
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= VK_F1 && key <= VK_F24) return L"F" + std::to_wstring(key - VK_F1 + 1);
    if (key == VK_SPACE) return L"Space";

    const auto scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    LONG key_data = static_cast<LONG>((scan & 0xff) << 16);
    if ((scan & 0xff00) != 0 || key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
        key == VK_PRIOR || key == VK_NEXT || key == VK_END || key == VK_HOME ||
        key == VK_INSERT || key == VK_DELETE || key == VK_NUMLOCK) key_data |= 1 << 24;
    wchar_t text[128]{};
    if (scan != 0 && GetKeyNameTextW(key_data, text, static_cast<int>(std::size(text))) > 0) return text;

    // Media and browser keys can lack scan codes on the active keyboard layout.
    switch (key) {
    case VK_BROWSER_BACK: return L"Browser Back";
    case VK_BROWSER_FORWARD: return L"Browser Forward";
    case VK_BROWSER_REFRESH: return L"Browser Refresh";
    case VK_BROWSER_STOP: return L"Browser Stop";
    case VK_BROWSER_SEARCH: return L"Browser Search";
    case VK_BROWSER_FAVORITES: return L"Browser Favorites";
    case VK_BROWSER_HOME: return L"Browser Home";
    case VK_VOLUME_MUTE: return L"Volume Mute";
    case VK_VOLUME_DOWN: return L"Volume Down";
    case VK_VOLUME_UP: return L"Volume Up";
    case VK_MEDIA_NEXT_TRACK: return L"Next Track";
    case VK_MEDIA_PREV_TRACK: return L"Previous Track";
    case VK_MEDIA_STOP: return L"Media Stop";
    case VK_MEDIA_PLAY_PAUSE: return L"Play/Pause";
    case VK_LAUNCH_MAIL: return L"Mail";
    case VK_LAUNCH_MEDIA_SELECT: return L"Media";
    case VK_LAUNCH_APP1: return L"Launch App 1";
    case VK_LAUNCH_APP2: return L"Launch App 2";
    case VK_SELECT: return L"Select";
    case VK_PRINT: return L"Print";
    case VK_EXECUTE: return L"Execute";
    case VK_HELP: return L"Help";
    case VK_APPS: return L"Menu";
    default: return L"Key " + std::to_wstring(key);
    }
}

bool read_dword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD type = 0, size = sizeof(value);
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(value);
}

std::wstring invalid_binding_error() {
    return L"普通键须含 Ctrl、Alt 或 Win，可叠加 Shift；也可使用 F1–F24（F12 除外）。";
}

std::wstring registration_error(Binding binding, DWORD code) {
    const auto label = name(binding);
    if (code == ERROR_HOTKEY_ALREADY_REGISTERED) {
        return L"快捷键 " + label + L" 已被其他程序或 Windows 占用，请换一个组合。";
    }
    return L"无法注册快捷键 " + label + L"，可能已被其他程序或 Windows 占用，请换一个组合。（错误 " +
        std::to_wstring(code) + L"）";
}

} // namespace

bool valid(Binding binding) {
    if ((binding.modifiers & ~kModifiers) != 0 || binding.key == VK_F12) return false;
    if (binding.key >= VK_F1 && binding.key <= VK_F24) return true;
    return (binding.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)) != 0 && ordinary_key(binding.key);
}

std::wstring name(Binding binding) {
    if (!valid(binding)) return {};
    std::wstring text;
    if ((binding.modifiers & MOD_CONTROL) != 0) text += L"Ctrl+";
    if ((binding.modifiers & MOD_ALT) != 0) text += L"Alt+";
    if ((binding.modifiers & MOD_SHIFT) != 0) text += L"Shift+";
    if ((binding.modifiers & MOD_WIN) != 0) text += L"Win+";
    return text + key_name(binding.key);
}

Binding load() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return kDefault;

    DWORD value = 0;
    if (read_dword(key, L"HotkeyBinding", value)) {
        const Binding binding{value >> 16, value & 0xffff};
        if (valid(binding)) {
            RegCloseKey(key);
            return binding;
        }
    }
    const bool has_legacy = read_dword(key, L"Hotkey", value);
    RegCloseKey(key);
    if (has_legacy && value == 1) return {MOD_CONTROL | MOD_ALT, VK_SPACE};
    if (has_legacy && value == 2) return {MOD_ALT, VK_SPACE};
    return kDefault;
}

bool save(Binding binding, std::wstring& error) {
    if (!valid(binding)) {
        error = invalid_binding_error();
        return false;
    }
    HKEY key = nullptr;
    auto status = RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                                  KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status == ERROR_SUCCESS) {
        const DWORD value = (binding.modifiers << 16) | binding.key;
        status = RegSetValueExW(key, L"HotkeyBinding", 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
    if (status != ERROR_SUCCESS) {
        error = L"无法保存快捷键设置。（错误 " + std::to_wstring(status) + L"）";
        return false;
    }
    error.clear();
    return true;
}

Registration::~Registration() { stop(); }

bool Registration::start(HWND window, Binding binding, std::wstring& error) {
    if (!window || !IsWindow(window)) {
        error = L"无法注册快捷键：主窗口尚未就绪。";
        return false;
    }
    if (!valid(binding)) {
        error = invalid_binding_error();
        return false;
    }
    if (active_ && window_ == window && current_ == binding) {
        error.clear();
        return true;
    }
    // Keep the requested binding and owner after a startup conflict so the
    // settings dialog displays it and can recover through change().
    if (!active_) {
        window_ = window;
        current_ = binding;
    }
    const int next_id = active_ ? 3 - id_ : id_;
    if (!RegisterHotKey(window, next_id, binding.modifiers | MOD_NOREPEAT, binding.key)) {
        const auto code = GetLastError();
        error = registration_error(binding, code);
        return false;
    }
    if (active_) UnregisterHotKey(window_, id_);
    window_ = window;
    current_ = binding;
    id_ = next_id;
    active_ = true;
    error.clear();
    return true;
}

bool Registration::change(Binding binding, std::wstring& error) {
    if (!valid(binding)) {
        error = invalid_binding_error();
        return false;
    }
    if (!window_ || !IsWindow(window_)) {
        error = L"无法注册快捷键：主窗口尚未就绪。";
        return false;
    }
    if (active_ && current_ == binding) return save(binding, error);

    const int next_id = active_ ? 3 - id_ : id_;
    if (!RegisterHotKey(window_, next_id, binding.modifiers | MOD_NOREPEAT, binding.key)) {
        const auto code = GetLastError();
        error = registration_error(binding, code);
        return false;
    }
    if (!save(binding, error)) {
        UnregisterHotKey(window_, next_id);
        return false;
    }
    if (active_) UnregisterHotKey(window_, id_);
    current_ = binding;
    id_ = next_id;
    active_ = true;
    return true;
}

void Registration::stop() {
    if (active_) UnregisterHotKey(window_, id_);
    active_ = false;
}

} // namespace glint::hotkey
