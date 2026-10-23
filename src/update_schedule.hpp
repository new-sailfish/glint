#pragma once

#include <algorithm>
#include <cstdint>

namespace glint::update {
struct Schedule {
    std::int64_t last_success = 0;
    std::int64_t retry_at = 0;
    std::int64_t server_limit = 0;
    unsigned failures = 0;
    bool due(std::int64_t now) const {
        if (now < server_limit || now < retry_at) return false;
        if (retry_at) return true;
        return !last_success || now < last_success || now - last_success >= 86400;
    }
    bool manual_allowed(std::int64_t now) const { return now >= server_limit; }
    void success(std::int64_t now) { last_success = now; retry_at = server_limit = 0; failures = 0; }
    void failure(std::int64_t now, std::uint64_t delay = 0) {
        ++failures;
        const auto backoff = failures == 1 ? 900 : failures == 2 ? 3600 : 21600;
        retry_at = now + backoff;
        if (delay) server_limit = now + static_cast<std::int64_t>((std::min)(delay, std::uint64_t{604800}));
    }
};
} // namespace glint::update
