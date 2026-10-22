#pragma once

#include "connection_overview.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace glint::setup {

struct ConnectionSummary {
    std::size_t profiles = 0;
    std::size_t tabs = 0;
};

inline ConnectionSummary summarize_connections(std::span<const glint::ConnectionInfo> connections,
                                               bool edge) {
    const std::wstring_view selected_browser = edge ? L"edge" : L"chrome";
    ConnectionOverview overview;
    overview.update(connections);
    ConnectionSummary summary;
    for (const auto& profile : overview.profiles()) {
        if (profile.state != ProfileConnectionState::connected || profile.browser != selected_browser) continue;
        ++summary.profiles;
        summary.tabs += profile.tab_count;
    }
    return summary;
}

} // namespace glint::setup
