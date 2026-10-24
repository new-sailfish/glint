#pragma once

#include "update_types.hpp"

#include <optional>
#include <map>
#include <mutex>
#include <span>
#include <string_view>

namespace glint::update {

std::optional<Release> find_release(std::string_view current_version, const Cancel &cancel = {});
Package download_release(Release release, const std::filesystem::path &cache_root, const Cancel &cancel = {},
                         const Progress &progress = {});
void verify_binary(const std::filesystem::path &path, const Release &release);
std::string file_sha256(const std::filesystem::path &path);

namespace feed {
inline constexpr std::uint64_t kMetadataLimit = 1024 * 1024;
inline constexpr std::uint64_t kBinaryLimit = 128 * 1024 * 1024;

// The transport checks each actual redirect hop against this shared origin cooldown.
class SourceCooldowns {
  public:
    explicit SourceCooldowns(std::function<std::int64_t()> now = {});
    void check(std::string_view url);
    void limit(std::string_view url, std::uint64_t seconds);

  private:
    std::function<std::int64_t()> now_;
    std::mutex mutex_;
    std::map<std::string, std::int64_t> until_;
};

struct Request {
    std::string url;
    bool octet_stream = false;
    std::uint64_t maximum_bytes = kMetadataLimit;
};
using Sink = std::function<void(std::span<const char>)>;
// Transport injection keeps the source fallback and filesystem checks testable offline.
using Fetch = std::function<void(const Request &, const Sink &, const Cancel &, const Progress &)>;

bool newer_version(std::string_view candidate, std::string_view current);
bool allowed_url(std::string_view url);
Release parse_latest(std::string_view json);
Release parse_manifest(std::string_view json, Release release = {});
std::optional<Release> find_release(std::string_view current_version, const Fetch &fetch, const Cancel &cancel = {});
Package download_release(Release release, const std::filesystem::path &cache_root, const Fetch &fetch,
                         const Cancel &cancel = {}, const Progress &progress = {});
} // namespace feed
} // namespace glint::update
