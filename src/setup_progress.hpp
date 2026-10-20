#pragma once

#include "glint/core.hpp"

#include <algorithm>
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
    ConnectionSummary summary;
    for (const auto& info : connections) {
        if (!info.synchronized || info.browser.size() != selected_browser.size()) continue;
        const bool matches = std::equal(info.browser.begin(), info.browser.end(), selected_browser.begin(),
            [](wchar_t actual, wchar_t expected) {
                if (actual >= L'A' && actual <= L'Z') actual += L'a' - L'A';
                return actual == expected;
            });
        if (!matches) continue;
        ++summary.profiles;
        summary.tabs += info.tab_count;
    }
    return summary;
}

} // namespace glint::setup
