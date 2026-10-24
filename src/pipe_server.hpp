#pragma once

#include "glint/core.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace glint {

class PipeServer {
public:
    using MessageCallback = std::function<void(ConnectionId, std::string)>;
    using DisconnectCallback = std::function<void(ConnectionId)>;
    using SendFailureCallback = std::function<void(ConnectionId, std::string)>;

    PipeServer(MessageCallback message, DisconnectCallback disconnect,
               SendFailureCallback send_failure = {}, std::wstring pipe_name = {});
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    void start();
    [[nodiscard]] bool ready() const;
    // True means queued, not delivered. Does not wait for the peer to read.
    bool send(ConnectionId id, std::string_view message);
    // Call while Glint still owns the foreground, before queuing activation.
    // Resolving the browser process happens on the accept worker, not here.
    bool allow_foreground(ConnectionId id);
    // Reads the browser path captured by the accept worker; never queries Shell.
    [[nodiscard]] std::wstring executable_path(ConnectionId id) const;
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace glint
