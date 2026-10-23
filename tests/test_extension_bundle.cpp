#include "extension_bundle.hpp"

#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
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

void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "open extension test fixture for writing");
    output << contents;
    require(output.good(), "write extension test fixture");
}

void require_no_temporary_resources(const std::filesystem::path& directory) {
    for (const auto& item : std::filesystem::recursive_directory_iterator(directory)) {
        const auto name = item.path().filename().wstring();
        require(name.find(L".tmp-") == std::wstring::npos && !name.starts_with(L".glint-refresh-"),
            "extension updates leave no temporary resources or directory guards");
    }
}

bool set_directory_junction(HANDLE handle, const std::filesystem::path& target) {
    const auto substitute = L"\\??\\" + target.wstring();
    const auto printable = target.wstring();
    struct JunctionData {
        DWORD tag;
        WORD length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
        wchar_t paths[4096];
    } data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = data.substitute_length + sizeof(wchar_t);
    data.print_length = static_cast<WORD>(printable.size() * sizeof(wchar_t));
    data.length = 8 + data.print_offset + data.print_length + sizeof(wchar_t);
    std::memcpy(data.paths, substitute.c_str(), data.substitute_length + sizeof(wchar_t));
    std::memcpy(reinterpret_cast<char*>(data.paths) + data.print_offset,
        printable.c_str(), data.print_length + sizeof(wchar_t));
    DWORD returned = 0;
    return DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.length + 8,
        nullptr, 0, &returned, nullptr) != FALSE;
}

class DirectoryJunction {
public:
    DirectoryJunction(const std::filesystem::path& link, const std::filesystem::path& target) : link_(link) {
        std::filesystem::create_directories(link);
        HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "open test junction");
        const bool created = set_directory_junction(handle, target);
        CloseHandle(handle);
        require(created, "create test junction without symlink privilege");
    }

    ~DirectoryJunction() { RemoveDirectoryW(link_.c_str()); }

private:
    std::filesystem::path link_;
};

class FileOplock {
public:
    explicit FileOplock(const std::filesystem::path& path) {
        completion_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(completion_.hEvent != nullptr, "create oplock event");
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        require(handle_ != INVALID_HANDLE_VALUE, "open runtime resource for oplock");
        DWORD returned = 0;
        const bool immediate = DeviceIoControl(handle_, FSCTL_REQUEST_OPLOCK_LEVEL_1,
            nullptr, 0, nullptr, 0, &returned, &completion_) != FALSE;
        require(!immediate && GetLastError() == ERROR_IO_PENDING, "hold exclusive resource oplock");
    }

    ~FileOplock() { release(); CloseHandle(completion_.hEvent); }
    bool wait_for_reader() const { return WaitForSingleObject(completion_.hEvent, 10000) == WAIT_OBJECT_0; }
    void release() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED completion_{};
};

