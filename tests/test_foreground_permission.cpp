#include "foreground_permission.hpp"

#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using glint::win::BrowserForegroundTarget;
using glint::win::detail::ForegroundProcessInfo;
constexpr auto bridge_image = L"D:\\Apps\\Glint\\Glint.exe";
constexpr auto cmd_image = L"C:\\Windows\\System32\\cmd.exe";
constexpr auto wow64_cmd_image = L"C:\\Windows\\SysWOW64\\cmd.exe";

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
DWORD select(const std::vector<ForegroundProcessInfo>& chain) {
    return glint::win::detail::select_browser_foreground_process(chain, bridge_image, cmd_image, wow64_cmd_image);
}
std::vector<ForegroundProcessInfo> direct(const wchar_t* browser = L"C:\\Chrome\\chrome.exe") {
    return {{30, 20, bridge_image, 300}, {20, 10, browser, 200}};
}
std::vector<ForegroundProcessInfo> wrapped(const wchar_t* browser = L"C:\\Edge\\msedge.exe") {
    return {{30, 25, bridge_image, 300}, {25, 20, cmd_image, 250}, {20, 10, browser, 200}};
}

void accepts_browser_native_host_chains() {
    require(select(direct()) == 20, "direct Chrome native host receives permission");
    require(select(direct(L"C:\\Edge\\msedge.exe")) == 20, "direct Edge native host receives permission");
    require(select(wrapped()) == 20, "system cmd wrapped Edge host receives permission");
    require(select(wrapped(L"C:\\Chrome\\chrome.exe")) == 20, "system cmd wrapped Chrome host receives permission");
    for (const auto browser : {L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
                              L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe"}) {
        auto wow64 = wrapped(browser);
        wow64[1].image = wow64_cmd_image;
        require(select(wow64) == 20, "32-bit Chrome or Edge using the system WOW64 cmd wrapper is accepted");
    }
    auto chain = direct(L"C:\\Chrome\\CHROME.EXE");
    chain[0].image = L"d:\\APPS\\glint\\glint.EXE";
    require(select(chain) == 20, "Windows process image comparison ignores case");
}

void rejects_wrong_images_and_non_browser_ancestors() {
    auto chain = direct();
    chain[0].image = L"D:\\other\\Glint.exe";
    require(select(chain) == 0, "same bridge filename in another directory is untrusted");
    chain = wrapped();
    chain[1].image = L"C:\\Users\\example-user\\cmd.exe";
    require(select(chain) == 0, "only system cmd is an accepted intermediary");
    chain[1].image = L"C:\\Users\\example-user\\SysWOW64\\cmd.exe";
    require(select(chain) == 0, "a lookalike WOW64 cmd outside the system directory is rejected");
    require(select(direct(L"C:\\Windows\\explorer.exe")) == 0, "unrelated direct parent is rejected");
    chain = wrapped();
    chain[1].image = L"C:\\Windows\\System32\\powershell.exe";
    require(select(chain) == 0, "unrelated intermediary is rejected even above browser");
    require(select(direct(L"C:\\Chrome\\notchrome.exe")) == 0, "browser filename must match exactly");
    require(select({}) == 0, "empty process chain is rejected");
    require(select({direct().front()}) == 0, "bridge without a parent is rejected");
}

void rejects_pid_reuse_or_disconnected_chain() {
    auto chain = direct();
    chain[1].creation_time = 301;
    require(select(chain) == 0, "parent newer than bridge is a reused PID");
    chain = wrapped();
    chain[2].creation_time = 251;
    require(select(chain) == 0, "browser newer than cmd is a reused PID");
    chain = direct();
    chain[0].parent_pid = 21;
    require(select(chain) == 0, "parent PID must match adjacent process");
    chain = direct();
    chain[1].pid = chain[0].pid;
    chain[0].parent_pid = chain[1].pid;
    require(select(chain) == 0, "self-parent process is rejected");
    chain = wrapped();
    chain[2].pid = chain[0].pid;
    chain[1].parent_pid = chain[2].pid;
    require(select(chain) == 0, "cycle returning to the bridge is rejected");
    chain = direct();
    chain[1].creation_time = 0;
    require(select(chain) == 0, "missing creation time cannot establish ancestry");
}

void safe_default_and_real_process_rejection() {
    static_assert(!std::is_copy_constructible_v<BrowserForegroundTarget>);
    static_assert(std::is_nothrow_move_constructible_v<BrowserForegroundTarget>);
    BrowserForegroundTarget empty;
    require(empty.process_id() == 0 && !empty.allow(), "empty target grants no permission");
    auto moved = std::move(empty);
    require(empty.process_id() == 0 && moved.process_id() == 0 && !moved.allow(), "empty moves remain inert");
    require(BrowserForegroundTarget::from_bridge(0).process_id() == 0, "PID zero is rejected");
    // This CTest process is launched by CTest/PowerShell, not a browser.
    // Exercise WinAPI discovery without changing focus or granting any permission.
    require(BrowserForegroundTarget::from_bridge(GetCurrentProcessId()).process_id() == 0,
        "ordinary test process cannot identify an unrelated browser target");
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        if (argc == 4 && std::wstring_view(argv[1]) == L"--inspect-bridge") {
            std::size_t consumed = 0;
            const auto pid = std::stoul(argv[2], &consumed);
            if (consumed != std::wstring_view(argv[2]).size() || !pid || pid > MAXDWORD)
                throw std::runtime_error("invalid bridge PID");
            std::cout << glint::win::detail::inspect_browser_foreground_process(static_cast<DWORD>(pid), argv[3]) << '\n';
            return 0;
        }
        if (argc != 1) throw std::runtime_error("usage: --inspect-bridge <pid> <expected-exe>");
        accepts_browser_native_host_chains();
        rejects_wrong_images_and_non_browser_ancestors();
        rejects_pid_reuse_or_disconnected_chain();
        safe_default_and_real_process_rejection();
        std::cout << "Foreground permission tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
