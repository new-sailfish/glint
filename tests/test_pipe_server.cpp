#include "pipe_server.hpp"
#include "win_common.hpp"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Events {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<glint::ConnectionId, std::string>> messages;
    std::vector<std::pair<glint::ConnectionId, std::string>> failures;
    std::vector<glint::ConnectionId> disconnected;
    template<class Predicate> bool wait(Predicate predicate, std::chrono::milliseconds timeout = 5000ms) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, predicate);
    }
};

struct Fixture {
    Events events;
    std::wstring name = L"\\\\.\\pipe\\Glint.ServerTests." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(GetTickCount64());
    glint::PipeServer server{
        [this](auto id, auto text) { std::lock_guard lock(events.mutex); events.messages.emplace_back(id, std::move(text)); events.changed.notify_all(); },
        [this](auto id) { std::lock_guard lock(events.mutex); events.disconnected.push_back(id); events.changed.notify_all(); },
        [this](auto id, auto text) { std::lock_guard lock(events.mutex); events.failures.emplace_back(id, std::move(text)); events.changed.notify_all(); }, name};
    Fixture() { server.start(); }
    ~Fixture() { server.stop(); }
};

class Client {
public:
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    explicit Client(const std::wstring& name) {
        for (unsigned attempt = 0; attempt < 200; ++attempt) {
            handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (handle != INVALID_HANDLE_VALUE) return;
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error("connect isolated test client");
    }
    ~Client() { close(); CloseHandle(stop); }
    void close() { SetEvent(stop); if (handle != INVALID_HANDLE_VALUE) { CancelIoEx(handle, nullptr); CloseHandle(handle); handle = INVALID_HANDLE_VALUE; } }
    void write(const std::string& text) {
        const auto frame = glint::encode_frame(text);
        require(glint::win::write_pipe_all(handle, frame.data(), frame.size(), stop, 1000), "write test client frame");
    }
    std::string read() {
        glint::FrameReader reader;
        std::vector<std::uint8_t> data(16384);
        for (;;) {
            DWORD count = 0;
            require(glint::win::read_pipe_some(handle, data.data(), data.size(), count, stop) && count, "read test client frame");
            auto messages = reader.feed(std::span<const std::uint8_t>(data.data(), count));
            if (!messages.empty()) return messages.front();
        }
    }
};

void independent_duplex_clients() {
    Fixture fixture;
    Client first(fixture.name), second(fixture.name);
    first.write("first"); second.write("second");
    require(fixture.events.wait([&] { return fixture.events.messages.size() == 2; }), "receive independent client frames");
    glint::ConnectionId first_id = 0, second_id = 0;
    { std::lock_guard lock(fixture.events.mutex); for (const auto& [id, text] : fixture.events.messages) (text == "first" ? first_id : second_id) = id; }
    require(first_id && second_id && first_id != second_id, "connection IDs are isolated");
    require(fixture.server.send(first_id, "reply-first") && fixture.server.send(second_id, "reply-second"), "queue both responses while reads are pending");
    require(first.read() == "reply-first" && second.read() == "reply-second", "responses stay on the requested connection");
    first.close();
    require(fixture.events.wait([&] { return !fixture.events.disconnected.empty(); }), "observe one disconnect");
    second.write("still-live");
    require(fixture.events.wait([&] { return fixture.events.messages.size() == 3; }), "one disconnect preserves other client");
}

void closed_tabs_update_search_without_reconnecting() {
    glint::TabIndex index;
    Events events;
    const auto name = L"\\\\.\\pipe\\Glint.IndexUpdateTests." + std::to_wstring(GetCurrentProcessId());
    glint::PipeServer server{
        [&](auto id, auto message) {
            const bool applied = index.apply(id, glint::Json::parse(message));
            std::lock_guard lock(events.mutex);
            events.messages.emplace_back(id, applied ? "applied" : "rejected");
            events.changed.notify_all();
        }, [&](auto id) {
            index.disconnect(id);
            std::lock_guard lock(events.mutex);
            events.disconnected.push_back(id); events.changed.notify_all();
        }, {}, name};
    server.start();
    Client first(name), second(name);
    const auto hello = R"({"type":"hello","version":1,"sessionId":"live","profileId":"p","browser":"chrome"})";
    const auto snapshot = R"({"type":"snapshot","version":1,"tabs":[{"tabId":1,"windowId":10,"title":"同名页面","url":"https://closed.test"}]})";
    first.write(hello); first.write(snapshot);
    second.write(hello); second.write(snapshot);
    require(events.wait([&] { return events.messages.size() == 4; }), "receive snapshots over real isolated IPC");
    require(index.search(L"closed.test").size() == 2, "duplicate tab IDs remain isolated by connection");
    first.write(R"({"type":"remove","version":1,"tabId":1})");
    require(events.wait([&] { return events.messages.size() == 5; }), "receive tab close without reconnect");
    require(index.search(L"closed.test").size() == 1 && index.connection_count() == 2,
        "search immediately loses only the closed tab while connections remain live");
    second.write(R"({"type":"remove","version":1,"tabId":1})");
    require(events.wait([&] { return events.messages.size() == 6; }), "receive second tab close");
    require(index.search(L"closed.test").empty() && index.search(L"").empty(), "closed results disappear for both keyword and empty search");
    require(!server.allow_foreground(999999), "unknown connections cannot receive foreground permission");
    first.close();
    require(events.wait([&] { return !events.disconnected.empty(); }), "observe disconnected search connection");
    glint::ConnectionId closed_id;
    { std::lock_guard lock(events.mutex); closed_id = events.disconnected.front(); }
    require(!server.allow_foreground(closed_id), "disconnected connections cannot receive foreground permission");
    server.stop();
}

void backpressure_is_async_bounded_and_reports_failure() {
    Fixture fixture;
    Client stalled(fixture.name), healthy(fixture.name);
    stalled.write("stalled"); healthy.write("healthy");
    require(fixture.events.wait([&] { return fixture.events.messages.size() == 2; }), "identify backpressure clients");
    glint::ConnectionId stalled_id = 0, healthy_id = 0;
    { std::lock_guard lock(fixture.events.mutex); for (const auto& [id, text] : fixture.events.messages) (text == "stalled" ? stalled_id : healthy_id) = id; }
    const std::string large(800000, 'x');
    auto sending = std::async(std::launch::async, [&] {
        for (int index = 0; index < 3; ++index) if (!fixture.server.send(stalled_id, large)) return false;
        return true;
    });
    const bool prompt = sending.wait_for(300ms) == std::future_status::ready;
    if (!prompt) { fixture.server.stop(); sending.wait(); }
    require(prompt && sending.get(), "send must return promptly while a peer does not read");
    unsigned rejected = 0;
    const auto began = std::chrono::steady_clock::now();
    for (int index = 0; index < 100; ++index) if (!fixture.server.send(stalled_id, large)) ++rejected;
    require(rejected > 0 && std::chrono::steady_clock::now() - began < 500ms, "outbound queue has a bounded capacity without blocking callers");
    require(fixture.events.wait([&] { return !fixture.events.failures.empty(); }, 4000ms), "stalled delivery reports a bounded failure");
    require(fixture.server.send(healthy_id, "healthy-reply"), "healthy peer can still queue a response");
    auto reading = std::async(std::launch::async, [&] { return healthy.read(); });
    const bool delivered = reading.wait_for(3500ms) == std::future_status::ready;
    if (!delivered) SetEvent(healthy.stop);
    require(delivered && reading.get() == "healthy-reply", "stalled peer does not permanently block another connection");
    healthy.write("healthy-inbound");
    require(fixture.events.wait([&] { return fixture.events.messages.size() == 3; }), "other connection still receives messages");
    const auto stopped_at = std::chrono::steady_clock::now();
    fixture.server.stop();
    require(std::chrono::steady_clock::now() - stopped_at < 1500ms, "stop cancels pending writes, reads and accept promptly");
    require(!fixture.server.send(healthy_id, "after-stop"), "stop rejects new outbound work");
}

void disconnect_racing_queued_sends_finishes() {
    Fixture fixture;
    DWORD initial_handles = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &initial_handles) != FALSE, "measure initial handles");
    for (int iteration = 0; iteration < 128; ++iteration) {
        Client client(fixture.name);
        client.write("race");
        require(fixture.events.wait([&] { return fixture.events.messages.size() == static_cast<std::size_t>(iteration + 1); }), "identify racing client");
        glint::ConnectionId id;
        { std::lock_guard lock(fixture.events.mutex); id = fixture.events.messages.back().first; }
        auto sending = std::async(std::launch::async, [&] { for (int count = 0; count < 32; ++count) fixture.server.send(id, "racing-response"); });
        client.close();
        require(sending.wait_for(1000ms) == std::future_status::ready, "disconnect does not trap a sender");
        sending.get();
        require(fixture.events.wait([&] { return fixture.events.disconnected.size() == static_cast<std::size_t>(iteration + 1); }), "racing client is cleaned up");
    }
    DWORD retained_handles = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &retained_handles) != FALSE, "measure handles after reconnects");
    require(retained_handles <= initial_handles + 16, "completed client threads must not retain one handle per reconnect");
    const auto began = std::chrono::steady_clock::now();
    fixture.server.stop();
    require(std::chrono::steady_clock::now() - began < 1500ms, "pending accept stops without a wake client");
}
} // namespace

int main() {
    try {
        independent_duplex_clients();
        closed_tabs_update_search_without_reconnecting();
        backpressure_is_async_bounded_and_reports_failure();
        disconnect_racing_queued_sends_finishes();
        std::cout << "pipe server tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "pipe server tests failed: " << error.what() << '\n'; return 1; }
}
