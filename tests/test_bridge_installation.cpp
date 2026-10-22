#include "bridge_installation.hpp"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::wstring environment_value() {
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), size);
    require(count && count < size, "read original LOCALAPPDATA");
    value.resize(count);
    return value;
}
struct Sandbox {
    std::wstring original = environment_value();
    std::filesystem::path directory;
    Sandbox() {
        static std::atomic<unsigned> sequence = 0;
        directory = std::filesystem::temp_directory_path() / (L"Glint-bridge-tests-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
            L"-" + std::to_wstring(sequence++));
        require(std::filesystem::create_directory(directory), "create unique test folder");
        require(SetEnvironmentVariableW(L"LOCALAPPDATA", directory.c_str()) != FALSE, "isolate bridge installation");
    }
    ~Sandbox() {
        SetEnvironmentVariableW(L"LOCALAPPDATA", original.empty() ? nullptr : original.c_str());
        if (directory.parent_path() == std::filesystem::temp_directory_path() &&
            directory.filename().wstring().starts_with(L"Glint-bridge-tests-")) {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    }
};
std::string bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "read comparison file");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::filesystem::path executable() {
    std::wstring value(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    require(count && count < value.size(), "get test executable");
    value.resize(count);
    return value;
}
void content_addressed_installation_and_reuse() {
    Sandbox sandbox;
    std::wstring error = L"previous error";
    const auto expected = glint::bridge_installation::path_for_current_executable(error);
    require(!expected.empty() && error.empty(), "calculate bridge identity");
    require(expected.filename() == L"GlintBridge.exe" && expected.parent_path().filename().wstring().size() == 64 &&
        expected.parent_path().parent_path() == sandbox.directory / L"Glint" / L"bridge", "use per-user content address");
    require(!std::filesystem::exists(sandbox.directory / L"Glint"), "path calculation is read-only");
    require(!glint::bridge_installation::is_current_bridge(expected), "missing installation is not current");
    std::filesystem::path installed;
    require(glint::bridge_installation::ensure_current(installed, error) && error.empty() && installed == expected, "install current bridge");
    require(bytes(installed) == bytes(executable()), "installed bridge exactly matches source bytes");
    require(glint::bridge_installation::is_current_bridge(installed) && glint::bridge_installation::is_installed_bridge(installed),
        "installed copy passes current and installed identity checks");
    require(glint::bridge_installation::is_current_bridge(installed.parent_path() / L"." / installed.filename()),
        "lexically equivalent absolute bridge path is current");
    const auto timestamp = std::filesystem::last_write_time(installed);
    HANDLE held = CreateFileW(installed.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(held != INVALID_HANDLE_VALUE, "simulate browser holding the installed version");
    const bool reused = glint::bridge_installation::ensure_current(installed, error);
    CloseHandle(held);
    require(reused && error.empty(), "identical in-use installation can be reused");
    require(std::filesystem::last_write_time(installed) == timestamp, "reuse does not rewrite executable");

    const auto other = sandbox.directory / L"other.exe";
    std::filesystem::copy_file(installed, other);
    require(!glint::bridge_installation::is_current_bridge(other) && !glint::bridge_installation::is_installed_bridge(other),
        "matching contents outside installed path are rejected");
    require(!glint::bridge_installation::is_installed_bridge(installed.parent_path() / L"Other.exe"),
        "bridge basename must match");
}
void detects_and_repairs_corruption_without_touching_old_versions() {
    Sandbox sandbox;
    std::wstring error;
    std::filesystem::path installed;
    require(glint::bridge_installation::ensure_current(installed, error), "prepare installation");
    // SHA-256("abc"), independent of the current executable's implementation.
    const auto legacy_directory = sandbox.directory / L"Glint" / L"bridge" /
        L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    std::filesystem::create_directories(legacy_directory);
    const auto legacy = legacy_directory / L"GlintBridge.exe";
    { std::ofstream old(legacy, std::ios::binary); old << "abc"; }
    const auto previous = bytes(legacy);
    require(glint::bridge_installation::is_installed_bridge(legacy) && !glint::bridge_installation::is_current_bridge(legacy),
        "older content-addressed version remains identifiable without being current");
    { std::ofstream corrupt(installed, std::ios::binary | std::ios::trunc); corrupt << "modified executable"; }
    require(!glint::bridge_installation::is_current_bridge(installed) && !glint::bridge_installation::is_installed_bridge(installed),
        "hash mismatch rejects a modified installed executable");
    require(glint::bridge_installation::ensure_current(installed, error) && error.empty(), "repair damaged copy atomically");
    require(bytes(installed) == bytes(executable()), "repair restores source bytes");
    require(bytes(legacy) == previous, "installation leaves previous version directories intact");
    for (const auto& item : std::filesystem::directory_iterator(installed.parent_path()))
        require(item.path().filename() == L"GlintBridge.exe", "successful install leaves no temporary files");
}
void installation_failure_has_no_usable_result() {
    Sandbox sandbox;
    { std::ofstream blocker(sandbox.directory / L"Glint"); blocker << "not a folder"; }
    std::wstring error;
    std::filesystem::path installed = L"old result";
    require(!glint::bridge_installation::ensure_current(installed, error) && installed.empty() && !error.empty(),
        "blocked installation reports a clear error and clears output path");
}
} // namespace
int main() {
    try {
        content_addressed_installation_and_reuse();
        detects_and_repairs_corruption_without_touching_old_versions();
        installation_failure_has_no_usable_result();
        std::cout << "bridge installation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bridge installation test failed: " << error.what() << '\n';
        return 1;
    }
}
