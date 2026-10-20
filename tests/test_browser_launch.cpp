#include "browser_launch.hpp"

#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace glint::browser_launch;
using detail::RegistryHive;
using detail::RegistryView;
using RegistryKey = std::tuple<RegistryHive, RegistryView, std::wstring>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct SearchFixture {
    std::map<RegistryKey, std::wstring> registry;
    std::map<std::wstring, std::wstring> environment;
    std::vector<std::filesystem::path> files;
    std::vector<RegistryKey> lookups;

    detail::SearchSources sources() {
        return {
            [this](RegistryHive hive, RegistryView view, const wchar_t* executable) {
                const RegistryKey key{hive, view, executable};
                lookups.push_back(key);
                const auto found = registry.find(key);
                return found == registry.end() ? std::wstring{} : found->second;
            },
            [this](const wchar_t* name) {
                const auto found = environment.find(name);
                return found == environment.end() ? std::wstring{} : found->second;
            },
            [this](const std::filesystem::path& candidate) {
                for (const auto& file : files) if (candidate == file) return true;
                return false;
            }
        };
    }
};

void pages_are_browser_specific() {
    require(std::wstring(extension_page(Browser::Chrome)) == L"chrome://extensions/", "Chrome internal extensions page");
    require(std::wstring(extension_page(Browser::Edge)) == L"edge://extensions/", "Edge internal extensions page");
}

void searches_both_registry_hives_and_views() {
    for (const auto hive : {RegistryHive::CurrentUser, RegistryHive::LocalMachine}) {
        for (const auto view : {RegistryView::View64, RegistryView::View32}) {
            SearchFixture fixture;
            const std::filesystem::path expected = L"D:\\Browser with spaces\\chrome.exe";
            fixture.registry[{hive, view, L"chrome.exe"}] = L"  \"" + expected.wstring() + L"\"  ";
            fixture.files = {expected};
            require(detail::find_executable(Browser::Chrome, fixture.sources()) == expected,
                    "resolve quoted App Paths executable in every hive and view");
        }
    }
}

void current_user_registration_wins_over_machine() {
    SearchFixture fixture;
    const std::filesystem::path user = L"D:\\User browser\\msedge.exe";
    const std::filesystem::path machine = L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe";
    fixture.registry[{RegistryHive::CurrentUser, RegistryView::View32, L"msedge.exe"}] = user.wstring();
    fixture.registry[{RegistryHive::LocalMachine, RegistryView::View64, L"msedge.exe"}] = machine.wstring();
    fixture.files = {user, machine};
    require(detail::find_executable(Browser::Edge, fixture.sources()) == user, "per-user install precedes machine install");
    require(fixture.lookups.size() == 2, "try both HKCU views before HKLM");
}

void stale_registry_falls_back_to_standard_installations() {
    for (const auto browser : {Browser::Chrome, Browser::Edge}) {
        for (const auto* variable : {L"LOCALAPPDATA", L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"}) {
            SearchFixture fixture;
            const auto* executable = browser == Browser::Chrome ? L"chrome.exe" : L"msedge.exe";
            fixture.registry[{RegistryHive::CurrentUser, RegistryView::View64, executable}] = L"D:\\Removed\\" + std::wstring(executable);
            fixture.environment[variable] = L"D:\\Installation Root";
            const auto expected = std::filesystem::path(L"D:\\Installation Root") /
                (browser == Browser::Chrome ? L"Google\\Chrome\\Application\\chrome.exe" : L"Microsoft\\Edge\\Application\\msedge.exe");
            fixture.files = {expected};
            require(detail::find_executable(browser, fixture.sources()) == expected, "fallback to existing standard install");
            require(fixture.lookups.size() == 4, "search all registry locations before standard directories");
        }
    }
}

void invalid_registry_values_never_launch_a_different_program() {
    for (const auto* value : {L"chrome.exe", L"C:\\Other\\msedge.exe", L"\"C:\\Apps\\chrome.exe\" --other", L"C:\\Apps\\chrome.exe\" --other"}) {
        SearchFixture fixture;
        fixture.registry[{RegistryHive::CurrentUser, RegistryView::View64, L"chrome.exe"}] = value;
        fixture.files = {std::filesystem::path(value)};
        require(detail::find_executable(Browser::Chrome, fixture.sources()).empty(), "reject relative path, wrong browser, or embedded arguments");
    }
}

void command_line_preserves_spaced_executable_and_uses_blank_page() {
    for (const auto browser : {Browser::Chrome, Browser::Edge}) {
        const std::filesystem::path executable = browser == Browser::Chrome
            ? L"D:\\Browser 安装目录\\chrome.exe" : L"D:\\Browser 安装目录\\msedge.exe";
        const auto command = detail::command_line(executable, browser);
        int count = 0;
        auto arguments = CommandLineToArgvW(command.c_str(), &count);
        require(arguments != nullptr, "parse generated Windows command line");
        const bool correct = count == 3 && arguments[0] == executable.wstring() &&
            std::wstring(arguments[1]) == L"--new-window" && std::wstring(arguments[2]) == L"about:blank";
        LocalFree(arguments);
        require(correct, "launch selected executable with a blank window, avoiding Chromium's internal URL filter");
    }
}

void missing_browser_returns_manual_url_without_launching() {
    for (const auto browser : {Browser::Chrome, Browser::Edge}) {
        SearchFixture fixture;
        bool launched = false;
        std::wstring error = L"stale";
        const bool result = detail::open_extensions(browser, error, fixture.sources(),
            [&](const auto&, const auto&) { launched = true; return std::uint32_t{0}; });
        require(!result && !launched, "missing browser never opens default browser");
        require(error.find(browser == Browser::Chrome ? L"Chrome" : L"Edge") != std::wstring::npos,
                "missing browser error identifies requested browser");
        require(error.find(extension_page(browser)) != std::wstring::npos, "missing browser error includes manual URL");
        require(error != L"stale", "replace previous errors");
    }
}

void launch_errors_and_success_are_reported() {
    SearchFixture fixture;
    const std::filesystem::path executable = L"D:\\Browser with spaces\\chrome.exe";
    fixture.registry[{RegistryHive::LocalMachine, RegistryView::View32, L"chrome.exe"}] = executable.wstring();
    fixture.files = {executable};
    std::wstring error;
    require(!detail::open_extensions(Browser::Chrome, error, fixture.sources(),
        [&](const auto& path, const auto& command) {
            require(path == executable, "launch passes absolute executable separately from arguments");
            require(command == detail::command_line(executable, Browser::Chrome), "launch uses tested argument construction");
            return std::uint32_t{ERROR_ACCESS_DENIED};
        }), "propagate launch failure");
    require(error.find(L"5") != std::wstring::npos && error.find(extension_page(Browser::Chrome)) != std::wstring::npos,
            "failure includes system code and manual page URL");
    require(detail::open_extensions(Browser::Chrome, error, fixture.sources(),
        [](const auto&, const auto&) { return std::uint32_t{0}; }), "report accepted browser launch");
    require(error.empty(), "successful launch clears stale errors");
}

} // namespace

int main() {
    try {
        pages_are_browser_specific();
        searches_both_registry_hives_and_views();
        current_user_registration_wins_over_machine();
        stale_registry_falls_back_to_standard_installations();
        invalid_registry_values_never_launch_a_different_program();
        command_line_preserves_spaced_executable_and_uses_blank_page();
        missing_browser_returns_manual_url_without_launching();
        launch_errors_and_success_are_reported();
        std::cout << "browser launch tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "browser launch test failed: " << error.what() << '\n';
        return 1;
    }
}
