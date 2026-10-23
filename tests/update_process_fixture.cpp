#include "update_install.hpp"
#include "update_feed.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
std::filesystem::path executable() {
    std::wstring path(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) throw std::runtime_error("fixture path unavailable");
    path.resize(count); return path;
}
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary); output << text;
    if (!output) throw std::runtime_error("fixture marker unavailable");
}
unsigned patch_version(const std::filesystem::path& path) {
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    std::vector<unsigned char> data(size);
    if (!size || !GetFileVersionInfoW(path.c_str(), 0, size, data.data())) throw std::runtime_error("fixture version missing");
    VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info || length < sizeof(*info))
        throw std::runtime_error("fixture version invalid");
    return HIWORD(info->dwFileVersionLS);
}
}

int wmain(int argc, wchar_t** argv) {
    if (const auto mode = glint::update::dispatch_update_mode(argc, argv)) return *mode;
    const auto self = executable();
    const auto folder = self.parent_path();
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--test-update-parent") {
            glint::update::Package package;
            package.executable = argv[2]; package.directory = package.executable.parent_path();
            package.release.version = "1.0.2";
            package.release.size = std::filesystem::file_size(package.executable);
            package.release.sha256 = glint::update::file_sha256(package.executable);
            std::wstring error;
            if (!glint::update::start_install(package, error)) {
                std::ofstream output(folder / L"start-error.txt", std::ios::binary);
                for (wchar_t ch : error) output << (ch < 128 ? static_cast<char>(ch) : '?');
                return 3;
            }
            write(folder / L"parent-ready.txt", "helper-ready");
            if (read(folder / L"behavior.txt") == "hold-parent") {
                Sleep(65000);
                write(folder / L"parent-wait-finished.txt", "finished");
            }
            return 0;
        }
        const auto patch = patch_version(self);
        if (patch == 2 && (!glint::update::installation_startup_result().empty() || !glint::update::failed_install_version().empty()))
            throw std::runtime_error("active update startup was reported as a previous failure");
        if (patch == 1 && (glint::update::failed_install_version() != "1.0.2" || glint::update::installation_startup_result().empty()))
            throw std::runtime_error("recovered desktop did not receive its failed update version");
        const auto behavior = read(folder / L"behavior.txt");
        if (patch == 2 && behavior == "exit-before-ack") return 7;
        if (patch == 2 && behavior == "no-ack") {
            write(folder / L"new-running.txt", "running");
            Sleep(35000);
            write(folder / L"new-exited.txt", "finished");
            return 0;
        }
        // This fixture has no windows or registry state. The marker stands in
        // for its own readiness, then uses the production inherited event.
        write(folder / (patch == 2 ? L"new-ready.txt" : L"old-ready.txt"), "ready");
        glint::update::acknowledge_update_startup();
        Sleep(1500);
        write(folder / (patch == 2 ? L"new-exited.txt" : L"old-exited.txt"), "finished");
        return 0;
    } catch (const std::exception& error) {
        try { write(folder / L"fixture-error.txt", error.what()); } catch (...) {}
        return 4;
    }
}
