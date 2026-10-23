#include "update_install.hpp"
#include <windows.h>
#include <winioctl.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace glint::update;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void write(const std::filesystem::path& path, const std::string& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc); file << data;
    require(file.good(), "write fixture");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct Fixture {
    inline static std::atomic<unsigned> sequence = 0;
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"Glint-update-install-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
    std::filesystem::path target = root / L"Glint.exe";
    Package package;
    Fixture() {
        require(std::filesystem::create_directory(root), "create sandbox");
        package.directory = root / L"0123456789abcdef0123456789abcdef";
        std::filesystem::create_directory(package.directory);
        package.executable = package.directory / L"candidate.exe";
        write(target, "old"); write(package.executable, "new");
        package.release.sha256 = "11507a0e2f5e69d5dfa40a62a1bd7b6ee57e6bcd85c67c9b8431b36fff21c437";
        package.release.size = 3;
        package.release.version = "1.2.3";
    }
    ~Fixture() {
        std::error_code ignored;
        if (root.parent_path() == std::filesystem::temp_directory_path() && root.filename().wstring().starts_with(L"Glint-update-install-test-"))
            std::filesystem::remove_all(root, ignored);
    }
    InstallTransaction prepare() { return prepare_install_transaction(target, package, package.directory.filename().wstring()); }
};
void replacement_and_confirmation() {
    Fixture f; const auto transaction = f.prepare(); int launched = 0;
    auto result = execute_install_transaction(transaction, [&](const auto& executable, bool rollback) {
        require(executable == f.target && !rollback, "launch precise updated path");
        require(read(executable) == "new", "new bytes visible at launch");
        ++launched; return LaunchOutcome::Confirmed;
    });
    if (result.outcome != InstallOutcome::Confirmed) std::wcerr << result.message << L'\n';
    require(result.outcome == InstallOutcome::Confirmed && launched == 1, "confirmed update succeeds");
    require(read(f.target) == "new", "replacement survives confirmation");
}
void launch_failure_restores_old_file() {
    Fixture f; const auto transaction = f.prepare(); int launches = 0;
    auto result = execute_install_transaction(transaction, [&](const auto& path, bool rollback) {
        ++launches; require(read(path) == (rollback ? "old" : "new"), "launch correct generation");
        return rollback ? LaunchOutcome::Confirmed : LaunchOutcome::Failed;
    });
    require(result.outcome == InstallOutcome::RolledBack && launches == 2, "failed launch rolls back and restarts");
    require(read(f.target) == "old", "old executable preserved");
}
void running_unconfirmed_process_is_preserved() {
    Fixture f; const auto transaction = f.prepare();
    auto result = execute_install_transaction(transaction, [](const auto&, bool rollback) {
        require(!rollback, "running new application must never be rolled back"); return LaunchOutcome::RunningWithoutConfirmation;
    });
    require(result.outcome == InstallOutcome::Pending && read(f.target) == "new", "timeout preserves running new version");
    require(std::filesystem::exists(f.root / L".glint-update-0123456789abcdef0123456789abcdef.backup"), "timeout preserves backup");
}
void changed_target_is_not_replaced() {
    Fixture f; const auto transaction = f.prepare(); write(f.target, "external change"); bool launched = false;
    auto result = execute_install_transaction(transaction, [&](const auto&, bool) { launched = true; return LaunchOutcome::Confirmed; });
    require(result.outcome == InstallOutcome::Failed && !launched && read(f.target) == "external change", "changed target is rejected");
}
void locked_target_is_not_replaced() {
    Fixture f; const auto transaction = f.prepare();
    HANDLE held = CreateFileW(f.target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(held != INVALID_HANDLE_VALUE, "lock target");
    auto result = execute_install_transaction(transaction, [](const auto&, bool) -> LaunchOutcome { throw std::runtime_error("locked target launched"); });
    CloseHandle(held);
    require(result.outcome == InstallOutcome::Failed && read(f.target) == "old", "locked target is preserved");
}
void duplicate_and_stale_transactions_are_rejected() {
    Fixture f; const auto transaction = f.prepare(); bool rejected = false;
    try { f.prepare(); } catch (...) { rejected = true; }
    require(rejected, "duplicate transaction must not overwrite its journal");
    auto first = execute_install_transaction(transaction, [](const auto&, bool) { return LaunchOutcome::Confirmed; });
    int launches = 0;
    auto second = execute_install_transaction(transaction, [&](const auto&, bool) { ++launches; return LaunchOutcome::Confirmed; });
    require(first.outcome == InstallOutcome::Confirmed && second.outcome == InstallOutcome::Failed && launches == 0, "stale transaction cannot reinstall");
}
void unsafe_and_foreign_recovery_paths_are_rejected() {
    Fixture f; auto package = f.package; bool rejected = false;
    try { prepare_install_transaction(f.root / L".." / f.root.filename() / L"Glint.exe", package, L"../outside"); } catch (...) { rejected = true; }
    require(rejected, "unsafe nonce and target rejected");
    const auto transaction = f.prepare();
    const auto other = f.root / L"unrelated.exe"; write(other, "unrelated");
    auto result = recover_install_transaction(other, transaction.directory);
    require(result.outcome == InstallOutcome::Failed && read(other) == "unrelated", "journal cannot redirect recovery to another target");
}
void set_state(const InstallTransaction& tx, const std::string& state) {
    auto content = read(tx.directory / L"transaction.txt");
    const auto first = content.find('\n') + 1;
    content.replace(first, content.find('\n', first) - first, state);
    write(tx.directory / L"transaction.txt", content);
}
void interrupted_replacement_restores_verified_backup() {
    Fixture f; const auto tx = f.prepare();
    set_state(tx, "replacing");
    const auto backup = f.root / (L".glint-update-" + tx.nonce + L".backup");
    require(ReplaceFileW(f.target.c_str(), f.package.executable.c_str(), backup.c_str(), 0, nullptr, nullptr) != FALSE, "simulate replacement before journal advance");
    auto result = recover_install_transaction(f.target, tx.directory);
    require(result.outcome == InstallOutcome::RolledBack && read(f.target) == "old", "interrupted replacement restores old executable");
}
void interrupted_missing_target_restores_verified_backup() {
    Fixture f; const auto tx = f.prepare(); set_state(tx, "replacing");
    const auto backup = f.root / (L".glint-update-" + tx.nonce + L".backup");
    std::filesystem::rename(f.target, backup);
    auto result = recover_install_transaction(f.target, tx.directory);
    require(result.outcome == InstallOutcome::RolledBack && read(f.target) == "old", "partial replacement restores missing target");
}
void identity_change_and_corrupt_candidate_are_rejected() {
    Fixture f; const auto tx = f.prepare();
    std::filesystem::rename(f.target, f.root / L"old.exe"); write(f.target, "old");
    auto result = execute_install_transaction(tx, [](const auto&, bool) { return LaunchOutcome::Confirmed; });
    require(result.outcome == InstallOutcome::Failed && read(f.target) == "old", "same bytes with changed file identity are rejected");
    Fixture second; const auto tx2 = second.prepare(); write(second.package.executable, "bad");
    result = execute_install_transaction(tx2, [](const auto&, bool) { return LaunchOutcome::Confirmed; });
    require(result.outcome == InstallOutcome::Failed && read(second.target) == "old", "changed staged bytes are rejected");
}
void duplicate_requests_are_serialized() {
    Fixture f; const auto tx = f.prepare(); bool tried = false;
    const auto result = execute_install_transaction(tx, [&](const auto&, bool) {
        const auto duplicate = execute_install_transaction(tx, [](const auto&, bool) { return LaunchOutcome::Confirmed; });
        require(duplicate.outcome == InstallOutcome::Failed, "in-flight update rejects duplicate execution");
        tried = true; return LaunchOutcome::Confirmed;
    });
    require(result.outcome == InstallOutcome::Confirmed && tried, "first transaction finishes normally");
}
void transient_journal_reader_does_not_abort_installation() {
    Fixture f; const auto tx = f.prepare();
    HANDLE reader = CreateFileW((tx.directory / L"transaction.txt").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(reader != INVALID_HANDLE_VALUE, "open a transient journal reader");
    std::thread release_reader([reader] { Sleep(150); CloseHandle(reader); });
    const auto result = execute_install_transaction(tx, [](const auto&, bool) { return LaunchOutcome::Confirmed; });
    release_reader.join();
    require(result.outcome == InstallOutcome::Confirmed && read(f.target) == "new", "transient journal readers do not abort replacement");
}
void installed_bytes_are_pinned_while_launch_is_confirmed() {
    Fixture f; const auto tx = f.prepare(); bool protected_bytes = false;
    const auto result = execute_install_transaction(tx, [&](const auto& path, bool rollback) {
        require(!rollback, "pinned update should not roll back");
        HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        protected_bytes = writer == INVALID_HANDLE_VALUE;
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        return LaunchOutcome::Confirmed;
    });
    require(result.outcome == InstallOutcome::Confirmed && protected_bytes, "verified installed bytes cannot change during launch");
}
void reparse_ancestor_is_rejected() {
    Fixture f;
    const auto link = f.root / L"junction";
    require(CreateDirectoryW(link.c_str(), nullptr) != FALSE, "create junction folder");
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open junction folder");
    const auto substitute = L"\\??\\" + f.root.wstring();
    const auto print = f.root.wstring();
    struct MountPoint {
        DWORD tag; WORD length; WORD reserved;
        WORD substitute_offset; WORD substitute_length; WORD print_offset; WORD print_length;
    } header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(8 + (substitute.size() + print.size() + 2) * sizeof(wchar_t)),
        0, 0, static_cast<WORD>(substitute.size() * sizeof(wchar_t)),
        static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t)), static_cast<WORD>(print.size() * sizeof(wchar_t))};
    std::vector<unsigned char> bytes(sizeof(header) + (substitute.size() + print.size() + 2) * sizeof(wchar_t), 0);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), substitute.data(), substitute.size() * sizeof(wchar_t));
    std::memcpy(bytes.data() + sizeof(header) + header.print_offset, print.data(), print.size() * sizeof(wchar_t));
    DWORD written = 0;
    const auto created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, 0, &written, nullptr);
    CloseHandle(handle); require(created != FALSE, "create test junction without symbolic-link privilege");
    bool rejected = false;
    try { prepare_install_transaction(link / L"Glint.exe", f.package, f.package.directory.filename().wstring()); } catch (...) { rejected = true; }
    require(RemoveDirectoryW(link.c_str()) != FALSE, "remove test junction");
    require(rejected && read(f.target) == "old", "target reparse ancestor is rejected");
}
}
int main() {
    try {
        replacement_and_confirmation(); launch_failure_restores_old_file(); running_unconfirmed_process_is_preserved();
        changed_target_is_not_replaced(); locked_target_is_not_replaced(); duplicate_and_stale_transactions_are_rejected();
        unsafe_and_foreign_recovery_paths_are_rejected();
        interrupted_replacement_restores_verified_backup(); interrupted_missing_target_restores_verified_backup();
        identity_change_and_corrupt_candidate_are_rejected(); duplicate_requests_are_serialized(); reparse_ancestor_is_rejected();
        transient_journal_reader_does_not_abort_installation();
        installed_bytes_are_pinned_while_launch_is_confirmed();
        std::cout << "Update installation tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
