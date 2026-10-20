#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace glint::browser_launch {

enum class Browser { Chrome, Edge };

const wchar_t* extension_page(Browser browser);
std::filesystem::path find_executable(Browser browser);

// Opens a blank browser window. The guide copies extension_page() separately
// for the user to paste, since Chromium filters external internal-page launches.
// Success only means that Windows accepted the browser launch request.
bool open_extensions(Browser browser, std::wstring& error);

namespace detail {

enum class RegistryHive { CurrentUser, LocalMachine };
enum class RegistryView { View64, View32 };

// Operating-system boundaries are injectable so tests never alter App Paths or
// open a user's browser. All path selection and command construction stay real.
struct SearchSources {
    std::function<std::wstring(RegistryHive, RegistryView, const wchar_t*)> app_path;
    std::function<std::wstring(const wchar_t*)> environment;
    std::function<bool(const std::filesystem::path&)> is_file;
};

using Launcher = std::function<std::uint32_t(const std::filesystem::path&, const std::wstring&)>;

std::filesystem::path find_executable(Browser browser, const SearchSources& sources);
std::wstring command_line(const std::filesystem::path& executable, Browser browser);
bool open_extensions(Browser browser, std::wstring& error, const SearchSources& sources, const Launcher& launch);

} // namespace detail
} // namespace glint::browser_launch