void guarded_directories_cannot_be_converted_to_junctions_during_refresh() {
    LocalAppDataSandbox sandbox;
    std::filesystem::path directory;
    std::wstring error;
    require(glint::extension_bundle::ensure_extracted(directory, error), "prepare concurrent mutation fixture");
    const auto locale = directory / L"_locales/en";
    std::filesystem::remove(locale / L"messages.json");
    const auto outside = sandbox.directory() / L"outside";
    std::filesystem::create_directories(outside);

    // Attribute-only preflight opens do not break this oplock. The first
    // content read pauses after all runtime paths have been checked and held.
    FileOplock reader_pause(directory / L"manifest.json");
    bool refreshed = false;
    bool changed = false;
    std::thread refresh([&] {
        refreshed = glint::extension_bundle::ensure_extracted(directory, error, &changed);
    });
    const bool paused = reader_pause.wait_for_reader();
    HANDLE writer = CreateFileW(locale.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    const DWORD open_error = GetLastError();
    const bool mutated = writer != INVALID_HANDLE_VALUE && set_directory_junction(writer, outside);
    const DWORD mutation_error = GetLastError();
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    reader_pause.release();
    refresh.join();
    if (mutated) RemoveDirectoryW(locale.c_str());

    require(paused, "pause a real extraction after path preflight");
    require(!mutated, "checked directory refuses concurrent conversion into a junction");
    require((writer == INVALID_HANDLE_VALUE && open_error == ERROR_SHARING_VIOLATION) ||
        (writer != INVALID_HANDLE_VALUE && mutation_error == ERROR_DIR_NOT_EMPTY),
        "directory guard prevents reparse mutation while permitting atomic child renames");
    require(refreshed && changed && error.empty(), "normal locale file creation still succeeds with guarded directories");
    require(std::filesystem::is_empty(outside), "concurrent mutation cannot redirect writes outside the bundle");
    require_no_temporary_resources(directory);
}

void existing_directory_writer_prevents_refresh_before_modifications() {
    LocalAppDataSandbox sandbox;
    std::filesystem::path directory;
    std::wstring error;
    require(glint::extension_bundle::ensure_extracted(directory, error), "prepare existing directory writer fixture");
    write_file(directory / L"background.js", "old worker");
    HANDLE writer = CreateFileW((directory / L"_locales/en").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(writer != INVALID_HANDLE_VALUE, "hold directory mutation access before preflight");
    bool changed = true;
    const bool refreshed = glint::extension_bundle::ensure_extracted(directory, error, &changed);
    CloseHandle(writer);
    require(!refreshed && !changed && !error.empty(), "preflight refuses existing directory mutation access");
    require(read_file(directory / L"background.js") == "old worker", "failed preflight leaves runtime untouched");
    require_no_temporary_resources(directory);
}

void extraction_refuses_reparse_ancestors_before_writing() {
    LocalAppDataSandbox sandbox;
    const auto outside = sandbox.directory() / L"outside";
    std::filesystem::create_directories(outside);
    DirectoryJunction junction(sandbox.directory() / L"Glint", outside);
    std::filesystem::path directory;
    std::wstring error;
    require(!glint::extension_bundle::ensure_extracted(directory, error),
        "reject a managed bundle reached through a junction");
    require(!error.empty(), "unsafe managed path reports an error");
    require(std::filesystem::is_empty(outside), "junction target stays untouched");
}

void extraction_reports_only_changes_to_an_existing_runtime() {
    LocalAppDataSandbox sandbox;
    std::filesystem::path directory;
    std::wstring error;
    bool changed = true;
    std::filesystem::create_directories(sandbox.directory() / L"Glint" / L"browser-extension");
    require(glint::extension_bundle::ensure_extracted(directory, error, &changed), "extract first runtime");
    require(!changed, "first extraction into an empty folder does not request a browser reload");
    changed = true;
    require(glint::extension_bundle::ensure_extracted(directory, error, &changed), "check unchanged runtime");
    require(!changed, "unchanged runtime does not request a browser reload");
    write_file(directory / L"background.js", "older worker");
    require(glint::extension_bundle::ensure_extracted(directory, error, &changed), "refresh older worker");
    require(changed, "updated existing runtime requests a browser reload");
    std::filesystem::remove(directory / L"_locales/en/messages.json");
    changed = false;
    require(glint::extension_bundle::ensure_extracted(directory, error, &changed), "add new runtime resource");
    require(changed, "adding a runtime file to an existing bundle requests reload");
}

std::filesystem::path adjacent_bundle(LocalAppDataSandbox& sandbox) {
    std::filesystem::path managed;
    std::wstring error;
    require(glint::extension_bundle::ensure_extracted(managed, error), "prepare adjacent bundle source");
    const auto adjacent = sandbox.directory() / L"application" / L"browser-extension";
    std::filesystem::create_directories(adjacent);
    std::filesystem::copy(managed, adjacent, std::filesystem::copy_options::recursive);
    return adjacent;
}

void adjacent_sync_updates_old_recognized_bundle_in_place() {
    LocalAppDataSandbox sandbox;
    const auto adjacent = adjacent_bundle(sandbox);
    const auto original_manifest = read_file(adjacent / L"manifest.json");
    const auto original_worker = read_file(adjacent / L"background.js");
    auto old_manifest = original_manifest;
    const auto name = old_manifest.find("__MSG_extensionName__");
    require(name != std::string::npos, "embedded manifest has localized name");
    old_manifest.replace(name, std::string("__MSG_extensionName__").size(), "Glint Tab Search Connector");
    const auto version = old_manifest.find("\"version\"");
    const auto version_start = old_manifest.find('"', old_manifest.find(':', version)) + 1;
    old_manifest.replace(version_start, old_manifest.find('"', version_start) - version_start, "0.1.1");
    write_file(adjacent / L"manifest.json", old_manifest);
    write_file(adjacent / L"background.js", "old worker");
    std::filesystem::remove_all(adjacent / L"_locales");
    write_file(adjacent / L"user-notes.txt", "retain unrelated file");
    bool changed = false;
    std::wstring error;
    require(glint::extension_bundle::sync_adjacent(adjacent.parent_path(), changed, error),
        "synchronize recognized old adjacent bundle");
    require(changed && error.empty(), "adjacent upgrade reports reload needed");
    require(read_file(adjacent / L"manifest.json") == original_manifest, "update manifest in its original directory");
    require(read_file(adjacent / L"background.js") == original_worker, "update adjacent worker");
    require(std::filesystem::is_regular_file(adjacent / L"_locales/en/messages.json"), "add newly introduced locale files");
    const auto managed = sandbox.directory() / L"Glint" / L"browser-extension";
    for (const auto& item : std::filesystem::recursive_directory_iterator(managed)) {
        if (item.is_regular_file()) require(read_file(item.path()) == read_file(adjacent / item.path().lexically_relative(managed)),
            "adjacent runtime matches every embedded file");
    }
    require(read_file(adjacent / L"user-notes.txt") == "retain unrelated file", "preserve unrelated adjacent files");
    const auto timestamp = std::filesystem::last_write_time(adjacent / L"manifest.json");
    require(glint::extension_bundle::sync_adjacent(adjacent.parent_path(), changed, error), "reuse current adjacent bundle");
    require(!changed && error.empty(), "unchanged adjacent bundle does not request reload");
    require(std::filesystem::last_write_time(adjacent / L"manifest.json") == timestamp, "unchanged adjacent files retain timestamps");
    require_no_temporary_resources(adjacent);
}

void adjacent_sync_skips_missing_foreign_and_incomplete_bundles() {
    LocalAppDataSandbox sandbox;
    bool changed = true;
    std::wstring error = L"old error";
    require(glint::extension_bundle::sync_adjacent(sandbox.directory(), changed, error), "missing adjacent directory is harmless");
    require(!changed && error.empty(), "skipped missing bundle clears outputs");
    require(!std::filesystem::exists(sandbox.directory() / L"browser-extension"), "do not create an adjacent bundle");
    const auto adjacent = adjacent_bundle(sandbox);
    const auto manifest = read_file(adjacent / L"manifest.json");
    const auto worker = read_file(adjacent / L"background.js");
    std::vector<std::string> foreign_manifests = {"invalid JSON", "{}", manifest, manifest, manifest};
    const auto key = foreign_manifests[2].find("MIIB");
    require(key != std::string::npos, "fixture has fixed key");
    foreign_manifests[2].replace(key, 4, "AAAA");
    const auto worker_name = foreign_manifests[3].find("background.js");
    foreign_manifests[3].replace(worker_name, std::string("background.js").size(), "foreign.js");
    foreign_manifests[4] += " trailing data";
    for (const auto& foreign : foreign_manifests) {
        write_file(adjacent / L"manifest.json", foreign);
        write_file(adjacent / L"background.js", "foreign content");
        changed = true;
        require(glint::extension_bundle::sync_adjacent(adjacent.parent_path(), changed, error), "unrecognized bundle is skipped");
        require(!changed && error.empty(), "unrecognized bundle is not an update failure");
        require(read_file(adjacent / L"manifest.json") == foreign, "leave unrecognized manifest untouched");
        require(read_file(adjacent / L"background.js") == "foreign content", "leave unrecognized runtime untouched");
    }
    write_file(adjacent / L"manifest.json", manifest);
    write_file(adjacent / L"background.js", worker);
    std::filesystem::remove(adjacent / L"background-core.js");
    require(glint::extension_bundle::sync_adjacent(adjacent.parent_path(), changed, error), "incomplete adjacent directory is skipped");
    require(!changed && !std::filesystem::exists(adjacent / L"background-core.js"), "missing expected structure is not adopted");
    std::filesystem::create_directory(adjacent / L"background-core.js");
    require(glint::extension_bundle::sync_adjacent(adjacent.parent_path(), changed, error), "unexpected adjacent file types are skipped");
    require(!changed && error.empty() && std::filesystem::is_directory(adjacent / L"background-core.js"),
        "an unrecognized directory layout remains untouched");
}

void nested_reparse_paths_are_rejected_before_any_refresh() {
    LocalAppDataSandbox sandbox;
    const auto adjacent = adjacent_bundle(sandbox);
    const auto outside = sandbox.directory() / L"outside-locales";
    std::filesystem::create_directories(outside);
    const auto managed = sandbox.directory() / L"Glint" / L"browser-extension";
    for (const auto& bundle : {managed, adjacent}) {
        for (const auto* relative : {L"_locales/en", L"_locales/en/messages.json", L"background-core.js"}) {
            std::filesystem::remove_all(bundle / relative);
            DirectoryJunction junction(bundle / relative, outside);
            write_file(bundle / L"background.js", "keep this old worker until preflight succeeds");
            bool changed = true;
            std::wstring error;
            auto result_directory = managed;
            const bool ok = bundle == managed
                ? glint::extension_bundle::ensure_extracted(result_directory, error, &changed)
                : glint::extension_bundle::sync_adjacent(adjacent.parent_path(), changed, error);
            require(!ok && !error.empty() && !changed, "reject nested runtime junction before any write");
            require(read_file(bundle / L"background.js") == "keep this old worker until preflight succeeds",
                "preflight prevents partially refreshing an unsafe destination");
            require(std::filesystem::is_empty(outside), "nested junction target remains untouched");
        }
    }
}

void adjacent_reparse_ancestors_are_rejected() {
    LocalAppDataSandbox sandbox;
    const auto adjacent = adjacent_bundle(sandbox);
    DirectoryJunction junction(sandbox.directory() / L"linked-application", adjacent.parent_path());
    write_file(adjacent / L"background.js", "old worker");
    bool changed = true;
    std::wstring error;
    require(!glint::extension_bundle::sync_adjacent(sandbox.directory() / L"linked-application", changed, error),
        "reject adjacent bundle reached through ancestor junction");
    require(!changed && !error.empty(), "unsafe adjacent path is reported without modifications");
    require(read_file(adjacent / L"background.js") == "old worker", "ancestor junction target is unchanged");
}

void adjacent_sync_reports_partial_changes_when_a_resource_is_locked() {
    LocalAppDataSandbox sandbox;
    const auto adjacent = adjacent_bundle(sandbox);
    const auto worker = read_file(adjacent / L"background.js");
    write_file(adjacent / L"background.js", "old worker");
    write_file(adjacent / L"background-core.js", "locked old core");
    HANDLE reader = CreateFileW((adjacent / L"background-core.js").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(reader != INVALID_HANDLE_VALUE, "lock adjacent runtime resource");
    bool changed = false;
    std::wstring error;
    const bool ok = glint::extension_bundle::sync_adjacent(adjacent.parent_path(), changed, error);
    CloseHandle(reader);
    require(!ok && changed && !error.empty(), "partial update reports both changes and failure");
    require(read_file(adjacent / L"background.js") == worker, "earlier successful replacements are reported");
    require(read_file(adjacent / L"background-core.js") == "locked old core", "locked destination remains complete");
    require(glint::extension_bundle::sync_adjacent(adjacent.parent_path(), changed, error) && changed && error.empty(),
        "adjacent update succeeds after lock release");
    write_file(adjacent / L"background-core.js", "locked again");
    reader = CreateFileW((adjacent / L"background-core.js").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(reader != INVALID_HANDLE_VALUE, "lock the only changed adjacent resource");
    const bool no_changes = glint::extension_bundle::sync_adjacent(adjacent.parent_path(), changed, error);
    CloseHandle(reader);
    require(!no_changes && !changed && !error.empty(), "a failed first replacement does not claim runtime changes");
    require_no_temporary_resources(adjacent);
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
        L"icon-16.png", L"icon-32.png", L"icon-48.png", L"icon-128.png",
        L"_locales/en/messages.json", L"_locales/zh_CN/messages.json", L"_locales/zh_TW/messages.json"
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
        L"icon-16.png", L"icon-32.png", L"icon-48.png", L"icon-128.png",
        L"_locales/en/messages.json", L"_locales/zh_CN/messages.json", L"_locales/zh_TW/messages.json"
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
    bool changed = false;
    const bool refreshed = glint::extension_bundle::ensure_extracted(directory, error, &changed);
    char retained[64]{};
    DWORD count = 0;
    const bool read_old = ReadFile(reader, retained, static_cast<DWORD>(sizeof(retained)), &count, nullptr) != FALSE;
    CloseHandle(reader);
    // MoveFileEx cannot replace an open destination, including when the reader
    // shares deletion. A failed update must retain the old resource for that
    // reader and leave a clear error; releasing it allows a clean retry.
    require(!refreshed && !error.empty(), "locked resource reports update failure");
    require(changed, "managed extraction reports successful replacements before a later locked resource");
    require(read_old && std::string(retained, count) == "old extension content", "existing reader still sees a complete old file");
    require(read_file(directory / L"background-core.js") == "old extension content",
        "failed replacement preserves the complete previous resource");
    require_no_temporary_resources(directory);
    require(glint::extension_bundle::ensure_extracted(directory, error) && error.empty(),
        "refresh succeeds after the reader releases the old resource");
    for (std::size_t i = 0; i < names.size(); ++i)
        require(read_file(directory / names[i]) == current[i], "retry updates every extension resource including icons");
    require_no_temporary_resources(directory);
}

} // namespace

int main() {
    try {
        guarded_directories_cannot_be_converted_to_junctions_during_refresh();
        existing_directory_writer_prevents_refresh_before_modifications();
        extraction_refuses_reparse_ancestors_before_writing();
        extraction_reports_only_changes_to_an_existing_runtime();
        adjacent_sync_updates_old_recognized_bundle_in_place();
        adjacent_sync_skips_missing_foreign_and_incomplete_bundles();
        nested_reparse_paths_are_rejected_before_any_refresh();
        adjacent_reparse_ancestors_are_rejected();
        adjacent_sync_reports_partial_changes_when_a_resource_is_locked();
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
