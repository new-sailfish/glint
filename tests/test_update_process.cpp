#include "update_feed.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {
namespace fs = std::filesystem;
void require(bool result, const std::string& message) { if (!result) throw std::runtime_error(message); }
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary); output << text; require(output.good(), "write fixture");
}
std::vector<wchar_t> environment(const fs::path& localappdata) {
    wchar_t* block = GetEnvironmentStringsW(); require(block != nullptr, "read child environment");
    std::vector<std::wstring> entries;
    for (auto* value = block; *value; value += wcslen(value) + 1) {
        if (_wcsnicmp(value, L"LOCALAPPDATA=", 13) != 0) entries.emplace_back(value);
    }
    FreeEnvironmentStringsW(block);
    entries.emplace_back(L"LOCALAPPDATA=" + localappdata.wstring());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> result;
    for (const auto& entry : entries) { result.insert(result.end(), entry.begin(), entry.end()); result.push_back(0); }
    result.push_back(0); return result;
}
struct Fixture {
    inline static std::atomic<unsigned> sequence = 0;
    fs::path root = fs::temp_directory_path() / (L"Glint-update-process-test-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
    fs::path target = root / L"install" / L"Glint.exe";
    fs::path candidate = root / L"download" / L"Glint.exe";
    fs::path appdata = root / L"appdata";
    fs::path cached_download;
    std::string older_hash;
    std::string newer_hash;
    HANDLE parent = nullptr;
    HANDLE held_target = INVALID_HANDLE_VALUE;
    Fixture(const fs::path& old_fixture, const fs::path& new_fixture, const std::string& behavior, bool lock_target = false) {
        require(fs::create_directories(target.parent_path()), "create isolated install directory");
        fs::create_directory(candidate.parent_path()); fs::create_directory(appdata);
        fs::copy_file(old_fixture, target); fs::copy_file(new_fixture, candidate);
        older_hash = glint::update::file_sha256(target); newer_hash = glint::update::file_sha256(candidate);
        cached_download = appdata / L"Glint" / L"updates" / L"downloads" /
            ("v1.0.2-" + newer_hash.substr(0, 16)) / L"Glint.exe";
        fs::create_directories(cached_download.parent_path()); fs::copy_file(candidate, cached_download);
        if (lock_target) {
            held_target = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            require(held_target != INVALID_HANDLE_VALUE, "simulate another process holding the original executable");
        }
        write(target.parent_path() / L"behavior.txt", behavior);
        auto variables = environment(appdata);
        auto command = L"\"" + target.wstring() + L"\" --test-update-parent \"" + candidate.wstring() + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        require(CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
            variables.data(), target.parent_path().c_str(), &startup, &process) != FALSE, "start isolated old desktop fixture");
        parent = process.hProcess; CloseHandle(process.hThread);
    }
    ~Fixture() {
        if (parent) CloseHandle(parent);
        if (held_target != INVALID_HANDLE_VALUE) CloseHandle(held_target);
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() && root.filename().wstring().starts_with(L"Glint-update-process-test-"))
            fs::remove_all(root, ignored);
    }
    std::string errors() const { return read(target.parent_path() / L"start-error.txt") + read(target.parent_path() / L"fixture-error.txt"); }
    fs::path journal() const {
        const auto updates = appdata / L"Glint" / L"updates";
        if (!fs::exists(updates)) return {};
        for (const auto& entry : fs::directory_iterator(updates))
            if (fs::exists(entry.path() / L"transaction.txt")) return entry.path() / L"transaction.txt";
        return {};
    }
    std::string state() const {
        const auto path = journal(); if (path.empty()) return {};
        std::ifstream input(path); std::string ignored, state;
        std::getline(input, ignored); std::getline(input, state); return state;
    }
    bool wait_state(const std::string& expected, DWORD timeout) const {
        const auto deadline = GetTickCount64() + timeout;
        while (GetTickCount64() < deadline) { if (state() == expected) return true; if (!errors().empty()) return false; Sleep(25); }
        return state() == expected;
    }
    bool wait_marker(const wchar_t* marker, DWORD timeout) const {
        const auto deadline = GetTickCount64() + timeout;
        while (GetTickCount64() < deadline) { if (fs::exists(target.parent_path() / marker)) return true; Sleep(25); }
        return fs::exists(target.parent_path() / marker);
    }
    fs::path backup() const {
        for (const auto& entry : fs::directory_iterator(target.parent_path()))
            if (entry.path().extension() == L".backup") return entry.path();
        return {};
    }
    void parent_exited() const {
        require(WaitForSingleObject(parent, 20000) == WAIT_OBJECT_0, "parent did not exit after helper readiness: " + errors());
        DWORD code = 0; require(GetExitCodeProcess(parent, &code) && code == 0, "parent failed: " + errors());
        require(fs::exists(target.parent_path() / L"parent-ready.txt"), "parent exits only after helper readiness");
    }
};
void confirmed_handoff(const fs::path& older, const fs::path& newer) {
    Fixture f(older, newer, "confirm"); f.parent_exited();
    require(f.wait_state("confirmed", 20000), "full handoff did not confirm: state=" + f.state() + " " + f.errors());
    require(f.wait_marker(L"new-ready.txt", 3000), "new exact-path process did not become ready");
    require(glint::update::file_sha256(f.target) == f.newer_hash, "new executable hash differs");
    require(f.wait_marker(L"new-exited.txt", 5000), "bounded confirmed fixture did not exit");
    require(f.backup().empty(), "confirmed update did not remove old executable backup");
    require(!fs::exists(f.cached_download), "confirmed update did not clean its verified download");
}
void exited_child_rolls_back(const fs::path& older, const fs::path& newer) {
    Fixture f(older, newer, "exit-before-ack"); f.parent_exited();
    require(f.wait_state("rolled-back", 20000), "exited child did not roll back: state=" + f.state() + " " + f.errors());
    require(f.wait_marker(L"old-ready.txt", 5000), "old executable did not restart after rollback: " + f.errors());
    require(glint::update::file_sha256(f.target) == f.older_hash, "rollback executable differs");
    require(f.wait_marker(L"old-exited.txt", 5000), "bounded old fixture did not exit");
}
void running_child_is_not_terminated(const fs::path& older, const fs::path& newer) {
    Fixture f(older, newer, "no-ack"); f.parent_exited();
    require(f.wait_marker(L"new-running.txt", 10000), "unconfirmed child did not start");
    require(f.wait_state("pending", 40000), "unconfirmed running child was not preserved: state=" + f.state() + " " + f.errors());
    require(!fs::exists(f.target.parent_path() / L"new-exited.txt"), "test expected child to remain running at timeout");
    require(glint::update::file_sha256(f.target) == f.newer_hash, "unconfirmed new executable was overwritten");
    require(!f.backup().empty() && glint::update::file_sha256(f.backup()) == f.older_hash, "unconfirmed backup was not preserved");
    require(fs::exists(f.cached_download), "unconfirmed update must retain its download");
    require(f.wait_marker(L"new-exited.txt", 10000), "new fixture was terminated instead of reaching its own normal exit");
}
void old_parent_is_never_terminated(const fs::path& older, const fs::path& newer) {
    Fixture f(older, newer, "hold-parent");
    require(f.wait_marker(L"parent-ready.txt", 20000), "helper did not become ready for delayed parent");
    require(f.wait_state("failed", 70000), "helper did not abandon a parent that remained alive: " + f.state() + " " + f.errors());
    require(WaitForSingleObject(f.parent, 0) == WAIT_TIMEOUT, "helper terminated the old desktop process");
    require(glint::update::file_sha256(f.target) == f.older_hash, "helper replaced the executable before its parent exited");
    require(!fs::exists(f.target.parent_path() / L"new-ready.txt"), "helper launched a new desktop while old parent remained alive");
    require(f.wait_marker(L"parent-wait-finished.txt", 15000), "old desktop fixture did not reach its own normal exit");
    f.parent_exited();
}
void replacement_failure_restarts_previous(const fs::path& older, const fs::path& newer) {
    Fixture f(older, newer, "locked-target", true); f.parent_exited();
    require(f.wait_state("failed", 20000), "locked target failure was not recorded: " + f.state() + " " + f.errors());
    require(f.wait_marker(L"old-ready.txt", 5000), "previous desktop did not restart after replacement failure: " + f.errors());
    require(glint::update::file_sha256(f.target) == f.older_hash, "locked previous executable changed");
    require(f.wait_marker(L"old-exited.txt", 5000), "restarted previous desktop did not exit normally");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 3 || argc == 4, "expected old and new versioned fixture executable paths");
        const fs::path older(argv[1]), newer(argv[2]);
        if (argc == 4 && std::wstring(argv[3]) == L"rollback") {
            exited_child_rolls_back(older, newer); std::cout << "Update rollback process test passed\n"; return 0;
        }
        if (argc == 4 && std::wstring(argv[3]) == L"locked") {
            replacement_failure_restarts_previous(older, newer); std::cout << "Update locked-target process test passed\n"; return 0;
        }
        confirmed_handoff(older, newer); exited_child_rolls_back(older, newer); running_child_is_not_terminated(older, newer);
        old_parent_is_never_terminated(older, newer);
        replacement_failure_restarts_previous(older, newer);
        std::cout << "Update helper process tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
