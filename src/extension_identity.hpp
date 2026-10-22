#pragma once

#include <filesystem>
#include <string>

namespace glint::setup {

// Derived from the RSA SPKI public key in extension/manifest.json.
inline constexpr char kBundledExtensionId[] = "pecljnjkgandbbelbajkppllibpehhpe";

// Compatibility identity for one known unpacked-extension directory. This is
// Chromium's Windows UTF-16 path hash, not a search of browser profile data.
// The caller must supply the absolute directory that the user loads in Chrome
// or Edge. Non-root trailing separators are removed; only the drive letter's
// case is normalized. Returns an empty string and sets error on failure.
std::wstring legacy_unpacked_id(const std::filesystem::path& directory, std::wstring& error);

} // namespace glint::setup
