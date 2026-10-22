#pragma once

#include <filesystem>
#include <string>

namespace glint::bridge_installation {

// Calculates the content-addressed per-user path without creating any files.
[[nodiscard]] std::filesystem::path path_for_current_executable(std::wstring& error);
// Installs a byte-verified copy. Existing identical copies are reused, including
// copies held open by a browser. Previous versions are deliberately left intact.
[[nodiscard]] bool ensure_current(std::filesystem::path& path, std::wstring& error);
// Requires both the expected absolute path and identical executable contents.
[[nodiscard]] bool is_current_bridge(const std::filesystem::path& path);
// Accepts an installed older version only when its content matches its exact
// per-user SHA-256 directory. Used for still-running browser bridge sessions.
[[nodiscard]] bool is_installed_bridge(const std::filesystem::path& path);

} // namespace glint::bridge_installation
