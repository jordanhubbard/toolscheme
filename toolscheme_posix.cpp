#include "toolscheme_posix.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <regex>
#include <set>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#if defined(__linux__)
#include <sys/inotify.h>
#endif
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#include <sys/sysctl.h>
#endif

extern char** environ;

namespace toolscheme {
namespace posix {
namespace {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// Argument-shape violations are language errors; the interpreter's capability
// dispatcher converts them into structured results for the caller.
[[noreturn]] void fail(const std::string& message) { throw Error(message); }

Value errno_error(std::string_view operation, int code, std::string_view path) {
    const char* text = std::strerror(code);
    std::string symbol;
    switch (code) {
    case EACCES: case EPERM: symbol = "permission-denied"; break;
    case ENOENT: symbol = "not-found"; break;
    case EEXIST: symbol = "already-exists"; break;
    case ENOTDIR: symbol = "not-a-directory"; break;
    case EISDIR: symbol = "is-a-directory"; break;
    case ENOTEMPTY: symbol = "not-empty"; break;
    case ENOSPC: symbol = "no-space"; break;
    case ELOOP: symbol = "symlink-loop"; break;
    case ENAMETOOLONG: symbol = "name-too-long"; break;
    case EAGAIN: symbol = "would-block"; break;
    default: symbol = "host-error"; break;
    }
    std::vector<Value> extra{field("errno", static_cast<std::int64_t>(code))};
    if (!path.empty()) extra.push_back(field("path", path));
    return error_result(text ? text : "host error", symbol, operation, std::move(extra));
}

std::string_view text_argument(const std::vector<Value>& arguments, std::size_t index) {
    if (index >= arguments.size() || arguments[index].type() != Value::Type::String)
        fail("this operation requires a string argument");
    return arguments[index].as_string();
}

Value options_at(const std::vector<Value>& arguments, std::size_t index) {
    if (index >= arguments.size()) return Value::nil();
    return arguments[index];
}

std::int64_t number_option(const Value& options, std::string_view name, std::int64_t fallback) {
    const Value value = option(options, name);
    if (value.type() != Value::Type::Integer) return fallback;
    return value.as_integer();
}

bool flag_option(const Value& options, std::string_view name, bool fallback = false) {
    const Value value = option(options, name);
    if (value.type() == Value::Type::Unspecified) return fallback;
    return value.truthy();
}

// Accepts both spellings, so `(mode write)` and `(mode "write")` mean the same thing.
std::string string_option(const Value& options, std::string_view name, std::string_view fallback) {
    const Value value = option(options, name);
    if (value.type() == Value::Type::String) return std::string(value.as_string());
    if (value.type() == Value::Type::Symbol) return std::string(value.as_symbol());
    return std::string(fallback);
}

std::vector<std::string> string_list(const Value& value) {
    std::vector<std::string> out;
    if (value.type() == Value::Type::String) { out.emplace_back(value.as_string()); return out; }
    if (!value.is_list()) return out;
    for (const Value& element : value.to_vector())
        if (element.type() == Value::Type::String) out.emplace_back(element.as_string());
    return out;
}

// Removes "." and collapses ".." textually before the path ever reaches the host.
std::string normalize(const std::string& path) {
    std::vector<std::string> parts;
    const bool absolute = !path.empty() && path[0] == '/';
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t slash = path.find('/', begin);
        const std::string part = path.substr(begin, slash == std::string::npos
                                                        ? std::string::npos : slash - begin);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (!parts.empty() && parts.back() != "..") parts.pop_back();
                else if (!absolute) parts.push_back("..");
            } else parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    std::string out = absolute ? "/" : "";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    if (out.empty()) out = absolute ? "/" : ".";
    return out;
}

bool inside(const std::string& root, const std::string& path) {
    if (root == "/") return true;
    if (path.size() < root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    return path.size() == root.size() || path[root.size()] == '/';
}

std::string real_or_empty(const std::string& path) {
    char buffer[PATH_MAX];
    if (!::realpath(path.c_str(), buffer)) return {};
    return std::string(buffer);
}

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------

std::string to_hex_bytes(const unsigned char* bytes, std::size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 0xF];
    }
    return out;
}

std::uint32_t rotate_left(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}
std::uint32_t rotate_right(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::string md5_digest(std::string_view input) {
    static const std::uint32_t shift[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    static std::uint32_t constants[64];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 64; ++i)
            constants[i] = static_cast<std::uint32_t>(
                std::floor(std::fabs(std::sin(i + 1.0)) * 4294967296.0));
        ready = true;
    }
    std::uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    std::string message(input);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(message.size()) * 8;
    message += static_cast<char>(0x80);
    while (message.size() % 64 != 56) message += '\0';
    for (int i = 0; i < 8; ++i) message += static_cast<char>((bit_length >> (8 * i)) & 0xFF);

    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t block[16];
        for (int i = 0; i < 16; ++i) {
            block[i] = 0;
            for (int b = 0; b < 4; ++b)
                block[i] |= static_cast<std::uint32_t>(
                                static_cast<unsigned char>(message[offset + i * 4 + b])) << (8 * b);
        }
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            f += a + constants[i] + block[g];
            a = d; d = c; c = b;
            b += rotate_left(f, static_cast<int>(shift[i]));
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    }
    unsigned char out[16];
    for (int i = 0; i < 4; ++i)
        for (int b = 0; b < 4; ++b)
            out[i * 4 + b] = static_cast<unsigned char>((state[i] >> (8 * b)) & 0xFF);
    return to_hex_bytes(out, sizeof out);
}

