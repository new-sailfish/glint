#include "update_install.hpp"
#include "update_feed.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include "language.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <fstream>
#include <vector>

namespace glint::update {
namespace {
namespace fs = std::filesystem;
class Handle {
public:
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};
std::atomic<bool> install_requested = false;
Handle startup_ack;
bool update_startup = false;
struct RunningIdentity {
    fs::path executable;
    std::string sha256;
    std::uint64_t index = 0;
    DWORD volume = 0;
};
std::optional<RunningIdentity> running_identity;
constexpr wchar_t helper_mode[] = L"--glint-update-helper";
constexpr wchar_t startup_mode[] = L"--glint-update-startup";

[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) fail("Invalid UTF-8 in update record");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::string narrow(const std::wstring& value) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) fail("Invalid update path");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring error_message(const std::exception& error) {
    try { return widen(error.what()); } catch (...) { return L"Update installation failed."; }
}
bool same_path(const fs::path& first, const fs::path& second) {
    return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
}
void safe_path(const fs::path& path, bool must_exist = true) {
    const auto value = path.wstring();
    if (!path.is_absolute() || value.size() < 4 || value[1] != L':' ||
        value[2] != L'\\' || value.find(L':', 2) != std::wstring::npos ||
        value.find_first_of(L"/\r\n\t") != std::wstring::npos || path.lexically_normal() != path)
        fail("Update location must be an absolute local path without traversal");
    for (wchar_t ch : value) if (ch < 32) fail("Invalid character in update location");
    fs::path current = path.root_path();
    for (const auto& part : path.relative_path()) {
        current /= part;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            if (!must_exist && current == path && GetLastError() == ERROR_FILE_NOT_FOUND) return;
            fail("Update location is unavailable");
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) fail("Update locations cannot contain reparse points");
    }
}
bool valid_hash(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
void validate_nonce(const std::wstring& nonce) {
    if (nonce.size() != 32 || !std::all_of(nonce.begin(), nonce.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    })) fail("Invalid update transaction identifier");
}
fs::path candidate_path(const InstallTransaction& tx) { return tx.directory / L"candidate.exe"; }
fs::path backup_path(const InstallTransaction& tx) { return tx.target.parent_path() / (L".glint-update-" + tx.nonce + L".backup"); }
fs::path stage_path(const InstallTransaction& tx) { return tx.target.parent_path() / (L".glint-update-" + tx.nonce + L".new"); }
fs::path journal_path(const InstallTransaction& tx) { return tx.directory / L"transaction.txt"; }
void validate_transaction(const InstallTransaction& tx) {
    validate_nonce(tx.nonce);
    safe_path(tx.target, false); safe_path(tx.directory);
    if (tx.directory.filename() != tx.nonce || !valid_hash(tx.previous_sha256) || !valid_hash(tx.release.sha256) ||
        tx.previous_sha256 == tx.release.sha256 || tx.release.size == 0 || tx.release.size > 128ull * 1024 * 1024)
        fail("Invalid update transaction");
    if (!std::all_of(tx.release.version.begin(), tx.release.version.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; }) ||
        tx.release.version.empty() || tx.release.version.size() > 32) fail("Invalid update version");
    if (same_path(tx.target, candidate_path(tx))) fail("Update target and candidate must differ");
}
BY_HANDLE_FILE_INFORMATION information(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        fail("Update executable identity is unavailable");
    return info;
}
std::uint64_t file_index(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}
Handle read_guard(const fs::path& path) {
    safe_path(path);
    Handle result(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!result.valid()) fail("Update file is locked or unreadable");
    information(result.value);
    return result;
}
void verify_target(const InstallTransaction& tx) {
    const auto guard = read_guard(tx.target);
    const auto info = information(guard.value);
    if (info.dwVolumeSerialNumber != tx.volume_serial || file_index(info) != tx.file_index || file_sha256(tx.target) != tx.previous_sha256)
        fail("The installed executable changed after update preparation");
}
void verify_candidate(const InstallTransaction& tx) {
    const auto path = candidate_path(tx);
    const auto guard = read_guard(path);
    if (fs::file_size(path) != tx.release.size || file_sha256(path) != tx.release.sha256)
        fail("The prepared update failed its integrity check");
}
std::string journal_content(const InstallTransaction& tx, const std::string& state) {
    return "GlintUpdate1\n" + state + "\n" + narrow(tx.target.wstring()) + "\n" + tx.previous_sha256 + "\n" +
        tx.release.sha256 + "\n" + tx.release.version + "\n" + std::to_string(tx.release.size) + "\n" +
        std::to_string(tx.file_index) + "\n" + std::to_string(tx.volume_serial) + "\n" + narrow(tx.nonce) + "\n";
}
void write_record(const InstallTransaction& tx, const std::string& state, bool initial = false) {
    safe_path(tx.directory);
    const auto journal = journal_path(tx);
    const auto path = initial ? journal : tx.directory / L"transaction.next";
    safe_path(path, false);
    if (!initial) {
        safe_path(journal);
        if (fs::exists(path)) fail("An interrupted journal write needs manual recovery");
    }
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) fail("Cannot create update transaction record");
    const auto bytes = journal_content(tx, state); DWORD written = 0;
    if (!WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() || !FlushFileBuffers(file.value))
        fail("Cannot persist update transaction record");
    CloseHandle(std::exchange(file.value, nullptr));
    if (!initial) {
        const auto deadline = GetTickCount64() + 2000;
        while (!MoveFileExW(path.c_str(), journal.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto error = GetLastError();
            if ((error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) || GetTickCount64() >= deadline)
                fail("Cannot commit update transaction record");
            Sleep(25);
        }
    }
}
std::uint64_t integer(const std::string& text) {
    std::uint64_t number = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) fail("Invalid integer in update record");
    return number;
}
std::pair<InstallTransaction, std::string> read_record(const fs::path& directory) {
    safe_path(directory); safe_path(directory / L"transaction.txt");
    if (fs::file_size(directory / L"transaction.txt") > 16384) fail("Oversized update transaction record");
    std::ifstream input(directory / L"transaction.txt", std::ios::binary);
    std::vector<std::string> fields; std::string line;
    while (std::getline(input, line)) fields.push_back(line);
    if (fields.size() != 10 || fields[0] != "GlintUpdate1") fail("Invalid update transaction record");
    InstallTransaction tx;
    tx.target = widen(fields[2]); tx.directory = directory; tx.previous_sha256 = fields[3];
    tx.release.sha256 = fields[4]; tx.release.version = fields[5]; tx.release.size = integer(fields[6]);
    tx.file_index = integer(fields[7]);
    const auto volume = integer(fields[8]);
    if (volume > UINT32_MAX) fail("Invalid volume in update record");
    tx.volume_serial = static_cast<std::uint32_t>(volume); tx.nonce = widen(fields[9]);
    validate_transaction(tx);
    const std::vector<std::string> states{"prepared", "replacing", "replaced", "launching", "confirmed", "pending", "rolled-back", "failed"};
    if (std::find(states.begin(), states.end(), fields[1]) == states.end()) fail("Invalid update transaction state");
    return {tx, fields[1]};
}
void remove_verified(const fs::path& path, const std::string& hash) {
    if (!fs::exists(path)) return;
    safe_path(path);
    if (file_sha256(path) != hash) fail("Update cleanup found a changed file; it was preserved");
    if (!DeleteFileW(path.c_str())) fail("Cannot remove a verified update temporary file");
}
Handle target_lock(const InstallTransaction& tx) {
    const auto path = tx.target.parent_path() / (L"." + tx.target.filename().wstring() + L".update-lock");
    safe_path(path, false);
    Handle lock(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!lock.valid()) fail("Another update is running, or the installation directory is not writable");
    information(lock.value);
    return lock;
}
void rollback(const InstallTransaction& tx) {
    safe_path(tx.target, false); safe_path(backup_path(tx));
    if ((fs::exists(tx.target) && file_sha256(tx.target) != tx.release.sha256) || file_sha256(backup_path(tx)) != tx.previous_sha256)
        fail("Update files changed; automatic restoration was stopped");
    const bool restored = fs::exists(tx.target)
        ? ReplaceFileW(tx.target.c_str(), backup_path(tx).c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE
        : MoveFileExW(backup_path(tx).c_str(), tx.target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!restored)
        fail("Cannot restore the previous executable; the verified backup was preserved");
    // Restoring a usable executable takes priority if the journal volume has
    // become unwritable. Recovery can still recognize the original hash.
    try { write_record(tx, "rolled-back"); } catch (...) {}
}
InstallResult execute_locked(const InstallTransaction& tx, const InstallLauncher& launch) {
    validate_transaction(tx);
    const auto [saved, state] = read_record(tx.directory);
    if (state != "prepared" || journal_content(saved, "prepared") != journal_content(tx, "prepared"))
        fail("This update transaction is stale or was changed");
    verify_candidate(tx); verify_target(tx);
    const auto candidate_guard = read_guard(candidate_path(tx));
    auto target_guard = read_guard(tx.target);
    safe_path(stage_path(tx), false); safe_path(backup_path(tx), false);
    if (fs::exists(stage_path(tx)) || fs::exists(backup_path(tx))) fail("An update temporary file already exists");
    if (!CopyFileW(candidate_path(tx).c_str(), stage_path(tx).c_str(), TRUE)) fail("Cannot stage the update in the installation directory");
    {
        Handle staged(CreateFileW(stage_path(tx).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        if (!staged.valid() || !FlushFileBuffers(staged.value)) fail("Cannot persist the staged update");
    }
    if (file_sha256(stage_path(tx)) != tx.release.sha256) fail("The staged update failed its integrity check");
    verify_target(tx);
    write_record(tx, "replacing");
    if (!ReplaceFileW(tx.target.c_str(), stage_path(tx).c_str(), backup_path(tx).c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        remove_verified(stage_path(tx), tx.release.sha256);
        fail("Cannot replace Glint; another process may be using this executable");
    }
    CloseHandle(std::exchange(target_guard.value, nullptr));
    LaunchOutcome outcome = LaunchOutcome::Failed;
    try {
        // Pin the installed name and bytes through CreateProcess and startup
        // confirmation, after the final rename and before any execution.
        Handle installed(CreateFileW(tx.target.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!installed.valid()) fail("Cannot protect the installed executable before startup");
        information(installed.value);
        if (fs::file_size(tx.target) != tx.release.size || file_sha256(tx.target) != tx.release.sha256)
            fail("The installed executable failed its integrity check");
        write_record(tx, "replaced");
        write_record(tx, "launching");
        outcome = launch(tx.target, false);
    } catch (...) {
        // No process is launched until the durable launch intent is written.
        // Production launch supervision converts process errors to outcomes.
        rollback(tx);
        launch(tx.target, true);
        return {InstallOutcome::RolledBack, i18n::text(L"The update was interrupted. The previous executable was restored.", L"更新中断，已恢复原来的程序。")};
    }
    if (outcome == LaunchOutcome::Confirmed) {
        write_record(tx, "confirmed");
        remove_verified(backup_path(tx), tx.previous_sha256);
        remove_verified(candidate_path(tx), tx.release.sha256);
        return {InstallOutcome::Confirmed, L"Glint was updated to " + widen(tx.release.version) + L"."};
    }
    if (outcome == LaunchOutcome::RunningWithoutConfirmation) {
        write_record(tx, "pending");
        return {InstallOutcome::Pending, L"The updated Glint is running but did not confirm startup. The previous executable backup was preserved."};
    }
    rollback(tx);
    const auto previous = launch(tx.target, true);
    return {InstallOutcome::RolledBack, previous == LaunchOutcome::Failed ?
        L"The update could not start. The previous executable was restored; open Glint again." :
        L"The update could not start. The previous executable was restored and restarted."};
}
fs::path current_executable() {
    std::wstring value(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!count || count >= value.size()) fail("Cannot locate the running executable");
    value.resize(count); fs::path result(value); safe_path(result); return result;
}
std::string native_version(const fs::path& executable) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
    if (!size) fail("The installed executable has no native version information");
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(executable.c_str(), 0, size, data.data())) fail("Cannot inspect the installed version");
    VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || length < sizeof(VS_FIXEDFILEINFO) || !info)
        fail("The installed version is invalid");
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
        std::to_string(HIWORD(info->dwFileVersionLS));
}
void capture_running_identity() {
    try {
        const auto path = current_executable();
        const auto guard = read_guard(path);
        const auto info = information(guard.value);
        running_identity = RunningIdentity{path, file_sha256(path), file_index(info), info.dwVolumeSerialNumber};
    } catch (...) { running_identity.reset(); }
}
fs::path update_root(bool create) {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!length || length > 32768) fail("Local application data is unavailable");
    std::wstring value(length, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), length);
    if (!copied || copied >= length) fail("Local application data is unavailable");
    value.resize(copied); fs::path path(value); safe_path(path);
    for (const auto* part : {L"Glint", L"updates"}) {
        path /= part;
        if (create && !fs::exists(path)) {
            if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) fail("Cannot create the update directory");
        }
        safe_path(path);
    }
    return path;
}
void cleanup_download(const InstallTransaction& tx) {
    // Derive only our cache name from validated release metadata; journals do
    // not supply a source path for cleanup. Unknown files are left untouched.
    try {
        const auto directory = update_root(false) / L"downloads" /
            (L"v" + widen(tx.release.version) + L"-" + widen(tx.release.sha256.substr(0, 16)));
        if (!fs::exists(directory)) return;
        safe_path(directory);
        remove_verified(directory / L"Glint.exe", tx.release.sha256);
        RemoveDirectoryW(directory.c_str()); // Only succeeds when empty.
    } catch (...) { /* A later confirmed startup can retry cleanup. */ }
}
std::wstring random_nonce() {
    unsigned char data[16];
    if (BCryptGenRandom(nullptr, data, sizeof(data), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) fail("Cannot create an update identifier");
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring value; for (auto byte : data) { value += digits[byte >> 4]; value += digits[byte & 15]; } return value;
}
std::wstring quote(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }
std::wstring handle_text(HANDLE handle) { return std::to_wstring(reinterpret_cast<std::uintptr_t>(handle)); }
HANDLE parse_handle(const wchar_t* text) {
    const auto value = integer(narrow(text));
    if (!value || value > UINTPTR_MAX) fail("Invalid inherited update handle");
    HANDLE result = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
    DWORD flags = 0;
    if (!GetHandleInformation(result, &flags) || !(flags & HANDLE_FLAG_INHERIT)) fail("Missing inherited update handle");
    return result;
}
Handle create_event() {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    Handle result(CreateEventW(&attributes, TRUE, FALSE, nullptr));
    if (!result.valid()) fail("Cannot create update handshake"); return result;
}
Handle spawn(const fs::path& executable, std::wstring arguments, const std::vector<HANDLE>& inherited) {
    safe_path(executable);
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) fail("Cannot initialize update process attributes");
    struct Cleanup { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Cleanup() { DeleteProcThreadAttributeList(value); } } cleanup{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        const_cast<HANDLE*>(inherited.data()), inherited.size() * sizeof(HANDLE), nullptr, nullptr)) fail("Cannot restrict inherited update handles");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    auto command = quote(executable) + L" " + arguments;
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process))
        fail("Cannot start the update process");
    CloseHandle(process.hThread); return Handle(process.hProcess);
}
LaunchOutcome launch_desktop(const fs::path& executable, bool rollback_launch) {
    try {
        auto ack = create_event();
        // A restored desktop starts normally so its controller can see the
        // failed version and suppress background retries of that update.
        auto process = spawn(executable, rollback_launch ? L"" : std::wstring(startup_mode) + L" " + handle_text(ack.value), {ack.value});
        if (rollback_launch) return LaunchOutcome::Confirmed;
        HANDLE handles[]{ack.value, process.value};
        const auto wait = WaitForMultipleObjects(2, handles, FALSE, 30000);
        if (wait == WAIT_OBJECT_0 && WaitForSingleObject(process.value, 0) != WAIT_OBJECT_0) return LaunchOutcome::Confirmed;
        if (wait == WAIT_OBJECT_0 + 1 || WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) return LaunchOutcome::Exited;
        return LaunchOutcome::RunningWithoutConfirmation;
    } catch (...) { return LaunchOutcome::Failed; }
}
int run_helper(int argc, wchar_t** argv) {
    if (argc != 6) return 2;
    std::optional<InstallTransaction> active;
    bool parent_exited = false;
    try {
        const fs::path directory(argv[2]);
        const auto [tx, state] = read_record(directory);
        if (state != "prepared" || !same_path(directory.parent_path(), update_root(false)) ||
            !same_path(current_executable(), directory / L"helper.exe") || file_sha256(current_executable()) != tx.previous_sha256)
            fail("The update helper location or identity is invalid");
        active = tx;
        if (!feed::newer_version(tx.release.version, native_version(current_executable()))) fail("The prepared update is not newer than the installed version");
        Handle parent(parse_handle(argv[3])); Handle ready(parse_handle(argv[4])); Handle proceed(parse_handle(argv[5]));
        std::wstring parent_image(32768, L'\0'); DWORD length = static_cast<DWORD>(parent_image.size());
        if (!QueryFullProcessImageNameW(parent.value, 0, parent_image.data(), &length)) fail("The original desktop process is unavailable");
        parent_image.resize(length);
        if (!same_path(fs::path(parent_image), tx.target)) fail("The update parent does not match the installed executable");
        auto lock = target_lock(tx);
        verify_target(tx); verify_candidate(tx); verify_binary(candidate_path(tx), tx.release);
        if (!SetEvent(ready.value)) fail("Cannot acknowledge update helper readiness");
        if (WaitForSingleObject(proceed.value, 15000) != WAIT_OBJECT_0 || WaitForSingleObject(parent.value, 60000) != WAIT_OBJECT_0) {
            write_record(tx, "failed"); return 3;
        }
        parent_exited = true;
        verify_binary(candidate_path(tx), tx.release);
        const auto result = execute_locked(tx, launch_desktop);
        if (result.outcome == InstallOutcome::Confirmed) cleanup_download(tx);
        return result.outcome == InstallOutcome::Confirmed ? 0 : 1;
    } catch (...) {
        if (active) {
            try {
                const auto [saved, state] = read_record(active->directory);
                if (state == "prepared" || (state == "replacing" && fs::exists(saved.target) && file_sha256(saved.target) == saved.previous_sha256))
                    write_record(saved, "failed");
            } catch (...) {}
            if (parent_exited) {
                try {
                    // Replacement can fail after the original desktop exited,
                    // for example because a separate bridge still holds it.
                    // Restart only the exact verified original bytes.
                    if (!fs::exists(active->target)) rollback(*active);
                    safe_path(active->target);
                    if (file_sha256(active->target) == active->previous_sha256) launch_desktop(active->target, true);
                } catch (...) {}
            }
        }
        return 1;
    }
}
std::optional<std::pair<InstallTransaction, std::string>> latest_transaction() {
    const auto target = current_executable();
    const auto hash = file_sha256(target);
    const auto root = update_root(false);
    std::optional<std::pair<InstallTransaction, std::string>> result;
    fs::file_time_type latest = fs::file_time_type::min();
    size_t inspected = 0;
    for (const auto& item : fs::directory_iterator(root)) {
        if (++inspected > 256) break;
        try {
            validate_nonce(item.path().filename().wstring());
            auto record = read_record(item.path());
            const auto& tx = record.first;
            if (!same_path(tx.target, target) || (hash != tx.previous_sha256 && hash != tx.release.sha256)) continue;
            const auto time = fs::last_write_time(journal_path(tx));
            if (time <= latest) continue;
            latest = time; result = std::move(record);
        } catch (...) { /* Unknown or untrusted records are never modified. */ }
    }
    return result;
}
} // namespace

