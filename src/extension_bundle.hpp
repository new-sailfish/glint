#pragma once

#include <filesystem>
#include <string>

namespace glint::extension_bundle {

// Extract the embedded browser extension to the per-user stable directory.
// On success, directory is set to %LOCALAPPDATA%\Glint\extension and error is empty.
bool ensure_extracted(std::filesystem::path& directory, std::wstring& error);

} // namespace glint::extension_bundle
