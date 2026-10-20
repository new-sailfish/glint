#include "browser_launch.hpp"

#include <windows.h>

#include <cwchar>

namespace glint::browser_launch {
namespace {

const wchar_t* executable_name(Browser browser) {
    return browser == Browser::Chrome ? L"chrome.exe" : L"msedge.exe";
}

const wchar_t* browser_name(Browser browser) {
    return browser == Browser::Chrome ? L"Chrome" : L"Edge";
}

std::wstring read_app_path(detail::RegistryHive hive, detail::RegistryView view, const wchar_t* executable) {
    const std::wstring key_path = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + std::wstring(executable);
    const HKEY root = hive == detail::RegistryHive::CurrentUser ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    const REGSAM access = KEY_QUERY_VALUE | (view == detail::RegistryView::View64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY);
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, key_path.c_str(), 0, access, &key) != ERROR_SUCCESS) return {};
    // RegGetValue expands REG_EXPAND_SZ before returning the executable path.
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    DWORD bytes = 0;
    std::wstring value;
    if (RegGetValueW(key, nullptr, nullptr, flags, nullptr, nullptr, &bytes) == ERROR_SUCCESS &&
        bytes >= sizeof(wchar_t) && bytes <= 32768 * sizeof(wchar_t)) {
        value.resize(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(key, nullptr, nullptr, flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS) value.clear();
    }
    RegCloseKey(key);
    if (!value.empty()) value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

std::wstring read_environment(const wchar_t* name) {
    const DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
    if (count == 0) return {};
    std::wstring value(count, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), count);
    if (copied == 0 || copied >= count) return {};
    value.resize(copied);
    return value;
}

detail::SearchSources system_sources() {
    return {read_app_path, read_environment, [](const std::filesystem::path& path) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }};
}

std::uint32_t launch_process(const std::filesystem::path& executable, const std::wstring& arguments) {
    // lpApplicationName pins the browser even if its path contains spaces. The
    // quoted argv[0] is still necessary for the browser's own command parser.
    auto command = arguments;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    const auto directory = executable.parent_path().wstring();
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        directory.c_str(), &startup, &process)) {
        const DWORD error = GetLastError();
        return error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ERROR_SUCCESS;
}

std::wstring system_error(std::uint32_t code) {
    wchar_t* text = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = count ? std::wstring(text, count) : std::wstring{};
    if (text) LocalFree(text);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) result.pop_back();
    return result;
}

std::filesystem::path candidate_path(std::wstring value, Browser browser, const detail::SearchSources& sources) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    value = value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
    if (value.find_first_of(L"\"\r\n") != std::wstring::npos) return {};
    const std::filesystem::path path(value);
    if (!path.is_absolute() || _wcsicmp(path.filename().c_str(), executable_name(browser)) != 0) return {};
    return sources.is_file(path) ? path : std::filesystem::path{};
}

} // namespace

const wchar_t* extension_page(Browser browser) {
    return browser == Browser::Chrome ? L"chrome://extensions/" : L"edge://extensions/";
}

std::filesystem::path find_executable(Browser browser) {
    return detail::find_executable(browser, system_sources());
}

bool open_extensions(Browser browser, std::wstring& error) {
    return detail::open_extensions(browser, error, system_sources(), launch_process);
}

namespace detail {

std::filesystem::path find_executable(Browser browser, const SearchSources& sources) {
    for (const auto hive : {RegistryHive::CurrentUser, RegistryHive::LocalMachine}) {
        for (const auto view : {RegistryView::View64, RegistryView::View32}) {
            const auto path = candidate_path(sources.app_path(hive, view, executable_name(browser)), browser, sources);
            if (!path.empty()) return path;
        }
    }
    const auto relative = browser == Browser::Chrome
        ? L"Google\\Chrome\\Application\\chrome.exe" : L"Microsoft\\Edge\\Application\\msedge.exe";
    for (const auto* variable : {L"LOCALAPPDATA", L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"}) {
        const auto root = sources.environment(variable);
        if (root.empty()) continue;
        const auto path = candidate_path((std::filesystem::path(root) / relative).wstring(), browser, sources);
        if (!path.empty()) return path;
    }
    return {};
}

std::wstring command_line(const std::filesystem::path& executable, Browser browser) {
    (void)browser;
    // Chromium filters privileged chrome:// and edge:// pages supplied by an
    // external command line. Open a neutral window; the guide copies the page
    // address for the user to paste into the browser's address bar.
    return L"\"" + executable.wstring() + L"\" --new-window \"about:blank\"";
}

bool open_extensions(Browser browser, std::wstring& error, const SearchSources& sources, const Launcher& launch) {
    error.clear();
    const auto executable = find_executable(browser, sources);
    if (executable.empty()) {
        error = L"未找到 " + std::wstring(browser_name(browser)) +
            L"。请确认已安装，也可手动启动该浏览器，在地址栏粘贴：\r\n" + extension_page(browser);
        return false;
    }
    const auto code = launch(executable, command_line(executable, browser));
    if (code == ERROR_SUCCESS) return true;
    error = L"无法打开 " + std::wstring(browser_name(browser)) + L"（Windows 错误 " + std::to_wstring(code);
    const auto description = system_error(code);
    if (!description.empty()) error += L"：" + description;
    error += L"）。\r\n请手动启动该浏览器，在地址栏粘贴：\r\n" + std::wstring(extension_page(browser));
    return false;
}

} // namespace detail
} // namespace glint::browser_launch
