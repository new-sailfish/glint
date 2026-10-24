#pragma once

#include <windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace glint::win {

// Captured off the UI thread from the authenticated named-pipe client process.
// Keeping a process handle prevents a reused PID from receiving foreground rights.
class BrowserForegroundTarget {
public:
    BrowserForegroundTarget() noexcept = default;
    ~BrowserForegroundTarget();
    BrowserForegroundTarget(BrowserForegroundTarget&& other) noexcept;
    BrowserForegroundTarget& operator=(BrowserForegroundTarget&& other) noexcept;
    BrowserForegroundTarget(const BrowserForegroundTarget&) = delete;
    BrowserForegroundTarget& operator=(const BrowserForegroundTarget&) = delete;

    [[nodiscard]] DWORD process_id() const noexcept;
    [[nodiscard]] const std::wstring& executable_path() const noexcept;
    [[nodiscard]] bool allow() const noexcept;
    [[nodiscard]] static BrowserForegroundTarget from_bridge(DWORD bridge_pid);

private:
    friend struct BrowserForegroundTargetTestAccess;
    explicit BrowserForegroundTarget(HANDLE process, DWORD pid, std::wstring executable_path) noexcept;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    std::wstring executable_path_;
};

namespace detail {
struct ForegroundProcessInfo {
    DWORD pid = 0;
    DWORD parent_pid = 0;
    std::wstring image;
    std::uint64_t creation_time = 0;
};

// The sequence starts with the bridge, followed by its immediate ancestors.
// Accept only our EXE -> browser, or our EXE -> system cmd.exe -> browser.
[[nodiscard]] DWORD select_browser_foreground_process(
    std::span<const ForegroundProcessInfo> chain,
    std::wstring_view bridge_image, std::wstring_view system_cmd_image,
    std::wstring_view wow64_cmd_image = {});

// Read-only diagnostic entry point. Uses production discovery and immediately
// releases its handle, without granting foreground permission to any process.
[[nodiscard]] DWORD inspect_browser_foreground_process(
    DWORD bridge_pid, std::wstring_view expected_bridge_image);
} // namespace detail
} // namespace glint::win
