#pragma once

#include "update_types.hpp"
#include <optional>

namespace glint::update {

// Called on the update worker. True means a trusted helper is ready and the
// desktop may now follow its ordinary exit path.
bool start_install(const Package& package, std::wstring& error);
std::optional<int> dispatch_update_mode(int argc, wchar_t** argv);
void acknowledge_update_startup();
std::wstring installation_startup_result();
std::string failed_install_version();

enum class LaunchOutcome { Confirmed, Exited, Failed, RunningWithoutConfirmation };
enum class InstallOutcome { Confirmed, RolledBack, Pending, Failed };
struct InstallResult {
    InstallOutcome outcome = InstallOutcome::Failed;
    std::wstring message;
};

// File transaction boundary. The caller supplies process supervision; tests
// exercise real Windows replacement operations in isolated directories.
struct InstallTransaction {
    std::filesystem::path target;
    std::filesystem::path directory;
    Release release;
    std::string previous_sha256;
    std::wstring nonce;
    std::uint64_t file_index = 0;
    std::uint32_t volume_serial = 0;
};
using InstallLauncher = std::function<LaunchOutcome(const std::filesystem::path&, bool rollback)>;
InstallTransaction prepare_install_transaction(const std::filesystem::path& target,
    const Package& package, const std::wstring& nonce);
InstallResult execute_install_transaction(const InstallTransaction&, const InstallLauncher&);
// Recovery never launches a path from a journal. The independently known target
// and matching content hashes bound the only files it can inspect or change.
InstallResult recover_install_transaction(const std::filesystem::path& target,
    const std::filesystem::path& transaction_directory);

} // namespace glint::update
