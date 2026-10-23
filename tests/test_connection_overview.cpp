#include "connection_overview.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

glint::ConnectionInfo profile(glint::ConnectionId id, std::string profile_id,
                              std::wstring browser, bool synchronized,
                              std::size_t tabs = 0, std::wstring label = {}) {
    return {id, "session", std::move(profile_id), std::move(browser),
            std::move(label), synchronized, tabs};
}

void test_connection_lifecycle() {
    glint::ConnectionOverview overview;
    overview.update({});
    require(overview.profiles().empty(), "An empty overview must not invent browser profiles");

    std::vector live{profile(1, "work", L"Chrome", false, 12, L"工作")};
    overview.update(live);
    require(overview.profiles().size() == 1, "A browser hello must add its profile to the overview");
    require(overview.connected_count() == 0, "A hello without a snapshot must not count as connected");
    require(overview.profiles()[0].state == glint::ProfileConnectionState::synchronizing,
            "An unsynchronized browser must show synchronization in progress");
    require(overview.profiles()[0].tab_count == 0, "Unsynchronized tabs must not be shown as current");
    require(overview.profiles()[0].name() == L"chrome-工作", "A labeled profile must include its browser");

    live[0].synchronized = true;
    overview.update(live);
    require(overview.connected_count() == 1 && overview.profiles()[0].tab_count == 12,
            "A snapshot must make the profile connected and show its tab count");
    require(overview.name_for(1) == L"chrome-工作", "Search rows must use the same profile name");

    overview.update({});
    require(overview.profiles().size() == 1 && overview.connected_count() == 0,
            "A disconnected browser must stay visible without counting as connected");
    require(overview.profiles()[0].state == glint::ProfileConnectionState::disconnected &&
                overview.profiles()[0].tab_count == 0,
            "A disconnected profile must clear its stale tab count");
    require(overview.name_for(1).empty(), "Disconnected connections must not label current search rows");

    live[0].connection_id = 7;
    live[0].profile_label = L"个人";
    live[0].tab_count = 0;
    overview.update(live);
    require(overview.profiles().size() == 1 && overview.connected_count() == 1,
            "A reconnect must reuse the browser-profile row and count an empty snapshot");
    require(overview.profiles()[0].name() == L"chrome-个人", "A reconnect must refresh the friendly label");
}

void test_profile_counts_and_reconnects() {
    glint::ConnectionOverview overview;
    std::vector live{profile(1, "a", L"cHrOmE", true, 3),
                     profile(2, "b", L"chrome", true, 4),
                     profile(3, "a", L"EDGE", true, 5)};
    overview.update(live);
    require(overview.connected_count() == 3, "Browser-profile pairs must count independently");
    require(overview.name_for(1) == L"chrome-配置 1" && overview.name_for(2) == L"chrome-配置 2" &&
                overview.name_for(3) == L"edge-配置 1",
            "Unnamed profiles must have stable readable names within their browser");

    live.push_back(profile(4, "a", L"chrome", false));
    overview.update(live);
    require(overview.profiles().size() == 3 && overview.connected_count() == 3,
            "A reconnecting transport must not duplicate the browser profile");
    require(overview.profiles()[0].tab_count == 3 && overview.name_for(4) == L"chrome-配置 1",
            "A usable snapshot must remain visible while a replacement transport synchronizes");

    live[3].synchronized = true;
    live[3].tab_count = 9;
    overview.update(live);
    require(overview.profiles()[0].tab_count == 9,
            "The newest synchronized transport must supply the current count without double counting");
    overview.update(std::span(live).subspan(1, 2));
    require(overview.profiles()[0].state == glint::ProfileConnectionState::disconnected &&
                overview.name_for(2) == L"chrome-配置 2",
            "Disconnecting one profile must preserve the other profiles and their names");
}

} // namespace

int main() {
    glint::i18n::current_language = glint::i18n::Language::Chinese;
    test_connection_lifecycle();
    test_profile_counts_and_reconnects();
    glint::ConnectionOverview overview;
    std::vector live{profile(1, "unnamed", L"", true), profile(2, "named", L"chrome", true, 0, L"工作")};
    overview.update(live);
    require(overview.name_for(1) == L"浏览器-配置 1", "Chinese fallback profile name");
    glint::i18n::current_language = glint::i18n::Language::English;
    require(overview.name_for(1) == L"Browser-Profile 1", "Language switch must translate existing fallback names");
    require(overview.name_for(2) == L"chrome-工作", "Language switch must preserve user labels");
    overview.update(live);
    require(overview.profiles().size() == 2, "Language switch must not change browser identity");
    std::cout << "Connection overview tests passed\n";
}
