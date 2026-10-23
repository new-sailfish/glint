#include "native_host.hpp"
#include "win_common.hpp"
#include "glint/core.hpp"
#include "language_sync.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "native host test failed: " << message << '\n';
    // Each case runs in its own process. A failed timeout must not then block
    // while joining the deliberately stuck relay that the assertion detected.
    ExitProcess(1);
}

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    void reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

bool complete_io(HANDLE pipe, OVERLAPPED& operation, BOOL started, DWORD error, DWORD& count) {
    if (started) return true;
    if (error != ERROR_IO_PENDING) return false;
    const auto wait = WaitForSingleObject(operation.hEvent, 1500);
    if (wait != WAIT_OBJECT_0) CancelIoEx(pipe, &operation);
    const auto complete = GetOverlappedResult(pipe, &operation, &count, TRUE);
    return wait == WAIT_OBJECT_0 && complete;
}

class Relay {
public:
    explicit Relay(DWORD settle_ms = 200, bool desktop_present = true) {
        name_ = L"\\\\.\\pipe\\Glint.NativeHostTests." + std::to_wstring(GetCurrentProcessId());
        HANDLE input_read = nullptr, input_write = nullptr, output_read = nullptr, output_write = nullptr;
        require(CreatePipe(&input_read, &input_write, nullptr, 4096) != FALSE, "create browser stdin pipe");
        input_read_.reset(input_read); input_write_.reset(input_write);
        require(CreatePipe(&output_read, &output_write, nullptr, 4096) != FALSE, "create browser stdout pipe");
        output_read_.reset(output_read); output_write_.reset(output_write);
        done_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        require(done_.get() != nullptr, "create relay completion event");
        worker_ = std::thread([this] {
            result_ = glint::relay_native_messages(input_read_.get(), output_write_.get(), name_);
            SetEvent(done_.get());
        });
        require(receive_browser() == R"({"state":"waiting","type":"desktop_status","version":1})",
            "persistent native port reports waiting without exiting while desktop is absent");
        expect_running();
        if (desktop_present) connect_desktop();
        // Start both worker reads before supplying any browser data. This is
        // the condition that serialized synchronous I/O on the duplex handle.
        std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
    }
    ~Relay() {
        require(WaitForSingleObject(done_.get(), 0) == WAIT_OBJECT_0, "relay must finish before fixture cleanup");
        worker_.join();
    }
    void send_browser(std::string_view message) {
        const auto frame = browser_frame(message);
        send_browser_bytes(frame);
    }
    std::string browser_message(std::string_view message) const {
        auto json = glint::Json::parse(message);
        json["desktopGeneration"] = generation_;
        return json.dump();
    }
    std::vector<std::uint8_t> browser_frame(std::string_view message) const {
        return glint::encode_frame(browser_message(message));
    }
    void send_browser_bytes(std::span<const std::uint8_t> bytes) {
        require(glint::win::write_all(input_write_.get(), bytes.data(), bytes.size()), "write browser native bytes");
    }
    std::string receive_desktop() {
        glint::FrameReader reader;
        std::array<std::uint8_t, 16 * 1024> buffer{};
        for (;;) {
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            require(event.get() != nullptr, "create desktop read event");
            OVERLAPPED operation{}; operation.hEvent = event.get();
            DWORD count = 0;
            const auto started = ReadFile(server_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, &operation);
            require(complete_io(server_.get(), operation, started, started ? ERROR_SUCCESS : GetLastError(), count) && count != 0,
                    "browser message must reach desktop while opposite-direction read is pending");
            const auto messages = reader.feed(std::span<const std::uint8_t>(buffer.data(), count));
            if (!messages.empty()) return messages.front();
        }
    }
    void send_desktop(std::string_view message) {
        const auto frame = glint::encode_frame(message);
        send_desktop_bytes(frame);
    }
    void send_desktop_bytes(std::span<const std::uint8_t> bytes) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        require(event.get() != nullptr, "create desktop write event");
        OVERLAPPED operation{}; operation.hEvent = event.get();
        DWORD count = 0;
        const auto started = WriteFile(server_.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, &operation);
        require(complete_io(server_.get(), operation, started, started ? ERROR_SUCCESS : GetLastError(), count) && count == bytes.size(), "send desktop bytes within timeout");
    }
    std::string receive_browser() {
        if (!browser_messages_.empty()) {
            auto next = std::move(browser_messages_.front()); browser_messages_.pop_front(); return next;
        }
        std::array<std::uint8_t, 16 * 1024> buffer{};
        const auto deadline = GetTickCount64() + 1500;
        while (GetTickCount64() < deadline) {
            DWORD available = 0;
            require(PeekNamedPipe(output_read_.get(), nullptr, 0, nullptr, &available, nullptr) != FALSE, "peek native stdout");
            if (!available) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            DWORD count = 0;
            require(glint::win::read_some(output_read_.get(), buffer.data(), (std::min)(available, static_cast<DWORD>(buffer.size())), count),
                    "read browser native response");
            const auto messages = browser_reader_.feed(std::span<const std::uint8_t>(buffer.data(), count));
            for (auto& message : messages) browser_messages_.push_back(std::move(message));
            if (!browser_messages_.empty()) {
                auto next = std::move(browser_messages_.front()); browser_messages_.pop_front(); return next;
            }
        }
        require(false, "desktop response must reach browser while stdin read is pending");
        return {};
    }
    void browser_eof() { input_write_.reset(); }
    void expect_no_forwarded_data() {
        for (const auto handle : {server_.get(), output_read_.get()}) {
            DWORD available = 0;
            const auto readable = PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr);
            const auto error = readable ? ERROR_SUCCESS : GetLastError();
            require(readable || error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED,
                    "inspect isolated pipe for rejected or incomplete frames");
            require(available == 0, "relay must not forward incomplete or invalid frames");
        }
    }
    void disconnect_desktop() {
        require(DisconnectNamedPipe(server_.get()) != FALSE, "disconnect test desktop pipe");
        server_.reset();
        expect_waiting();
    }
    void expect_waiting() {
        require(receive_browser() == R"({"state":"waiting","type":"desktop_status","version":1})",
            "desktop disconnect keeps native port alive and reports waiting");
        expect_running();
    }
    void close_desktop() { server_.reset(); }
    void connect_desktop() {
        server_.reset(CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0, nullptr));
        require(server_.get() != INVALID_HANDLE_VALUE, "create isolated desktop pipe");
        Handle accepted(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        require(accepted.get() != nullptr, "create pipe acceptance event");
        OVERLAPPED operation{}; operation.hEvent = accepted.get();
        const BOOL connected = ConnectNamedPipe(server_.get(), &operation);
        const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        require(connected || error == ERROR_IO_PENDING || error == ERROR_PIPE_CONNECTED, "begin asynchronous desktop pipe acceptance");
        DWORD ignored = 0;
        require(error == ERROR_PIPE_CONNECTED || complete_io(server_.get(), operation, connected, error, ignored),
            "same native relay connects when the desktop pipe becomes available");
        const auto status = glint::Json::parse(receive_browser());
        require(status.at("type").as_string() == "desktop_status" && status.at("state").as_string() == "connected" &&
            status.at("version").as_integer() == 1, "connected status follows a successful real pipe connection");
        const auto next_generation = status.at("generation").as_integer();
        require(next_generation > generation_, "each new desktop pipe receives a distinct increasing generation");
        generation_ = next_generation;
        expect_running();
    }
    void expect_running() { require(WaitForSingleObject(done_.get(), 0) == WAIT_TIMEOUT, "native relay remains the same live invocation"); }
    void wait_done() {
        require(WaitForSingleObject(done_.get(), 2000) == WAIT_OBJECT_0, "relay must stop both workers within timeout");
        require(result_ == 0, "connected relay must return success on shutdown");
    }
