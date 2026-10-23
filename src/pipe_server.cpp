#include "pipe_server.hpp"
#include "win_common.hpp"
#include "foreground_permission.hpp"
#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace glint {
struct PipeClient {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ConnectionId id = 0;
    std::mutex write_mutex;
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    win::BrowserForegroundTarget foreground;
    ~PipeClient() { if (stop_event) CloseHandle(stop_event); }
};

class PipeServer::Impl {
public:
    using MessageCallback = std::function<void(ConnectionId, std::string)>;
    using DisconnectCallback = std::function<void(ConnectionId)>;

    Impl(MessageCallback message, DisconnectCallback disconnect, SendFailureCallback failure, std::wstring name)
        : on_failure_(std::move(failure)), pipe_name_(name.empty() ? win::pipe_name() : std::move(name)),
          on_message_(std::move(message)), on_disconnect_(std::move(disconnect)) {}
    ~Impl() { stop(); if (stop_event_) CloseHandle(stop_event_); }

    void start() {
        if (!stop_event_) return;
        sender_thread_ = std::thread([this] { send_loop(); });
        thread_ = std::thread([this] { accept_loop(); });
    }

    bool ready() const { return accepting_.load() && !stopping_.load(); }

    bool send(ConnectionId id, std::string_view message) {
        if (stopping_.load() || message.empty() || message.size() > kMaxMessageBytes || !valid_utf8(message)) return false;
        std::shared_ptr<PipeClient> client;
        {
            std::lock_guard lock(mutex_);
            const auto found = clients_.find(id);
            if (found == clients_.end()) return false;
            client = found->second;
        }
        {
            std::lock_guard lock(outbound_mutex_);
            if (stopping_.load() || WaitForSingleObject(client->stop_event, 0) != WAIT_TIMEOUT ||
                outbound_.size() >= 64 || message.size() > kQueueBytes - queued_bytes_) return false;
            outbound_.push_back({std::move(client), std::string(message)});
            queued_bytes_ += message.size();
        }
        outbound_changed_.notify_one();
        return true;
    }

    bool allow_foreground(ConnectionId id) {
        std::lock_guard lock(mutex_);
        const auto found = clients_.find(id);
        return found != clients_.end() && !stopping_.load() &&
            WaitForSingleObject(found->second->stop_event, 0) == WAIT_TIMEOUT &&
            found->second->foreground.allow();
    }

    void stop() {
        if (stopping_.exchange(true)) return;
        SetEvent(stop_event_);
        if (thread_.joinable()) thread_.join();
        std::unordered_map<ConnectionId, std::shared_ptr<PipeClient>> clients;
        {
            std::lock_guard lock(mutex_);
            clients.swap(clients_);
        }
        for (auto& [id, client] : clients) {
            (void)id;
            SetEvent(client->stop_event);
            CancelIoEx(client->handle, nullptr);
        }
        outbound_changed_.notify_all();
        if (sender_thread_.joinable()) sender_thread_.join();
        std::vector<ClientWorker> client_threads;
        {
            std::lock_guard lock(thread_mutex_);
            client_threads.swap(client_threads_);
        }
        for (auto& worker : client_threads) if (worker.thread.joinable()) worker.thread.join();
        for (auto& [id, client] : clients) {
            (void)id;
            std::lock_guard write_lock(client->write_mutex);
            DisconnectNamedPipe(client->handle);
            CloseHandle(client->handle);
            client->handle = INVALID_HANDLE_VALUE;
        }
    }

private:
    SendFailureCallback on_failure_;
    std::wstring pipe_name_;
    MessageCallback on_message_;
    DisconnectCallback on_disconnect_;
    std::atomic_bool stopping_ = false;
    HANDLE stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread_;
    std::mutex mutex_;
    std::unordered_map<ConnectionId, std::shared_ptr<PipeClient>> clients_;
    std::mutex thread_mutex_;
    struct ClientWorker { std::thread thread; std::shared_ptr<std::atomic_bool> done; };
    std::vector<ClientWorker> client_threads_;
    std::atomic<ConnectionId> next_id_ = 1;
    struct Outbound { std::shared_ptr<PipeClient> client; std::string message; };
    static constexpr std::size_t kQueueBytes = 4 * 1024 * 1024;
    std::mutex outbound_mutex_;
    std::condition_variable outbound_changed_;
    std::deque<Outbound> outbound_;
    std::size_t queued_bytes_ = 0;
    std::thread sender_thread_;

    void send_loop() {
        for (;;) {
            Outbound item;
            {
                std::unique_lock lock(outbound_mutex_);
                outbound_changed_.wait(lock, [this] { return stopping_.load() || !outbound_.empty(); });
                if (stopping_.load()) { outbound_.clear(); queued_bytes_ = 0; return; }
                item = std::move(outbound_.front());
                outbound_.pop_front();
                queued_bytes_ -= item.message.size();
            }
            const auto frame = encode_frame(item.message);
            bool sent = false;
            {
                std::lock_guard lock(item.client->write_mutex);
                if (item.client->handle != INVALID_HANDLE_VALUE) {
                    sent = win::write_pipe_all(item.client->handle, frame.data(), frame.size(), item.client->stop_event, 2000);
                    if (!sent) {
                        SetEvent(item.client->stop_event);
                        CancelIoEx(item.client->handle, nullptr);
                    }
                }
            }
            if (!sent && !stopping_.load() && on_failure_) on_failure_(item.client->id, std::move(item.message));
        }
    }

