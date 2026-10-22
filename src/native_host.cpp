#include "win_app.hpp"
#include "native_host.hpp"

#include "glint/core.hpp"
#include "win_common.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace glint {
namespace {

struct DesktopSession {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::int64_t generation = 0;
    ~DesktopSession() {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        if (stop) CloseHandle(stop);
    }
};

class PersistentRelay {
public:
    PersistentRelay(HANDLE input, HANDLE output, std::wstring_view pipe_name)
        : input_(input), output_(output), pipe_name_(pipe_name) {}
    ~PersistentRelay() { if (stop_) CloseHandle(stop_); }

    int run() {
        if (!stop_) return 2;
        std::atomic_bool input_done = false, output_done = false;
        std::thread browser([&] { read_browser(); request_stop(); input_done = true; });
        std::thread desktop([&] { manage_desktop(); request_stop(); output_done = true; });
        WaitForSingleObject(stop_, INFINITE);
        while (!input_done.load() || !output_done.load()) {
            // Repeated cancellation covers a worker entering synchronous
            // stdin/stdout I/O just after it checked the lifetime stop event.
            if (!input_done.load()) CancelSynchronousIo(browser.native_handle());
            if (!output_done.load()) CancelSynchronousIo(desktop.native_handle());
            const auto session = current_session();
            if (session) { SetEvent(session->stop); CancelIoEx(session->pipe, nullptr); }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        browser.join();
        desktop.join();
        return 0;
    }

private:
    HANDLE input_, output_;
    const std::wstring pipe_name_;
    HANDLE stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::mutex session_mutex_;
    std::shared_ptr<DesktopSession> session_;
    std::int64_t generation_ = 0;

    bool stopped() const { return WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT; }
    std::shared_ptr<DesktopSession> current_session() {
        std::lock_guard lock(session_mutex_);
        return session_;
    }
    void request_stop() {
        SetEvent(stop_);
        const auto session = current_session();
        if (session) SetEvent(session->stop);
    }
    bool write_browser(std::string_view message) {
        if (stopped()) return false;
        const auto frame = encode_frame(message);
        // This is the only stdout writer: status and relayed frames cannot
        // interleave, including while a pipe session is being replaced.
        return win::write_all(output_, frame.data(), frame.size());
    }
    bool report_status(const char* state, std::int64_t generation = 0) {
        Json status = Json::Object{{"type", "desktop_status"}, {"version", 1}, {"state", state}};
        if (generation) status["generation"] = generation;
        return write_browser(status.dump());
    }

    void read_browser() {
        FrameReader reader;
        std::vector<std::uint8_t> buffer(16 * 1024);
        while (!stopped()) {
            DWORD count = 0;
            if (!win::read_some(input_, buffer.data(), buffer.size(), count) || !count) return;
            try {
                const auto messages = reader.feed(std::span<const std::uint8_t>(buffer.data(), count));
                for (const auto& message : messages) {
                    if (stopped()) return;
                    const auto payload = Json::parse(message);
                    const auto* generation = payload.find("desktopGeneration");
                    const auto session = current_session();
                    // Drop offline metadata, plus data buffered before a newer
                    // connected status. There is no replay queue or stale index.
                    if (!session || !generation || !generation->is_integer() ||
                        generation->as_integer() != session->generation ||
                        WaitForSingleObject(session->stop, 0) != WAIT_TIMEOUT) continue;
                    const auto frame = encode_frame(message);
                    if (!win::write_pipe_all(session->pipe, frame.data(), frame.size(), session->stop, 2000))
                        SetEvent(session->stop);
                }
            } catch (const ProtocolError&) { return; }
        }
    }

    void relay_desktop(const std::shared_ptr<DesktopSession>& session) {
        FrameReader reader;
        std::vector<std::uint8_t> buffer(16 * 1024);
        while (!stopped() && WaitForSingleObject(session->stop, 0) == WAIT_TIMEOUT) {
            DWORD count = 0;
            if (!win::read_pipe_some(session->pipe, buffer.data(), buffer.size(), count, session->stop) || !count) return;
            try {
                const auto messages = reader.feed(std::span<const std::uint8_t>(buffer.data(), count));
                for (const auto& message : messages) {
                    if (stopped() || WaitForSingleObject(session->stop, 0) != WAIT_TIMEOUT) return;
                    if (!write_browser(message)) { request_stop(); return; }
                }
            } catch (const ProtocolError&) { return; }
        }
    }

    void manage_desktop() {
        if (!report_status("waiting")) return;
        DWORD retry_ms = 100;
        while (!stopped()) {
            const auto pipe = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                if (WaitForSingleObject(stop_, retry_ms) != WAIT_TIMEOUT) return;
                retry_ms = (std::min)(DWORD{1000}, retry_ms * 2);
                continue;
            }
            auto session = std::make_shared<DesktopSession>();
            session->pipe = pipe;
            if (!session->stop) return;
            DWORD mode = PIPE_READMODE_BYTE;
            if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
                if (WaitForSingleObject(stop_, retry_ms) != WAIT_TIMEOUT) return;
                retry_ms = (std::min)(DWORD{1000}, retry_ms * 2);
                continue;
            }
            session->generation = ++generation_;
            {
                std::lock_guard lock(session_mutex_);
                session_ = session;
            }
            if (!report_status("connected", session->generation)) { request_stop(); return; }
            relay_desktop(session);
            SetEvent(session->stop);
            CancelIoEx(session->pipe, nullptr);
            {
                std::lock_guard lock(session_mutex_);
                session_.reset();
            }
            if (stopped()) return;
            if (!report_status("waiting")) return;
            // Avoid reconnect spin if a desktop repeatedly accepts then closes.
            if (WaitForSingleObject(stop_, 100) != WAIT_TIMEOUT) return;
            retry_ms = 100;
        }
    }
};

} // namespace

int relay_native_messages(HANDLE input, HANDLE output, std::wstring_view pipe_name) {
    return PersistentRelay(input, output, pipe_name).run();
}

int run_native_host() {
    return relay_native_messages(GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), win::pipe_name());
}

} // namespace glint
