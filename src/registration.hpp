#pragma once

#include <string>
#include <string_view>

namespace glint::setup {

struct RegistrationState {
    std::wstring chrome_id;
    std::wstring edge_id;
    bool chrome_current = false;
    bool edge_current = false;
};

RegistrationState read_registration();
bool valid_extension_id(std::wstring_view id);
int register_hosts(std::wstring_view chrome_id, std::wstring_view edge_id, std::wstring& error);
bool register_bundled_host(bool edge, std::wstring& error);
bool onboarding_dismissed();
bool dismiss_onboarding();
bool should_show_onboarding(bool dismissed, const RegistrationState& state);

} // namespace glint::setup
