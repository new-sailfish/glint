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
    require(directory == sandbox.directory() / L"Glint" / L"browser-extension", "use stable public-key bundle directory");

    const std::vector<std::wstring> names = {
        L"manifest.json", L"background.js", L"background-core.js", L"options.html", L"options.js",
        L"icon-16.png", L"icon-32.png", L"icon-48.png", L"icon-128.png"
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

void extraction_keeps_legacy_identity_files_untouched() {
    LocalAppDataSandbox sandbox;
    const auto legacy_directory = sandbox.directory() / L"Glint" / L"extension";
    std::filesystem::create_directories(legacy_directory);
    const auto legacy_manifest = legacy_directory / L"manifest.json";
    const std::string legacy = R"({"manifest_version":3,"name":"Legacy Glint","version":"0.0.1"})";
    { std::ofstream output(legacy_manifest, std::ios::binary); output << legacy; }
    const auto timestamp = std::filesystem::last_write_time(legacy_manifest);
    std::filesystem::path directory;
    std::wstring error;
    require(glint::extension_bundle::ensure_extracted(directory, error), "extract fixed-key bundle beside legacy installation");
    require(directory != legacy_directory, "avoid Chrome invalid-path records from legacy identity");
    require(read_file(legacy_manifest) == legacy, "never replace the legacy manifest with a different identity");
    require(std::filesystem::last_write_time(legacy_manifest) == timestamp, "leave legacy files unchanged");
    require(read_file(directory / L"manifest.json").find("\"key\"") != std::string::npos, "new directory always has bundled public key");
}

void refresh_preserves_a_locked_resource_and_succeeds_after_release() {
    LocalAppDataSandbox sandbox;
    std::filesystem::path directory;
    std::wstring error;
    require(glint::extension_bundle::ensure_extracted(directory, error), "prepare current resource bytes");
    const std::vector<std::wstring> names = {
        L"manifest.json", L"background.js", L"background-core.js", L"options.html", L"options.js",
        L"icon-16.png", L"icon-32.png", L"icon-48.png", L"icon-128.png"
    };
    std::vector<std::string> current;
    for (const auto& name : names) {
        current.push_back(read_file(directory / name));
        std::ofstream old(directory / name, std::ios::binary | std::ios::trunc);
        old << (name == L"manifest.json" ? "{\"manifest_version\":3,\"version\":\"0.1.1\"}" : "old extension content");
    }
    HANDLE reader = CreateFileW((directory / L"background-core.js").c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(reader != INVALID_HANDLE_VALUE, "simulate browser holding an old resource without write sharing");
    const bool refreshed = glint::extension_bundle::ensure_extracted(directory, error);
    char retained[64]{};
    DWORD count = 0;
    const bool read_old = ReadFile(reader, retained, static_cast<DWORD>(sizeof(retained)), &count, nullptr) != FALSE;
    CloseHandle(reader);
    // MoveFileEx cannot replace an open destination, including when the reader
    // shares deletion. A failed update must retain the old resource for that
    // reader and leave a clear error; releasing it allows a clean retry.
    require(!refreshed && !error.empty(), "locked resource reports update failure");
    require(read_old && std::string(retained, count) == "old extension content", "existing reader still sees a complete old file");
    require(read_file(directory / L"background-core.js") == "old extension content",
        "failed replacement preserves the complete previous resource");
    for (const auto& item : std::filesystem::directory_iterator(directory))
        require(item.path().filename().wstring().find(L".tmp-") == std::wstring::npos,
            "failed refresh removes temporary resources");
    require(glint::extension_bundle::ensure_extracted(directory, error) && error.empty(),
        "refresh succeeds after the reader releases the old resource");
    for (std::size_t i = 0; i < names.size(); ++i)
        require(read_file(directory / names[i]) == current[i], "retry updates every extension resource including icons");
    for (const auto& item : std::filesystem::directory_iterator(directory))
        require(item.path().filename().wstring().find(L".tmp-") == std::wstring::npos,
            "successful retry leaves no temporary resources behind");
}

} // namespace

int main() {
    try {
        extraction_keeps_legacy_identity_files_untouched();
        extraction_writes_stable_bundle();
        refresh_preserves_a_locked_resource_and_succeeds_after_release();
        std::cout << "extension bundle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "extension bundle test failed: " << error.what() << '\n';
        return 1;
    }
}
