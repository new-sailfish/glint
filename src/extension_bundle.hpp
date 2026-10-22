#pragma once

#include <filesystem>
#include <string>

namespace glint::extension_bundle {

// Extract the embedded browser extension to the per-user stable directory.
// On success, directory is %LOCALAPPDATA%\Glint\browser-extension and error is empty.
// The legacy Glint\extension directory is never overwritten or removed.
bool ensure_extracted(std::filesystem::path& directory, std::wstring& error);

} // namespace glint::extension_bundle
