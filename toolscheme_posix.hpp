#ifndef TOOLSCHEME_POSIX_HPP
#define TOOLSCHEME_POSIX_HPP

#include "toolscheme.hpp"

#include <memory>
#include <string>
#include <vector>

// Reference host adapters over POSIX APIs for Linux, macOS, and a portable core.
// These live outside the interpreter translation unit on purpose: an embedder may
// use them, replace them, or install nothing and run with every host operation
// denied. Platform-specific operations report structured unsupported results.
namespace toolscheme {
namespace posix {

struct Policy {
    // Every filesystem path resolves inside this root. Traversal and symlink escapes
    // are rejected before any host call.
    std::string root = ".";
    bool writable = true;
    bool follow_symlinks = true;

    // Child processes are refused unless explicitly enabled. An empty allowlist with
    // `allow_process` set permits any program on the search path.
    bool allow_process = false;
    std::vector<std::string> allowed_programs;
    std::vector<std::string> search_paths;

    // Environment variables visible to `env` and inherited by children.
    std::vector<std::string> environment_allowlist;

    std::size_t output_limit = 10u * 1024u * 1024u;
    std::int64_t default_timeout_ms = 30000;

    bool allow_terminal = false;
    bool allow_service = false;
    bool allow_logging = false;
    bool allow_desktop = false;

    // How long `wait-for` may block when the caller names no deadline of its own,
    // and the ceiling it may never exceed however long the caller asks for.
    std::int64_t default_wait_ms = 60000;
    std::int64_t max_wait_ms = 3600000;
};

std::shared_ptr<FileSystemCapability> make_filesystem(const Policy& policy);
std::shared_ptr<ProcessCapability> make_process(const Policy& policy);
std::shared_ptr<ClockCapability> make_clock();
std::shared_ptr<SystemCapability> make_system(const Policy& policy);
std::shared_ptr<TerminalCapability> make_terminal(const Policy& policy);
std::shared_ptr<CryptoCapability> make_crypto();
std::shared_ptr<ShellCapability> make_shell(const Policy& policy,
                                            std::shared_ptr<ProcessCapability> processes);
std::shared_ptr<ServiceCapability> make_service(const Policy& policy,
                                                std::shared_ptr<ProcessCapability> processes);
std::shared_ptr<LoggingCapability> make_logging(const Policy& policy);
std::shared_ptr<WatchCapability> make_watch(const Policy& policy);
std::shared_ptr<DesktopCapability> make_desktop(const Policy& policy,
                                                std::shared_ptr<ProcessCapability> processes);
std::shared_ptr<HttpCapability> make_http(const Policy& policy,
                                          std::shared_ptr<ProcessCapability> processes);

// Installs every adapter the policy permits and enables the matching primitive
// groups. Capabilities are bound under their kind name, for example `filesystem`.
void install_all(Interpreter& interpreter, const Policy& policy);

// Digest helpers, also reachable through the crypto capability.
std::string md5(std::string_view bytes);
std::string sha1(std::string_view bytes);
std::string sha256(std::string_view bytes);

} // namespace posix
} // namespace toolscheme

#endif
