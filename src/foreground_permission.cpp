#include "foreground_permission.hpp"
#include "bridge_installation.hpp"

#include <tlhelp32.h>

#include <array>
#include <utility>
#include <vector>

namespace glint::win {
namespace {
class OwnedHandle {
public:
    explicit OwnedHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~OwnedHandle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    OwnedHandle(OwnedHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    OwnedHandle& operator=(OwnedHandle&& other) noexcept {
        if (this != &other) {
            OwnedHandle previous(std::exchange(value_, std::exchange(other.value_, nullptr)));
        }
        return *this;
    }
    HANDLE get() const noexcept { return value_; }
    HANDLE release() noexcept { return std::exchange(value_, nullptr); }
private:
    HANDLE value_;
};

bool equal_image(std::wstring_view left, std::wstring_view right) {
    return !left.empty() && left.size() == right.size() &&
        CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool browser_image(std::wstring_view image) {
    const auto slash = image.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return false;
    const auto filename = image.substr(slash + 1);
    return equal_image(filename, L"chrome.exe") || equal_image(filename, L"msedge.exe");
}

std::wstring process_image(HANDLE process) {
    std::wstring image(32768, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process, 0, image.data(), &size)) return {};
    image.resize(size);
    return image;
}

std::uint64_t process_creation_time(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

std::wstring system_cmd_image(bool wow64 = false) {
    std::wstring directory(32768, L'\0');
    const auto capacity = static_cast<UINT>(directory.size());
    const auto count = wow64 ? GetSystemWow64DirectoryW(directory.data(), capacity) :
        GetSystemDirectoryW(directory.data(), capacity);
    if (!count || count >= directory.size()) return {};
    directory.resize(count);
    directory += L"\\cmd.exe";
    return directory;
}
} // namespace

BrowserForegroundTarget::BrowserForegroundTarget(HANDLE process, DWORD pid, std::wstring executable_path) noexcept
    : process_(process), pid_(pid), executable_path_(std::move(executable_path)) {}
BrowserForegroundTarget::~BrowserForegroundTarget() { if (process_) CloseHandle(process_); }
BrowserForegroundTarget::BrowserForegroundTarget(BrowserForegroundTarget&& other) noexcept
    : process_(std::exchange(other.process_, nullptr)), pid_(std::exchange(other.pid_, 0)),
      executable_path_(std::exchange(other.executable_path_, {})) {}
BrowserForegroundTarget& BrowserForegroundTarget::operator=(BrowserForegroundTarget&& other) noexcept {
    if (this != &other) {
        if (process_) CloseHandle(process_);
        process_ = std::exchange(other.process_, nullptr);
        pid_ = std::exchange(other.pid_, 0);
        executable_path_ = std::exchange(other.executable_path_, {});
    }
    return *this;
}
DWORD BrowserForegroundTarget::process_id() const noexcept { return pid_; }
const std::wstring& BrowserForegroundTarget::executable_path() const noexcept { return executable_path_; }
bool BrowserForegroundTarget::allow() const noexcept {
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT &&
        AllowSetForegroundWindow(pid_) != FALSE;
}

namespace {
struct ForegroundDiscovery {
    OwnedHandle process{};
    DWORD pid = 0;
    std::wstring executable_path;
};

ForegroundDiscovery discover_browser_foreground_target(DWORD bridge_pid, std::wstring_view expected_bridge_image) {
    if (!bridge_pid) return {};
    const auto our_image = expected_bridge_image;
    const auto cmd_image = system_cmd_image();
    const auto wow64_cmd_image = system_cmd_image(true);
    if (our_image.empty() || cmd_image.empty()) return {};

    // Opening before taking the snapshot also lets the later checks reject a
    // terminated/reused client process instead of trusting a bare PID.
    std::array<OwnedHandle, 3> processes;
    processes[0] = OwnedHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, bridge_pid));
    if (!processes[0].get() || WaitForSingleObject(processes[0].get(), 0) != WAIT_TIMEOUT ||
        !equal_image(process_image(processes[0].get()), our_image)) return {};

    OwnedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return {};
    std::vector<std::pair<DWORD, DWORD>> parents;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) return {};
    do { parents.emplace_back(entry.th32ProcessID, entry.th32ParentProcessID); }
    while (Process32NextW(snapshot.get(), &entry));

    std::vector<detail::ForegroundProcessInfo> chain;
    DWORD pid = bridge_pid;
    for (std::size_t depth = 0; depth < processes.size(); ++depth) {
        DWORD parent_pid = 0;
        for (const auto& [candidate, parent] : parents) {
            if (candidate == pid) { parent_pid = parent; break; }
        }
        if (depth != 0) {
            processes[depth] = OwnedHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        }
        const auto process = processes[depth].get();
        if (!process || WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return {};
        chain.push_back({pid, parent_pid, process_image(process), process_creation_time(process)});
        if (depth != 0 && browser_image(chain.back().image)) {
            if (detail::select_browser_foreground_process(chain, our_image, cmd_image, wow64_cmd_image) != pid) return {};
            // All ancestor handles remain open through selection; the browser
            // handle stays open for the connection's lifetime.
            for (std::size_t i = 0; i <= depth; ++i) {
                if (WaitForSingleObject(processes[i].get(), 0) != WAIT_TIMEOUT) return {};
            }
            return {std::move(processes[depth]), pid, std::move(chain.back().image)};
        }
        if (depth == 1 && !equal_image(chain.back().image, cmd_image) &&
            !equal_image(chain.back().image, wow64_cmd_image)) return {};
        if (!parent_pid || parent_pid == pid) return {};
        pid = parent_pid;
    }
    return {};
}
} // namespace

BrowserForegroundTarget BrowserForegroundTarget::from_bridge(DWORD bridge_pid) {
    OwnedHandle bridge(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, bridge_pid));
    if (!bridge.get() || WaitForSingleObject(bridge.get(), 0) != WAIT_TIMEOUT) return {};
    const auto image = process_image(bridge.get());
    if (!equal_image(image, process_image(GetCurrentProcess())) &&
        !bridge_installation::is_installed_bridge(image)) return {};
    auto discovered = discover_browser_foreground_target(bridge_pid, image);
    return BrowserForegroundTarget(discovered.process.release(), discovered.pid, std::move(discovered.executable_path));
}

DWORD detail::select_browser_foreground_process(std::span<const ForegroundProcessInfo> chain,
    std::wstring_view bridge_image, std::wstring_view system_cmd_image, std::wstring_view wow64_cmd_image) {
    if ((chain.size() != 2 && chain.size() != 3) || !equal_image(chain[0].image, bridge_image)) return 0;
    if (!browser_image(chain.back().image)) return 0;
    if (chain.size() == 3 && !equal_image(chain[1].image, system_cmd_image) &&
        !equal_image(chain[1].image, wow64_cmd_image)) return 0;
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (!chain[i].pid || !chain[i].creation_time) return 0;
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (chain[previous].pid == chain[i].pid) return 0;
        }
        if (i > 0 && (chain[i - 1].parent_pid != chain[i].pid ||
            chain[i].creation_time > chain[i - 1].creation_time)) return 0;
    }
    return chain.back().pid;
}

DWORD detail::inspect_browser_foreground_process(DWORD bridge_pid, std::wstring_view expected_bridge_image) {
    return discover_browser_foreground_target(bridge_pid, expected_bridge_image).pid;
}
} // namespace glint::win
