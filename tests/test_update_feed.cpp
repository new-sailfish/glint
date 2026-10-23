#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "glint/core.hpp"
#include "update_feed.hpp"
#include <windows.h>

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
const std::string hash(64, 'a');
std::string metadata() {
    return R"({"tag_name":"v1.2.3","draft":false,"prerelease":false,"html_url":")" + root +
           R"(tag/v1.2.3","assets":[{"name":"Glint.exe","size":4096,"browser_download_url":")" + root +
           R"(download/v1.2.3/Glint.exe","url":")" + api +
           R"(assets/11"},{"name":"release-manifest.json","size":400,"browser_download_url":")" + root +
           R"(download/v1.2.3/release-manifest.json","url":")" + api + R"(assets/12"}]})";
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
    require(feed::allowed_url(
                "https://release-assets.githubusercontent.com/github-production-release-asset/123/abc?sig=ok"),
            "allow GitHub asset CDN");
    for (auto url : {"http://github.com/new-sailfish/glint/releases/latest",
                     "https://github.com:444/new-sailfish/glint/releases/latest",
                     "https://user@github.com/new-sailfish/glint/releases/latest",
                     "https://github.com/other/glint/releases/latest", "https://evil.example/Glint.exe",
                     "https://github.com/new-sailfish/glint/releases/../evil",
                     "https://github.com/new-sailfish/glint/releases/%2e%2e/evil",
                     "https://release-assets.githubusercontent.com.evil.example/x",
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
void source_fallbacks() {
    std::vector<std::string> calls;
    feed::Fetch fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        if (request.url == api + "latest") {
            emit(sink, metadata());
            return;
        }
        if (request.url == api + "assets/12") {
            require(request.octet_stream, "API assets require octet-stream");
            emit(sink, manifest());
            return;
        }
        throw Error("browser source unavailable", FailureKind::Network);
    };
    auto release = feed::find_release("0.0.0", fetch);
    require(release && release->sha256 == hash && calls.size() == 3,
            "manifest browser failure falls back to pinned API asset");
    calls.clear();
    fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        if (request.url == api + "latest")
            throw Error("API unavailable", FailureKind::Network);
        emit(sink, manifest());
    };
    release = feed::find_release("0.0.0", fetch);
    require(release && release->binary_url == root + "download/v1.2.3/Glint.exe" && calls.size() == 2,
            "API outage uses official manifest with pinned EXE");
    require(!feed::find_release("999.0.1", fetch), "local version is not downgraded");
    rejects([&] { feed::find_release("0.0.0", fetch, [] { return true; }); }, "cancel before source request");
    calls.clear();
    fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        if (request.url == api + "latest") throw Error("limited", FailureKind::RateLimit, 600);
        emit(sink, manifest());
    };
    try {
        feed::find_release("0.0.0", fetch);
        throw std::runtime_error("rate limit not thrown");
    } catch (const Error &error) {
        require(error.kind == FailureKind::RateLimit && error.retry_after_seconds == 600, "preserve retry-after");
        require(calls.size() == 1, "rate limit stops source attempts rather than clearing cooldown on success");
    }
    calls.clear();
    fetch = [&](const feed::Request &request, const feed::Sink &sink, const Cancel &, const Progress &) {
        calls.push_back(request.url);
        if (request.url == api + "latest") emit(sink, metadata());
        else if (request.url == api + "assets/12") emit(sink, manifest());
        else throw Error("limited manifest", FailureKind::RateLimit, 300);
    };
    try {
        feed::find_release("0.0.0", fetch);
        throw std::runtime_error("manifest rate limit not thrown");
    } catch (const Error &error) {
        require(error.kind == FailureKind::RateLimit && error.retry_after_seconds == 300 && calls.size() == 2,
                "manifest rate limit stops source attempts");
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
    try {
        feed::download_release(release, cache / "limited", limited);
        throw std::runtime_error("binary rate limit not thrown");
    } catch (const Error& error) {
        require(error.kind == FailureKind::RateLimit && error.retry_after_seconds == 180 && limited_attempts == 1,
                "binary rate limit stops source attempts");
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
        source_fallbacks();
        binaries(std::filesystem::path(argv[1]));
        std::cout << "update feed tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "update feed test failed: " << error.what() << '\n';
        return 1;
    }
}
