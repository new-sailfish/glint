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
        const auto encoded = Json(source).dump();
        assert(Json::parse(encoded).dump() == encoded);
        const std::string escaped = "{\"s\":\"\\ud83d\\ude00\"}";
        const auto parsed_escaped = Json::parse(escaped);
        assert(parsed_escaped.at("s").as_string() == "😀");
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
        assert(index.apply(11, Json::Object{{"type", "snapshot"}, {"version", 1}, {"tabs", Json::Array{
            tab(1, "Docs", "https://example.test/docs", true), tab(2, "中文页面", "https://example.test/cn")}}}));
        assert(index.apply(22, Json::Object{{"type", "snapshot"}, {"version", 1}, {"tabs", Json::Array{
            tab(1, "Mail", "https://mail.test")}}}));
        assert(index.connection_count() == 2 && index.tab_count() == 3);
        assert(index.contains({11, 1, glint::TabSource::Browser, L""}));
        assert(!index.contains({11, 1, glint::TabSource::Window, L"w:11:1"}));
        assert(!index.contains({11, 1, glint::TabSource::UiTab, L"u:11:1"}));
        assert(index.search(L"DOC").size() == 1);
        assert(index.search(L"中文").size() == 1);
        assert(index.search(L"").size() == 3);
        assert(index.apply(11, Json::Object{{"type", "remove"}, {"version", 1}, {"tabId", 1}}));
        assert(index.search(L"docs").empty());
        assert(!index.apply(99, Json::Object{{"type", "remove"}, {"version", 1}, {"tabId", 1}}));
        assert(index.disconnect(22) && index.connection_count() == 1 && index.tab_count() == 1);
    }
    {
        glint::TabIndex index;
        assert(index.apply(11, hello("Chrome", "Default")));
        auto recent = tab(1, "Recent", "https://recent.test");
        recent["lastAccessed"] = std::int64_t{1730000000123};
        assert(index.apply(11, Json::Object{{"type", "snapshot"}, {"version", 1},
            {"tabs", Json::Array{recent, tab(2, "Legacy", "https://legacy.test")}}}));
        auto rows = index.search(L"");
        assert(rows[0].last_accessed == 0);
        assert(rows[1].last_accessed == 0);
        for (const auto& invalid : {Json(-1), Json("yesterday"), Json(1.5)}) {
            auto malformed = recent;
            malformed["lastAccessed"] = invalid;
            assert(!index.apply(11, Json::Object{{"type", "upsert"}, {"version", 1}, {"tab", malformed}}));
            assert(index.search(L"Recent")[0].last_accessed == 0);
        }
        recent["lastAccessed"] = std::int64_t{1730000000999};
        assert(index.apply(11, Json::Object{{"type", "upsert"}, {"version", 1}, {"tab", recent}}));
        assert(index.search(L"Recent")[0].last_accessed == 0);
    }
    {
        glint::TabIndex index;
        assert(index.apply(11, hello("Chrome", "Default")));
        assert(index.apply(22, hello("Edge", "Work")));
        const Json snapshot = Json::Object{{"type", "snapshot"}, {"version", 1},
            {"tabs", Json::Array{tab(1, "First", "https://first.test"), tab(2, "Second", "https://second.test")}}};
        assert(index.apply(11, snapshot));
        assert(index.apply(22, snapshot));
        const auto result = [](const char* request, bool ok) {
            return Json(Json::Object{{"type", "activate_result"}, {"version", 1}, {"requestId", request}, {"ok", ok}});
        };
        const glint::TabKey second{11, 2};
        assert(index.track_activation(second, "card", 2000));
        // Older extensions send only this acknowledgement, with no access-time upsert.
        assert(index.apply(11, result("card", true)));
        assert(index.search(L"Second")[0].last_accessed == 2000);
        assert(index.search(L"Second")[1].last_accessed == 0);
        assert(index.apply(11, Json::Object{{"type", "upsert"}, {"version", 1},
            {"tab", tab(2, "Second renamed", "https://second.test")}}));
        assert(index.search(L"Second")[0].last_accessed == 2000);
        assert(index.apply(11, snapshot));
        assert(index.search(L"Second")[0].last_accessed == 2000);

        assert(index.track_activation(second, "next", 3000));
        assert(index.apply(22, result("next", true))); // Wrong connection cannot confirm it.
        assert(index.apply(11, result("unknown", true)));
        assert(index.search(L"Second")[0].last_accessed == 2000);
        assert(index.apply(11, result("next", false)));
        assert(index.apply(11, result("next", true))); // Failure consumed the request.
        assert(index.search(L"Second")[0].last_accessed == 2000);

        assert(index.track_activation(second, "older", 2500));
        assert(index.track_activation(second, "newer", 4000));
        assert(index.apply(11, result("newer", true)));
        assert(index.apply(11, result("older", true)));
        assert(index.search(L"Second")[0].last_accessed == 4000);
        auto visited = tab(2, "Second", "https://second.test");
        visited["lastAccessed"] = 5000;
        assert(index.apply(11, Json::Object{{"type", "upsert"}, {"version", 1}, {"tab", visited}}));
        assert(index.search(L"Second")[0].last_accessed == 4000);

        assert(index.track_activation(second, "closed", 6000));
        assert(index.apply(11, Json::Object{{"type", "remove"}, {"version", 1}, {"tabId", 2}}));
        assert(index.apply(11, snapshot));
        assert(index.apply(11, result("closed", true)));
        assert(index.search(L"Second")[0].last_accessed == 0);
        assert(index.track_activation(second, "session", 7000));
        assert(index.apply(11, hello("Chrome", "Replacement session")));
        assert(index.apply(11, snapshot));
        assert(index.apply(11, result("session", true)));
        assert(index.search(L"Second")[0].last_accessed == 0);
        assert(!index.track_activation({11, 99}, "missing", 8000));
        assert(!index.track_activation({11, 2, glint::TabSource::Window}, "native", 8000));
    }
    std::cout << "core tests passed\n";
}
