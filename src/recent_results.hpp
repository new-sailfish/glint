#pragma once

#include "glint/core.hpp"

#include <algorithm>
#include <tuple>

namespace glint {

// Merge all sources before selecting the cards. Unknown timestamps stay zero;
// identity provides a repeatable fallback without inventing access history.
[[nodiscard]] inline std::vector<Tab> select_recent_results(
    std::vector<Tab> rows, std::size_t limit = 6) {
    std::sort(rows.begin(), rows.end(), [](const Tab& left, const Tab& right) {
        if (left.last_accessed != right.last_accessed)
            return left.last_accessed > right.last_accessed;
        return std::tie(left.key.source, left.browser, left.profile_label,
                   left.key.connection_id, left.window_id, left.index,
                   left.key.tab_id, left.key.target_id) <
            std::tie(right.key.source, right.browser, right.profile_label,
                right.key.connection_id, right.window_id, right.index,
                right.key.tab_id, right.key.target_id);
    });
    if (rows.size() > limit) rows.resize(limit);
    return rows;
}

} // namespace glint
