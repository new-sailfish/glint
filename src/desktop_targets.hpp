#pragma once

#include "glint/core.hpp"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace glint::desktop {

enum class UiAdapter { None, Notepad };

struct WindowFacts {
    HWND window = nullptr;
    DWORD process_id = 0;
    std::uint64_t process_creation_time = 0;
    std::wstring title;
    std::wstring image_name;
    bool visible = false;
    bool cloaked = false;
    bool tool_window = false;
    bool shell_window = false;
    std::wstring class_name;
    bool foreground = false;
};

[[nodiscard]] bool is_connected_browser_image(std::wstring_view image_name);
[[nodiscard]] bool should_include_window(const WindowFacts& facts, DWORD own_process_id);
[[nodiscard]] UiAdapter uia_adapter(std::wstring_view image_name);
[[nodiscard]] std::wstring application_name(
    std::wstring_view image_name, std::wstring_view file_description);
[[nodiscard]] std::wstring choose_tab_title(
    std::wstring_view accessible_name, std::span<const std::wstring> child_text);
[[nodiscard]] std::wstring runtime_id_text(std::span<const int> runtime_id);

[[nodiscard]] Tab make_window_target(const WindowFacts& facts, std::wstring_view application);
[[nodiscard]] Tab make_uia_tab_target(
    const WindowFacts& facts, std::wstring_view application, std::size_t index,
    std::wstring_view title, bool active, std::vector<int> runtime_id);
[[nodiscard]] std::vector<Tab> search_targets(
    std::span<const Tab> targets, std::wstring_view query, std::size_t limit = 1000);

// Owns background Win32/UIA discovery. The UI receives one notification for a
// changed snapshot or an activation failure and otherwise never waits on UIA.
class DesktopTargetService {
public:
    DesktopTargetService();
    ~DesktopTargetService();
    DesktopTargetService(const DesktopTargetService&) = delete;
    DesktopTargetService& operator=(const DesktopTargetService&) = delete;

    void start(HWND notification_window, UINT notification_message);
    void stop();
    [[nodiscard]] std::vector<Tab> search(std::wstring_view query) const;
    [[nodiscard]] std::size_t target_count() const;
    // Queues activation after the caller grants foreground permission and
    // hides Glint. The worker reports failures through take_activation_errors.
    [[nodiscard]] bool request_activation(const Tab& target);
    [[nodiscard]] std::vector<std::wstring> take_activation_errors();

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace glint::desktop
