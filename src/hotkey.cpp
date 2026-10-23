#include "hotkey.hpp"
#include "language.hpp"

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

    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        return std::wstring(i18n::text(L"Num ", L"小键盘 ")) + std::to_wstring(key - VK_NUMPAD0);
    // Named keys follow Glint's language, independently of the Windows display
    // language. Physical letters, digits, and modifier legends stay familiar.
    switch (key) {
    case VK_BACK: return i18n::text(L"Backspace", L"退格");
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_PAUSE: return i18n::text(L"Pause", L"暂停");
    case VK_CAPITAL: return L"Caps Lock";
    case VK_ESCAPE: return L"Esc";
    case VK_PRIOR: return i18n::text(L"Page Up", L"上一页");
    case VK_NEXT: return i18n::text(L"Page Down", L"下一页");
    case VK_END: return L"End";
    case VK_HOME: return L"Home";
    case VK_LEFT: return i18n::text(L"Left", L"左方向键");
    case VK_UP: return i18n::text(L"Up", L"上方向键");
    case VK_RIGHT: return i18n::text(L"Right", L"右方向键");
    case VK_DOWN: return i18n::text(L"Down", L"下方向键");
    case VK_SNAPSHOT: return i18n::text(L"Print Screen", L"截屏");
    case VK_INSERT: return i18n::text(L"Insert", L"插入");
    case VK_DELETE: return i18n::text(L"Delete", L"删除");
    case VK_NUMLOCK: return L"Num Lock";
    case VK_SCROLL: return L"Scroll Lock";
    case VK_MULTIPLY: return i18n::text(L"Num *", L"小键盘 *");
    case VK_ADD: return i18n::text(L"Num +", L"小键盘 +");
    case VK_SEPARATOR: return i18n::text(L"Num Separator", L"小键盘分隔符");
    case VK_SUBTRACT: return i18n::text(L"Num -", L"小键盘 -");
    case VK_DECIMAL: return i18n::text(L"Num .", L"小键盘 .");
    case VK_DIVIDE: return i18n::text(L"Num /", L"小键盘 /");
    case VK_BROWSER_BACK: return i18n::text(L"Browser Back", L"浏览器后退");
    case VK_BROWSER_FORWARD: return i18n::text(L"Browser Forward", L"浏览器前进");
    case VK_BROWSER_REFRESH: return i18n::text(L"Browser Refresh", L"浏览器刷新");
    case VK_BROWSER_STOP: return i18n::text(L"Browser Stop", L"浏览器停止");
    case VK_BROWSER_SEARCH: return i18n::text(L"Browser Search", L"浏览器搜索");
    case VK_BROWSER_FAVORITES: return i18n::text(L"Browser Favorites", L"浏览器收藏夹");
    case VK_BROWSER_HOME: return i18n::text(L"Browser Home", L"浏览器主页");
    case VK_VOLUME_MUTE: return i18n::text(L"Volume Mute", L"静音");
    case VK_VOLUME_DOWN: return i18n::text(L"Volume Down", L"降低音量");
    case VK_VOLUME_UP: return i18n::text(L"Volume Up", L"提高音量");
    case VK_MEDIA_NEXT_TRACK: return i18n::text(L"Next Track", L"下一曲");
    case VK_MEDIA_PREV_TRACK: return i18n::text(L"Previous Track", L"上一曲");
    case VK_MEDIA_STOP: return i18n::text(L"Media Stop", L"停止播放");
    case VK_MEDIA_PLAY_PAUSE: return i18n::text(L"Play/Pause", L"播放/暂停");
    case VK_LAUNCH_MAIL: return i18n::text(L"Mail", L"邮件");
    case VK_LAUNCH_MEDIA_SELECT: return i18n::text(L"Media", L"媒体");
    case VK_LAUNCH_APP1: return i18n::text(L"Launch App 1", L"启动应用 1");
    case VK_LAUNCH_APP2: return i18n::text(L"Launch App 2", L"启动应用 2");
    case VK_SELECT: return i18n::text(L"Select", L"选择");
    case VK_PRINT: return i18n::text(L"Print", L"打印");
    case VK_EXECUTE: return i18n::text(L"Execute", L"执行");
    case VK_HELP: return i18n::text(L"Help", L"帮助");
    case VK_APPS: return i18n::text(L"Menu", L"菜单");
    default: break;
    }

    // Punctuation follows the user's physical keyboard layout.
    const auto scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    const LONG key_data = static_cast<LONG>((scan & 0xff) << 16) | ((scan & 0xff00) ? 1 << 24 : 0);
    wchar_t text[128]{};
    if (scan != 0 && GetKeyNameTextW(key_data, text, static_cast<int>(std::size(text))) > 0) return text;
    return std::wstring(i18n::text(L"Key ", L"按键 ")) + std::to_wstring(key);
}

bool read_dword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD type = 0, size = sizeof(value);
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(value);
}

std::wstring invalid_binding_error() {
    return i18n::text(L"Use Ctrl, Alt, or Win with a key; Shift is optional. You can also use F1–F24, except F12.",
        L"普通键须含 Ctrl、Alt 或 Win，可叠加 Shift；也可使用 F1–F24（F12 除外）。");
}

std::wstring registration_error(Binding binding, DWORD code) {
    const auto label = name(binding);
    if (code == ERROR_HOTKEY_ALREADY_REGISTERED) {
        return std::wstring(i18n::text(L"Shortcut ", L"快捷键 ")) + label +
            i18n::text(L" is already in use by another app or Windows. Choose another shortcut.", L" 已被其他程序或 Windows 占用，请换一个组合。");
    }
    return std::wstring(i18n::text(L"Could not register shortcut ", L"无法注册快捷键 ")) + label +
        i18n::text(L". It may be in use by another app or Windows. Choose another shortcut. (Error ",
            L"，可能已被其他程序或 Windows 占用，请换一个组合。（错误 ") + std::to_wstring(code) + i18n::text(L")", L"）");
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
        error = std::wstring(i18n::text(L"Could not save shortcut settings. (Error ", L"无法保存快捷键设置。（错误 ")) +
            std::to_wstring(status) + i18n::text(L")", L"）");
        return false;
    }
    error.clear();
    return true;
}

Registration::~Registration() { stop(); }

bool Registration::start(HWND window, Binding binding, std::wstring& error) {
    if (!window || !IsWindow(window)) {
        error = i18n::text(L"Could not register the shortcut: the main window is not ready.", L"无法注册快捷键：主窗口尚未就绪。");
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
        error = i18n::text(L"Could not register the shortcut: the main window is not ready.", L"无法注册快捷键：主窗口尚未就绪。");
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