std::string sha1_digest(std::string_view input) {
    std::uint32_t state[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::string message(input);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(message.size()) * 8;
    message += static_cast<char>(0x80);
    while (message.size() % 64 != 56) message += '\0';
    for (int i = 7; i >= 0; --i) message += static_cast<char>((bit_length >> (8 * i)) & 0xFF);

    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(message[offset + i * 4])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(message[offset + i * 4 + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(message[offset + i * 4 + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(message[offset + i * 4 + 3]));
        for (int i = 16; i < 80; ++i) w[i] = rotate_left(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const std::uint32_t next = rotate_left(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotate_left(b, 30); b = a; a = next;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
    }
    unsigned char out[20];
    for (int i = 0; i < 5; ++i)
        for (int b = 0; b < 4; ++b)
            out[i * 4 + b] = static_cast<unsigned char>((state[i] >> (24 - 8 * b)) & 0xFF);
    return to_hex_bytes(out, sizeof out);
}

std::string sha256_digest(std::string_view input) {
    static const std::uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::uint32_t state[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                              0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    std::string message(input);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(message.size()) * 8;
    message += static_cast<char>(0x80);
    while (message.size() % 64 != 56) message += '\0';
    for (int i = 7; i >= 0; --i) message += static_cast<char>((bit_length >> (8 * i)) & 0xFF);

    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(message[offset + i * 4])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(message[offset + i * 4 + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(message[offset + i * 4 + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(message[offset + i * 4 + 3]));
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
    unsigned char out[32];
    for (int i = 0; i < 8; ++i)
        for (int b = 0; b < 4; ++b)
            out[i * 4 + b] = static_cast<unsigned char>((state[i] >> (24 - 8 * b)) & 0xFF);
    return to_hex_bytes(out, sizeof out);
}

// ---------------------------------------------------------------------------
// Filesystem
// ---------------------------------------------------------------------------

struct OpenFile {
    int descriptor = -1;
    std::string path;
    bool writable = false;
    ~OpenFile() { if (descriptor >= 0) ::close(descriptor); }
};

struct OpenDirectory {
    DIR* handle = nullptr;
    std::string path;
    ~OpenDirectory() { if (handle) ::closedir(handle); }
};

class PosixFileSystem final : public FileSystemCapability {
public:
    explicit PosixFileSystem(const Policy& policy) : policy_(policy) {
        root_ = real_or_empty(policy.root);
        if (root_.empty()) root_ = normalize(policy.root);
        working_ = root_;
    }

    bool supports(std::string_view operation) const override {
        static const std::set<std::string_view> read_only = {
            "pwd", "cd", "ls", "cat", "stat", "file", "find", "du", "df", "readlink", "realpath",
            "glob", "tree", "read-file", "search", "rg", "file-open", "file-close", "file-read",
            "file-read-at", "file-stat", "directory-open", "directory-read", "directory-close",
            "file-seek"
        };
        if (!policy_.writable && !read_only.count(operation)) return false;
        return true;
    }

    Value invoke(Interpreter& vm, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        const std::string op(operation);
        if (op == "pwd") return run_pwd();
        if (op == "cd") return run_cd(arguments);
        if (op == "ls") return run_ls(arguments);
        if (op == "cat") return run_cat(arguments);
        if (op == "stat") return run_stat(arguments);
        if (op == "file") return run_file(arguments);
        if (op == "mkdir") return run_mkdir(arguments);
        if (op == "rmdir") return run_rmdir(arguments);
        if (op == "rm") return run_rm(arguments);
        if (op == "unlink") return run_unlink(arguments);
        if (op == "touch") return run_touch(arguments);
        if (op == "truncate") return run_truncate(arguments);
        if (op == "chmod") return run_chmod(arguments);
        if (op == "cp") return run_cp(arguments);
        if (op == "mv") return run_mv(arguments);
        if (op == "ln" || op == "link") return run_link(arguments, op == "link");
        if (op == "readlink") return run_readlink(arguments);
        if (op == "realpath") return run_realpath(arguments);
        if (op == "du") return run_du(arguments);
        if (op == "df") return run_df(arguments);
        if (op == "sync") { ::sync(); return ok_result({field("synced", true)}); }
        if (op == "dd") return run_dd(arguments);
        if (op == "find" || op == "glob") return run_glob(arguments, op == "find");
        if (op == "tree") return run_tree(arguments);
        if (op == "read-file") return run_read_file(arguments);
        if (op == "write-file") return run_write_file(arguments);
        if (op == "search" || op == "rg") return run_search(arguments, op);
        if (op == "mktemp" || op == "temp-file") return run_temp(arguments, false);
        if (op == "temp-directory") return run_temp(arguments, true);
        if (op == "file-open") return run_file_open(vm, arguments);
        if (op == "file-close") return run_file_close(vm, arguments);
        if (op == "file-read") return run_file_read(vm, arguments, false);
        if (op == "file-read-at") return run_file_read(vm, arguments, true);
        if (op == "file-write") return run_file_write(vm, arguments, false);
        if (op == "file-write-at") return run_file_write(vm, arguments, true);
        if (op == "file-seek") return run_file_seek(vm, arguments);
        if (op == "file-stat") return run_file_stat(vm, arguments);
        if (op == "file-truncate") return run_file_truncate(vm, arguments);
        if (op == "file-flush") return run_file_flush(vm, arguments);
        if (op == "directory-open") return run_directory_open(vm, arguments);
        if (op == "directory-read") return run_directory_read(vm, arguments);
        if (op == "directory-close") return run_directory_close(vm, arguments);
        return unsupported_result(operation, "the POSIX filesystem adapter does not implement " + op);
    }

private:
    Policy policy_;
    std::string root_;
    std::string working_;

    struct Resolved {
        bool ok = false;
        std::string path;
        std::string reason;
        std::string code;
    };

    // Resolves a caller path against the capability root. Embedded NULs, traversal,
    // and parents that resolve outside the root are rejected. The final component is
    // deliberately left unresolved so `stat`, `readlink`, `unlink`, and `rm` see the
    // link itself; a final symlink whose target escapes the root is still refused, so
    // following operations such as `read-file` cannot be tricked into leaving.
    //
    // `follow` says whether the caller will read through a final symlink. Operations
    // that act on the link itself -- `readlink`, `unlink`, `rm`, and `stat` with
    // following disabled -- pass false and are allowed to name a link whose target is
    // outside the root, because they never dereference it.
    Resolved resolve(std::string_view raw, bool must_exist, bool follow = true) const {
        Resolved out;
        const std::string path(raw);
        if (path.empty()) { out.reason = "empty path"; out.code = "invalid-path"; return out; }
        if (path.find('\0') != std::string::npos) {
            out.reason = "path contains an embedded NUL";
            out.code = "invalid-path";
            return out;
        }
        const std::string joined = path[0] == '/' ? path : working_ + "/" + path;
        const std::string lexical = normalize(joined);
        if (!inside(root_, lexical)) {
            out.reason = "path escapes the capability root";
            out.code = "outside-root";
            return out;
        }
        if (lexical == root_) { out.ok = true; out.path = root_; return out; }

        const std::size_t slash = lexical.find_last_of('/');
        const std::string parent = slash == 0 ? "/" : lexical.substr(0, slash);
        const std::string base = lexical.substr(slash + 1);

        // Walk up through components that do not exist yet, so creating a path inside
        // a missing directory still validates the part that does exist.
        std::string probe = parent;
        std::string remainder;
        std::string parent_real;
        for (;;) {
            const std::string real = real_or_empty(probe);
            if (!real.empty()) {
                if (!inside(root_, real)) {
                    out.reason = "path resolves outside the capability root";
                    out.code = "outside-root";
                    return out;
                }
                parent_real = remainder.empty() ? real : real + "/" + remainder;
                break;
            }
            const std::size_t up = probe.find_last_of('/');
            if (up == std::string::npos || probe == "/") {
                out.reason = "no existing ancestor inside the capability root";
                out.code = "outside-root";
                return out;
            }
            remainder = remainder.empty() ? probe.substr(up + 1)
                                          : probe.substr(up + 1) + "/" + remainder;
            probe = up == 0 ? "/" : probe.substr(0, up);
        }

        const std::string full = parent_real == "/" ? "/" + base : parent_real + "/" + base;
        struct stat info {};
        if (::lstat(full.c_str(), &info) == 0) {
            if (follow && S_ISLNK(info.st_mode)) {
                const std::string target = real_or_empty(full);
                if (!target.empty() && !inside(root_, target)) {
                    out.reason = "symlink target escapes the capability root";
                    out.code = "outside-root";
                    return out;
                }
            }
        } else if (must_exist) {
            out.reason = "no such file or directory";
            out.code = "not-found";
            return out;
        }
        out.ok = true;
        out.path = full;
        return out;
    }

    Value reject(const Resolved& resolved, std::string_view operation, std::string_view path) const {
        return error_result(resolved.reason, resolved.code, operation, {field("path", path)});
    }

    std::string relative(const std::string& full) const {
        if (full == root_) return ".";
        if (inside(root_, full) && root_ != "/") return full.substr(root_.size() + 1);
        return full;
    }

    static const char* kind_of(mode_t mode) {
        if (S_ISREG(mode)) return "file";
        if (S_ISDIR(mode)) return "directory";
        if (S_ISLNK(mode)) return "symlink";
        if (S_ISFIFO(mode)) return "fifo";
        if (S_ISSOCK(mode)) return "socket";
        if (S_ISCHR(mode)) return "character-device";
        if (S_ISBLK(mode)) return "block-device";
        return "unknown";
    }

    Value stat_record(const std::string& full, const struct stat& info) const {
        ListBuilder out(14);
        out.field("path", relative(full));
        out.symbol_field("kind", kind_of(info.st_mode));
        out.field("size", static_cast<std::int64_t>(info.st_size));
        out.field("mode", static_cast<std::int64_t>(info.st_mode & 07777));
        out.field("links", static_cast<std::int64_t>(info.st_nlink));
        out.field("uid", static_cast<std::int64_t>(info.st_uid));
        out.field("gid", static_cast<std::int64_t>(info.st_gid));
        out.field("modified", static_cast<std::int64_t>(info.st_mtime));
        out.field("accessed", static_cast<std::int64_t>(info.st_atime));
        out.field("changed", static_cast<std::int64_t>(info.st_ctime));
        out.field("inode", static_cast<std::int64_t>(info.st_ino));
        out.field("readable", ::access(full.c_str(), R_OK) == 0);
        out.field("writable", ::access(full.c_str(), W_OK) == 0);
        out.field("executable", ::access(full.c_str(), X_OK) == 0);
        return out.build();
    }

    Value run_pwd() const {
        return ok_result({field("path", relative(working_)), field("root", root_)});
    }

    // The capability owns the interpreter-visible working directory; `cd` moves it
    // and stays inside the root like every other path operation.
    Value run_cd(const std::vector<Value>& arguments) {
        const std::string path = arguments.empty() || arguments[0].type() != Value::Type::String
                                     ? std::string(".")
                                     : std::string(arguments[0].as_string());
        const Resolved resolved = resolve(path, true);
        if (!resolved.ok) return reject(resolved, "cd", path);
        struct stat info {};
        if (::stat(resolved.path.c_str(), &info) != 0) return errno_error("cd", errno, path);
        if (!S_ISDIR(info.st_mode))
            return error_result("not a directory", "not-a-directory", "cd", {field("path", path)});
        const std::string previous = relative(working_);
        working_ = real_or_empty(resolved.path);
        if (working_.empty()) working_ = resolved.path;
        return ok_result({field("path", relative(working_)), field("previous", previous),
                          field("root", root_)});
    }

    Value run_ls(const std::vector<Value>& arguments) {
        const std::string requested = arguments.empty() || arguments[0].type() != Value::Type::String
                                          ? std::string(".")
                                          : std::string(arguments[0].as_string());
        const Value options = options_at(arguments, arguments.empty() ? 0 : 1);
        const bool show_hidden = flag_option(options, "hidden");
        const Resolved resolved = resolve(requested, true);
        if (!resolved.ok) return reject(resolved, "ls", requested);
        DIR* directory = ::opendir(resolved.path.c_str());
        if (!directory) return errno_error("ls", errno, requested);
        std::vector<Value> entries;
        while (const dirent* item = ::readdir(directory)) {
            const std::string name = item->d_name;
            if (name == "." || name == "..") continue;
            if (!show_hidden && !name.empty() && name[0] == '.') continue;
            struct stat info {};
            const std::string child = resolved.path + "/" + name;
            if (::lstat(child.c_str(), &info) != 0) continue;
            entries.push_back(stat_record(child, info));
        }
        ::closedir(directory);
        std::sort(entries.begin(), entries.end(), [](const Value& a, const Value& b) {
            return option(a, "path").as_string() < option(b, "path").as_string();
        });
        const std::int64_t count = static_cast<std::int64_t>(entries.size());
        return ok_result({field("entries", Value::list(std::move(entries))), field("count", count),
                          field("path", relative(resolved.path))});
    }

    bool read_whole(const std::string& full, std::string& out, std::size_t limit, bool& truncated) {
        const int descriptor = ::open(full.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) return false;
        char buffer[65536];
        truncated = false;
        for (;;) {
            const ssize_t got = ::read(descriptor, buffer, sizeof buffer);
            if (got < 0) { ::close(descriptor); return false; }
            if (got == 0) break;
            if (out.size() + static_cast<std::size_t>(got) > limit) {
                out.append(buffer, limit - out.size());
                truncated = true;
                break;
            }
            out.append(buffer, static_cast<std::size_t>(got));
        }
        ::close(descriptor);
        return true;
    }

    Value run_cat(const std::vector<Value>& arguments) {
        if (arguments.empty()) fail("cat expects paths");
        const std::vector<std::string> paths = string_list(arguments[0]);
        const Value options = options_at(arguments, 1);
        const std::size_t limit = static_cast<std::size_t>(
            number_option(options, "limit", static_cast<std::int64_t>(policy_.output_limit)));
        std::string combined;
        std::vector<Value> records;
        bool truncated = false;
        for (const std::string& path : paths) {
            const Resolved resolved = resolve(path, true);
            if (!resolved.ok) return reject(resolved, "cat", path);
            std::string content;
            bool cut = false;
            if (!read_whole(resolved.path, content, limit, cut))
                return errno_error("cat", errno, path);
            truncated = truncated || cut;
            combined += content;
            records.push_back(ok_result({field("path", relative(resolved.path)),
                                         field("size", static_cast<std::int64_t>(content.size()))}));
        }
        return ok_result({field("text", combined),
                          field("files", Value::list(std::move(records))),
                          field("bytes", static_cast<std::int64_t>(combined.size())),
                          field("truncated", truncated)});
    }

    Value run_stat(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Value options = options_at(arguments, 1);
        const bool follow = flag_option(options, "follow-symlinks", policy_.follow_symlinks);
        const Resolved resolved = resolve(path, true, follow);
        if (!resolved.ok) return reject(resolved, "stat", path);
        struct stat info {};
        if ((follow ? ::stat(resolved.path.c_str(), &info)
                    : ::lstat(resolved.path.c_str(), &info)) != 0)
            return errno_error("stat", errno, path);
        return stat_record(resolved.path, info);
    }

    Value run_file(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Resolved resolved = resolve(path, true);
        if (!resolved.ok) return reject(resolved, "file", path);
        struct stat info {};
        if (::lstat(resolved.path.c_str(), &info) != 0) return errno_error("file", errno, path);
        std::string description = kind_of(info.st_mode);
        bool binary = false;
        if (S_ISREG(info.st_mode)) {
            std::string head;
            bool truncated = false;
            read_whole(resolved.path, head, 8192, truncated);
            for (const char c : head)
                if (c == '\0') { binary = true; break; }
            if (head.rfind("\x7f" "ELF", 0) == 0) description = "elf-binary";
            else if (head.rfind("#!", 0) == 0) description = "script";
            else if (binary) description = "binary";
            else description = "text";
        }
        return ok_result({field("path", relative(resolved.path)),
                          symbol_field("kind", kind_of(info.st_mode)),
                          symbol_field("description", description), field("binary", binary)});
    }

    Value run_mkdir(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Value options = options_at(arguments, 1);
        const Resolved resolved = resolve(path, false);
        if (!resolved.ok) return reject(resolved, "mkdir", path);
        const mode_t mode = static_cast<mode_t>(number_option(options, "mode", 0755));
        if (flag_option(options, "parents")) {
            std::string built = resolved.path[0] == '/' ? "/" : "";
            std::size_t begin = resolved.path[0] == '/' ? 1 : 0;
            while (begin <= resolved.path.size()) {
                const std::size_t slash = resolved.path.find('/', begin);
                const std::string part = resolved.path.substr(
                    begin, slash == std::string::npos ? std::string::npos : slash - begin);
                if (!part.empty()) {
                    if (built.size() > 1) built += '/';
                    else if (built.empty()) built = "";
                    built += part;
                    if (::mkdir(built.c_str(), mode) != 0 && errno != EEXIST)
                        return errno_error("mkdir", errno, path);
                }
                if (slash == std::string::npos) break;
                begin = slash + 1;
            }
            return ok_result({field("path", relative(resolved.path)), field("created", true)});
        }
        if (::mkdir(resolved.path.c_str(), mode) != 0) return errno_error("mkdir", errno, path);
        return ok_result({field("path", relative(resolved.path)), field("created", true)});
    }

    Value run_rmdir(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Resolved resolved = resolve(path, true);
        if (!resolved.ok) return reject(resolved, "rmdir", path);
        if (resolved.path == root_)
            return error_result("refusing to remove the capability root", "permission-denied",
                                "rmdir", {field("path", path)});
        if (::rmdir(resolved.path.c_str()) != 0) return errno_error("rmdir", errno, path);
        return ok_result({field("path", relative(resolved.path)), field("removed", true)});
    }

    bool remove_tree(const std::string& full, std::int64_t& removed) {
        struct stat info {};
        if (::lstat(full.c_str(), &info) != 0) return false;
        if (S_ISDIR(info.st_mode)) {
            DIR* directory = ::opendir(full.c_str());
            if (!directory) return false;
            while (const dirent* item = ::readdir(directory)) {
                const std::string name = item->d_name;
                if (name == "." || name == "..") continue;
                if (!remove_tree(full + "/" + name, removed)) { ::closedir(directory); return false; }
            }
            ::closedir(directory);
            if (::rmdir(full.c_str()) != 0) return false;
        } else if (::unlink(full.c_str()) != 0) return false;
        ++removed;
        return true;
    }

    Value run_rm(const std::vector<Value>& arguments) {
        if (arguments.empty()) fail("rm expects paths");
        const std::vector<std::string> paths = string_list(arguments[0]);
        const Value options = options_at(arguments, 1);
        const bool recursive = flag_option(options, "recursive");
        const bool force = flag_option(options, "force");
        std::int64_t removed = 0;
        std::vector<Value> failures;
        for (const std::string& path : paths) {
            const Resolved resolved = resolve(path, !force, false);
            if (!resolved.ok) {
                if (force) continue;
                return reject(resolved, "rm", path);
            }
            if (resolved.path == root_)
                return error_result("refusing to remove the capability root", "permission-denied",
                                    "rm", {field("path", path)});
            struct stat info {};
            if (::lstat(resolved.path.c_str(), &info) != 0) {
                if (force) continue;
                return errno_error("rm", errno, path);
            }
            if (S_ISDIR(info.st_mode) && !recursive)
                return error_result("path is a directory", "is-a-directory", "rm",
                                    {field("path", path)});
            if (!remove_tree(resolved.path, removed)) {
                if (force) continue;
                failures.push_back(errno_error("rm", errno, path));
            }
        }
        if (!failures.empty()) return failures.front();
        return ok_result({field("removed", removed)});
    }

    Value run_unlink(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Resolved resolved = resolve(path, true, false);
        if (!resolved.ok) return reject(resolved, "unlink", path);
        if (::unlink(resolved.path.c_str()) != 0) return errno_error("unlink", errno, path);
        return ok_result({field("path", relative(resolved.path)), field("removed", true)});
    }

    Value run_touch(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Resolved resolved = resolve(path, false);
        if (!resolved.ok) return reject(resolved, "touch", path);
        const int descriptor = ::open(resolved.path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (descriptor < 0) return errno_error("touch", errno, path);
        ::close(descriptor);
        if (::utimes(resolved.path.c_str(), nullptr) != 0) return errno_error("touch", errno, path);
        return ok_result({field("path", relative(resolved.path)), field("touched", true)});
    }

    Value run_truncate(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Value options = options_at(arguments, 1);
        const Resolved resolved = resolve(path, true);
        if (!resolved.ok) return reject(resolved, "truncate", path);
        const std::int64_t size = number_option(options, "size", 0);
        if (size < 0) return error_result("size must not be negative", "invalid-argument", "truncate");
        if (::truncate(resolved.path.c_str(), static_cast<off_t>(size)) != 0)
            return errno_error("truncate", errno, path);
        return ok_result({field("path", relative(resolved.path)), field("size", size)});
    }

    Value run_chmod(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Value options = options_at(arguments, 1);
        const Resolved resolved = resolve(path, true);
        if (!resolved.ok) return reject(resolved, "chmod", path);
        const std::int64_t mode = number_option(options, "mode", -1);
        if (mode < 0 || mode > 07777)
            return error_result("chmod requires an octal mode between 0 and 07777",
                                "invalid-argument", "chmod");
        if (::chmod(resolved.path.c_str(), static_cast<mode_t>(mode)) != 0)
            return errno_error("chmod", errno, path);
        return ok_result({field("path", relative(resolved.path)), field("mode", mode)});
    }

    bool copy_file(const std::string& from, const std::string& to, mode_t mode) {
        const int source = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
        if (source < 0) return false;
        const int target = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
        if (target < 0) { ::close(source); return false; }
        char buffer[65536];
        for (;;) {
            const ssize_t got = ::read(source, buffer, sizeof buffer);
            if (got < 0) { ::close(source); ::close(target); return false; }
            if (got == 0) break;
            ssize_t written = 0;
            while (written < got) {
                const ssize_t step = ::write(target, buffer + written,
                                             static_cast<std::size_t>(got - written));
                if (step <= 0) { ::close(source); ::close(target); return false; }
                written += step;
            }
        }
        ::close(source);
        ::close(target);
        return true;
    }

    bool copy_tree(const std::string& from, const std::string& to, std::int64_t& copied) {
        struct stat info {};
        if (::lstat(from.c_str(), &info) != 0) return false;
        if (S_ISDIR(info.st_mode)) {
            if (::mkdir(to.c_str(), info.st_mode & 07777) != 0 && errno != EEXIST) return false;
            DIR* directory = ::opendir(from.c_str());
            if (!directory) return false;
            while (const dirent* item = ::readdir(directory)) {
                const std::string name = item->d_name;
                if (name == "." || name == "..") continue;
                if (!copy_tree(from + "/" + name, to + "/" + name, copied)) {
                    ::closedir(directory);
                    return false;
                }
            }
            ::closedir(directory);
            ++copied;
            return true;
        }
        if (S_ISLNK(info.st_mode)) {
            char buffer[PATH_MAX];
            const ssize_t length = ::readlink(from.c_str(), buffer, sizeof buffer - 1);
            if (length < 0) return false;
            buffer[length] = '\0';
            if (::symlink(buffer, to.c_str()) != 0 && errno != EEXIST) return false;
            ++copied;
            return true;
        }
        if (!copy_file(from, to, info.st_mode & 07777)) return false;
        ++copied;
        return true;
    }

    Value run_cp(const std::vector<Value>& arguments) {
        const std::string from(text_argument(arguments, 0));
        const std::string to(text_argument(arguments, 1));
        const Value options = options_at(arguments, 2);
        const Resolved source = resolve(from, true);
        if (!source.ok) return reject(source, "cp", from);
        const Resolved target = resolve(to, false);
        if (!target.ok) return reject(target, "cp", to);
        struct stat info {};
        if (::lstat(source.path.c_str(), &info) != 0) return errno_error("cp", errno, from);
        if (S_ISDIR(info.st_mode) && !flag_option(options, "recursive"))
            return error_result("source is a directory", "is-a-directory", "cp",
                                {field("path", from)});
        std::int64_t copied = 0;
        if (!copy_tree(source.path, target.path, copied)) return errno_error("cp", errno, to);
        return ok_result({field("from", relative(source.path)), field("to", relative(target.path)),
                          field("copied", copied)});
    }

    Value run_mv(const std::vector<Value>& arguments) {
        const std::string from(text_argument(arguments, 0));
        const std::string to(text_argument(arguments, 1));
        const Resolved source = resolve(from, true);
        if (!source.ok) return reject(source, "mv", from);
        const Resolved target = resolve(to, false);
        if (!target.ok) return reject(target, "mv", to);
        if (::rename(source.path.c_str(), target.path.c_str()) != 0) {
            if (errno != EXDEV) return errno_error("mv", errno, from);
            std::int64_t copied = 0;
            if (!copy_tree(source.path, target.path, copied)) return errno_error("mv", errno, to);
            std::int64_t removed = 0;
            if (!remove_tree(source.path, removed)) return errno_error("mv", errno, from);
        }
        return ok_result({field("from", relative(source.path)), field("to", relative(target.path)),
                          field("moved", true)});
    }

    Value run_link(const std::vector<Value>& arguments, bool hard_only) {
        const std::string from(text_argument(arguments, 0));
        const std::string to(text_argument(arguments, 1));
        const Value options = options_at(arguments, 2);
        const bool symbolic = !hard_only && flag_option(options, "symbolic", true);
        const Resolved source = resolve(from, !symbolic);
        if (!source.ok && !symbolic) return reject(source, hard_only ? "link" : "ln", from);
        const Resolved target = resolve(to, false);
        if (!target.ok) return reject(target, hard_only ? "link" : "ln", to);
        const std::string source_path = source.ok ? source.path : from;
        const int result = symbolic ? ::symlink(source_path.c_str(), target.path.c_str())
                                    : ::link(source_path.c_str(), target.path.c_str());
        if (result != 0) return errno_error(hard_only ? "link" : "ln", errno, to);
        return ok_result({field("from", source.ok ? relative(source.path) : from),
                          field("to", relative(target.path)),
                          symbol_field("kind", symbolic ? "symbolic" : "hard")});
    }

    Value run_readlink(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Resolved resolved = resolve(path, true, false);
        if (!resolved.ok) return reject(resolved, "readlink", path);
        char buffer[PATH_MAX];
        const ssize_t length = ::readlink(resolved.path.c_str(), buffer, sizeof buffer - 1);
        if (length < 0) return errno_error("readlink", errno, path);
        buffer[length] = '\0';
        return ok_result({field("path", relative(resolved.path)), field("target", buffer)});
    }

    Value run_realpath(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Resolved resolved = resolve(path, true);
        if (!resolved.ok) return reject(resolved, "realpath", path);
        return ok_result({field("path", relative(resolved.path)),
                          field("absolute", resolved.path)});
    }

    void measure(const std::string& full, std::int64_t& bytes, std::int64_t& files) {
        struct stat info {};
        if (::lstat(full.c_str(), &info) != 0) return;
        bytes += static_cast<std::int64_t>(info.st_size);
        ++files;
        if (!S_ISDIR(info.st_mode)) return;
        DIR* directory = ::opendir(full.c_str());
        if (!directory) return;
        while (const dirent* item = ::readdir(directory)) {
            const std::string name = item->d_name;
            if (name == "." || name == "..") continue;
            measure(full + "/" + name, bytes, files);
        }
        ::closedir(directory);
    }

    Value run_du(const std::vector<Value>& arguments) {
        const std::string path = arguments.empty() || arguments[0].type() != Value::Type::String
                                     ? std::string(".")
                                     : std::string(arguments[0].as_string());
        const Resolved resolved = resolve(path, true);
        if (!resolved.ok) return reject(resolved, "du", path);
        std::int64_t bytes = 0, files = 0;
        measure(resolved.path, bytes, files);
        return ok_result({field("path", relative(resolved.path)), field("bytes", bytes),
                          field("entries", files)});
    }

    Value run_df(const std::vector<Value>& arguments) {
        const std::string path = arguments.empty() || arguments[0].type() != Value::Type::String
                                     ? std::string(".")
                                     : std::string(arguments[0].as_string());
        const Resolved resolved = resolve(path, true);
        if (!resolved.ok) return reject(resolved, "df", path);
#if defined(__linux__) || defined(__APPLE__)
        struct statvfs info {};
        if (::statvfs(resolved.path.c_str(), &info) != 0) return errno_error("df", errno, path);
        const std::int64_t block = static_cast<std::int64_t>(info.f_frsize);
        return ok_result({field("path", relative(resolved.path)),
                          field("block-size", block),
                          field("total", static_cast<std::int64_t>(info.f_blocks) * block),
                          field("free", static_cast<std::int64_t>(info.f_bfree) * block),
                          field("available", static_cast<std::int64_t>(info.f_bavail) * block)});
#else
        return unsupported_result("df", "statvfs is unavailable on this platform");
#endif
    }

    Value run_dd(const std::vector<Value>& arguments) {
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        const std::string from = string_option(options, "from", "");
        const std::string to = string_option(options, "to", "");
        if (from.empty() || to.empty())
            return error_result("dd requires `from` and `to` paths", "invalid-argument", "dd");
        const std::int64_t skip = number_option(options, "skip", 0);
        const std::int64_t count = number_option(options, "count", -1);
        const Resolved source = resolve(from, true);
        if (!source.ok) return reject(source, "dd", from);
        const Resolved target = resolve(to, false);
        if (!target.ok) return reject(target, "dd", to);
        const int in = ::open(source.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) return errno_error("dd", errno, from);
        if (skip > 0 && ::lseek(in, static_cast<off_t>(skip), SEEK_SET) < 0) {
            ::close(in);
            return errno_error("dd", errno, from);
        }
        const int out = ::open(target.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (out < 0) { ::close(in); return errno_error("dd", errno, to); }
        char buffer[65536];
        std::int64_t moved = 0;
        for (;;) {
            std::size_t want = sizeof buffer;
            if (count >= 0 && moved + static_cast<std::int64_t>(want) > count)
                want = static_cast<std::size_t>(count - moved);
            if (want == 0) break;
            const ssize_t got = ::read(in, buffer, want);
            if (got <= 0) break;
            if (::write(out, buffer, static_cast<std::size_t>(got)) != got) {
                ::close(in);
                ::close(out);
                return errno_error("dd", errno, to);
            }
            moved += got;
        }
        ::close(in);
        ::close(out);
        return ok_result({field("from", relative(source.path)), field("to", relative(target.path)),
                          field("bytes", moved)});
    }

    // Shell-style glob matching with `*`, `?`, and character classes. `**` spans
    // directory separators.
    static bool glob_match(const std::string& pattern, const std::string& text,
                           std::size_t pi = 0, std::size_t ti = 0) {
        while (pi < pattern.size()) {
            const char p = pattern[pi];
            if (p == '*') {
                const bool deep = pi + 1 < pattern.size() && pattern[pi + 1] == '*';
                std::size_t next = pi + (deep ? 2 : 1);
                if (deep && next < pattern.size() && pattern[next] == '/') ++next;
                for (std::size_t skip = ti; skip <= text.size(); ++skip) {
                    if (!deep && skip > ti && text[skip - 1] == '/') break;
                    if (glob_match(pattern, text, next, skip)) return true;
                }
                return false;
            }
            if (ti >= text.size()) return false;
            if (p == '?') {
                if (text[ti] == '/') return false;
                ++pi; ++ti;
                continue;
            }
            if (p == '[') {
                std::size_t close = pattern.find(']', pi + 1);
                if (close == std::string::npos) return false;
                bool negate = pattern[pi + 1] == '!' || pattern[pi + 1] == '^';
                bool hit = false;
                for (std::size_t i = pi + 1 + (negate ? 1 : 0); i < close; ++i) {
                    if (i + 2 < close && pattern[i + 1] == '-') {
                        if (text[ti] >= pattern[i] && text[ti] <= pattern[i + 2]) hit = true;
                        i += 2;
                    } else if (pattern[i] == text[ti]) hit = true;
                }
                if (hit == negate) return false;
                pi = close + 1;
                ++ti;
                continue;
            }
            if (p != text[ti]) return false;
            ++pi; ++ti;
        }
        return ti == text.size();
    }

    struct Walk {
        std::vector<Value> entries;
        std::int64_t visited = 0;
        bool truncated = false;
    };

    // `find`-style predicates. `name` matches the basename (what `-name` means);
    // the glob `pattern` matches the whole relative path.
    struct Predicates {
        std::string name;               // -name
        std::int64_t min_size = -1;     // -size +N
        std::int64_t max_size = -1;     // -size -N
        std::int64_t newer_than = -1;   // -newermt
        std::int64_t older_than = -1;
        bool empty_only = false;        // -empty
        bool active() const {
            return !name.empty() || min_size >= 0 || max_size >= 0 || newer_than >= 0 ||
                   older_than >= 0 || empty_only;
        }
        bool accepts(const std::string& base, const struct stat& info) const {
            if (!name.empty() && !glob_match(name, base)) return false;
            const std::int64_t size = static_cast<std::int64_t>(info.st_size);
            if (min_size >= 0 && size < min_size) return false;
            if (max_size >= 0 && size > max_size) return false;
            const std::int64_t mtime = static_cast<std::int64_t>(info.st_mtime);
            if (newer_than >= 0 && mtime < newer_than) return false;
            if (older_than >= 0 && mtime > older_than) return false;
            if (empty_only && size != 0) return false;
            return true;
        }
    };

    void walk(const std::string& base, const std::string& prefix, int depth, int max_depth,
              bool hidden, std::int64_t limit, const std::string& pattern,
              const std::vector<std::string>& excludes, const char* want_kind, Walk& out,
              const Predicates* predicates = nullptr) {
        if (max_depth >= 0 && depth > max_depth) return;
        DIR* directory = ::opendir(base.c_str());
        if (!directory) return;
        std::vector<std::string> names;
        while (const dirent* item = ::readdir(directory)) {
            const std::string name = item->d_name;
            if (name == "." || name == "..") continue;
            if (!hidden && !name.empty() && name[0] == '.') continue;
            names.push_back(name);
        }
        ::closedir(directory);
        std::sort(names.begin(), names.end());
        for (const std::string& name : names) {
            if (limit > 0 && static_cast<std::int64_t>(out.entries.size()) >= limit) {
                out.truncated = true;
                return;
            }
            const std::string child = base + "/" + name;
            const std::string shown = prefix.empty() ? name : prefix + "/" + name;
            struct stat info {};
            if (::lstat(child.c_str(), &info) != 0) continue;
            ++out.visited;
            bool excluded = false;
            for (const std::string& exclude : excludes)
                if (glob_match(exclude, shown)) { excluded = true; break; }
            if (!excluded) {
                const bool kind_ok = !want_kind ||
                                     std::strcmp(kind_of(info.st_mode), want_kind) == 0;
                const bool matches = kind_ok && (pattern.empty() || glob_match(pattern, shown)) &&
                                     (!predicates || predicates->accepts(name, info));
                if (matches) out.entries.push_back(stat_record(child, info));
            }
            if (S_ISDIR(info.st_mode) && !excluded)
                walk(child, shown, depth + 1, max_depth, hidden, limit, pattern, excludes,
                     want_kind, out, predicates);
        }
    }

    Value run_glob(const std::vector<Value>& arguments, bool as_find) {
        const char* name = as_find ? "find" : "glob";
        const std::string pattern = arguments.empty() || arguments[0].type() != Value::Type::String
                                        ? std::string()
                                        : std::string(arguments[0].as_string());
        const Value options = options_at(arguments, 1);
        const std::string base = string_option(options, "directory", ".");
        const Resolved resolved = resolve(base, true);
        if (!resolved.ok) return reject(resolved, name, base);
        const Value kind = option(options, "kind");
        const std::string want = kind.type() == Value::Type::Symbol ? std::string(kind.as_symbol())
                                                                    : std::string();
        // `find` additionally accepts the predicates agents actually use.
        Predicates predicates;
        predicates.name = string_option(options, "name", "");
        predicates.min_size = number_option(options, "min-size", -1);
        predicates.max_size = number_option(options, "max-size", -1);
        predicates.newer_than = number_option(options, "newer-than", -1);
        predicates.older_than = number_option(options, "older-than", -1);
        predicates.empty_only = flag_option(options, "empty");

        Walk out;
        walk(resolved.path, {}, 0, static_cast<int>(number_option(options, "max-depth", -1)),
             flag_option(options, "hidden"), number_option(options, "limit", 100000), pattern,
             string_list(option(options, "exclude")), want.empty() ? nullptr : want.c_str(), out,
             predicates.active() ? &predicates : nullptr);
        const std::int64_t count = static_cast<std::int64_t>(out.entries.size());
        return ok_result({field("entries", Value::list(std::move(out.entries))),
                          field("count", count), field("visited", out.visited),
                          field("truncated", out.truncated)});
    }

    Value tree_of(const std::string& full, const std::string& name, int depth, int max_depth,
                  bool hidden, std::int64_t& budget) {
        struct stat info {};
        if (::lstat(full.c_str(), &info) != 0) return Value::nil();
        ListBuilder node(4);
        node.field("name", name);
        node.symbol_field("kind", kind_of(info.st_mode));
        node.field("size", static_cast<std::int64_t>(info.st_size));
        if (!S_ISDIR(info.st_mode) || (max_depth >= 0 && depth >= max_depth) || budget <= 0)
            return node.build();
        DIR* directory = ::opendir(full.c_str());
        if (!directory) return node.build();
        std::vector<std::string> names;
        while (const dirent* item = ::readdir(directory)) {
            const std::string child = item->d_name;
            if (child == "." || child == "..") continue;
            if (!hidden && !child.empty() && child[0] == '.') continue;
            names.push_back(child);
        }
        ::closedir(directory);
        std::sort(names.begin(), names.end());
        std::vector<Value> children;
        for (const std::string& child : names) {
            if (budget-- <= 0) break;
            children.push_back(tree_of(full + "/" + child, child, depth + 1, max_depth, hidden,
                                       budget));
        }
        node.field("children", Value::list(std::move(children)));
        return node.build();
    }

    Value run_tree(const std::vector<Value>& arguments) {
        const std::string base = arguments.empty() || arguments[0].type() != Value::Type::String
                                     ? std::string(".")
                                     : std::string(arguments[0].as_string());
        const Value options = options_at(arguments, 1);
        const Resolved resolved = resolve(base, true);
        if (!resolved.ok) return reject(resolved, "tree", base);
        std::int64_t budget = number_option(options, "limit", 10000);
        const std::int64_t requested = budget;
        Value node = tree_of(resolved.path, relative(resolved.path),
                             0, static_cast<int>(number_option(options, "max-depth", 5)),
                             flag_option(options, "hidden"), budget);
        return ok_result({field("tree", node), field("truncated", budget <= 0),
                          field("limit", requested)});
    }

    Value run_read_file(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Value options = options_at(arguments, 1);
        const Resolved resolved = resolve(path, true);
        if (!resolved.ok) return reject(resolved, "read-file", path);
        const std::size_t limit = static_cast<std::size_t>(
            number_option(options, "limit", static_cast<std::int64_t>(policy_.output_limit)));
        std::string content;
        bool truncated = false;
        if (!read_whole(resolved.path, content, limit, truncated))
            return errno_error("read-file", errno, path);

        const std::int64_t byte_offset = number_option(options, "byte-offset", 0);
        const std::int64_t byte_count = number_option(options, "byte-count", -1);
        if (byte_offset > 0 || byte_count >= 0) {
            const std::size_t begin = std::min<std::size_t>(
                static_cast<std::size_t>(std::max<std::int64_t>(0, byte_offset)), content.size());
            const std::size_t length = byte_count < 0 ? content.size() - begin
                                                      : static_cast<std::size_t>(byte_count);
            content = content.substr(begin, length);
        }
        bool binary = content.find('\0') != std::string::npos;
        if (binary && !flag_option(options, "allow-binary"))
            return error_result("file contains NUL bytes", "binary-file", "read-file",
                                {field("path", relative(resolved.path)),
                                 field("bytes", static_cast<std::int64_t>(content.size()))});

        const std::int64_t first_line = number_option(options, "first-line", 0);
        const std::int64_t last_line = number_option(options, "last-line", 0);
        if (first_line > 0 || last_line > 0) {
            std::vector<std::string> lines;
            std::size_t begin = 0;
            for (;;) {
                const std::size_t at = content.find('\n', begin);
                if (at == std::string::npos) break;
                lines.push_back(content.substr(begin, at - begin));
                begin = at + 1;
            }
            if (begin < content.size()) lines.push_back(content.substr(begin));
            const std::size_t from = first_line > 0 ? static_cast<std::size_t>(first_line - 1) : 0;
            const std::size_t to = last_line > 0
                                       ? std::min<std::size_t>(static_cast<std::size_t>(last_line),
                                                               lines.size())
                                       : lines.size();
            std::string picked;
            for (std::size_t i = from; i < to; ++i) picked += lines[i] + "\n";
            content = picked;
        }
        ListBuilder out(5);
        out.field("path", relative(resolved.path));
        out.field("text", content);
        out.field("bytes", static_cast<std::int64_t>(content.size()));
        out.field("truncated", truncated);
        out.field("binary", binary);
        return out.build();
    }

    Value run_write_file(const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const std::string content(text_argument(arguments, 1));
        const Value options = options_at(arguments, 2);
        const Resolved resolved = resolve(path, false);
        if (!resolved.ok) return reject(resolved, "write-file", path);
        const bool append = flag_option(options, "append");
        const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
        const int descriptor = ::open(resolved.path.c_str(), flags,
                                      static_cast<mode_t>(number_option(options, "mode", 0644)));
        if (descriptor < 0) return errno_error("write-file", errno, path);
        std::size_t written = 0;
        while (written < content.size()) {
            const ssize_t step = ::write(descriptor, content.data() + written,
                                         content.size() - written);
            if (step <= 0) { ::close(descriptor); return errno_error("write-file", errno, path); }
            written += static_cast<std::size_t>(step);
        }
        ::close(descriptor);
        return ok_result({field("path", relative(resolved.path)),
                          field("bytes", static_cast<std::int64_t>(written)),
                          field("appended", append)});
    }

    Value run_search(const std::vector<Value>& arguments, const std::string& name) {
        if (arguments.empty()) fail("search expects a pattern");
        const std::string pattern(text_argument(arguments, 0));
        const std::vector<std::string> roots =
            arguments.size() > 1 ? string_list(arguments[1]) : std::vector<std::string>{"."};
        const Value options = options_at(arguments, 2);
        const bool literal = flag_option(options, "literal");
        const bool ignore_case = flag_option(options, "ignore-case");
        const std::int64_t limit = number_option(options, "limit", 1000);
        const std::vector<std::string> includes = string_list(option(options, "include"));
        const std::vector<std::string> excludes = string_list(option(options, "exclude"));
        const std::int64_t before = number_option(options, "before", 0);
        const std::int64_t after = number_option(options, "after", 0);

        std::regex expression;
        if (!literal) {
            auto flags = std::regex::ECMAScript | std::regex::optimize;
            if (ignore_case) flags |= std::regex::icase;
            try {
                expression = std::regex(pattern, flags);
            } catch (const std::regex_error& error) {
                return error_result(error.what(), "invalid-pattern", name);
            }
        }
        std::string needle = pattern;
        if (literal && ignore_case)
            for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        std::vector<Value> matches;
        std::int64_t scanned = 0;
        bool truncated = false;
        for (const std::string& base : roots) {
            const Resolved resolved = resolve(base, true);
            if (!resolved.ok) return reject(resolved, name, base);
            Walk found;
            walk(resolved.path, {}, 0, static_cast<int>(number_option(options, "max-depth", -1)),
                 flag_option(options, "hidden"), 0, {}, excludes, "file", found);
            for (const Value& entry : found.entries) {
                if (limit > 0 && static_cast<std::int64_t>(matches.size()) >= limit) {
                    truncated = true;
                    break;
                }
                const Value shown = option(entry, "path");
                if (shown.type() != Value::Type::String) continue;
                const std::string relative_path(shown.as_string());
                if (!includes.empty()) {
                    bool wanted = false;
                    for (const std::string& include : includes)
                        if (glob_match(include, relative_path)) { wanted = true; break; }
                    if (!wanted) continue;
                }
                const std::string full = root_ == "/" ? "/" + relative_path
                                                      : root_ + "/" + relative_path;
                std::string content;
                bool cut = false;
                if (!read_whole(full, content, policy_.output_limit, cut)) continue;
                if (content.find('\0') != std::string::npos) continue;
                ++scanned;
                std::vector<std::string> lines;
                std::size_t begin = 0;
                for (;;) {
                    const std::size_t at = content.find('\n', begin);
                    if (at == std::string::npos) break;
                    lines.push_back(content.substr(begin, at - begin));
                    begin = at + 1;
                }
                if (begin < content.size()) lines.push_back(content.substr(begin));
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    if (limit > 0 && static_cast<std::int64_t>(matches.size()) >= limit) {
                        truncated = true;
                        break;
                    }
                    std::size_t column = 0, length = 0;
                    if (literal) {
                        std::string haystack = lines[i];
                        if (ignore_case)
                            for (char& c : haystack)
                                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        const std::size_t at = haystack.find(needle);
                        if (at == std::string::npos) continue;
                        column = at + 1;
                        length = needle.size();
                    } else {
                        std::smatch match;
                        if (!std::regex_search(lines[i], match, expression)) continue;
                        column = static_cast<std::size_t>(match.position(0)) + 1;
                        length = static_cast<std::size_t>(match.length(0));
                    }
                    ListBuilder record(8);
                    record.field("path", relative_path);
                    record.field("line", static_cast<std::int64_t>(i + 1));
                    record.field("column", static_cast<std::int64_t>(column));
                    record.field("length", static_cast<std::int64_t>(length));
                    record.field("text", lines[i]);
                    if (before > 0) {
                        std::vector<Value> context;
                        for (std::size_t j = i >= static_cast<std::size_t>(before) ? i - before : 0;
                             j < i; ++j)
                            context.push_back(Value::string(lines[j]));
                        record.field("before", Value::list(std::move(context)));
                    }
                    if (after > 0) {
                        std::vector<Value> context;
                        for (std::size_t j = i + 1;
                             j < lines.size() && j <= i + static_cast<std::size_t>(after); ++j)
                            context.push_back(Value::string(lines[j]));
                        record.field("after", Value::list(std::move(context)));
                    }
                    matches.push_back(record.build());
                }
            }
        }
        const std::int64_t count = static_cast<std::int64_t>(matches.size());
        return ok_result({field("matches", Value::list(std::move(matches))), field("count", count),
                          field("files-scanned", scanned), field("truncated", truncated)});
    }

    Value run_temp(const std::vector<Value>& arguments, bool directory) {
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        const std::string prefix = string_option(options, "prefix", "toolscheme");
        std::string templated = root_ + "/" + prefix + "XXXXXX";
        std::vector<char> buffer(templated.begin(), templated.end());
        buffer.push_back('\0');
        if (directory) {
            if (!::mkdtemp(buffer.data()))
                return errno_error(directory ? "temp-directory" : "temp-file", errno, prefix);
            return ok_result({field("path", relative(buffer.data())),
                              symbol_field("kind", "directory")});
        }
        const int descriptor = ::mkstemp(buffer.data());
        if (descriptor < 0) return errno_error("temp-file", errno, prefix);
        ::close(descriptor);
        return ok_result({field("path", relative(buffer.data())), symbol_field("kind", "file")});
    }

    std::shared_ptr<OpenFile> file_of(Interpreter& vm, const Value& handle) {
        return std::static_pointer_cast<OpenFile>(vm.handle_token(handle, "file"));
    }

    Value run_file_open(Interpreter& vm, const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Value options = options_at(arguments, 1);
        const std::string mode = string_option(options, "mode", "read");
        int flags = O_CLOEXEC;
        bool writable = false;
        if (mode == "read") flags |= O_RDONLY;
        else if (mode == "write") { flags |= O_WRONLY | O_CREAT | O_TRUNC; writable = true; }
        else if (mode == "append") { flags |= O_WRONLY | O_CREAT | O_APPEND; writable = true; }
        else if (mode == "update") { flags |= O_RDWR | O_CREAT; writable = true; }
        else return error_result("unknown open mode: " + mode, "invalid-argument", "file-open");
        if (writable && !policy_.writable)
            return denied_result("file-open", "this filesystem capability is read-only");
        if (!flag_option(options, "follow-symlinks", policy_.follow_symlinks)) flags |= O_NOFOLLOW;
        const Resolved resolved = resolve(path, !writable);
        if (!resolved.ok) return reject(resolved, "file-open", path);
        const int descriptor = ::open(resolved.path.c_str(), flags,
                                      static_cast<mode_t>(number_option(options, "mode-bits", 0644)));
        if (descriptor < 0) return errno_error("file-open", errno, path);
        auto file = std::make_shared<OpenFile>();
        file->descriptor = descriptor;
        file->path = resolved.path;
        file->writable = writable;
        return ok_result({field("file", vm.make_handle(*this, "file", file)),
                          field("path", relative(resolved.path)), symbol_field("mode", mode)});
    }

    Value run_file_close(Interpreter& vm, const std::vector<Value>& arguments) {
        if (arguments.empty()) fail("file-close expects a handle");
        const std::shared_ptr<OpenFile> file = file_of(vm, arguments[0]);
        if (!file) return error_result("file handle is not live", "invalid-handle", "file-close");
        vm.revoke_handle(arguments[0]);
        return ok_result({field("closed", true)});
    }

    Value run_file_read(Interpreter& vm, const std::vector<Value>& arguments, bool positioned) {
        if (arguments.empty()) fail("file-read expects a handle");
        const std::shared_ptr<OpenFile> file = file_of(vm, arguments[0]);
        if (!file) return error_result("file handle is not live", "invalid-handle", "file-read");
        const Value options = options_at(arguments, positioned ? 2 : 1);
        const std::int64_t want = number_option(options, "count", 65536);
        if (want < 0 || want > 1 << 26)
            return error_result("read size is out of range", "invalid-argument", "file-read");
        std::string buffer(static_cast<std::size_t>(want), '\0');
        ssize_t got;
        if (positioned) {
            if (arguments.size() < 2 || arguments[1].type() != Value::Type::Integer)
                fail("file-read-at expects an offset");
            got = ::pread(file->descriptor, buffer.data(), buffer.size(),
                          static_cast<off_t>(arguments[1].as_integer()));
        } else {
            got = ::read(file->descriptor, buffer.data(), buffer.size());
        }
        if (got < 0) return errno_error(positioned ? "file-read-at" : "file-read", errno, file->path);
        buffer.resize(static_cast<std::size_t>(got));
        return ok_result({field("text", buffer), field("bytes", static_cast<std::int64_t>(got)),
                          field("eof", got == 0)});
    }

    Value run_file_write(Interpreter& vm, const std::vector<Value>& arguments, bool positioned) {
        if (arguments.size() < 2) fail("file-write expects a handle and data");
        const std::shared_ptr<OpenFile> file = file_of(vm, arguments[0]);
        if (!file) return error_result("file handle is not live", "invalid-handle", "file-write");
        if (!file->writable)
            return denied_result("file-write", "this file handle was opened read-only");
        const std::size_t data_index = positioned ? 2 : 1;
        if (arguments.size() <= data_index || arguments[data_index].type() != Value::Type::String)
            fail("file-write expects string data");
        const std::string_view data = arguments[data_index].as_string();
        ssize_t written;
        if (positioned) {
            if (arguments[1].type() != Value::Type::Integer) fail("file-write-at expects an offset");
            written = ::pwrite(file->descriptor, data.data(), data.size(),
                               static_cast<off_t>(arguments[1].as_integer()));
        } else {
            written = ::write(file->descriptor, data.data(), data.size());
        }
        if (written < 0)
            return errno_error(positioned ? "file-write-at" : "file-write", errno, file->path);
        return ok_result({field("bytes", static_cast<std::int64_t>(written))});
    }

    Value run_file_seek(Interpreter& vm, const std::vector<Value>& arguments) {
        if (arguments.size() < 2) fail("file-seek expects a handle and an offset");
        const std::shared_ptr<OpenFile> file = file_of(vm, arguments[0]);
        if (!file) return error_result("file handle is not live", "invalid-handle", "file-seek");
        const Value options = options_at(arguments, 2);
        const std::string whence = string_option(options, "whence", "start");
        const int mode = whence == "current" ? SEEK_CUR : whence == "end" ? SEEK_END : SEEK_SET;
        if (arguments[1].type() != Value::Type::Integer) fail("file-seek expects an integer offset");
        const off_t position = ::lseek(file->descriptor,
                                       static_cast<off_t>(arguments[1].as_integer()), mode);
        if (position < 0) return errno_error("file-seek", errno, file->path);
        return ok_result({field("position", static_cast<std::int64_t>(position))});
    }

    Value run_file_stat(Interpreter& vm, const std::vector<Value>& arguments) {
        if (arguments.empty()) fail("file-stat expects a handle");
        const std::shared_ptr<OpenFile> file = file_of(vm, arguments[0]);
        if (!file) return error_result("file handle is not live", "invalid-handle", "file-stat");
        struct stat info {};
        if (::fstat(file->descriptor, &info) != 0)
            return errno_error("file-stat", errno, file->path);
        return stat_record(file->path, info);
    }

    Value run_file_truncate(Interpreter& vm, const std::vector<Value>& arguments) {
        if (arguments.size() < 2) fail("file-truncate expects a handle and a size");
        const std::shared_ptr<OpenFile> file = file_of(vm, arguments[0]);
        if (!file) return error_result("file handle is not live", "invalid-handle", "file-truncate");
        if (!file->writable)
            return denied_result("file-truncate", "this file handle was opened read-only");
        if (::ftruncate(file->descriptor, static_cast<off_t>(arguments[1].as_integer())) != 0)
            return errno_error("file-truncate", errno, file->path);
        return ok_result({field("size", arguments[1].as_integer())});
    }

    Value run_file_flush(Interpreter& vm, const std::vector<Value>& arguments) {
        if (arguments.empty()) fail("file-flush expects a handle");
        const std::shared_ptr<OpenFile> file = file_of(vm, arguments[0]);
        if (!file) return error_result("file handle is not live", "invalid-handle", "file-flush");
        if (::fsync(file->descriptor) != 0) return errno_error("file-flush", errno, file->path);
        return ok_result({field("flushed", true)});
    }

    Value run_directory_open(Interpreter& vm, const std::vector<Value>& arguments) {
        const std::string path(text_argument(arguments, 0));
        const Resolved resolved = resolve(path, true);
        if (!resolved.ok) return reject(resolved, "directory-open", path);
        DIR* handle = ::opendir(resolved.path.c_str());
        if (!handle) return errno_error("directory-open", errno, path);
        auto directory = std::make_shared<OpenDirectory>();
        directory->handle = handle;
        directory->path = resolved.path;
        return ok_result({field("directory", vm.make_handle(*this, "directory", directory)),
                          field("path", relative(resolved.path))});
    }

    Value run_directory_read(Interpreter& vm, const std::vector<Value>& arguments) {
        if (arguments.empty()) fail("directory-read expects a handle");
        const auto directory =
            std::static_pointer_cast<OpenDirectory>(vm.handle_token(arguments[0], "directory"));
        if (!directory)
            return error_result("directory handle is not live", "invalid-handle", "directory-read");
        errno = 0;
        const dirent* item = ::readdir(directory->handle);
        while (item && (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0))
            item = ::readdir(directory->handle);
        if (!item) {
            if (errno) return errno_error("directory-read", errno, directory->path);
            return ok_result({field("end", true)});
        }
        struct stat info {};
        const std::string child = directory->path + "/" + item->d_name;
        if (::lstat(child.c_str(), &info) != 0) return errno_error("directory-read", errno, child);
        return ok_result({field("entry", stat_record(child, info)), field("end", false)});
    }

    Value run_directory_close(Interpreter& vm, const std::vector<Value>& arguments) {
        if (arguments.empty()) fail("directory-close expects a handle");
        const auto directory =
            std::static_pointer_cast<OpenDirectory>(vm.handle_token(arguments[0], "directory"));
        if (!directory)
            return error_result("directory handle is not live", "invalid-handle", "directory-close");
        vm.revoke_handle(arguments[0]);
        return ok_result({field("closed", true)});
    }
};

// ---------------------------------------------------------------------------
// Processes and jobs
// ---------------------------------------------------------------------------

struct Job {
    pid_t pid = -1;
    int input = -1, output = -1, errors = -1;
    std::string program;
    std::string out, err;
    bool finished = false;
    bool cancelled = false;
    int exit_status = -1;
    int termination_signal = 0;
    bool output_truncated = false, error_truncated = false;
    std::int64_t timeout_ms = 0;
    std::chrono::steady_clock::time_point started;

    ~Job() {
        if (input >= 0) ::close(input);
        if (output >= 0) ::close(output);
        if (errors >= 0) ::close(errors);
        if (pid > 0 && !finished) {
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }
};

class PosixProcess final : public ProcessCapability {
public:
    explicit PosixProcess(const Policy& policy) : policy_(policy) {}

    bool supports(std::string_view operation) const override {
        if (!policy_.allow_process) {
            // Reporting the names as supported keeps the denial visible at call time
            // instead of making the primitive vanish from the registry.
            return true;
        }
        static const std::set<std::string_view> known = {
            "process-start", "process-poll", "process-wait", "process-cancel", "process-write",
            "process-close-input", "process-read-output", "process-read-errors", "job-poll",
            "job-wait", "job-cancel", "job-input", "job-close-input", "job-output",
            "job-error-output", "job-status", "ps", "pgrep", "pkill", "kill", "nice", "timeout",
            "wait4path"
        };
        return known.count(operation) != 0;
    }

    Value invoke(Interpreter& vm, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        const std::string op(operation);
        if (op == "wait4path") return run_wait4path(arguments);
        if (op == "ps") return run_ps(arguments);
        if (op == "pgrep" || op == "pkill") return run_pgrep(arguments, op == "pkill");
        if (op == "kill") return run_kill(arguments);
        if (op == "nice") return run_nice(arguments);
        if (!policy_.allow_process)
            return denied_result(operation, "child processes are disabled by policy");
        if (op == "process-start") return run_start(vm, arguments);
        if (op == "timeout") return run_timeout(vm, arguments);
        if (op == "process-poll" || op == "job-poll") return run_poll(vm, arguments, false);
        if (op == "process-wait" || op == "job-wait") return run_poll(vm, arguments, true);
        if (op == "job-status") return run_poll(vm, arguments, false);
        if (op == "process-cancel" || op == "job-cancel") return run_cancel(vm, arguments);
        if (op == "process-write" || op == "job-input") return run_write(vm, arguments);
        if (op == "process-close-input" || op == "job-close-input") return run_close_input(vm, arguments);
        if (op == "process-read-output" || op == "job-output") return run_read(vm, arguments, false);
        if (op == "process-read-errors" || op == "job-error-output") return run_read(vm, arguments, true);
        return unsupported_result(operation, "the POSIX process adapter does not implement " + op);
    }

    // Used by the shell and service adapters so they never fork directly.
    Value start(Interpreter& vm, const std::string& program,
                const std::vector<std::string>& args, const Value& options) {
        return launch(vm, program, args, options);
    }

private:
    Policy policy_;

    std::shared_ptr<Job> job_of(Interpreter& vm, const Value& handle) {
        return std::static_pointer_cast<Job>(vm.handle_token(handle, "job"));
    }

    bool allowed(const std::string& program) const {
        if (policy_.allowed_programs.empty()) return true;
        for (const std::string& candidate : policy_.allowed_programs) {
            if (candidate == program) return true;
            const std::size_t slash = program.find_last_of('/');
            if (slash != std::string::npos && program.substr(slash + 1) == candidate) return true;
        }
        return false;
    }

    std::string locate(const std::string& program) const {
        if (program.find('/') != std::string::npos) return program;
        std::vector<std::string> paths = policy_.search_paths;
        if (paths.empty()) {
            const char* env = ::getenv("PATH");
            const std::string text = env ? env : "/usr/bin:/bin";
            std::size_t begin = 0;
            for (;;) {
                const std::size_t colon = text.find(':', begin);
                paths.push_back(text.substr(begin, colon == std::string::npos
                                                       ? std::string::npos : colon - begin));
                if (colon == std::string::npos) break;
                begin = colon + 1;
            }
        }
        for (const std::string& directory : paths) {
            if (directory.empty()) continue;
            const std::string candidate = directory + "/" + program;
            if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        return {};
    }

    void drain(std::shared_ptr<Job>& job) {
        char buffer[65536];
        const auto pull = [&](int& descriptor, std::string& sink, bool& truncated) {
            if (descriptor < 0) return;
            for (;;) {
                const ssize_t got = ::read(descriptor, buffer, sizeof buffer);
                if (got > 0) {
                    if (sink.size() + static_cast<std::size_t>(got) > policy_.output_limit) {
                        sink.append(buffer, policy_.output_limit - sink.size());
                        truncated = true;
                    } else {
                        sink.append(buffer, static_cast<std::size_t>(got));
                    }
                    continue;
                }
                if (got == 0) { ::close(descriptor); descriptor = -1; return; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR) continue;
                ::close(descriptor);
                descriptor = -1;
                return;
            }
        };
        pull(job->output, job->out, job->output_truncated);
        pull(job->errors, job->err, job->error_truncated);
    }

    bool reap(std::shared_ptr<Job>& job, bool block) {
        if (job->finished) return true;
        int status = 0;
        const pid_t result = ::waitpid(job->pid, &status, block ? 0 : WNOHANG);
        if (result != job->pid) return false;
        job->finished = true;
        if (WIFEXITED(status)) job->exit_status = WEXITSTATUS(status);
        if (WIFSIGNALED(status)) {
            job->termination_signal = WTERMSIG(status);
            job->exit_status = 128 + job->termination_signal;
        }
        return true;
    }

    Value job_record(Interpreter& vm, const Value& handle, std::shared_ptr<Job>& job,
                     bool include_output) {
        ListBuilder out(12);
        out.field("job", handle);
        out.field("pid", static_cast<std::int64_t>(job->pid));
        out.field("program", job->program);
        out.symbol_field("state", job->finished ? (job->cancelled ? "cancelled" : "exited")
                                                : "running");
        if (job->finished) {
            out.field("exit-status", static_cast<std::int64_t>(job->exit_status));
            out.field("signal", static_cast<std::int64_t>(job->termination_signal));
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - job->started);
        out.field("elapsed-ms", static_cast<std::int64_t>(elapsed.count()));
        if (include_output) {
            out.field("stdout", job->out);
            out.field("stderr", job->err);
            out.field("stdout-truncated", job->output_truncated);
            out.field("stderr-truncated", job->error_truncated);
        }
        (void)vm;
        return out.build();
    }

    Value launch(Interpreter& vm, const std::string& program,
                 const std::vector<std::string>& args, const Value& options) {
        if (!allowed(program))
            return denied_result("process-start", "program is not in the executable allowlist: " +
                                                      program);
        const std::string resolved = locate(program);
        if (resolved.empty())
            return error_result("program not found: " + program, "not-found", "process-start");
        if (::access(resolved.c_str(), X_OK) != 0)
            return errno_error("process-start", errno, resolved);

        int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
        if (::pipe(in_pipe) != 0) return errno_error("process-start", errno, program);
        if (::pipe(out_pipe) != 0) {
            ::close(in_pipe[0]); ::close(in_pipe[1]);
            return errno_error("process-start", errno, program);
        }
        if (::pipe(err_pipe) != 0) {
            ::close(in_pipe[0]); ::close(in_pipe[1]);
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            return errno_error("process-start", errno, program);
        }

        std::vector<std::string> environment;
        const Value requested = option(options, "environment");
        if (requested.is_list() && !requested.is_nil()) {
            for (const Value& entry : requested.to_vector()) {
                if (!entry.is_list() || entry.list_size() < 2) continue;
                const Value name = entry.car(), value = entry.list_at(1);
                if (name.type() != Value::Type::String || value.type() != Value::Type::String) continue;
                environment.push_back(std::string(name.as_string()) + "=" +
                                      std::string(value.as_string()));
            }
        } else {
            for (char** entry = environ; entry && *entry; ++entry) {
                const std::string text = *entry;
                if (policy_.environment_allowlist.empty()) { environment.push_back(text); continue; }
                const std::size_t equals = text.find('=');
                const std::string name = text.substr(0, equals);
                if (std::find(policy_.environment_allowlist.begin(),
                              policy_.environment_allowlist.end(),
                              name) != policy_.environment_allowlist.end())
                    environment.push_back(text);
            }
        }
        // Every other path in the API is relative to the sandbox root; a child's
        // working directory has to be too, or callers silently get a chdir failure
        // (exit 126, no output) for a path that reads as perfectly valid. An
        // absolute path is still accepted, and still has to be inside the root.
        std::string directory = string_option(options, "directory", policy_.root);
        if (!directory.empty() && directory != policy_.root) {
            if (directory.front() != '/') directory = policy_.root + "/" + directory;
            char real[PATH_MAX], root_real[PATH_MAX];
            if (::realpath(directory.c_str(), real) == nullptr)
                return errno_error("process-start", errno, directory);
            directory = real;
            if (::realpath(policy_.root.c_str(), root_real) != nullptr) {
                const std::string root_text(root_real);
                if (directory != root_text &&
                    directory.compare(0, root_text.size() + 1, root_text + "/") != 0)
                    return denied_result("process-start",
                                         "working directory escapes the sandbox root");
            }
        }

        std::vector<std::string> argv_storage{resolved};
        argv_storage.insert(argv_storage.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (std::string& entry : argv_storage) argv.push_back(entry.data());
        argv.push_back(nullptr);
        std::vector<char*> envp;
        for (std::string& entry : environment) envp.push_back(entry.data());
        envp.push_back(nullptr);

        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(in_pipe[0]); ::close(in_pipe[1]);
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            ::close(err_pipe[0]); ::close(err_pipe[1]);
            return errno_error("process-start", errno, program);
        }
        if (pid == 0) {
            ::dup2(in_pipe[0], STDIN_FILENO);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::dup2(err_pipe[1], STDERR_FILENO);
            ::close(in_pipe[0]); ::close(in_pipe[1]);
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            ::close(err_pipe[0]); ::close(err_pipe[1]);
            // A fresh session keeps the child off the embedding process's terminal.
            ::setsid();
            if (!directory.empty() && ::chdir(directory.c_str()) != 0) ::_exit(126);
            ::execve(resolved.c_str(), argv.data(), envp.data());
            ::_exit(127);
        }
        ::close(in_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        ::fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
        ::fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

        auto job = std::make_shared<Job>();
        job->pid = pid;
        job->input = in_pipe[1];
        job->output = out_pipe[0];
        job->errors = err_pipe[0];
        job->program = resolved;
        job->started = std::chrono::steady_clock::now();
        job->timeout_ms = number_option(options, "timeout-ms", policy_.default_timeout_ms);

        const Value stdin_text = option(options, "stdin");
        if (stdin_text.type() == Value::Type::String) {
            const std::string_view data = stdin_text.as_string();
            std::size_t written = 0;
            while (written < data.size()) {
                const ssize_t step = ::write(job->input, data.data() + written,
                                             data.size() - written);
                if (step <= 0) break;
                written += static_cast<std::size_t>(step);
            }
            ::close(job->input);
            job->input = -1;
        }
        const Value handle = vm.make_handle(*this, "job", job);
        return job_record(vm, handle, job, false);
    }

    Value run_start(Interpreter& vm, const std::vector<Value>& arguments) {
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        const Value program = option(options, "program");
        if (program.type() != Value::Type::String)
            return error_result("process-start requires a program name", "invalid-argument",
                                "process-start");
        return launch(vm, std::string(program.as_string()),
                      string_list(option(options, "arguments")), options);
    }

    Value run_poll(Interpreter& vm, const std::vector<Value>& arguments, bool block) {
        if (arguments.empty()) fail("this operation expects a job handle");
        std::shared_ptr<Job> job = job_of(vm, arguments[0]);
        if (!job) return error_result("job handle is not live", "invalid-handle", "job-poll");
        const Value options = options_at(arguments, 1);
        const std::int64_t deadline = number_option(options, "timeout-ms", job->timeout_ms);
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            drain(job);
            if (reap(job, false)) break;
            if (!block) break;
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (deadline > 0 && waited.count() >= deadline) {
                ::kill(job->pid, SIGKILL);
                reap(job, true);
                drain(job);
                job->cancelled = true;
                Value record = job_record(vm, arguments[0], job, true);
                return Value::improper(record.to_vector(),
                                       Value::list({field("timed-out", true)}));
            }
            struct pollfd fds[2];
            int count = 0;
            if (job->output >= 0) { fds[count].fd = job->output; fds[count].events = POLLIN; ++count; }
            if (job->errors >= 0) { fds[count].fd = job->errors; fds[count].events = POLLIN; ++count; }
            if (count == 0) { reap(job, true); drain(job); break; }
            ::poll(fds, static_cast<nfds_t>(count), 50);
        }
        return job_record(vm, arguments[0], job, true);
    }

    Value run_timeout(Interpreter& vm, const std::vector<Value>& arguments) {
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        Value started = run_start(vm, arguments);
        const Value handle = option(started, "job");
        if (handle.type() != Value::Type::Handle) return started;
        std::vector<Value> wait_arguments{handle,
                                          Value::list({field("timeout-ms",
                                                             number_option(options, "timeout-ms",
                                                                           policy_.default_timeout_ms))})};
        return run_poll(vm, wait_arguments, true);
    }

    Value run_cancel(Interpreter& vm, const std::vector<Value>& arguments) {
        if (arguments.empty()) fail("job-cancel expects a job handle");
        std::shared_ptr<Job> job = job_of(vm, arguments[0]);
        if (!job) return error_result("job handle is not live", "invalid-handle", "job-cancel");
        const Value options = options_at(arguments, 1);
        const std::int64_t which = number_option(options, "signal", SIGTERM);
        if (!job->finished) {
            ::kill(job->pid, static_cast<int>(which));
            job->cancelled = true;
            reap(job, false);
        }
        drain(job);
        return job_record(vm, arguments[0], job, true);
    }

    Value run_write(Interpreter& vm, const std::vector<Value>& arguments) {
        if (arguments.size() < 2) fail("job-input expects a handle and data");
        std::shared_ptr<Job> job = job_of(vm, arguments[0]);
        if (!job) return error_result("job handle is not live", "invalid-handle", "job-input");
        if (job->input < 0)
            return error_result("job input is closed", "closed-stream", "job-input");
        if (arguments[1].type() != Value::Type::String) fail("job-input expects string data");
        const std::string_view data = arguments[1].as_string();
        std::size_t written = 0;
        while (written < data.size()) {
            const ssize_t step = ::write(job->input, data.data() + written, data.size() - written);
            if (step <= 0) break;
            written += static_cast<std::size_t>(step);
        }
        return ok_result({field("bytes", static_cast<std::int64_t>(written))});
    }

    Value run_close_input(Interpreter& vm, const std::vector<Value>& arguments) {
        if (arguments.empty()) fail("job-close-input expects a handle");
        std::shared_ptr<Job> job = job_of(vm, arguments[0]);
        if (!job) return error_result("job handle is not live", "invalid-handle", "job-close-input");
        if (job->input >= 0) { ::close(job->input); job->input = -1; }
        return ok_result({field("closed", true)});
    }

    Value run_read(Interpreter& vm, const std::vector<Value>& arguments, bool errors) {
        if (arguments.empty()) fail("this operation expects a job handle");
        std::shared_ptr<Job> job = job_of(vm, arguments[0]);
        if (!job) return error_result("job handle is not live", "invalid-handle", "job-output");
        drain(job);
        reap(job, false);
        const std::string& sink = errors ? job->err : job->out;
        return ok_result({field("text", sink),
                          field("bytes", static_cast<std::int64_t>(sink.size())),
                          field("truncated", errors ? job->error_truncated : job->output_truncated),
                          field("finished", job->finished)});
    }

    Value run_wait4path(const std::vector<Value>& arguments) {
        if (arguments.empty()) fail("wait4path expects a path");
        const std::string requested(text_argument(arguments, 0));
        // Relative paths are interpreted against the same root the filesystem
        // capability uses, so waiting never reaches outside the sandbox.
        const std::string path = normalize(requested.empty() || requested[0] == '/'
                                               ? requested
                                               : policy_.root + "/" + requested);
        if (!inside(real_or_empty(policy_.root), path) && !inside(policy_.root, path))
            return error_result("path escapes the capability root", "outside-root", "wait4path",
                                {field("path", requested)});
        const Value options = options_at(arguments, 1);
        const std::int64_t deadline = number_option(options, "timeout-ms", 5000);
        const std::int64_t interval = std::max<std::int64_t>(
            1, number_option(options, "interval-ms", 25));
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            if (::access(path.c_str(), F_OK) == 0)
                return ok_result({field("path", requested), field("present", true)});
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (waited.count() >= deadline)
                return ok_result({field("path", requested), field("present", false),
                                  field("timed-out", true)});
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    }

    // Process inspection reads the kernel's own listing; there is no shelling out.
    std::vector<std::pair<pid_t, std::string>> process_table() const {
        std::vector<std::pair<pid_t, std::string>> out;
#if defined(__linux__)
        DIR* directory = ::opendir("/proc");
        if (!directory) return out;
        while (const dirent* item = ::readdir(directory)) {
            const std::string name = item->d_name;
            if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;
            const pid_t pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
            const std::string command_path = "/proc/" + name + "/cmdline";
            const int descriptor = ::open(command_path.c_str(), O_RDONLY | O_CLOEXEC);
            std::string command;
            if (descriptor >= 0) {
                char buffer[4096];
                const ssize_t got = ::read(descriptor, buffer, sizeof buffer);
                ::close(descriptor);
                for (ssize_t i = 0; i < got; ++i)
                    command += buffer[i] == '\0' ? ' ' : buffer[i];
                while (!command.empty() && command.back() == ' ') command.pop_back();
            }
            if (command.empty()) {
                const std::string status_path = "/proc/" + name + "/comm";
                const int status = ::open(status_path.c_str(), O_RDONLY | O_CLOEXEC);
                if (status >= 0) {
                    char buffer[256];
                    const ssize_t got = ::read(status, buffer, sizeof buffer);
                    ::close(status);
                    if (got > 0) command.assign(buffer, static_cast<std::size_t>(got));
                    while (!command.empty() && command.back() == '\n') command.pop_back();
                }
            }
            out.emplace_back(pid, command);
        }
        ::closedir(directory);
#elif defined(__APPLE__)
        int name[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
        std::size_t size = 0;
        if (::sysctl(name, 4, nullptr, &size, nullptr, 0) != 0) return out;
        std::vector<char> buffer(size);
        if (::sysctl(name, 4, buffer.data(), &size, nullptr, 0) != 0) return out;
        const auto* entries = reinterpret_cast<const struct kinfo_proc*>(buffer.data());
        const std::size_t count = size / sizeof(struct kinfo_proc);
        for (std::size_t i = 0; i < count; ++i)
            out.emplace_back(entries[i].kp_proc.p_pid, entries[i].kp_proc.p_comm);
#endif
        std::sort(out.begin(), out.end());
        return out;
    }

    Value run_ps(const std::vector<Value>& arguments) {
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        const auto table = process_table();
        if (table.empty())
            return unsupported_result("ps", "process listing is unavailable on this platform");
        const std::int64_t limit = number_option(options, "limit", 4096);
        std::vector<Value> entries;
        for (const auto& entry : table) {
            if (limit > 0 && static_cast<std::int64_t>(entries.size()) >= limit) break;
            entries.push_back(ok_result({field("pid", static_cast<std::int64_t>(entry.first)),
                                         field("command", entry.second)}));
        }
        const std::int64_t count = static_cast<std::int64_t>(entries.size());
        return ok_result({field("processes", Value::list(std::move(entries))), field("count", count)});
    }

    Value run_pgrep(const std::vector<Value>& arguments, bool terminate) {
        if (arguments.empty()) fail("pgrep expects a pattern");
        const std::string pattern(text_argument(arguments, 0));
        const Value options = options_at(arguments, 1);
        if (terminate && !policy_.allow_process)
            return denied_result("pkill", "signalling processes is disabled by policy");
        std::regex expression;
        try {
            expression = std::regex(pattern, std::regex::ECMAScript);
        } catch (const std::regex_error& error) {
            return error_result(error.what(), "invalid-pattern", terminate ? "pkill" : "pgrep");
        }
        const auto table = process_table();
        if (table.empty())
            return unsupported_result(terminate ? "pkill" : "pgrep",
                                      "process listing is unavailable on this platform");
        const int which = static_cast<int>(number_option(options, "signal", SIGTERM));
        std::vector<Value> entries;
        std::int64_t signalled = 0;
        for (const auto& entry : table) {
            if (!std::regex_search(entry.second, expression)) continue;
            if (entry.first == ::getpid()) continue;
            entries.push_back(ok_result({field("pid", static_cast<std::int64_t>(entry.first)),
                                         field("command", entry.second)}));
            if (terminate && ::kill(entry.first, which) == 0) ++signalled;
        }
        const std::int64_t count = static_cast<std::int64_t>(entries.size());
        if (terminate)
            return ok_result({field("matched", Value::list(std::move(entries))), field("count", count),
                              field("signalled", signalled)});
        return ok_result({field("matched", Value::list(std::move(entries))), field("count", count)});
    }

    Value run_kill(const std::vector<Value>& arguments) {
        if (!policy_.allow_process)
            return denied_result("kill", "signalling processes is disabled by policy");
        if (arguments.empty() || arguments[0].type() != Value::Type::Integer)
            return error_result("kill expects a process id", "invalid-argument", "kill");
        const Value options = options_at(arguments, 1);
        const int which = static_cast<int>(number_option(options, "signal", SIGTERM));
        const pid_t pid = static_cast<pid_t>(arguments[0].as_integer());
        if (pid <= 0)
            return error_result("refusing to signal a process group or every process",
                                "invalid-argument", "kill");
        if (::kill(pid, which) != 0) return errno_error("kill", errno, {});
        return ok_result({field("pid", static_cast<std::int64_t>(pid)),
                          field("signal", static_cast<std::int64_t>(which))});
    }

    Value run_nice(const std::vector<Value>& arguments) {
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        const std::int64_t increment = number_option(options, "increment", 0);
        errno = 0;
        const int current = ::nice(0);
        if (increment == 0)
            return ok_result({field("priority", static_cast<std::int64_t>(current))});
        if (!policy_.allow_process)
            return denied_result("nice", "changing scheduling priority is disabled by policy");
        errno = 0;
        const int updated = ::nice(static_cast<int>(increment));
        if (updated == -1 && errno) return errno_error("nice", errno, {});
        return ok_result({field("priority", static_cast<std::int64_t>(updated)),
                          field("previous", static_cast<std::int64_t>(current))});
    }
};

// ---------------------------------------------------------------------------
// Clock, system, terminal, crypto, shell, service, logging, desktop
// ---------------------------------------------------------------------------

class PosixClock final : public ClockCapability {
public:
    bool supports(std::string_view operation) const override {
        static const std::set<std::string_view> known = {"date", "sleep", "time", "uptime"};
        return known.count(operation) != 0;
    }

    Value invoke(Interpreter&, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        if (operation == "date") {
            const std::int64_t when = number_option(options, "epoch-seconds",
                                                    static_cast<std::int64_t>(std::time(nullptr)));
            const bool utc = flag_option(options, "utc", true);
            const std::time_t stamp = static_cast<std::time_t>(when);
            struct tm parts {};
            if (utc) ::gmtime_r(&stamp, &parts);
            else ::localtime_r(&stamp, &parts);
            char iso[64];
            std::strftime(iso, sizeof iso, utc ? "%Y-%m-%dT%H:%M:%SZ" : "%Y-%m-%dT%H:%M:%S%z", &parts);
            const std::string format = string_option(options, "format", "");
            std::string formatted = iso;
            if (!format.empty()) {
                char custom[256];
                const std::size_t length = std::strftime(custom, sizeof custom, format.c_str(), &parts);
                formatted.assign(custom, length);
            }
            return ok_result({field("epoch-seconds", when), field("iso", iso),
                              field("text", formatted), field("utc", utc),
                              field("year", static_cast<std::int64_t>(parts.tm_year + 1900)),
                              field("month", static_cast<std::int64_t>(parts.tm_mon + 1)),
                              field("day", static_cast<std::int64_t>(parts.tm_mday)),
                              field("hour", static_cast<std::int64_t>(parts.tm_hour)),
                              field("minute", static_cast<std::int64_t>(parts.tm_min)),
                              field("second", static_cast<std::int64_t>(parts.tm_sec))});
        }
        if (operation == "time") {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return ok_result({field("epoch-seconds", static_cast<std::int64_t>(
                                  std::chrono::duration_cast<std::chrono::seconds>(now).count())),
                              field("epoch-milliseconds", static_cast<std::int64_t>(
                                  std::chrono::duration_cast<std::chrono::milliseconds>(now).count())),
                              field("monotonic-nanoseconds", static_cast<std::int64_t>(
                                  std::chrono::steady_clock::now().time_since_epoch().count()))});
        }
        if (operation == "sleep") {
            std::int64_t milliseconds = number_option(options, "milliseconds", -1);
            if (milliseconds < 0 && !arguments.empty()) {
                if (arguments[0].type() == Value::Type::Integer)
                    milliseconds = arguments[0].as_integer() * 1000;
                else if (arguments[0].type() == Value::Type::Float)
                    milliseconds = static_cast<std::int64_t>(arguments[0].as_float() * 1000);
            }
            if (milliseconds < 0) milliseconds = 0;
            // Bounded so a runaway script cannot park the embedding agent forever.
            if (milliseconds > 3600000)
                return error_result("sleep is limited to one hour", "invalid-argument", "sleep");
            const auto start = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            return ok_result({symbol_field("state", "completed"),
                              field("requested-ms", milliseconds),
                              field("elapsed-ms", static_cast<std::int64_t>(elapsed.count()))});
        }
        if (operation == "uptime") {
#if defined(__linux__)
            struct sysinfo info {};
            if (::sysinfo(&info) != 0) return errno_error("uptime", errno, {});
            return ok_result({field("seconds", static_cast<std::int64_t>(info.uptime)),
                              field("processes", static_cast<std::int64_t>(info.procs)),
                              field("load-1", static_cast<std::int64_t>(info.loads[0]))});
#elif defined(__APPLE__)
            struct timeval boot {};
            std::size_t size = sizeof boot;
            int name[2] = {CTL_KERN, KERN_BOOTTIME};
            if (::sysctl(name, 2, &boot, &size, nullptr, 0) != 0)
                return errno_error("uptime", errno, {});
            return ok_result({field("seconds", static_cast<std::int64_t>(
                                  std::time(nullptr) - boot.tv_sec))});
#else
            return unsupported_result("uptime", "uptime is unavailable on this platform");
#endif
        }
        return unsupported_result(operation, "the POSIX clock adapter does not implement this");
    }
};

class PosixSystem final : public SystemCapability {
public:
    explicit PosixSystem(const Policy& policy) : policy_(policy) {}

    bool supports(std::string_view operation) const override {
        static const std::set<std::string_view> known = {
            "env", "hostname", "id", "uname", "users", "who", "whoami", "which", "whereis"};
        return known.count(operation) != 0;
    }

    Value invoke(Interpreter&, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        if (operation == "env") return run_env(arguments);
        if (operation == "hostname") {
            char buffer[256];
            if (::gethostname(buffer, sizeof buffer) != 0) return errno_error("hostname", errno, {});
            buffer[sizeof buffer - 1] = '\0';
            return ok_result({field("hostname", buffer)});
        }
        if (operation == "whoami" || operation == "id") return run_identity(operation);
        if (operation == "uname") {
            struct utsname info {};
            if (::uname(&info) != 0) return errno_error("uname", errno, {});
            return ok_result({field("system", info.sysname), field("node", info.nodename),
                              field("release", info.release), field("version", info.version),
                              field("machine", info.machine)});
        }
        if (operation == "users" || operation == "who") {
            // Reporting only the current session avoids exposing the host's session
            // database through a capability that was granted for identity alone.
            const passwd* entry = ::getpwuid(::getuid());
            std::vector<Value> sessions;
            if (entry)
                sessions.push_back(ok_result({field("user", entry->pw_name),
                                              field("terminal", ::ttyname(STDIN_FILENO)
                                                                    ? ::ttyname(STDIN_FILENO) : "")}));
            const std::int64_t count = static_cast<std::int64_t>(sessions.size());
            return ok_result({field("sessions", Value::list(std::move(sessions))),
                              field("count", count),
                              field("partial", true)});
        }
        if (operation == "which" || operation == "whereis") return run_which(arguments, operation);
        return unsupported_result(operation, "the POSIX system adapter does not implement this");
    }

private:
    Policy policy_;

    bool visible(const std::string& name) const {
        if (policy_.environment_allowlist.empty()) return true;
        return std::find(policy_.environment_allowlist.begin(), policy_.environment_allowlist.end(),
                         name) != policy_.environment_allowlist.end();
    }

    Value run_env(const std::vector<Value>& arguments) const {
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        const std::string wanted = string_option(options, "name", "");
        std::vector<Value> entries;
        for (char** entry = environ; entry && *entry; ++entry) {
            const std::string text = *entry;
            const std::size_t equals = text.find('=');
            if (equals == std::string::npos) continue;
            const std::string name = text.substr(0, equals);
            if (!visible(name)) continue;
            if (!wanted.empty() && wanted != name) continue;
            entries.push_back(Value::list({Value::string(name), Value::string(text.substr(equals + 1))}));
        }
        const std::int64_t count = static_cast<std::int64_t>(entries.size());
        return ok_result({field("variables", Value::list(std::move(entries))), field("count", count),
                          field("filtered", !policy_.environment_allowlist.empty())});
    }

    Value run_identity(std::string_view operation) const {
        const uid_t uid = ::getuid();
        const gid_t gid = ::getgid();
        const passwd* user = ::getpwuid(uid);
        const group* primary = ::getgrgid(gid);
        if (operation == "whoami")
            return ok_result({field("user", user ? user->pw_name : "")});
        return ok_result({field("user", user ? user->pw_name : ""),
                          field("uid", static_cast<std::int64_t>(uid)),
                          field("gid", static_cast<std::int64_t>(gid)),
                          field("group", primary ? primary->gr_name : ""),
                          field("effective-uid", static_cast<std::int64_t>(::geteuid())),
                          field("effective-gid", static_cast<std::int64_t>(::getegid()))});
    }

    Value run_which(const std::vector<Value>& arguments, std::string_view operation) const {
        if (arguments.empty()) fail("which expects a program name");
        const std::string program(text_argument(arguments, 0));
        std::vector<std::string> paths = policy_.search_paths;
        if (paths.empty()) {
            const char* env = ::getenv("PATH");
            const std::string text = env ? env : "/usr/bin:/bin";
            std::size_t begin = 0;
            for (;;) {
                const std::size_t colon = text.find(':', begin);
                paths.push_back(text.substr(begin, colon == std::string::npos
                                                       ? std::string::npos : colon - begin));
                if (colon == std::string::npos) break;
                begin = colon + 1;
            }
        }
        std::vector<Value> found;
        for (const std::string& directory : paths) {
            if (directory.empty()) continue;
            const std::string candidate = directory + "/" + program;
            if (::access(candidate.c_str(), X_OK) != 0) continue;
            found.push_back(Value::string(candidate));
            if (operation == "which") break;
        }
        if (found.empty())
            return error_result("program not found: " + program, "not-found", operation);
        return ok_result({field("program", program), field("paths", Value::list(found)),
                          field("path", found.front())});
    }
};

class PosixTerminal final : public TerminalCapability {
public:
    explicit PosixTerminal(const Policy& policy) : policy_(policy) {}

    Value invoke(Interpreter&, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        if (operation == "tty") {
            const char* name = ::ttyname(STDIN_FILENO);
            return ok_result({field("tty", name ? name : ""),
                              field("interactive", ::isatty(STDIN_FILENO) == 1)});
        }
        if (operation == "stty") {
            // `stty` never touches the embedding process's terminal implicitly: it
            // requires an explicit terminal handle, which this adapter does not mint.
            const Value options = arguments.empty() ? Value::nil() : arguments[0];
            if (option(options, "terminal").type() == Value::Type::Unspecified)
                return error_result("stty requires an explicit terminal handle",
                                    "invalid-argument", "stty");
            if (!policy_.allow_terminal)
                return denied_result("stty", "terminal control is disabled by policy");
            return unsupported_result("stty",
                                      "this adapter does not mint terminal handles; install a "
                                      "terminal capability that does");
        }
        return unsupported_result(operation, "the POSIX terminal adapter does not implement this");
    }

private:
    Policy policy_;
};

class NativeCrypto final : public CryptoCapability {
public:
    bool supports(std::string_view operation) const override {
        static const std::set<std::string_view> known = {"hash", "md5", "shasum"};
        return known.count(operation) != 0;
    }

    Value invoke(Interpreter&, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        if (arguments.empty() || arguments[0].type() != Value::Type::String)
            return error_result("a digest requires string input", "invalid-argument", operation);
        const std::string_view input = arguments[0].as_string();
        const Value options = options_at(arguments, 1);
        std::string algorithm = string_option(options, "algorithm", "");
        if (algorithm.empty()) {
            const Value symbolic = option(options, "algorithm");
            if (symbolic.type() == Value::Type::Symbol) algorithm = std::string(symbolic.as_symbol());
        }
        if (operation == "md5") algorithm = "md5";
        if (operation == "shasum" && algorithm.empty()) algorithm = "sha1";
        if (algorithm.empty()) algorithm = "sha256";

        std::string digest;
        if (algorithm == "md5") digest = md5_digest(input);
        else if (algorithm == "sha1") digest = sha1_digest(input);
        else if (algorithm == "sha256") digest = sha256_digest(input);
        else
            return error_result("unsupported digest algorithm: " + algorithm, "unsupported",
                                operation, {field("available",
                                                  Value::list({Value::symbol("md5"),
                                                               Value::symbol("sha1"),
                                                               Value::symbol("sha256")}))});
        std::string raw;
        for (std::size_t i = 0; i + 1 < digest.size(); i += 2)
            raw += static_cast<char>(std::strtol(digest.substr(i, 2).c_str(), nullptr, 16));
        std::vector<Value> bytes;
        for (const char c : raw) bytes.push_back(Value::integer(static_cast<unsigned char>(c)));
        return ok_result({symbol_field("algorithm", algorithm), field("hex", digest),
                          field("bytes", Value::list(std::move(bytes))),
                          field("length", static_cast<std::int64_t>(input.size()))});
    }
};

// Shell dialects run as ordinary child processes. No shell implementation is embedded,
// so nothing here inherits a copyleft licence from a shell code base.
class PosixShell final : public ShellCapability {
public:
    PosixShell(const Policy& policy, std::shared_ptr<PosixProcess> processes)
        : policy_(policy), processes_(std::move(processes)) {}

    Value invoke(Interpreter& vm, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        if (!policy_.allow_process)
            return denied_result(operation, "shell dispatch requires the process capability");
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        const Value command = option(options, "command");
        if (command.type() != Value::Type::String)
            return error_result("a shell invocation requires a command string", "invalid-argument",
                                operation);
        const std::string dialect(operation);
        std::vector<std::string> args;
        if (flag_option(options, "login")) args.push_back("-l");
        args.push_back("-c");
        args.emplace_back(command.as_string());
        Value started = processes_->start(vm, dialect, args, options);
        if (option(started, "error").type() == Value::Type::String) return started;
        std::vector<Value> fields = started.to_vector();
        fields.push_back(symbol_field("dialect", dialect));
        return Value::list(std::move(fields));
    }

private:
    Policy policy_;
    std::shared_ptr<PosixProcess> processes_;
};

class PosixService final : public ServiceCapability {
public:
    PosixService(const Policy& policy, std::shared_ptr<PosixProcess> processes)
        : policy_(policy), processes_(std::move(processes)) {}

    Value invoke(Interpreter& vm, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        if (!policy_.allow_service)
            return denied_result(operation, "service control is disabled by policy");
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        if (operation == "service-list") {
#if defined(__APPLE__)
            return processes_->start(vm, "launchctl", {"list"}, options);
#elif defined(__linux__)
            return processes_->start(vm, "systemctl", {"list-units", "--no-pager"}, options);
#else
            return unsupported_result("service-list", "no service manager on this platform");
#endif
        }
        std::vector<std::string> args;
        for (const std::string& argument : string_list(option(options, "arguments")))
            args.push_back(argument);
        const Value subcommand = option(options, "operation");
        if (subcommand.type() == Value::Type::Symbol)
            args.insert(args.begin(), std::string(subcommand.as_symbol()));
        return processes_->start(vm, std::string(operation), args, options);
    }

private:
    Policy policy_;
    std::shared_ptr<PosixProcess> processes_;
};

// Typed HTTP over the host's curl. The value here is the typed request and the
// structured response -- status, headers, and body as separate fields -- not
// avoiding curl itself, which the roadmap keeps host-provided. Swapping in a
// libcurl-backed adapter later changes nothing above this class.
std::vector<std::string> split_lines_local(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == '\n') { out.push_back(current); current.clear(); }
        else if (c != '\r') current += c;
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

class PosixHttp final : public HttpCapability {
public:
    PosixHttp(const Policy& policy, std::shared_ptr<PosixProcess> processes)
        : policy_(policy), processes_(std::move(processes)) {}

    bool supports(std::string_view operation) const override {
        return operation == "http-request" || operation == "curl";
    }

    Value invoke(Interpreter& vm, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        if (!policy_.allow_process)
            return denied_result(operation, "HTTP requires the process capability");
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        const Value url = option(options, "url");
        if (url.type() != Value::Type::String)
            return error_result("a request needs a url", "invalid-argument", operation);

        const std::string method = string_option(options, "method", "GET");
        const std::int64_t timeout = number_option(options, "timeout-ms",
                                                   policy_.default_timeout_ms);
        const std::int64_t max_bytes = number_option(options, "max-bytes",
                                                     static_cast<std::int64_t>(policy_.output_limit));

        // `-D -` writes the response headers to stdout ahead of the body, separated
        // by a blank line, so one request yields both without a temporary file.
        std::vector<std::string> args{"--silent", "--show-error", "--dump-header", "-",
                                      "--max-time", std::to_string((timeout + 999) / 1000),
                                      "--request", method};
        if (flag_option(options, "follow-redirects", true)) args.push_back("--location");
        if (max_bytes > 0) {
            args.push_back("--max-filesize");
            args.push_back(std::to_string(max_bytes));
        }
        const Value headers = option(options, "headers");
        if (headers.is_list())
            for (const Value& entry : headers.to_vector()) {
                if (!entry.is_list() || entry.list_size() < 2) continue;
                const Value name = entry.car(), value = entry.list_at(1);
                if (name.type() != Value::Type::String || value.type() != Value::Type::String)
                    continue;
                args.push_back("--header");
                args.push_back(std::string(name.as_string()) + ": " + std::string(value.as_string()));
            }
        const Value body = option(options, "body");
        if (body.type() == Value::Type::String) {
            args.push_back("--data-binary");
            args.push_back(std::string(body.as_string()));
        }
        args.push_back(std::string(url.as_string()));

        const Value started = processes_->start(vm, "curl", args, options);
        if (option(started, "error").type() == Value::Type::String) return started;
        const Value job = option(started, "job");
        if (job.type() != Value::Type::Handle)
            return error_result("no job handle for the request", "host-error", operation);
        const Value finished = processes_->invoke(vm, "process-wait", {job});
        if (option(finished, "error").type() == Value::Type::String) return finished;

        const Value out = option(finished, "stdout");
        const Value err = option(finished, "stderr");
        const std::int64_t exit_status = option(finished, "exit-status").type() ==
                                                 Value::Type::Integer
                                             ? option(finished, "exit-status").as_integer()
                                             : -1;
        if (exit_status != 0)
            return error_result(err.type() == Value::Type::String && !err.as_string().empty()
                                    ? std::string(err.as_string())
                                    : "request failed",
                                "host-error", operation,
                                {field("exit-status", exit_status),
                                 field("url", url)});

        std::string text = out.type() == Value::Type::String ? std::string(out.as_string()) : "";
        // Redirects emit one header block per hop; the last one describes the answer.
        std::int64_t status = 0;
        std::vector<Value> header_fields;
        std::size_t body_at = 0;
        for (;;) {
            const std::size_t blank = text.find("\r\n\r\n", body_at);
            const std::size_t blank2 = text.find("\n\n", body_at);
            std::size_t end = blank, skip = 4;
            if (blank == std::string::npos || (blank2 != std::string::npos && blank2 < blank)) {
                end = blank2;
                skip = 2;
            }
            if (end == std::string::npos) break;
            const std::string block = text.substr(body_at, end - body_at);
            if (block.rfind("HTTP/", 0) != 0) break;
            header_fields.clear();
            status = 0;
            for (const std::string& raw : split_lines_local(block)) {
                if (raw.rfind("HTTP/", 0) == 0) {
                    const std::size_t space = raw.find(' ');
                    if (space != std::string::npos)
                        status = std::strtoll(raw.c_str() + space + 1, nullptr, 10);
                    continue;
                }
                const std::size_t colon = raw.find(':');
                if (colon == std::string::npos) continue;
                std::string name = raw.substr(0, colon);
                std::string value = raw.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.erase(value.begin());
                while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
                    value.pop_back();
                for (char& c : name) c = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(c)));
                header_fields.push_back(Value::list({Value::string(name), Value::string(value)}));
            }
            body_at = end + skip;
        }
        const std::string response_body = text.substr(std::min(body_at, text.size()));
        ListBuilder result(6);
        result.field("status", status);
        result.field("ok", status >= 200 && status < 300);
        result.field("headers", Value::list(std::move(header_fields)));
        result.field("body", response_body);
        result.field("bytes", static_cast<std::int64_t>(response_body.size()));
        result.field("url", url);
        return result.build();
    }

private:
    Policy policy_;
    std::shared_ptr<PosixProcess> processes_;
};

class PosixLogging final : public LoggingCapability {
public:
    explicit PosixLogging(const Policy& policy) : policy_(policy) {}

    Value invoke(Interpreter&, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        if (!policy_.allow_logging)
            return denied_result(operation, "system logging is disabled by policy");
        const Value options = arguments.empty() ? Value::nil() : arguments[0];
        const std::string message = string_option(options, "message", "");
        if (message.empty())
            return error_result("logger requires a message", "invalid-argument", "logger");
        const std::string tag = string_option(options, "tag", "toolscheme");
        ::openlog(tag.c_str(), LOG_PID, LOG_USER);
        ::syslog(LOG_INFO, "%s", message.c_str());
        ::closelog();
        return ok_result({field("logged", true), field("tag", tag),
                          field("bytes", static_cast<std::int64_t>(message.size()))});
    }

private:
    Policy policy_;
};

class PosixDesktop final : public DesktopCapability {
public:
    PosixDesktop(const Policy& policy, std::shared_ptr<PosixProcess> processes)
        : policy_(policy), processes_(std::move(processes)) {}

    Value invoke(Interpreter& vm, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        if (!policy_.allow_desktop)
            return denied_result(operation, "desktop integration is disabled by policy");
        if (arguments.empty() || arguments[0].type() != Value::Type::String)
            return error_result("open requires a target", "invalid-argument", "open");
        const std::string target(arguments[0].as_string());
        const Value options = options_at(arguments, 1);
#if defined(__APPLE__)
        return processes_->start(vm, "open", {target}, options);
#elif defined(__linux__)
        return processes_->start(vm, "xdg-open", {target}, options);
#else
        (void)vm; (void)options;
        return unsupported_result("open", "no desktop opener on this platform");
#endif
    }

private:
    Policy policy_;
    std::shared_ptr<PosixProcess> processes_;
};

} // namespace

std::string md5(std::string_view bytes) { return md5_digest(bytes); }
std::string sha1(std::string_view bytes) { return sha1_digest(bytes); }
std::string sha256(std::string_view bytes) { return sha256_digest(bytes); }

std::shared_ptr<FileSystemCapability> make_filesystem(const Policy& policy) {
    return std::make_shared<PosixFileSystem>(policy);
}
std::shared_ptr<ProcessCapability> make_process(const Policy& policy) {
    return std::make_shared<PosixProcess>(policy);
}
std::shared_ptr<ClockCapability> make_clock() { return std::make_shared<PosixClock>(); }
std::shared_ptr<SystemCapability> make_system(const Policy& policy) {
    return std::make_shared<PosixSystem>(policy);
}
std::shared_ptr<TerminalCapability> make_terminal(const Policy& policy) {
    return std::make_shared<PosixTerminal>(policy);
}
std::shared_ptr<CryptoCapability> make_crypto() { return std::make_shared<NativeCrypto>(); }

std::shared_ptr<ShellCapability> make_shell(const Policy& policy,
                                            std::shared_ptr<ProcessCapability> processes) {
    auto backend = std::dynamic_pointer_cast<PosixProcess>(processes);
    if (!backend) backend = std::make_shared<PosixProcess>(policy);
    return std::make_shared<PosixShell>(policy, std::move(backend));
}

std::shared_ptr<ServiceCapability> make_service(const Policy& policy,
                                                std::shared_ptr<ProcessCapability> processes) {
    auto backend = std::dynamic_pointer_cast<PosixProcess>(processes);
    if (!backend) backend = std::make_shared<PosixProcess>(policy);
    return std::make_shared<PosixService>(policy, std::move(backend));
}

std::shared_ptr<LoggingCapability> make_logging(const Policy& policy) {
    return std::make_shared<PosixLogging>(policy);
}

std::shared_ptr<DesktopCapability> make_desktop(const Policy& policy,
                                                std::shared_ptr<ProcessCapability> processes) {
    auto backend = std::dynamic_pointer_cast<PosixProcess>(processes);
    if (!backend) backend = std::make_shared<PosixProcess>(policy);
    return std::make_shared<PosixDesktop>(policy, std::move(backend));
}

std::shared_ptr<HttpCapability> make_http(const Policy& policy,
                                          std::shared_ptr<ProcessCapability> processes) {
    auto backend = std::dynamic_pointer_cast<PosixProcess>(processes);
    if (!backend) backend = std::make_shared<PosixProcess>(policy);
    return std::make_shared<PosixHttp>(policy, std::move(backend));
}


// ---------------------------------------------------------------------------
// Waiting for something to happen
// ---------------------------------------------------------------------------
//
// The measured reason this exists: across 30 Codex sessions, 17.7% of all tool
// time -- 9.3 hours of 52.8 -- went to `sleep`, and the four most repeated
// invocations in the entire corpus were identical sleeps of 45 to 60 seconds.
// Beside them sat 1,318 calls writing an empty string to an interactive session
// to find out whether it had finished. That is a fixed guess standing in for an
// event.
//
// Two mechanisms, deliberately: inotify makes this *responsive*, and polling makes
// it *correct*. Events can be missed -- a queue overflows, a path is replaced
// rather than modified, a filesystem does not report at all -- so the predicate is
// re-evaluated on a bounded backoff regardless. Neither is trusted alone.
class PosixWatch final : public WatchCapability {
public:
    explicit PosixWatch(const Policy& policy) : policy_(policy) {}

    bool supports(std::string_view operation) const override { return operation == "wait-for"; }

    Value invoke(Interpreter& vm, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        (void)vm;
        if (operation != "wait-for")
            return unsupported_result(std::string(operation), "unknown watch operation");
        if (arguments.empty())
            return error_result("wait-for needs a condition", "invalid-argument", "wait-for");

        const Value condition = arguments[0];
        const Value options = options_at(arguments, 1);
        std::string kind;
        std::vector<std::string> parts;
        if (!read_condition(condition, kind, parts))
            return error_result("a condition is (changed PATH...), (exists PATH), "
                                "(missing PATH) or (matches PATH TEXT)",
                                "invalid-argument", "wait-for");

        std::int64_t budget = number_option(options, "timeout-ms", policy_.default_wait_ms);
        if (budget < 0) budget = 0;
        if (budget > policy_.max_wait_ms) budget = policy_.max_wait_ms;

        // Paths are resolved through the policy exactly as every other filesystem
        // operation is; waiting on a path is no reason to see outside the root.
        std::vector<std::string> paths;
        const std::size_t path_count = (kind == "matches") ? 1 : parts.size();
        for (std::size_t i = 0; i < path_count; ++i) {
            const Resolved resolved = resolve(parts[i], false);
            if (!resolved.ok) return reject(resolved, "wait-for", parts[i]);
            paths.push_back(resolved.path);
        }
        if (paths.empty())
            return error_result("wait-for needs at least one path", "invalid-argument", "wait-for");
        const std::string needle = (kind == "matches" && parts.size() > 1) ? parts[1] : std::string();

        const auto started = std::chrono::steady_clock::now();
        const auto elapsed_ms = [&started] {
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count());
        };

        Watcher watcher(paths);
        std::string matched;
        for (;;) {
            if (satisfied(kind, paths, needle, matched))
                return report(true, kind, matched, elapsed_ms(), "satisfied");
            const std::int64_t left = budget - elapsed_ms();
            if (left <= 0) return report(false, kind, "", elapsed_ms(), "timeout");
            // Bounded so a missed event costs a quarter second, not the whole wait.
            watcher.wait(std::min<std::int64_t>(left, 250));
        }
    }

private:
    // Paths are watched through their parent directory as well as directly, because
    // a file that is created, renamed over, or deleted produces no event on itself.
    struct Watcher {
        int fd = -1;
        explicit Watcher(const std::vector<std::string>& paths) {
#if defined(__linux__)
            fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (fd < 0) return;
            const unsigned mask = IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO |
                                  IN_MOVED_FROM | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF;
            for (const std::string& path : paths) {
                ::inotify_add_watch(fd, path.c_str(), mask);
                const std::size_t slash = path.find_last_of('/');
                const std::string parent = slash == std::string::npos ? "." : path.substr(0, slash);
                if (!parent.empty()) ::inotify_add_watch(fd, parent.c_str(), mask);
            }
#else
            (void)paths;
#endif
        }
        ~Watcher() { if (fd >= 0) ::close(fd); }
        Watcher(const Watcher&) = delete;
        Watcher& operator=(const Watcher&) = delete;

        // Sleeps until an event arrives or the slice expires. Without inotify this
        // is simply the sleep, which is what every other platform gets until it has
        // an adapter of its own.
        void wait(std::int64_t slice_ms) {
            if (fd < 0) {
                struct timespec pause;
                pause.tv_sec = static_cast<time_t>(slice_ms / 1000);
                pause.tv_nsec = static_cast<long>((slice_ms % 1000) * 1000000L);
                ::nanosleep(&pause, nullptr);
                return;
            }
            struct pollfd entry;
            entry.fd = fd;
            entry.events = POLLIN;
            entry.revents = 0;
            if (::poll(&entry, 1, static_cast<int>(slice_ms)) > 0) {
                char buffer[4096];
                while (::read(fd, buffer, sizeof buffer) > 0) {}
            }
        }
    };

    static bool read_condition(const Value& condition, std::string& kind,
                               std::vector<std::string>& parts) {
        if (!condition.is_list() || condition.is_nil()) return false;
        const Value head = condition.car();
        if (head.type() != Value::Type::Symbol) return false;
        kind = std::string(head.as_symbol());
        if (kind != "changed" && kind != "exists" && kind != "missing" && kind != "matches")
            return false;
        for (Value rest = condition.cdr(); !rest.is_nil(); rest = rest.cdr()) {
            const Value item = rest.car();
            if (item.type() != Value::Type::String) return false;
            parts.push_back(std::string(item.as_string()));
        }
        return !parts.empty() && (kind != "matches" || parts.size() == 2);
    }

    // A change is judged by the identity and size of the file, not by its
    // modification time alone: a build that rewrites a file within the same second
    // leaves mtime untouched on filesystems with coarse timestamps.
    struct Fingerprint {
        bool present = false;
        dev_t device = 0;
        ino_t inode = 0;
        off_t size = 0;
        struct timespec modified {};
        bool operator!=(const Fingerprint& other) const {
            return present != other.present || device != other.device ||
                   inode != other.inode || size != other.size ||
                   modified.tv_sec != other.modified.tv_sec ||
                   modified.tv_nsec != other.modified.tv_nsec;
        }
    };

    static Fingerprint fingerprint(const std::string& path) {
        Fingerprint print;
        struct stat info;
        if (::lstat(path.c_str(), &info) != 0) return print;
        print.present = true;
        print.device = info.st_dev;
        print.inode = info.st_ino;
        print.size = info.st_size;
#if defined(__APPLE__)
        print.modified = info.st_mtimespec;
#else
        print.modified = info.st_mtim;
#endif
        return print;
    }

    bool satisfied(const std::string& kind, const std::vector<std::string>& paths,
                   const std::string& needle, std::string& matched) {
        if (kind == "changed") {
            if (baseline_.empty())
                for (const std::string& path : paths) baseline_.push_back(fingerprint(path));
            for (std::size_t i = 0; i < paths.size(); ++i) {
                if (fingerprint(paths[i]) != baseline_[i]) { matched = paths[i]; return true; }
            }
            return false;
        }
        if (kind == "exists" || kind == "missing") {
            const bool want = (kind == "exists");
            for (const std::string& path : paths) {
                if (fingerprint(path).present == want) { matched = path; return true; }
            }
            return false;
        }
        // matches: the pattern may arrive in a file that does not exist yet, and the
        // file may be large, so this reads with the same bound as everything else
        // rather than pulling an unbounded log into memory on every poll.
        const int descriptor = ::open(paths[0].c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) return false;
        std::string text;
        char buffer[65536];
        for (;;) {
            const ssize_t got = ::read(descriptor, buffer, sizeof buffer);
            if (got <= 0) break;
            text.append(buffer, static_cast<std::size_t>(got));
            if (text.size() > policy_.output_limit) break;
        }
        ::close(descriptor);
        if (text.find(needle) == std::string::npos) return false;
        matched = paths[0];
        return true;
    }

    Value report(bool ok, const std::string& kind, const std::string& path,
                 std::int64_t waited, const char* reason) {
        std::vector<Value> fields;
        fields.push_back(field("satisfied", ok));
        fields.push_back(symbol_field("condition", kind));
        if (!path.empty()) fields.push_back(field("path", relative(path)));
        fields.push_back(symbol_field("reason", reason));
        // Volatile by the usual rule: how long the wait took is host churn, and two
        // identical calls must still read identically unless it is asked for.
        fields.push_back(field("elapsed-ms", waited));
        return ok_result(std::move(fields));
    }

    Policy policy_;
    std::vector<Fingerprint> baseline_;

    // The same containment rules the filesystem capability applies, using the same
    // helpers: normalize lexically, refuse anything outside the root, then resolve
    // the parent for real so a symlink cannot walk out. The final component is left
    // alone because waiting on a path that does not exist yet is the point.
    struct Resolved { bool ok = false; std::string path; std::string reason; std::string code; };

    Resolved resolve(const std::string& raw, bool) const {
        Resolved out;
        if (raw.empty()) { out.reason = "empty path"; out.code = "invalid-path"; return out; }
        if (raw.find('\0') != std::string::npos) {
            out.reason = "path contains an embedded NUL";
            out.code = "invalid-path";
            return out;
        }
        const std::string lexical = normalize(raw[0] == '/' ? raw : policy_.root + "/" + raw);
        if (!inside(policy_.root, lexical)) {
            out.reason = "path escapes the capability root";
            out.code = "outside-root";
            return out;
        }
        const std::size_t slash = lexical.find_last_of('/');
        const std::string parent = slash == 0 ? "/" : lexical.substr(0, slash);
        char real[PATH_MAX];
        if (::realpath(parent.c_str(), real) != nullptr && !inside(policy_.root, real)) {
            out.reason = "path escapes the capability root through a symbolic link";
            out.code = "outside-root";
            return out;
        }
        out.ok = true;
        out.path = lexical;
        return out;
    }

    Value reject(const Resolved& resolved, const char* operation, const std::string& path) const {
        return error_result(resolved.reason, resolved.code, operation, {field("path", path)});
    }

    std::string relative(const std::string& full) const {
        if (full == policy_.root) return ".";
        if (inside(policy_.root, full) && policy_.root != "/")
            return full.substr(policy_.root.size() + 1);
        return full;
    }
};

std::shared_ptr<WatchCapability> make_watch(const Policy& policy) {
    return std::make_shared<PosixWatch>(policy);
}

void install_all(Interpreter& interpreter, const Policy& policy) {
    interpreter.install("filesystem", make_filesystem(policy));
    const std::shared_ptr<ProcessCapability> processes = make_process(policy);
    interpreter.install("process", processes);
    interpreter.install("clock", make_clock());
    interpreter.install("system", make_system(policy));
    interpreter.install("terminal", make_terminal(policy));
    interpreter.install("crypto", make_crypto());
    interpreter.install("shell", make_shell(policy, processes));
    interpreter.install("service", make_service(policy, processes));
    interpreter.install("logging", make_logging(policy));
    interpreter.install("desktop", make_desktop(policy, processes));
    interpreter.install("http", make_http(policy, processes));
    interpreter.install("watch", make_watch(policy));
    // Pure groups need no host capability at all.
    interpreter.enable_text_primitives();
    interpreter.enable_group(PrimitiveGroup::Json);
    interpreter.enable_group(PrimitiveGroup::Encoding);
}

} // namespace posix
} // namespace toolscheme
