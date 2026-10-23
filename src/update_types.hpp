#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace glint::update {

struct Release {
    std::string version;
    std::string page_url;
    std::string manifest_url;
    std::string manifest_api_url;
    std::string binary_url;
    std::string binary_api_url;
    std::string sha256;
    std::uint64_t size = 0;
};

struct Package {
    Release release;
    std::filesystem::path executable;
    std::filesystem::path directory;
};

enum class FailureKind { Network, RateLimit, Invalid, Cancelled, Filesystem };
class Error : public std::runtime_error {
public:
    Error(std::string message, FailureKind kind = FailureKind::Invalid,
          std::uint64_t retry_after_seconds = 0)
        : std::runtime_error(std::move(message)), kind(kind), retry_after_seconds(retry_after_seconds) {}
    FailureKind kind;
    std::uint64_t retry_after_seconds;
};
using Cancel = std::function<bool()>;
using Progress = std::function<void(std::uint64_t received, std::uint64_t total)>;

} // namespace glint::update
