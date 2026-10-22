#include "extension_identity.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::wstring identity(const std::filesystem::path& directory) {
    std::wstring error = L"stale error";
    const auto result = glint::setup::legacy_unpacked_id(directory, error);
    require(!result.empty() && error.empty(), "successful identity calculation must clear errors");
    require(result.size() == 32 && result.find_first_not_of(L"abcdefghijklmnop") == std::wstring::npos,
            "identity contains 32 lowercase a-p characters");
    return result;
}

void fixed_public_key_identity_is_unchanged() {
    require(std::string_view(glint::setup::kBundledExtensionId) == "pecljnjkgandbbelbajkppllibpehhpe",
            "compatibility helper must preserve bundled public-key identity");
}

void known_windows_directory_matches_chromium() {
    require(identity(L"C:\\Users\\example-user\\AppData\\Local\\Glint\\extension") == L"bpdmcjnniapdcljcffclnhkccandgmjm",
            "match Chromium's installed path-derived identity");
    require(identity(L"D:\\Browser with spaces\\extension") == L"mhbeleaingbhgnhpmhoaolomgiboamfl",
            "hash paths containing spaces as UTF-16LE");
}

void normalizes_only_the_drive_letter_and_trailing_separators() {
    const auto expected = identity(L"C:\\Users\\example-user\\AppData\\Local\\Glint\\extension");
    require(identity(L"c:\\Users\\example-user\\AppData\\Local\\Glint\\extension") == expected,
            "lowercase drive letter matches uppercase drive letter");
    require(identity(L"C:\\Users\\example-user\\AppData\\Local\\Glint\\extension\\\\") == expected,
            "remove non-root trailing backslashes before hashing");
    require(identity(L"C:\\Users\\example-user\\AppData\\Local\\Glint\\extension//") == expected,
            "remove non-root trailing forward slashes before hashing");
    require(identity(L"C:\\Users\\Example-user\\AppData\\Local\\Glint\\extension") != expected,
            "preserve directory character case even on a case-insensitive filesystem");
    require(identity(L"C:\\") == identity(L"c:\\\\"), "keep a drive root absolute when trimming separators");
}

void unicode_and_unc_directories_preserve_their_path_bytes() {
    require(identity(L"D:\\中文路径\\扩展") == L"ffomejcpobcellachfbjbcbkcipcmbfc",
            "Chinese directory name uses UTF-16LE code units");
    require(identity(L"D:\\中文路径\\另一扩展") == L"bhcnkcfpedcehplgejbkmfmdmeimbikn",
            "different Chinese directories have different compatibility identities");
    require(identity(L"\\\\server\\share\\Glint\\extension") == L"amhahcdmeipnblonphkdbnjpdjncebnp",
            "support an absolute UNC installation directory");
}

void invalid_input_reports_an_error() {
    for (const auto& path : {std::filesystem::path{}, std::filesystem::path(L"extension"),
             std::filesystem::path(L"C:extension"), std::filesystem::path(L"\\extension"),
             std::filesystem::path(std::wstring(L"C:\\Glint\\extension") + L'\0' + L"suffix")}) {
        std::wstring error;
        require(glint::setup::legacy_unpacked_id(path, error).empty() && !error.empty(),
                "reject empty, relative, or embedded-NUL paths with an explicit error");
    }
}

} // namespace

int main() {
    try {
        fixed_public_key_identity_is_unchanged();
        known_windows_directory_matches_chromium();
        normalizes_only_the_drive_letter_and_trailing_separators();
        unicode_and_unc_directories_preserve_their_path_bytes();
        invalid_input_reports_an_error();
        std::cout << "extension identity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "extension identity test failed: " << error.what() << '\n';
        return 1;
    }
}
