#pragma once

#include "glint/core.hpp"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace glint {

enum class ProfileConnectionState { disconnected, synchronizing, connected };

struct ProfileConnection {
    std::wstring browser;
    std::string profile_id;
    std::wstring profile_label;
    std::size_t unnamed_number = 0;
    ProfileConnectionState state = ProfileConnectionState::disconnected;
    std::size_t tab_count = 0;
    std::vector<ConnectionId> connection_ids;
    ConnectionId current_connection_id = 0;

    std::wstring name() const {
        return browser + L"-" + (profile_label.empty()
            ? L"配置 " + std::to_wstring(unnamed_number) : profile_label);
    }
};

// Remembers profiles observed during this desktop session so a disconnected
// browser remains visible without keeping its stale tabs in the search index.
class ConnectionOverview {
public:
    void update(std::span<const ConnectionInfo> connections) {
        for (auto& profile : profiles_) {
            profile.state = ProfileConnectionState::disconnected;
            profile.tab_count = 0;
            profile.connection_ids.clear();
            profile.current_connection_id = 0;
        }
        for (const auto& connection : connections) {
            if (connection.connection_id == 0) continue;
            std::wstring browser = connection.browser;
            std::transform(browser.begin(), browser.end(), browser.begin(),
                [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (browser.empty()) browser = L"浏览器";
            const std::string profile_id = connection.profile_id.empty()
                ? "connection-" + std::to_string(connection.connection_id) : connection.profile_id;
            auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto& profile) {
                return profile.browser == browser && profile.profile_id == profile_id;
            });
            if (found == profiles_.end()) {
                const auto number = static_cast<std::size_t>(std::count_if(profiles_.begin(), profiles_.end(),
                    [&](const auto& profile) { return profile.browser == browser; })) + 1;
                ProfileConnection profile;
                profile.browser = std::move(browser);
                profile.profile_id = profile_id;
                profile.unnamed_number = number;
                profiles_.push_back(std::move(profile));
                found = std::prev(profiles_.end());
            }
            auto& profile = *found;
            profile.connection_ids.push_back(connection.connection_id);
            const bool choose = connection.synchronized
                ? profile.state != ProfileConnectionState::connected ||
                    connection.connection_id > profile.current_connection_id
                : profile.state == ProfileConnectionState::disconnected ||
                    (profile.state == ProfileConnectionState::synchronizing &&
                     connection.connection_id > profile.current_connection_id);
            if (!choose) continue;
            profile.current_connection_id = connection.connection_id;
            profile.profile_label = connection.profile_label;
            profile.state = connection.synchronized
                ? ProfileConnectionState::connected : ProfileConnectionState::synchronizing;
            profile.tab_count = connection.synchronized ? connection.tab_count : 0;
        }
    }
    const std::vector<ProfileConnection>& profiles() const { return profiles_; }
    std::size_t connected_count() const {
        return static_cast<std::size_t>(std::count_if(profiles_.begin(), profiles_.end(), [](const auto& profile) {
            return profile.state == ProfileConnectionState::connected;
        }));
    }
    std::wstring name_for(ConnectionId id) const {
        for (const auto& profile : profiles_) {
            if (std::find(profile.connection_ids.begin(), profile.connection_ids.end(), id) != profile.connection_ids.end())
                return profile.name();
        }
        return {};
    }

private:
    std::vector<ProfileConnection> profiles_;
};

} // namespace glint