    HANDLE make_server_pipe() {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const std::wstring sddl = L"D:(A;;GA;;;" + win::current_user_sid() + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return INVALID_HANDLE_VALUE;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        HANDLE pipe = CreateNamedPipeW(pipe_name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
            1024 * 1024, 1024 * 1024, 0, &attributes);
        LocalFree(descriptor);
        return pipe;
    }

    void accept_loop() {
        struct ClearReadiness {
            std::atomic<bool>& ready;
            ~ClearReadiness() { ready = false; }
        } clear_readiness{accepting_};
        if (!stop_event_) return;
        while (!stopping_.load()) {
            HANDLE pipe = make_server_pipe();
            if (pipe == INVALID_HANDLE_VALUE) return;
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!operation.hEvent) { CloseHandle(pipe); return; }
            BOOL connected = ConnectNamedPipe(pipe, &operation);
            const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
            accepting_ = connected || error == ERROR_IO_PENDING || error == ERROR_PIPE_CONNECTED;
            if (!connected && error == ERROR_IO_PENDING) {
                const HANDLE events[] = {operation.hEvent, stop_event_};
                if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0) CancelIoEx(pipe, &operation);
                DWORD transferred = 0;
                connected = GetOverlappedResult(pipe, &operation, &transferred, TRUE);
            } else if (error == ERROR_PIPE_CONNECTED) connected = TRUE;
            CloseHandle(operation.hEvent);
            if (!connected) { CloseHandle(pipe); continue; }
            if (stopping_.load()) { DisconnectNamedPipe(pipe); CloseHandle(pipe); break; }
            auto client = std::make_shared<PipeClient>();
            if (!client->stop_event) { DisconnectNamedPipe(pipe); CloseHandle(pipe); continue; }
            client->handle = pipe;
            client->id = next_id_++;
            ULONG bridge_pid = 0;
            if (GetNamedPipeClientProcessId(pipe, &bridge_pid))
                client->foreground = win::BrowserForegroundTarget::from_bridge(bridge_pid);
            {
                std::lock_guard lock(mutex_);
                clients_.emplace(client->id, client);
            }
            std::lock_guard lock(thread_mutex_);
            // Release completed Windows thread handles as profiles reconnect.
            // Only join workers that have already returned from their I/O loop.
            for (auto worker = client_threads_.begin(); worker != client_threads_.end();) {
                if (!worker->done->load()) { ++worker; continue; }
                worker->thread.join();
                worker = client_threads_.erase(worker);
            }
            auto done = std::make_shared<std::atomic_bool>(false);
            client_threads_.push_back({std::thread([this, client, done] {
                client_loop(client);
                *done = true;
            }), std::move(done)});
        }
    }

    void client_loop(const std::shared_ptr<PipeClient>& client) {
        FrameReader reader;
        std::vector<std::uint8_t> buffer(16 * 1024);
        bool valid = true;
        while (!stopping_.load()) {
            DWORD count = 0;
            if (!win::read_pipe_some(client->handle, buffer.data(), buffer.size(), count, client->stop_event) || count == 0) { valid = false; break; }
            try {
                const auto messages = reader.feed(std::span<const std::uint8_t>(buffer.data(), count));
                for (const auto& message : messages) on_message_(client->id, message);
            } catch (const ProtocolError&) { valid = false; break; }
        }
        (void)valid;
        // Signal before cancelling: a queued sender may start after CancelIoEx.
        SetEvent(client->stop_event);
        bool close_handle = false;
        {
            std::lock_guard lock(mutex_);
            close_handle = clients_.erase(client->id) != 0;
        }
        if (close_handle) {
            CancelIoEx(client->handle, nullptr);
            std::lock_guard write_lock(client->write_mutex);
            DisconnectNamedPipe(client->handle);
            CloseHandle(client->handle);
            client->handle = INVALID_HANDLE_VALUE;
        }
        on_disconnect_(client->id);
    }
    std::atomic<bool> accepting_{false};
};

PipeServer::PipeServer(MessageCallback message, DisconnectCallback disconnect,
                       SendFailureCallback failure, std::wstring name)
    : impl_(std::make_unique<Impl>(std::move(message), std::move(disconnect), std::move(failure), std::move(name))) {}
PipeServer::~PipeServer() = default;
void PipeServer::start() { impl_->start(); }
bool PipeServer::ready() const { return impl_->ready(); }
bool PipeServer::send(ConnectionId id, std::string_view message) { return impl_->send(id, message); }
bool PipeServer::allow_foreground(ConnectionId id) { return impl_->allow_foreground(id); }
void PipeServer::stop() { impl_->stop(); }
} // namespace glint