InstallTransaction prepare_install_transaction(const fs::path& target, const Package& package, const std::wstring& nonce) {
    validate_nonce(nonce); safe_path(target); safe_path(package.directory); safe_path(package.executable);
    if (package.directory.filename() != nonce || !same_path(package.executable, package.directory / L"candidate.exe"))
        fail("Invalid prepared update location");
    auto target_guard = read_guard(target);
    const auto info = information(target_guard.value);
    InstallTransaction tx{target, package.directory, package.release, file_sha256(target), nonce, file_index(info), info.dwVolumeSerialNumber};
    validate_transaction(tx); verify_candidate(tx);
    write_record(tx, "prepared", true);
    return tx;
}
InstallResult execute_install_transaction(const InstallTransaction& tx, const InstallLauncher& launch) {
    try { validate_transaction(tx); auto lock = target_lock(tx); return execute_locked(tx, launch); }
    catch (const std::exception& error) { return {InstallOutcome::Failed, error_message(error)}; }
}
InstallResult recover_install_transaction(const fs::path& target, const fs::path& directory) {
    try {
        safe_path(target, false);
        const auto [tx, state] = read_record(directory);
        if (!same_path(tx.target, target)) fail("The recovery record belongs to a different installation");
        auto lock = target_lock(tx);
        if (state == "confirmed") {
            if (file_sha256(target) != tx.release.sha256) fail("The updated installation has changed");
            remove_verified(backup_path(tx), tx.previous_sha256);
            return {InstallOutcome::Confirmed, L"Glint was updated to " + widen(tx.release.version) + L"."};
        }
        if (state == "replaced" || state == "replacing") {
            if (fs::exists(target) && file_sha256(target) == tx.previous_sha256)
                return {InstallOutcome::RolledBack, i18n::text(L"The interrupted update left the previous executable intact.", L"更新中断，原来的程序保持完好。")};
            rollback(tx);
            return {InstallOutcome::RolledBack, L"An interrupted update was restored to the previous executable."};
        }
        if (state == "launching" || state == "pending")
            return {InstallOutcome::Pending, L"An earlier update did not confirm startup. Its backup was preserved; close Glint before manual recovery."};
        if (state == "rolled-back") return {InstallOutcome::RolledBack, L"The previous update failed and the old executable was restored."};
        return {InstallOutcome::Failed, L"An earlier update was interrupted before installation. Check for updates to try again."};
    } catch (const std::exception& error) { return {InstallOutcome::Failed, error_message(error)}; }
}
bool start_install(const Package& package, std::wstring& error) {
    error.clear();
    if (install_requested.exchange(true)) { error = L"An update installation is already in progress."; return false; }
    try {
        const auto target = current_executable();
        if (!running_identity || !same_path(target, running_identity->executable)) fail("The running executable identity was not captured at startup");
        auto target_guard = read_guard(target);
        const auto info = information(target_guard.value);
        if (info.dwVolumeSerialNumber != running_identity->volume || file_index(info) != running_identity->index ||
            file_sha256(target) != running_identity->sha256) fail("The installed executable changed since Glint started");
        if (!feed::newer_version(package.release.version, native_version(target))) fail("The prepared update is not newer than the installed version");
        safe_path(package.directory); safe_path(package.executable);
        if (!same_path(package.executable.parent_path(), package.directory)) fail("The prepared update is outside its download directory");
        auto download_guard = read_guard(package.executable);
        verify_binary(package.executable, package.release);
        const auto directory = update_root(true) / random_nonce();
        if (!CreateDirectoryW(directory.c_str(), nullptr)) fail("Cannot create the installation transaction directory");
        Package prepared{package.release, directory / L"candidate.exe", directory};
        if (!CopyFileW(package.executable.c_str(), prepared.executable.c_str(), TRUE)) fail("Cannot prepare the update executable");
        const auto tx = prepare_install_transaction(target, prepared, directory.filename().wstring());
        if (!CopyFileW(target.c_str(), (directory / L"helper.exe").c_str(), TRUE) || file_sha256(directory / L"helper.exe") != tx.previous_sha256)
            fail("Cannot prepare a trusted update helper");
        verify_binary(prepared.executable, prepared.release);
        auto ready = create_event(); auto proceed = create_event();
        Handle parent;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent.value,
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) fail("Cannot capture the original desktop process");
        const auto arguments = std::wstring(helper_mode) + L" " + quote(directory) + L" " + handle_text(parent.value) + L" " +
            handle_text(ready.value) + L" " + handle_text(proceed.value);
        auto helper = spawn(directory / L"helper.exe", arguments, {parent.value, ready.value, proceed.value});
        HANDLE handles[]{ready.value, helper.value};
        if (WaitForMultipleObjects(2, handles, FALSE, 15000) != WAIT_OBJECT_0 || WaitForSingleObject(helper.value, 0) == WAIT_OBJECT_0)
            fail("The update helper could not prepare the installation. Glint will remain open.");
        if (!SetEvent(proceed.value)) fail("Cannot authorize the prepared update helper");
        return true;
    } catch (const std::exception& exception) {
        install_requested = false; error = error_message(exception); return false;
    }
}
std::optional<int> dispatch_update_mode(int argc, wchar_t** argv) {
    if (argc > 1 && argv && argv[1]) {
        if (std::wstring(argv[1]) == helper_mode) return run_helper(argc, argv);
        if (std::wstring(argv[1]) == startup_mode) {
            if (argc != 3) return 2;
            try { startup_ack.value = parse_handle(argv[2]); update_startup = true; }
            catch (...) { return 2; }
        }
    }
    capture_running_identity();
    return std::nullopt;
}
void acknowledge_update_startup() {
    if (startup_ack.valid()) {
        SetEvent(startup_ack.value);
        CloseHandle(std::exchange(startup_ack.value, nullptr));
    }
}
std::wstring installation_startup_result() {
    // The helper owns this startup's result until it receives the inherited
    // acknowledgement. Its transient 'launching' state is not a prior error.
    if (update_startup) return {};
    try {
        const auto record = latest_transaction();
        if (!record) return {};
        const auto& [tx, state] = *record;
        if (state == "confirmed") {
            // The helper cannot delete its own mapped image. A later ordinary
            // startup removes only these derived files after rechecking hashes.
            if (file_sha256(tx.target) == tx.release.sha256) {
                remove_verified(tx.directory / L"helper.exe", tx.previous_sha256);
                remove_verified(candidate_path(tx), tx.release.sha256);
                remove_verified(backup_path(tx), tx.previous_sha256);
                cleanup_download(tx);
            }
            return {}; // Do not repeat a success notification on every launch.
        }
        if (state == "rolled-back")
            return i18n::text(L"The previous update failed. The old executable was restored.", L"上次更新失败，已恢复原来的程序。");
        if (state == "pending" || state == "launching" || state == "replaced" || state == "replacing")
            return i18n::text(L"The previous update did not confirm startup. Its backup was preserved in the installation directory.", L"上次更新未确认启动成功，安装目录中的原程序备份已保留。");
        if (state == "failed" || state == "prepared")
            return i18n::text(L"The previous update did not finish. Check for updates to retry manually.", L"上次更新未完成，请手动检查更新后重试。");
        return {};
    } catch (...) { return {}; }
}
std::string failed_install_version() {
    if (update_startup) return {};
    try {
        const auto record = latest_transaction();
        if (!record || record->second == "confirmed") return {};
        return record->first.release.version;
    } catch (...) { return {}; }
}
} // namespace glint::update
