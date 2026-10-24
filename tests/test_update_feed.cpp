#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "glint/core.hpp"
#include "update_feed.hpp"
#include <windows.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
using namespace glint::update;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void rejects(F &&work, const char *message) {
    try {
        work();
    } catch (const Error &) {
        return;
    }
    throw std::runtime_error(message);
}
const std::string root = "https://github.com/new-sailfish/glint/releases/";
const std::string api = "https://api.github.com/repos/new-sailfish/glint/releases/";
const std::array<std::string, 4> manifest_roots{
    "https://raw.githubusercontent.com/new-sailfish/glint/update-feed/",
    "https://new-sailfish.github.io/glint/updates/",
    "https://gcore.jsdelivr.net/gh/new-sailfish/glint@update-feed/",
    "https://testingcf.jsdelivr.net/gh/new-sailfish/glint@update-feed/"};
const std::array<std::string, 4> proxies{
    "https://ghfast.top/", "https://gh-proxy.com/", "https://ghfile.geekertao.top/", "https://ghproxy.net/"};
const std::string hash(64, 'a');
std::vector<std::string> manifest_sources(bool pinned) {
    const std::string suffix = pinned ? "v1.2.3/release-manifest.json" : "release-manifest.json";
    const auto github = root + (pinned ? "download/v1.2.3/" : "latest/download/") + "release-manifest.json";
    std::vector<std::string> sources{github};
    if (pinned)
        sources.push_back(api + "assets/12");
    for (const auto &mirror : manifest_roots)
        sources.push_back(mirror + suffix);
    for (const auto &proxy : proxies)
        sources.push_back(proxy + github);
    return sources;
}
std::string metadata() {
    return R"({"tag_name":"v1.2.3","draft":false,"prerelease":false,"html_url":")" + root +
           R"(tag/v1.2.3","assets":[{"name":"Glint.exe","size":4096,"browser_download_url":")" + root +
           R"(download/v1.2.3/Glint.exe","url":")" + api +
           R"(assets/11"},{"name":"release-manifest.json","size":400,"browser_download_url":")" + root +
           R"(download/v1.2.3/release-manifest.json","url":")" + api + R"(assets/12"}]})";
}
std::string metadata_for(std::string version, std::uint64_t size) {
    auto json = glint::Json::parse(metadata());
    json["tag_name"] = "v" + version;
    json["html_url"] = root + "tag/v" + version;
    auto &assets = json["assets"].as_array();
    assets[0]["size"] = static_cast<std::int64_t>(size);
    assets[0]["browser_download_url"] = root + "download/v" + version + "/Glint.exe";
    assets[0]["digest"] = "sha256:" + std::string(64, 'b');
    assets[1]["browser_download_url"] = root + "download/v" + version + "/release-manifest.json";
    return json.dump();
}
std::string manifest(std::string version = "1.2.3", std::string digest = hash) {
    return R"({"Product":"Glint","Version":")" + version + R"(","FileVersion":")" + version +
           R"(.0","Binary":"Glint.exe","Files":[{"Path":"Glint.exe","SHA256":")" + digest + R"("}]})";
}
void emit(const feed::Sink &sink, const std::string &text) {
    sink(std::span(text.data(), text.size()));
}
void parsers_and_urls() {
    require(feed::newer_version("1.2.10", "1.2.9"), "numeric version ordering");
    require(!feed::newer_version("1.2.3", "999.0.1"), "local versions stay newer naturally");
    for (auto invalid : {"v1.2.3", "1.2", "1.2.3-beta", "01.2.3", "65536.1.1", "1.2.3.0", "1.2.3."})
        rejects([&] { feed::newer_version(invalid, "0.0.0"); }, "reject noncanonical numeric versions");
    require(feed::allowed_url(root + "download/v1.2.3/Glint.exe"), "allow pinned EXE");
    require(feed::allowed_url(api + "tags/v1.2.3"), "allow version-pinned API release lookup");
    for (const auto &suffix : {"tags/v01.2.3", "tags/v1.2", "tags/v1.2.3.0", "tags/v1.2.3-beta",
                               "tags/1.2.3", "tags/v1.2.3/extra", "tags/v1.2.3?redirect=evil",
                               "tags/v1.2.3#fragment", "tags/../latest", "tags/%76%31.2.3"})
        require(!feed::allowed_url(api + suffix), "reject noncanonical or malformed API tag lookup");
    require(feed::allowed_url(
                "https://release-assets.githubusercontent.com/github-production-release-asset/123/abc?sig=ok"),
            "allow GitHub asset CDN");
    for (const auto &mirror : manifest_roots) {
        require(feed::allowed_url(mirror + "release-manifest.json"), "allow latest mirror manifest");
        require(feed::allowed_url(mirror + "v1.2.3/release-manifest.json"), "allow pinned mirror manifest");
        for (const auto &suffix : {"../release-manifest.json", "%2e%2e/release-manifest.json",
                                   "v01.2.3/release-manifest.json", "v1.2/release-manifest.json",
                                   "v1.2.3/other.exe", "v1.2.3/release-manifest.json/extra",
                                   "release-manifest.json?redirect=evil", "Glint.exe"})
            require(!feed::allowed_url(mirror + suffix), "reject malformed mirror paths or unpinned binaries");
        auto insecure = mirror;
        insecure.replace(0, 5, "http");
        require(!feed::allowed_url(insecure + "release-manifest.json"), "mirror HTTPS cannot downgrade");
        auto userinfo = mirror;
        userinfo.insert(8, "user@");
        require(!feed::allowed_url(userinfo + "release-manifest.json"), "reject mirror userinfo");
        auto port = mirror;
        port.insert(port.find('/', 8), ":443");
        require(!feed::allowed_url(port + "release-manifest.json"), "reject explicit mirror ports");
    }
    require(feed::allowed_url(manifest_roots[1] + "v1.2.3/Glint.exe"), "allow version-pinned Pages binary");
    for (const auto index : {0, 2, 3})
        require(!feed::allowed_url(manifest_roots[index] + "v1.2.3/Glint.exe"),
                "raw and jsDelivr serve manifests only");
    for (const auto &proxy : proxies) {
        require(feed::allowed_url(proxy + root + "download/v1.2.3/Glint.exe"), "allow pinned proxy binary");
        require(feed::allowed_url(proxy + root + "download/v1.2.3/release-manifest.json"),
                "allow pinned proxy manifest");
        require(feed::allowed_url(proxy + root + "latest/download/release-manifest.json"),
                "allow latest proxy manifest");
        for (const auto &destination : {
                 "https://github.com/other/glint/releases/download/v1.2.3/Glint.exe",
                 "https://github.com/new-sailfish/glint/releases/latest/download/Glint.exe",
                 "https://github.com/new-sailfish/glint/releases/download/v01.2.3/Glint.exe",
                 "https://github.com/new-sailfish/glint/releases/download/v1.2.3/other.exe",
                 "https://github.com/new-sailfish/glint/releases/download/v1.2.3/../Glint.exe",
                 "https://github.com/new-sailfish/glint/releases/download/v1.2.3/%2e%2e/Glint.exe",
                 "https://github.com/new-sailfish/glint/releases/download/v1.2.3/Glint.exe?next=evil",
                 "http://github.com/new-sailfish/glint/releases/download/v1.2.3/Glint.exe",
                 "https://user@github.com/new-sailfish/glint/releases/download/v1.2.3/Glint.exe",
                 "https://github.com:443/new-sailfish/glint/releases/download/v1.2.3/Glint.exe",
                 "https://evil.example/Glint.exe", "https://api.github.com/repos/new-sailfish/glint/releases/assets/11"})
            require(!feed::allowed_url(proxy + destination), "proxy cannot wrap an arbitrary or unpinned destination");
        auto userinfo = proxy;
        userinfo.insert(8, "user@");
        require(!feed::allowed_url(userinfo + root + "download/v1.2.3/Glint.exe"), "reject proxy userinfo");
        auto port = proxy;
        port.insert(port.find('/', 8), ":443");
        require(!feed::allowed_url(port + root + "download/v1.2.3/Glint.exe"), "reject explicit proxy port");
    }
    for (auto url : {"http://github.com/new-sailfish/glint/releases/latest",
                     "https://github.com:444/new-sailfish/glint/releases/latest",
                     "https://user@github.com/new-sailfish/glint/releases/latest",
                     "https://github.com/other/glint/releases/latest", "https://evil.example/Glint.exe",
                     "https://github.com/new-sailfish/glint/releases/../evil",
                     "https://github.com/new-sailfish/glint/releases/%2e%2e/evil",
                     "https://release-assets.githubusercontent.com.evil.example/x",
                     "https://api.github.com/repos/other/glint/releases/tags/v1.2.3",
                     "https://api.github.com:443/repos/new-sailfish/glint/releases/tags/v1.2.3",
                     "https://user@api.github.com/repos/new-sailfish/glint/releases/tags/v1.2.3",
                     "http://api.github.com/repos/new-sailfish/glint/releases/tags/v1.2.3",
                     "https://raw.githubusercontent.com/other/glint/update-feed/release-manifest.json",
                     "https://new-sailfish.github.io/other/updates/release-manifest.json",
                     "https://gcore.jsdelivr.net/gh/new-sailfish/glint@master/release-manifest.json",
                     "https://testingcf.jsdelivr.net/gh/other/glint@update-feed/release-manifest.json",
                     "https://ghfast.top.evil.example/https://github.com/new-sailfish/glint/releases/download/v1.2.3/Glint.exe",
                     "https://github.com/new-sailfish/glint/releases/latest/download/Glint.exe"})
        require(!feed::allowed_url(url), "reject untrusted or unpinned URL");
    auto release = feed::parse_latest(metadata());
    require(release.version == "1.2.3" && release.size == 4096 && release.sha256.empty(), "API candidate metadata");
    release = feed::parse_manifest(manifest(), release);
    require(release.sha256 == hash, "manifest supplies trusted digest");
    rejects([&] { feed::parse_manifest(manifest("1.2.4"), release); }, "API and manifest versions must match");
    rejects([&] { feed::parse_manifest(manifest("1.2.3", "abc")); }, "reject invalid hash");
    auto fallback = feed::parse_manifest(manifest());
    require(fallback.binary_url == root + "download/v1.2.3/Glint.exe" && !fallback.size, "fallback pins binary URL");
    auto draft = glint::Json::parse(metadata());
    draft["draft"] = true;
    rejects([&] { feed::parse_latest(draft.dump()); }, "reject draft release");
    draft = glint::Json::parse(metadata());
    draft["prerelease"] = true;
    rejects([&] { feed::parse_latest(draft.dump()); }, "reject prerelease");
    draft = glint::Json::parse(metadata());
    draft["assets"].as_array()[0]["size"] = std::int64_t(feed::kBinaryLimit + 1);
    rejects([&] { feed::parse_latest(draft.dump()); }, "reject oversized binary");
    draft = glint::Json::parse(metadata());
    draft["assets"].as_array()[0]["browser_download_url"] = root + "download/v1.2.4/Glint.exe";
    rejects([&] { feed::parse_latest(draft.dump()); }, "asset URL must match release version");
    draft = glint::Json::parse(metadata());
    draft["assets"].as_array().push_back(draft["assets"].as_array()[0]);
    rejects([&] { feed::parse_latest(draft.dump()); }, "reject duplicate binary assets");
    draft = glint::Json::parse(manifest());
    draft["Binary"] = "other.exe";
    rejects([&] { feed::parse_manifest(draft.dump()); }, "manifest binary must name Glint.exe");
    draft = glint::Json::parse(manifest());
    draft["Files"] = glint::Json::Array{};
    rejects([&] { feed::parse_manifest(draft.dump(), release); },
            "cannot inherit an unverified digest from a previous manifest");
    rejects([&] { feed::parse_manifest(std::string(100, '[')); }, "reject excessive nesting before JSON recursion");
    rejects([&] { feed::parse_manifest(std::string(feed::kMetadataLimit + 1, ' ')); }, "reject oversized manifest");
}
void source_cooldowns() {
    std::int64_t now = 1000;
    feed::SourceCooldowns cooldowns([&] { return now; });
    const std::string redirected =
        "https://release-assets.githubusercontent.com/github-production-release-asset/123/abc?sig=first";
    const std::string same_origin =
        "https://release-assets.githubusercontent.com/github-production-release-asset/456/def?sig=second";
    const auto limited = [&](const std::string &url, std::uint64_t remaining) {
        try {
            cooldowns.check(url);
            throw std::runtime_error("limited origin must fail before another request");
        } catch (const Error &error) {
            require(error.kind == FailureKind::RateLimit && error.retry_after_seconds == remaining,
                    "cooldown reports the remaining delay for the actual response origin");
            require(error.source_origin == (url.starts_with("https://release-assets.githubusercontent.com/")
                                                ? "https://release-assets.githubusercontent.com"
                                                : "https://raw.githubusercontent.com"),
                    "cooldown error identifies the actual limited origin");
        }
    };
    cooldowns.limit(redirected, 120);
    cooldowns.check(root + "download/v1.2.3/Glint.exe");
    cooldowns.check(api + "assets/11");
    cooldowns.check(proxies[0] + root + "download/v1.2.3/Glint.exe");
    limited(same_origin, 120);
    now += 59;
    limited(same_origin, 61);
    now += 61;
    cooldowns.check(same_origin);
    cooldowns.limit(manifest_roots[0] + "release-manifest.json", 180);
    cooldowns.check(same_origin);
    cooldowns.check(root + "download/v1.2.3/Glint.exe");
    limited(manifest_roots[0] + "v1.2.3/release-manifest.json", 180);
    cooldowns.limit(redirected, 0);
    limited(same_origin, 3600);
    cooldowns.check(api + "latest");
}
void source_fallbacks() {
    const auto latest_sources = manifest_sources(false);
    auto discovery_sources = latest_sources;
    discovery_sources.insert(discovery_sources.begin() + 5, api + "latest");
    const std::vector<std::string> api_prefix(discovery_sources.begin(), discovery_sources.begin() + 6);
    std::vector<std::string> calls;
    feed::Fetch fetch;
    std::optional<Release> release;
    for (std::size_t selected = 0; selected < discovery_sources.size(); ++selected) {
        if (selected == 5)
            continue; // The API supplies a candidate, rather than a manifest.
        calls.clear();
        fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
            calls.push_back(request.url);
            require(request.maximum_bytes == feed::kMetadataLimit && !request.octet_stream,
                    "latest manifest sources retain JSON headers and metadata limit");
            if (request.url == discovery_sources[selected]) {
                emit(sink, manifest());
                return;
            }
            throw Error("source unavailable", FailureKind::Network);
        };
        release = feed::find_release("0.0.0", fetch);
        const std::vector<std::string> expected(discovery_sources.begin(), discovery_sources.begin() + selected + 1);
        require(calls == expected, "latest discovery tries five official manifests before API, then four proxies");
        require(release && release->version == "1.2.3" && release->sha256 == hash &&
                    release->binary_url == root + "download/v1.2.3/Glint.exe" &&
                    release->size == 0 && release->binary_api_url.empty(),
                "manifest discovery pins binary and digest without querying API for asset metadata");
        if (selected < 5)
            require(calls.size() == selected + 1 && calls.back() != api + "latest",
                    "successful official manifest stops discovery before any API request");
    }
    calls.clear();
    fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        require(request.url == latest_sources[0], "primary manifest succeeds without any API query");
        emit(sink, manifest());
    };
    release = feed::find_release("0.0.0", fetch);
    require(release && calls == std::vector<std::string>{latest_sources[0]}, "primary success makes exactly one request");
    calls.clear();
    require(!feed::find_release("999.0.1", fetch) && calls.size() == 1, "local version is not downgraded");
    calls.clear();
    rejects([&] { feed::find_release("0.0.0", fetch, [] { return true; }); }, "cancel before source request");
    require(calls.empty(), "initial cancellation makes no metadata requests");

    const auto pinned_sources = manifest_sources(true);
    for (std::size_t selected = 0; selected < pinned_sources.size(); ++selected) {
        calls.clear();
        fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
            calls.push_back(request.url);
            require(request.maximum_bytes == feed::kMetadataLimit, "all pinned manifest sources retain metadata limit");
            require(request.octet_stream == (request.url == api + "assets/12"),
                    "only GitHub API assets request octet-stream");
            if (request.url == api + "latest") {
                emit(sink, metadata());
                return;
            }
            if (request.url == pinned_sources[selected]) {
                emit(sink, manifest());
                return;
            }
            throw Error("source unavailable", FailureKind::Network);
        };
        release = feed::find_release("0.0.0", fetch);
        auto expected = api_prefix;
        expected.insert(expected.end(), pinned_sources.begin(), pinned_sources.begin() + selected + 1);
        require(calls == expected, "API backup is queried only after five failures and then uses pinned sources in order");
        require(release && release->version == "1.2.3" && release->sha256 == hash &&
                    release->binary_url == root + "download/v1.2.3/Glint.exe" && release->size == 4096,
                "API fallback preserves version pinning, digest and binary size metadata");
    }
    calls.clear();
    fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        if (request.url == latest_sources[0]) throw Error("GitHub limited", FailureKind::RateLimit, 300);
        if (request.url == latest_sources[1]) {
            emit(sink, manifest());
            return;
        }
        throw Error("unexpected source", FailureKind::Network);
    };
    release = feed::find_release("0.0.0", fetch);
    require(release && calls == std::vector<std::string>{latest_sources[0], latest_sources[1]},
            "primary GitHub rate limit still permits the raw origin without API");
    calls.clear();
    fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        require(request.url != pinned_sources[0], "rate-limited GitHub origin is not requested again in this operation");
        if (request.url == latest_sources[0]) throw Error("GitHub limited", FailureKind::RateLimit, 300);
        if (request.url == api + "latest") emit(sink, metadata());
        else if (request.url == api + "assets/12") emit(sink, manifest());
        else throw Error("source unavailable", FailureKind::Network);
    };
    release = feed::find_release("0.0.0", fetch);
    auto expected_limited_github = api_prefix;
    expected_limited_github.push_back(api + "assets/12");
    require(release && calls == expected_limited_github,
            "limited primary origin is skipped for pinned manifest while independent API asset can succeed");
    calls.clear();
    fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        if (request.url == latest_sources[0])
            throw Error("CDN limited", FailureKind::RateLimit, 300, "https://release-assets.githubusercontent.com");
        if (request.url == api + "latest") emit(sink, metadata());
        else if (request.url == pinned_sources[0]) emit(sink, manifest());
        else throw Error("source unavailable", FailureKind::Network);
    };
    release = feed::find_release("0.0.0", fetch);
    auto expected_limited_cdn = api_prefix;
    expected_limited_cdn.push_back(pinned_sources[0]);
    require(release && calls == expected_limited_cdn,
            "redirected CDN rate limit leaves the original GitHub manifest origin available");
    calls.clear();
    fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        require(request.url != api + "assets/12", "limited API origin does not trigger an API asset request");
        if (request.url == api + "latest") throw Error("API limited", FailureKind::RateLimit, 600);
        if (request.url == latest_sources[5]) {
            emit(sink, manifest());
            return;
        }
        throw Error("source unavailable", FailureKind::Network);
    };
    release = feed::find_release("0.0.0", fetch);
    const std::vector<std::string> expected_limited_api(discovery_sources.begin(), discovery_sources.begin() + 7);
    require(release && release->binary_api_url.empty() && calls == expected_limited_api,
            "API rate limit falls through to latest proxy manifest without retrying API assets");
    calls.clear();
    fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        if (request.url == api + "latest") emit(sink, metadata());
        else if (request.url == pinned_sources[0]) emit(sink, manifest("1.2.4"));
        else if (request.url == pinned_sources[2]) emit(sink, manifest());
        else throw Error("source unavailable", FailureKind::Network);
    };
    release = feed::find_release("0.0.0", fetch);
    auto expected_mismatch = api_prefix;
    expected_mismatch.insert(expected_mismatch.end(), pinned_sources.begin(), pinned_sources.begin() + 3);
    require(release && release->version == "1.2.3" && calls == expected_mismatch,
            "stale or mismatched manifest is discarded before the next pinned source");
    for (const bool all_limited : {false, true}) {
        calls.clear();
        fetch = [&](const feed::Request &request, const feed::Sink &, const Cancel &, const Progress &) {
            calls.push_back(request.url);
            if (all_limited || request.url == api + "latest")
                throw Error("limited", FailureKind::RateLimit, 600);
            throw Error("network unavailable", FailureKind::Network);
        };
        try {
            feed::find_release("0.0.0", fetch);
            throw std::runtime_error("all unavailable manifest sources must fail");
        } catch (const Error &error) {
            require(all_limited ? error.kind == FailureKind::RateLimit && error.retry_after_seconds == 600
                                : error.kind != FailureKind::RateLimit,
                    "global cooldown is reported only when all manifest sources are limited");
            require(calls == discovery_sources, "all-source failure exhausts every independent manifest origin");
        }
    }
    for (const auto kind : {FailureKind::Cancelled, FailureKind::Filesystem}) {
        calls.clear();
        fetch = [&](const feed::Request &request, const feed::Sink &, const Cancel &, const Progress &) {
            calls.push_back(request.url);
            throw Error("stop", kind);
        };
        try {
            feed::find_release("0.0.0", fetch);
            throw std::runtime_error("terminal manifest failure must escape");
        } catch (const Error &error) {
            require(error.kind == kind && calls.size() == 1, "cancellation and filesystem failure stop manifest fallback");
        }
    }
}
std::string native_version(const std::filesystem::path &path) {
    DWORD ignored = 0;
    std::vector<char> data(GetFileVersionInfoSizeW(path.c_str(), &ignored));
    require(!data.empty() && GetFileVersionInfoW(path.c_str(), 0, DWORD(data.size()), data.data()),
            "fixture has version info");
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    require(VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length), "fixture has fixed version");
    return std::to_string(HIWORD(info->dwProductVersionMS)) + "." + std::to_string(LOWORD(info->dwProductVersionMS)) +
           "." + std::to_string(HIWORD(info->dwProductVersionLS));
}
void binaries(const std::filesystem::path &fixture) {
    auto cache = std::filesystem::temp_directory_path() / ("glint-feed-test-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(cache);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{cache};
    Release release = feed::parse_manifest(manifest(native_version(fixture), file_sha256(fixture)));
    release.size = std::filesystem::file_size(fixture);
    std::ifstream fixture_file(fixture, std::ios::binary);
    const std::string fixture_bytes((std::istreambuf_iterator<char>(fixture_file)), {});
    verify_binary(fixture, release);
    auto wrong = release;
    wrong.version = "0.0.0";
    rejects([&] { verify_binary(fixture, wrong); }, "native product/file version must match manifest");
    wrong = release;
    wrong.sha256 = hash;
    rejects([&] { verify_binary(fixture, wrong); }, "detect corrupt digest");
    auto corrupt = cache / "bad.exe";
    std::filesystem::copy_file(fixture, corrupt);
    {
        std::fstream file(corrupt, std::ios::in | std::ios::out | std::ios::binary);
        file.put('X');
    }
    wrong = release;
    wrong.sha256 = file_sha256(corrupt);
    rejects([&] { verify_binary(corrupt, wrong); }, "valid hash cannot admit non-PE file");
    int attempts = 0;
    release.binary_api_url = api + "assets/11";
    feed::Fetch fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        ++attempts;
        if (request.url == release.binary_url) {
            emit(sink, "truncated");
            throw Error("truncated", FailureKind::Network);
        }
        std::ifstream file(fixture, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)), {});
        emit(sink, bytes);
    };
    auto package = feed::download_release(release, cache / "downloads", fetch);
    require(attempts == 2 && file_sha256(package.executable) == release.sha256,
            "download candidate retry discards partial bytes");
    require(!std::filesystem::exists(package.directory / "Glint.exe.part"), "remove .part after success");
    feed::download_release(release, cache / "downloads", fetch);
    require(attempts == 2, "reuse verified cached download");
    {
        std::fstream file(package.executable, std::ios::in | std::ios::out | std::ios::binary);
        file.put('X');
    }
    feed::download_release(release, cache / "downloads", fetch);
    require(attempts == 4, "replace corrupt cached file");
    auto fallback = release;
    fallback.size = 0;
    const auto normalized = feed::download_release(fallback, cache / "downloads", fetch);
    require(normalized.release.size == std::filesystem::file_size(fixture),
            "cached fallback package records verified size");
    int limited_attempts = 0;
    feed::Fetch limited = [&](const feed::Request&, const feed::Sink& sink, const Cancel&, const Progress&) {
        if (++limited_attempts == 1) throw Error("limited binary", FailureKind::RateLimit, 180);
        std::ifstream file(fixture, std::ios::binary);
        emit(sink, std::string((std::istreambuf_iterator<char>(file)), {}));
    };
    const auto limited_package = feed::download_release(release, cache / "limited", limited);
    require(limited_attempts == 2 && file_sha256(limited_package.executable) == release.sha256,
            "binary rate limit still permits an independent source to complete");
    std::vector<std::string> binary_sources{
        release.binary_url, release.binary_api_url, manifest_roots[1] + "v" + release.version + "/Glint.exe"};
    for (const auto &proxy : proxies)
        binary_sources.push_back(proxy + release.binary_url);
    std::vector<std::string> calls;
    for (std::size_t selected = 0; selected < binary_sources.size(); ++selected) {
        calls.clear();
        feed::Fetch mirrors = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
            calls.push_back(request.url);
            require(request.maximum_bytes == release.size, "binary mirror retains API size bound");
            require(request.octet_stream == (request.url == release.binary_api_url),
                    "binary API asset retains octet-stream header");
            if (request.url != binary_sources[selected])
                throw Error("binary source unavailable", FailureKind::Network);
            emit(sink, fixture_bytes);
        };
        const auto downloaded = feed::download_release(release, cache / ("mirror-" + std::to_string(selected)), mirrors);
        const std::vector<std::string> expected(binary_sources.begin(), binary_sources.begin() + selected + 1);
        require(calls == expected, "binary fallback tries direct, API, pinned Pages, then all four proxies in order");
        require(file_sha256(downloaded.executable) == release.sha256 && downloaded.release.size == release.size,
                "every binary source produces a verified package");
        for (const auto &entry : std::filesystem::directory_iterator(downloaded.directory))
            require(entry.path().filename() == L"Glint.exe", "successful mirror attempt removes temporary files");
    }
    calls.clear();
    auto without_api = release;
    without_api.binary_api_url.clear();
    without_api.size = 0;
    const auto tagged_api = api + "tags/v" + release.version;
    feed::Fetch direct_manifest = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        require(request.url == release.binary_url && !request.octet_stream && request.maximum_bytes == feed::kBinaryLimit,
                "successful manifest binary never queries API metadata");
        emit(sink, fixture_bytes);
    };
    const auto direct_manifest_package = feed::download_release(without_api, cache / "manifest-direct", direct_manifest);
    require(calls == std::vector<std::string>{release.binary_url} &&
                file_sha256(direct_manifest_package.executable) == release.sha256,
            "manifest-only release makes one direct request when the binary is available");
    calls.clear();
    feed::Fetch lazy_api = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        if (request.url == release.binary_url)
            throw Error("direct binary unavailable", FailureKind::Network);
        if (request.url == tagged_api) {
            require(!request.octet_stream && request.maximum_bytes == feed::kMetadataLimit,
                    "lazy API lookup requests bounded JSON for the chosen tag");
            emit(sink, metadata_for(release.version, release.size));
            return;
        }
        require(request.url == release.binary_api_url && request.octet_stream,
                "successful lazy lookup supplies the API asset backup");
        emit(sink, fixture_bytes);
    };
    const auto lazy_package = feed::download_release(without_api, cache / "lazy-api", lazy_api);
    require(calls == std::vector<std::string>{release.binary_url, tagged_api, release.binary_api_url} &&
                lazy_package.release.version == release.version && lazy_package.release.sha256 == release.sha256 &&
                file_sha256(lazy_package.executable) == release.sha256,
            "direct failure lazily resolves same-version API asset without replacing the chosen manifest digest");
    for (const int scenario : {0, 1, 2}) {
        calls.clear();
        feed::Fetch failed_lookup = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
            calls.push_back(request.url);
            if (request.url == release.binary_url)
                throw Error("direct binary unavailable", FailureKind::Network);
            if (request.url == tagged_api) {
                require(!request.octet_stream && request.maximum_bytes == feed::kMetadataLimit,
                        "failed lazy lookup still requests bounded tag metadata");
                if (scenario == 0)
                    throw Error("API unavailable", FailureKind::Network);
                if (scenario == 1)
                    throw Error("API limited", FailureKind::RateLimit, 600);
                emit(sink, metadata_for(release.version == "1.2.3" ? "1.2.4" : "1.2.3", release.size));
                return;
            }
            require(request.url == binary_sources[2] && !request.octet_stream,
                    "unusable tag metadata must proceed to the chosen version on Pages");
            emit(sink, fixture_bytes);
        };
        const auto recovered_lookup = feed::download_release(without_api, cache / ("lookup-failure-" + std::to_string(scenario)), failed_lookup);
        require(calls == std::vector<std::string>{release.binary_url, tagged_api, binary_sources[2]} &&
                    recovered_lookup.release.version == release.version && recovered_lookup.release.sha256 == release.sha256 &&
                    file_sha256(recovered_lookup.executable) == release.sha256,
                "unavailable, limited or mismatched API lookup cannot replace chosen version or digest and still permits Pages");
    }
    calls.clear();
    feed::Fetch no_api = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        if (request.url == tagged_api) {
            require(!request.octet_stream && request.maximum_bytes == feed::kMetadataLimit,
                    "manifest-only backup queries tag metadata after direct binary failure");
            throw Error("API unavailable", FailureKind::Network);
        }
        require(request.maximum_bytes == feed::kBinaryLimit && !request.octet_stream,
                "manifest-only binary uses bounded downloads without an API asset header");
        if (request.url != binary_sources.back())
            throw Error("binary source unavailable", FailureKind::Network);
        emit(sink, fixture_bytes);
    };
    const auto without_api_package = feed::download_release(without_api, cache / "without-api", no_api);
    auto expected_without_api = binary_sources;
    expected_without_api[1] = tagged_api;
    require(calls == expected_without_api && file_sha256(without_api_package.executable) == release.sha256 &&
                without_api_package.release.size == release.size,
            "manifest-only release tries lazy API lookup before Pages and every proxy backup");
    calls.clear();
    feed::Fetch corrupt_mirrors = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        if (request.url == release.binary_url)
            throw Error("direct unavailable", FailureKind::Network);
        if (request.url == release.binary_api_url)
            throw Error("API limited", FailureKind::RateLimit, 180);
        if (request.url == binary_sources[2]) {
            auto bytes = fixture_bytes;
            bytes.back() ^= 1;
            emit(sink, bytes);
            return;
        }
        if (request.url == binary_sources[3]) {
            emit(sink, fixture_bytes.substr(0, fixture_bytes.size() / 2));
            return;
        }
        emit(sink, fixture_bytes);
    };
    const auto recovered = feed::download_release(release, cache / "corrupt-mirrors", corrupt_mirrors);
    require(calls == std::vector<std::string>(binary_sources.begin(), binary_sources.begin() + 5) &&
                file_sha256(recovered.executable) == release.sha256,
            "hash-corrupt and truncated mirror bytes are discarded before a later proxy succeeds");
    for (const auto &entry : std::filesystem::directory_iterator(recovered.directory))
        require(entry.path().filename() == L"Glint.exe", "failed mirror candidates leave no partial bytes");
    for (const bool all_limited : {false, true}) {
        calls.clear();
        feed::Fetch unavailable = [&](const feed::Request &request, const feed::Sink &, const Cancel &, const Progress &) {
            calls.push_back(request.url);
            if (all_limited || request.url == release.binary_url)
                throw Error("limited", FailureKind::RateLimit, 180);
            throw Error("network unavailable", FailureKind::Network);
        };
        try {
            feed::download_release(release, cache / (all_limited ? "all-limited" : "mixed-failure"), unavailable);
            throw std::runtime_error("all unavailable binary sources must fail");
        } catch (const Error &error) {
            require(all_limited ? error.kind == FailureKind::RateLimit && error.retry_after_seconds == 180
                                : error.kind != FailureKind::RateLimit,
                    "global cooldown is reported only when every binary source is limited");
            require(calls == binary_sources, "all-source binary failure exhausts every independent origin");
        }
    }
    for (const auto kind : {FailureKind::Cancelled, FailureKind::Filesystem}) {
        calls.clear();
        feed::Fetch terminal = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
            calls.push_back(request.url);
            emit(sink, "partial");
            throw Error("stop", kind);
        };
        try {
            feed::download_release(release, cache / (kind == FailureKind::Cancelled ? "cancelled-source" : "filesystem-source"), terminal);
            throw std::runtime_error("terminal binary failure must escape");
        } catch (const Error &error) {
            require(error.kind == kind && calls.size() == 1, "cancellation and filesystem failure stop binary fallback");
        }
    }
    bool nested = false;
    feed::Fetch overlapping = [&](const feed::Request&, const feed::Sink& sink, const Cancel&, const Progress&) {
        if (!nested) {
            nested = true;
            auto second = feed::download_release(release, cache / "overlapping", overlapping);
            require(file_sha256(second.executable) == release.sha256, "overlapping download completes independently");
        }
        std::ifstream file(fixture, std::ios::binary);
        emit(sink, std::string((std::istreambuf_iterator<char>(file)), {}));
    };
    const auto overlap = feed::download_release(release, cache / "overlapping", overlapping);
    require(file_sha256(overlap.executable) == release.sha256, "each attempt owns its staging directory");
    for (const auto& entry : std::filesystem::directory_iterator(overlap.directory))
        require(entry.path().filename() == L"Glint.exe", "temporary attempts are cleaned after completion");
    auto link = cache / "junction";
    const std::wstring command =
        L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + (cache / "downloads").wstring() + L"\" >nul";
    require(_wsystem(command.c_str()) == 0, "create junction fixture");
    rejects([&] { feed::download_release(release, link, fetch); }, "reject a cache junction");
    std::filesystem::remove(link);
    rejects([&] { feed::download_release(release, cache / "cancel", fetch, [] { return true; }); }, "cancel download");
}
} // namespace
int main(int argc, char **argv) {
    try {
        require(argc == 2, "pass built Glint.exe as offline PE fixture");
        parsers_and_urls();
        source_cooldowns();
        source_fallbacks();
        binaries(std::filesystem::path(argv[1]));
        std::cout << "update feed tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "update feed test failed: " << error.what() << '\n';
        return 1;
    }
}
