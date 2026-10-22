#include "setup_progress.hpp"

#include <cstdlib>
#include <iostream>
#include <utility>

using glint::Json;

static void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

static Json hello(const char* browser, const char* profile = "Default") {
    return Json::Object{{"type", "hello"}, {"version", 1}, {"sessionId", profile},
                        {"profileId", profile}, {"browser", browser}, {"profileLabel", profile}};
}

static Json tab(std::int64_t id) {
    return Json::Object{{"tabId", id}, {"windowId", 1}, {"index", id},
                        {"title", "A tab"}, {"url", "https://example.test"},
                        {"active", false}, {"pinned", false}};
}

static Json snapshot(Json::Array tabs = {}) {
    return Json::Object{{"type", "snapshot"}, {"version", 1}, {"tabs", std::move(tabs)}};
}

static void expect_summary(const glint::TabIndex& index, bool edge,
                           std::size_t profiles, std::size_t tabs, const char* message) {
    const auto connections = index.connections();
    const auto summary = glint::setup::summarize_connections(connections, edge);
    require(summary.profiles == profiles && summary.tabs == tabs, message);
}

static void test_connection_lifecycle() {
    glint::TabIndex index;
    expect_summary(index, false, 0, 0, "No connections must leave Chrome waiting");
    expect_summary(index, true, 0, 0, "No connections must leave Edge waiting");

    require(index.apply(1, hello("Chrome")), "Chrome hello must be accepted");
    expect_summary(index, false, 0, 0, "A hello without a snapshot must leave Chrome waiting");
    require(index.apply(1, snapshot({tab(1), tab(2)})), "Chrome snapshot must be accepted");
    expect_summary(index, false, 1, 2, "A synchronized Chrome connection must count its tabs");
    expect_summary(index, true, 0, 0, "Chrome synchronization must not complete Edge setup");

    require(index.apply(2, hello("Edge")), "Edge hello must be accepted");
    expect_summary(index, true, 0, 0, "Edge hello must wait even when Chrome has synchronized");
    require(index.apply(2, snapshot()), "Empty Edge snapshot must be accepted");
    expect_summary(index, true, 1, 0, "An empty snapshot must count as a synchronized Edge profile");
    expect_summary(index, false, 1, 2, "Edge synchronization must not affect Chrome totals");

    require(index.disconnect(1), "Chrome disconnect must be accepted");
    expect_summary(index, false, 0, 0, "A disconnected Chrome profile must stop completing setup");
    expect_summary(index, true, 1, 0, "Chrome disconnect must preserve Edge readiness");
    require(index.disconnect(2), "Edge disconnect must be accepted");
    expect_summary(index, true, 0, 0, "A disconnected Edge profile must stop completing setup");
}

static void test_selected_browser_and_multiple_connections() {
    glint::TabIndex index;
    require(index.apply(1, hello("cHrOmE", "Shared")), "Mixed-case Chrome hello must be accepted");
    require(index.apply(1, snapshot({tab(1)})), "First Chrome snapshot must be accepted");
    require(index.apply(2, hello("CHROME", "Shared")), "Uppercase Chrome hello must be accepted");
    require(index.apply(2, snapshot({tab(1), tab(2)})), "Second Chrome snapshot must be accepted");
    require(index.apply(3, hello("eDgE")), "Mixed-case Edge hello must be accepted");
    require(index.apply(3, snapshot({tab(1), tab(2), tab(3)})), "Edge snapshot must be accepted");
    require(index.apply(4, hello("Chromium")), "Other browser hello must be accepted");
    require(index.apply(4, snapshot({tab(1)})), "Other browser snapshot must be accepted");
    require(index.apply(5, hello("Chrome Canary")), "Browser suffix hello must be accepted");
    require(index.apply(5, snapshot({tab(1)})), "Browser suffix snapshot must be accepted");
    require(index.apply(6, hello("Microsoft Edge")), "Browser prefix hello must be accepted");
    require(index.apply(6, snapshot({tab(1)})), "Browser prefix snapshot must be accepted");
    require(index.apply(7, hello("Chrome")), "Pending Chrome hello must be accepted");
    require(index.apply(8, hello("Edge")), "Pending Edge hello must be accepted");

    expect_summary(index, false, 1, 2,
                   "Chrome must count each profile once using its newest synchronized transport");
    expect_summary(index, true, 1, 3,
                   "Edge must exclude Chrome, similar names, and unsynchronized Edge connections");

    require(index.apply(1, hello("Chrome", "Shared")), "A repeated hello must be accepted");
    expect_summary(index, false, 1, 2, "A repeated hello must preserve readiness from the profile's other synchronized transport");
    require(index.apply(1, snapshot()), "Empty Chrome snapshot must be accepted");
    expect_summary(index, false, 1, 2, "An older empty snapshot must not replace the newest synchronized transport");
    require(index.disconnect(2), "The newest Chrome transport must disconnect");
    expect_summary(index, false, 1, 0, "The remaining synchronized transport must preserve readiness with its empty snapshot");
    require(index.apply(9, hello("Chrome", "Work")), "A separate Chrome profile hello must be accepted");
    require(index.apply(9, snapshot({tab(1), tab(2)})), "A separate Chrome profile snapshot must be accepted");
    expect_summary(index, false, 2, 2, "Distinct Chrome profiles must count independently");
}

static void test_tab_totals_follow_index_updates() {
    glint::TabIndex index;
    require(index.apply(1, hello("Chrome")), "Chrome hello must be accepted");
    require(index.apply(1, snapshot()), "Empty Chrome snapshot must be accepted");
    expect_summary(index, false, 1, 0, "Chrome can synchronize with zero tabs");
    require(index.apply(1, Json::Object{{"type", "upsert"}, {"version", 1}, {"tab", tab(9)}}),
            "Tab upsert must be accepted");
    expect_summary(index, false, 1, 1, "The summary must use the current indexed tab count");
    require(index.apply(1, Json::Object{{"type", "remove"}, {"version", 1}, {"tabId", 9}}),
            "Tab removal must be accepted");
    expect_summary(index, false, 1, 0, "Removing the final tab must preserve profile readiness");
}

int main() {
    const glint::setup::ConnectionSummary empty;
    require(empty.profiles == 0 && empty.tabs == 0, "ConnectionSummary must default to zero");
    test_connection_lifecycle();
    test_selected_browser_and_multiple_connections();
    test_tab_totals_follow_index_updates();
    std::cout << "setup progress tests passed\n";
}
