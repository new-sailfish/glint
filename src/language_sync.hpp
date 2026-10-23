#pragma once

#include "glint/core.hpp"
#include "language.hpp"

#include <span>
#include <string>

namespace glint {

inline std::string language_settings_message(i18n::Preference preference, i18n::Language language) {
    const auto* preference_name = preference == i18n::Preference::English ? "en"
        : preference == i18n::Preference::Chinese ? "zh-CN" : "system";
    return Json(Json::Object{{"type", "language_settings"}, {"version", static_cast<int>(kProtocolVersion)},
        {"preference", preference_name}, {"language", language == i18n::Language::Chinese ? "zh-CN" : "en"}}).dump();
}

template<class Send>
bool send_language_settings(std::span<const ConnectionInfo> connections, i18n::Preference preference,
                            i18n::Language language, Send&& send) {
    const auto message = language_settings_message(preference, language);
    bool all_queued = true;
    // A valid hello is enough: settings must arrive before tab synchronization.
    for (const auto& connection : connections) {
        if (connection.connection_id && !send(connection.connection_id, message)) all_queued = false;
    }
    return all_queued;
}

} // namespace glint
