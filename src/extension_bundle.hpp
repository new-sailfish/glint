#pragma once

#include <filesystem>
#include <string>

namespace glint::extension_bundle {

// Extract the embedded browser extension to the per-user stable directory.
// On success, directory is %LOCALAPPDATA%\Glint\browser-extension and error is empty.
// The legacy Glint\extension directory is never overwritten or removed.
// changed_existing reports completed runtime writes to a previously extracted
// bundle, including partial updates when a later file fails. First extraction
// and identical files do not require an existing browser installation to reload.
bool ensure_extracted(std::filesystem::path& directory, std::wstring& error,
                      bool* changed_existing = nullptr);

// Refresh an existing browser-extension directory beside the executable only
// when its manifest has Glint's fixed public key and expected runtime structure.
// Missing/unrecognized bundles are left untouched and return success. Unsafe
// reparse paths and write failures return false; completed writes still set
// changed_existing so callers can request a manual browser extension reload.
bool sync_adjacent(const std::filesystem::path& executable_directory,
                   bool& changed_existing, std::wstring& error);

} // namespace glint::extension_bundle