private:
    std::wstring name_;
    Handle server_, input_read_, input_write_, output_read_, output_write_, done_;
    std::thread worker_;
    int result_ = -1;
    std::int64_t generation_ = 0;
    glint::FrameReader browser_reader_;
    std::deque<std::string> browser_messages_;
};

void run_case(std::string_view name) {
    if (name == "missing-desktop") {
        Relay relay(0, false);
        // More than an anonymous pipe can buffer: an offline bridge must keep
        // draining input instead of retaining metadata or blocking the browser.
        for (int i = 0; i < 80; ++i)
            relay.send_browser(glint::Json(glint::Json::Object{{"type", "upsert"}, {"padding", std::string(8192, 'x')}}).dump());
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        relay.expect_running();
        relay.browser_eof();
        relay.wait_done();
        return;
    }
    if (name == "invalid-frames") {
        for (const bool browser : {true, false}) {
            for (const auto length : {0u, static_cast<unsigned>(glint::kMaxMessageBytes + 1)}) {
                Relay relay;
                const std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>(length),
                    static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length >> 16),
                    static_cast<std::uint8_t>(length >> 24)};
                if (browser) relay.send_browser_bytes(header); else {
                    relay.send_desktop_bytes(header);
                    // Invalid desktop framing closes just that pipe session.
                    relay.expect_waiting();
                    relay.close_desktop();
                    relay.connect_desktop();
                    relay.send_browser(R"({"type":"snapshot","version":1,"tabs":[]})");
                    require(relay.receive_desktop() == relay.browser_message(R"({"type":"snapshot","version":1,"tabs":[]})"),
                        "invalid desktop framing recovers through a fresh session");
                    relay.browser_eof();
                }
                relay.wait_done();
                if (browser) relay.expect_no_forwarded_data();
            }
        }
        return;
    }
    if (name == "shutdown-race") {
        for (int attempt = 0; attempt != 40; ++attempt) {
            Relay relay(0);
            if (attempt % 2) relay.browser_eof(); else { relay.disconnect_desktop(); relay.browser_eof(); }
            relay.wait_done();
        }
        return;
    }
    Relay relay;
    if (name == "duplex") {
        constexpr auto hello = R"({"type":"hello","version":1,"browser":"Chrome","sessionId":"test","profileId":"test"})";
        constexpr auto response = R"({"type":"activate","version":1,"tabId":1,"windowId":1})";
        relay.send_browser(hello);
        require(relay.receive_desktop() == relay.browser_message(hello), "preserve browser frame payload");
        relay.send_desktop(response);
        require(relay.receive_browser() == response, "preserve desktop frame payload");
        const auto language = glint::language_settings_message(glint::i18n::Preference::English, glint::i18n::Language::English);
        relay.send_desktop(language);
        require(relay.receive_browser() == language, "desktop language settings reach the extension unchanged over the live relay");
        relay.browser_eof();
    } else if (name == "browser-eof") {
        relay.browser_eof();
    } else if (name == "desktop-disconnect") {
        relay.disconnect_desktop();
        relay.browser_eof();
    } else if (name == "desktop-reconnect") {
        constexpr auto hello = R"({"type":"hello","version":1,"browser":"Chrome","sessionId":"fresh","profileId":"test"})";
        const auto stale_frame = relay.browser_frame(R"({"type":"upsert","version":1,"tab":{"tabId":900}})");
        relay.send_browser(hello);
        require(relay.receive_desktop() == relay.browser_message(hello), "initial metadata reaches initial desktop");
        const auto initial_language = glint::language_settings_message(glint::i18n::Preference::English, glint::i18n::Language::English);
        relay.send_desktop(initial_language);
        require(relay.receive_browser() == initial_language, "initial desktop supplies its language after hello");
        relay.disconnect_desktop();
        relay.send_browser_bytes(stale_frame);
        relay.connect_desktop();
        // Also model data that was already in the browser's asynchronous send
        // path before waiting but arrives after the next pipe is connected.
        relay.send_browser_bytes(stale_frame);
        constexpr auto snapshot = R"({"type":"snapshot","version":1,"tabs":[]})";
        relay.send_browser(hello);
        require(relay.receive_desktop() == relay.browser_message(hello), "new desktop receives a fresh hello and no offline increment");
        const auto current_language = glint::language_settings_message(glint::i18n::Preference::System, glint::i18n::Language::Chinese);
        relay.send_desktop(current_language);
        require(relay.receive_browser() == current_language,
            "reconnected desktop refreshes the extension language on the same native port before a snapshot");
        relay.send_browser(snapshot);
        require(relay.receive_desktop() == relay.browser_message(snapshot), "same relay forwards the fresh reconnect snapshot");
        relay.browser_eof();
    } else if (name == "stdout-backpressure") {
        relay.send_desktop(glint::Json(glint::Json::Object{{"type", "activate"},
            {"padding", std::string(64 * 1024, 'x')}}).dump());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        relay.browser_eof();
    } else if (name == "fragmented-frames") {
        const auto message = R"({"type":"hello","version":1,"browser":"Edge","sessionId":"fragmented","profileId":"test"})";
        const auto frame = relay.browser_frame(message);
        relay.send_browser_bytes(std::span(frame).first(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        relay.expect_no_forwarded_data();
        relay.send_browser_bytes(std::span(frame).subspan(2, 3));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        relay.expect_no_forwarded_data();
        relay.send_browser_bytes(std::span(frame).subspan(5));
        require(relay.receive_desktop() == relay.browser_message(message), "relay must reassemble fragmented browser headers and payloads");
        const auto desktop_frame = glint::encode_frame(message);
        relay.send_desktop_bytes(std::span(desktop_frame).first(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        relay.expect_no_forwarded_data();
        relay.send_desktop_bytes(std::span(desktop_frame).subspan(1));
        require(relay.receive_browser() == message, "relay must reassemble fragmented desktop frames");
        relay.browser_eof();
    } else if (name == "partial-eof") {
        const auto frame = glint::encode_frame(R"({"type":"hello","version":1})");
        relay.send_browser_bytes(std::span(frame).first(7));
        relay.browser_eof();
        relay.wait_done();
        relay.expect_no_forwarded_data();
    } else {
        require(false, "unknown native host test case");
    }
    relay.wait_done();
}

void run_child(const wchar_t* executable, const wchar_t* name) {
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --case " + name;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != FALSE, "start isolated native relay test process");
    Handle child(process.hProcess), thread(process.hThread);
    if (WaitForSingleObject(child.get(), 15000) != WAIT_OBJECT_0) {
        TerminateProcess(child.get(), 2);
        WaitForSingleObject(child.get(), 1000);
        require(false, "native relay case exceeded watchdog timeout");
    }
    DWORD exit_code = 0;
    require(GetExitCodeProcess(child.get(), &exit_code) != FALSE, "read native relay case result");
    if (exit_code != 0) std::wcerr << L"native relay failed case: " << name << L'\n';
    require(exit_code == 0, "native relay child case must pass");
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--case") { run_case(argv[2]); return 0; }
    std::wstring executable(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    require(count != 0 && count < executable.size(), "locate native relay test executable");
    executable.resize(count);
    for (const auto name : {L"duplex", L"browser-eof", L"desktop-disconnect", L"stdout-backpressure", L"shutdown-race",
         L"invalid-frames", L"fragmented-frames", L"partial-eof", L"missing-desktop", L"desktop-reconnect"})
        run_child(executable.c_str(), name);
    std::cout << "native host tests passed\n";
}
