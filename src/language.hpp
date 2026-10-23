#pragma once

#include <string>
#include <string_view>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

namespace glint::i18n {

enum class Language { English, Chinese };
enum class Preference { System, English, Chinese };
inline std::atomic<Language> current_language{Language::English};
inline Preference current_preference = Preference::System;

inline Preference parse_preference(std::wstring_view value) {
    if (value == L"en") return Preference::English;
    if (value == L"zh-CN") return Preference::Chinese;
    return Preference::System;
}

inline const wchar_t* preference_name(Preference preference) {
    return preference == Preference::English ? L"en" : preference == Preference::Chinese ? L"zh-CN" : L"system";
}

inline Language resolve(Preference preference, std::wstring_view system_locale) {
    if (preference == Preference::English) return Language::English;
    if (preference == Preference::Chinese) return Language::Chinese;
    const bool chinese = system_locale.size() >= 2 &&
        (system_locale[0] == L'z' || system_locale[0] == L'Z') &&
        (system_locale[1] == L'h' || system_locale[1] == L'H') &&
        (system_locale.size() == 2 || system_locale[2] == L'-' || system_locale[2] == L'_');
    return chinese ? Language::Chinese : Language::English;
}

inline const wchar_t* text(const wchar_t* english, const wchar_t* chinese) {
    return current_language == Language::Chinese ? chinese : english;
}

inline const wchar_t* activation_error(std::string_view code) {
    if (code == "send_failed") return text(L"Could not send the switch request. Please try again.", L"无法发送切换请求，请稍后重试。");
    if (code == "invalid_tab_id") return text(L"The tab is no longer available.", L"标签页已不可用。");
    if (code == "invalid_window_id") return text(L"The browser window is no longer available.", L"浏览器窗口已不可用。");
    if (code == "activation_failed") return text(L"The browser could not activate the tab.", L"浏览器未能激活标签页。");
    if (code == "window_not_focused") return text(L"The browser window did not come to the foreground. Please try again.", L"浏览器窗口未能切到前台，请重试。");
    if (code == "activation_superseded") return text(L"The tab or window changed during activation. Please try again.", L"切换过程中标签页或窗口已变化，请重试。");
    return nullptr;
}

#ifdef _WIN32
inline constexpr wchar_t settings_key[] = L"Software\\Glint";

inline Preference load_preference(const wchar_t* key = settings_key) {
    wchar_t value[16]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"Language", RRF_RT_REG_SZ,
                    nullptr, value, &bytes) != ERROR_SUCCESS) return Preference::System;
    return parse_preference(value);
}

inline Language system_language() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Language::Chinese : Language::English;
}

inline void refresh() {
    current_language = current_preference == Preference::System ? system_language()
        : current_preference == Preference::Chinese ? Language::Chinese : Language::English;
}

inline void initialize() {
    current_preference = load_preference();
    refresh();
}

// Change in-memory state only after persistence succeeds. The optional key lets
// tests exercise the real registry without changing the user's preferences.
inline bool save_preference(Preference preference, std::wstring& error, const wchar_t* key = settings_key) {
    HKEY handle = nullptr;
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr);
    if (result == ERROR_SUCCESS) {
        const auto* value = preference_name(preference);
        result = RegSetValueExW(handle, L"Language", 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
            static_cast<DWORD>((std::wstring_view(value).size() + 1) * sizeof(wchar_t)));
        RegCloseKey(handle);
    }
    if (result != ERROR_SUCCESS) {
        error = std::wstring(text(L"Could not save the language preference. Windows error ", L"无法保存语言设置。Windows 错误 ")) + std::to_wstring(result);
        return false;
    }
    error.clear();
    current_preference = preference;
    refresh();
    return true;
}

inline DWORD message_language() {
    return MAKELANGID(current_language == Language::Chinese ? LANG_CHINESE : LANG_ENGLISH,
        current_language == Language::Chinese ? SUBLANG_CHINESE_SIMPLIFIED : SUBLANG_ENGLISH_US);
}
#endif

} // namespace glint::i18n
