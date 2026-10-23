#include "language.hpp"
#include "language_sync.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace glint::i18n;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

void check_language_settings_message() {
    const auto chinese_system = glint::language_settings_message(Preference::System,
        resolve(Preference::System, L"zh-HK"));
    require(chinese_system == R"({"language":"zh-CN","preference":"system","type":"language_settings","version":1})",
        "language settings carry the desktop preference and effective Chinese language");
    const auto explicit_english = glint::language_settings_message(Preference::English,
        resolve(Preference::English, L"zh-CN"));
    require(explicit_english == R"({"language":"en","preference":"en","type":"language_settings","version":1})",
        "explicit English overrides the desktop Chinese UI locale in the wire message");
    const auto explicit_chinese = glint::language_settings_message(Preference::Chinese,
        resolve(Preference::Chinese, L"en-US"));
    require(explicit_chinese == R"({"language":"zh-CN","preference":"zh-CN","type":"language_settings","version":1})",
        "explicit Chinese overrides the desktop English UI locale in the wire message");
    glint::FrameReader reader;
    const auto framed = glint::encode_frame(chinese_system);
    const auto messages = reader.feed(framed);
    require(messages.size() == 1 && messages.front() == chinese_system,
        "language settings use the existing native message framing unchanged");
}

void check_language_settings_fanout() {
    glint::ConnectionInfo pending, synchronized, invalid;
    pending.connection_id = 41;
    synchronized.connection_id = 72;
    synchronized.synchronized = true;
    const std::vector connections{pending, synchronized, invalid};
    std::vector<glint::ConnectionId> recipients;
    std::vector<std::string> messages;
    const bool all_queued = glint::send_language_settings(connections, Preference::System, Language::English,
        [&](glint::ConnectionId id, const std::string& message) {
            recipients.push_back(id);
            messages.push_back(message);
            return id != 41;
        });
    require(!all_queued, "a rejected send asks the caller to retry delivery");
    require(recipients == std::vector<glint::ConnectionId>{41, 72},
        "broadcast reaches pending and synchronized profiles, skips invalid IDs, and continues after rejection");
    require(messages.size() == 2 && messages[0] == messages[1] &&
        messages[0] == R"({"language":"en","preference":"system","type":"language_settings","version":1})",
        "all profiles receive the same snapshot of the desktop language");
    require(glint::send_language_settings(connections, Preference::English, Language::English,
        [](glint::ConnectionId, const std::string&) { return true; }),
        "successful broadcasts need no retry");
    require(glint::send_language_settings(std::span<const glint::ConnectionInfo>{}, Preference::Chinese, Language::Chinese,
        [&](glint::ConnectionId, const std::string&) { require(false, "empty connection list must not send"); return false; }),
        "no connected profiles is a successful empty broadcast");
}

int main() {
    check_language_settings_message();
    check_language_settings_fanout();
    for (const auto* locale : {L"zh", L"zh-CN", L"zh-TW", L"zh-HK", L"zh-Hans", L"zh-Hant", L"ZH_cn"})
        require(resolve(Preference::System, locale) == Language::Chinese, "Chinese system UI locales must default to Chinese");
    for (const auto* locale : {L"en-US", L"ja-JP", L"fr-FR", L"", L"zhx"})
        require(resolve(Preference::System, locale) == Language::English, "All other system locales must default to English");
    require(resolve(Preference::English, L"zh-CN") == Language::English, "English override must win on a Chinese system");
    require(resolve(Preference::Chinese, L"en-US") == Language::Chinese, "Chinese override must win on an English system");
    require(parse_preference(L"garbage") == Preference::System, "Invalid settings must fall back to system");
    for (const auto preference : {Preference::System, Preference::English, Preference::Chinese})
        require(parse_preference(preference_name(preference)) == preference, "Stored preferences must round trip");
    current_language = Language::English;
    require(std::wstring(text(L"Search", L"搜索")) == L"Search", "English text selected");
    current_language = Language::Chinese;
    require(std::wstring(text(L"Search", L"搜索")) == L"搜索", "Chinese text selected");
    require(std::wstring(activation_error("window_not_focused")).find(L"浏览器") != std::wstring::npos, "Known bridge errors use desktop language");
    require(activation_error("unknown_browser_error") == nullptr, "Unknown external errors keep original diagnostic");
#ifdef _WIN32
    const std::wstring key = L"Software\\Glint.Tests\\Language-" + std::to_wstring(GetCurrentProcessId());
    require(load_preference(key.c_str()) == Preference::System, "Absent registry setting uses system");
    std::wstring error;
    for (const auto preference : {Preference::English, Preference::Chinese, Preference::System}) {
        require(save_preference(preference, error, key.c_str()), "Preference must save successfully");
        require(load_preference(key.c_str()) == preference, "Persisted choice must survive reload");
        require(error.empty(), "Successful save clears errors");
    }
    HKEY handle = nullptr;
    require(RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &handle) == ERROR_SUCCESS, "Open test key");
    DWORD invalid = 42;
    RegSetValueExW(handle, L"Language", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&invalid), sizeof(invalid));
    RegCloseKey(handle);
    require(load_preference(key.c_str()) == Preference::System, "Invalid registry type falls back safely");
    RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
#endif
    std::cout << "Language tests passed\n";
}
