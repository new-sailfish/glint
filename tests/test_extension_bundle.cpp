#include "extension_bundle.hpp"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::wstring environment_value(const wchar_t* name) {
    const DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
    if (!count) return {};
    std::wstring value(count, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), count);
    require(copied != 0 && copied < count, "read environment variable");
    value.resize(copied);
    return value;
}

class LocalAppDataSandbox {
public:
    LocalAppDataSandbox() {
        static std::atomic<unsigned long long> sequence = 0;
        const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(sequence.fetch_add(1));
        directory_ = std::filesystem::temp_directory_path() / (L"Glint-extension-tests-" + suffix);
        std::error_code error;
        std::filesystem::create_directories(directory_, error);
        require(!error, "create extension test directory");
        original_ = environment_value(L"LOCALAPPDATA");
        existed_ = !original_.empty() || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        require(SetEnvironmentVariableW(L"LOCALAPPDATA", directory_.c_str()) != FALSE,
                "redirect LOCALAPPDATA for extension test");
    }

    ~LocalAppDataSandbox() {
        SetEnvironmentVariableW(L"LOCALAPPDATA", existed_ ? original_.c_str() : nullptr);
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    const std::filesystem::path& directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    std::wstring original_;
    bool existed_ = false;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "open extracted extension file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void extraction_writes_stable_bundle() {
    LocalAppDataSandbox sandbox;
    std::filesystem::path directory = L"caller value is replaced";
    std::wstring error;
    require(glint::extension_bundle::ensure_extracted(directory, error), "extract embedded extension");
    require(error.empty(), "successful extraction has no error");
    require(directory == sandbox.directory() / L"Glint" / L"extension", "use stable LOCALAPPDATA path");

    const std::vector<std::wstring> names = {
        L"manifest.json", L"background.js", L"background-core.js", L"options.html", L"options.js"
    };
    for (const auto& name : names) {
        const auto path = directory / name;
        require(std::filesystem::is_regular_file(path), "extract every extension resource");
        require(!read_file(path).empty(), "embedded extension resource is non-empty");
    }

    const auto manifest = directory / L"manifest.json";
    const auto original = read_file(manifest);
    {
        std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
        output << "changed";
    }
    require(glint::extension_bundle::ensure_extracted(directory, error), "repair changed extension file");
    require(read_file(manifest) == original, "overwrite changed content");

    const auto timestamp = std::filesystem::last_write_time(manifest);
    require(glint::extension_bundle::ensure_extracted(directory, error), "reuse identical extension files");
    require(std::filesystem::last_write_time(manifest) == timestamp, "skip identical content");
}

} // namespace

int main() {
    try {
        extraction_writes_stable_bundle();
        std::cout << "extension bundle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "extension bundle test failed: " << error.what() << '\n';
        return 1;
    }
}
