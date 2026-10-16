#include "glint/core.hpp"

#include <cassert>
#include <iostream>

using glint::Json;

static Json hello(const char* browser, const char* profile) {
    return Json::Object{{"type", "hello"}, {"version", 1}, {"sessionId", profile},
                        {"profileId", profile}, {"browser", browser}, {"profileLabel", profile}};
}

static Json tab(std::int64_t id, const char* title, const char* url, bool active = false) {
    return Json::Object{{"tabId", id}, {"windowId", 7}, {"index", id}, {"title", title},
                        {"url", url}, {"active", active}, {"pinned", false}};
}

int main() {
    {
        const auto source = Json::Object{{"message", "中文 😀\n"}, {"ok", true}, {"n", 42}};
        const auto encoded = source.dump();
        assert(Json::parse(encoded).dump() == encoded);
        assert(Json::parse(R"({"s":"\ud83d\ude00"})").at("s").as_string() == "😀");
        bool rejected = false;
        try { Json::parse(R"({"x":1,"x":2})"); } catch (const glint::ProtocolError&) { rejected = true; }
        assert(rejected);
    }
    {
        const std::string one = R"({"type":"remove","tabId":2})";
        const auto frame = glint::encode_frame(one);
        glint::FrameReader reader;
        std::vector<std::uint8_t> first(frame.begin(), frame.begin() + 2);
        assert(reader.feed(first).empty());
        assert(reader.has_partial_frame());
        auto second = std::vector<std::uint8_t>(frame.begin() + 2, frame.end());
        auto messages = reader.feed(second);
        assert(messages.size() == 1 && messages.front() == one);
    }
    {
        glint::TabIndex index;
        assert(index.apply(11, hello("Chrome", "Default")));
        assert(index.apply(22, hello("Edge", "Work")));
        assert(index.apply(11, Json::Object{{"type", "snapshot"}, {"tabs", Json::Array{
            tab(1, "Docs", "https://example.test/docs", true), tab(2, "中文页面", "https://example.test/cn")}}}));
        assert(index.apply(22, Json::Object{{"type", "snapshot"}, {"tabs", Json::Array{
            tab(1, "Mail", "https://mail.test")}}}));
        assert(index.connection_count() == 2 && index.tab_count() == 3);
        assert(index.search(L"DOC").size() == 1);
        assert(index.search(L"中文").size() == 1);
        assert(index.search(L"").size() == 3);
        assert(index.apply(11, Json::Object{{"type", "remove"}, {"tabId", 1}}));
        assert(index.search(L"docs").empty());
        assert(!index.apply(99, Json::Object{{"type", "remove"}, {"tabId", 1}}));
        assert(index.disconnect(22) && index.connection_count() == 1 && index.tab_count() == 1);
    }
    std::cout << "core tests passed\n";
}
