#include "win_app.hpp"

#include "glint/core.hpp"
#include "win_common.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace glint {
namespace {

HANDLE connect_pipe() {
    const auto name = win::pipe_name();
    for (int attempt = 0; attempt != 40; ++attempt) {
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
            return pipe;
        }
        if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return INVALID_HANDLE_VALUE;
}

void browser_to_desktop(HANDLE input, HANDLE pipe, std::atomic_bool& stopped) {
    FrameReader reader;
    std::vector<std::uint8_t> buffer(16 * 1024);
    for (;;) {
        DWORD count = 0;
        if (!win::read_some(input, buffer.data(), buffer.size(), count) || count == 0) break;
        try {
            const auto messages = reader.feed(std::span<const std::uint8_t>(buffer.data(), count));
            for (const auto& message : messages) {
                const auto frame = encode_frame(message);
                if (!win::write_all(pipe, frame.data(), frame.size())) { stopped = true; return; }
            }
        } catch (const ProtocolError&) {
            break;
        }
    }
    stopped = true;
    CancelSynchronousIo(GetCurrentThread());
}

void desktop_to_browser(HANDLE pipe, HANDLE output, std::atomic_bool& stopped) {
    FrameReader reader;
    std::vector<std::uint8_t> buffer(16 * 1024);
    for (;;) {
        DWORD count = 0;
        if (!win::read_some(pipe, buffer.data(), buffer.size(), count) || count == 0) break;
        try {
            const auto messages = reader.feed(std::span<const std::uint8_t>(buffer.data(), count));
            for (const auto& message : messages) {
                const auto frame = encode_frame(message);
                if (!win::write_all(output, frame.data(), frame.size())) { stopped = true; return; }
            }
        } catch (const ProtocolError&) {
            break;
        }
    }
    stopped = true;
}

} // namespace

int run_native_host() {
    HANDLE pipe = connect_pipe();
    if (pipe == INVALID_HANDLE_VALUE) return 2;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    std::atomic_bool stopped = false;
    std::thread to_desktop(browser_to_desktop, input, pipe, std::ref(stopped));
    std::thread to_browser(desktop_to_browser, pipe, output, std::ref(stopped));

    while (!stopped.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CancelSynchronousIo(to_desktop.native_handle());
    CancelSynchronousIo(to_browser.native_handle());
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    if (to_desktop.joinable()) to_desktop.join();
    if (to_browser.joinable()) to_browser.join();
    return 0;
}

} // namespace glint
