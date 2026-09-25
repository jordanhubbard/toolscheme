#ifndef TOOLSCHEME_HPP
#define TOOLSCHEME_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace toolscheme {

class Interpreter;
struct ValueData;

// Reader positions travel with parse and evaluation errors so embedding agents can
// report the origin of generated Scheme instead of a bare message.
struct SourceLocation {
    std::string file;
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;
    bool known = false;
};

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message);
    Error(const std::string& message, SourceLocation where);
    const SourceLocation& where() const noexcept { return where_; }

private:
    SourceLocation where_;
};

class Value {
public:
    enum class Type {
        Unspecified, Nil, Boolean, Integer, Float, Character,
        String, Symbol, Pair, Procedure, Capability, Handle
    };

    Value() noexcept;
    explicit Value(std::int64_t value) noexcept;
    explicit Value(double value);
    explicit Value(bool value) noexcept;
    explicit Value(char value) noexcept;
    explicit Value(std::string_view value);
    explicit Value(const char* value);

    static Value unspecified() noexcept;
    static Value nil() noexcept;
    static Value boolean(bool value) noexcept;
    static Value integer(std::int64_t value) noexcept;
    static Value real(double value);
    static Value character(char value) noexcept;
    static Value symbol(std::string_view name);
    static Value string(std::string_view bytes);
    static Value string(const void* bytes, std::size_t size);

    // Bulk builders. Constructing a proper list costs one aggregate allocation.
    static Value list(std::vector<Value> values);
    static Value improper(std::vector<Value> values, Value tail);

    Type type() const noexcept;
    bool is_nil() const noexcept;
    bool is_list() const noexcept;
    bool truthy() const noexcept;
    std::int64_t as_integer() const;
    double as_float() const;
    bool as_boolean() const;
    char as_character() const;
    std::string_view as_string() const;
    std::string_view as_symbol() const;
    Value car() const;
    Value cdr() const;

    // O(1) for normalized proper lists; walks segment boundaries otherwise.
    std::size_t list_size() const;
    Value list_at(std::size_t zero_based_index) const;
    Value list_tail(std::size_t zero_based_index) const;
    std::vector<Value> to_vector() const;

    // Layout begins with a native size_t, followed by bytes and a trailing NUL.
    const unsigned char* string_data() const;
    std::size_t string_size() const;
    std::string to_string() const;

    bool same_object(const Value& other) const noexcept;

private:
    explicit Value(Type type, std::shared_ptr<ValueData> data) noexcept;

    Type type_ = Type::Nil;
    union Immediate {
        bool boolean;
        std::int64_t integer;
        double real;
        char character;
    } immediate_{};
    std::shared_ptr<ValueData> data_;

    friend class Interpreter;
    friend struct ValueAccess;
};

// Accumulates elements and emits one contiguous proper or improper list.
class ListBuilder {
public:
    ListBuilder() = default;
    explicit ListBuilder(std::size_t reserve) { values_.reserve(reserve); }

    ListBuilder& add(Value value) { values_.push_back(std::move(value)); return *this; }
    ListBuilder& add_all(const std::vector<Value>& values) {
        values_.insert(values_.end(), values.begin(), values.end());
        return *this;
    }
    // Association-list field: (name value).
    ListBuilder& field(std::string_view name, Value value);
    ListBuilder& field(std::string_view name, std::string_view text);
    ListBuilder& field(std::string_view name, std::int64_t number);
    ListBuilder& field(std::string_view name, bool flag);
    ListBuilder& symbol_field(std::string_view name, std::string_view symbol);

    std::size_t size() const noexcept { return values_.size(); }
    Value build() { return Value::list(std::move(values_)); }
    Value build(Value tail) { return Value::improper(std::move(values_), std::move(tail)); }

private:
    std::vector<Value> values_;
};

// Result helpers shared by primitives and host adapters. Every utility result is a
// proper list whose written form is valid, evaluable Scheme.
Value field(std::string_view name, Value value);
Value field(std::string_view name, std::string_view text);
Value field(std::string_view name, const char* text);
Value field(std::string_view name, std::int64_t number);
Value field(std::string_view name, bool flag);
Value symbol_field(std::string_view name, std::string_view symbol);
Value ok_result(std::vector<Value> fields);
Value error_result(std::string_view message, std::string_view code,
                   std::string_view operation, std::vector<Value> extra = {});
Value denied_result(std::string_view operation, std::string_view detail);
Value unsupported_result(std::string_view operation, std::string_view detail);

// Association-list lookup over a result or option record.
bool has_option(const Value& options, std::string_view name);
Value option(const Value& options, std::string_view name, Value fallback = Value::unspecified());

using NativeFunction = std::function<Value(Interpreter&, const std::vector<Value>&)>;

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------
//
// Host operations are reached only through capabilities. A primitive validates and
// types its arguments, dispatches one operation, then validates that the host
// returned a proper list. Capabilities never see raw Scheme source, and primitives
// never touch the host directly.

class Capability {
public:
    virtual ~Capability() = default;

    // Stable capability family name, for example "filesystem" or "process".
    virtual std::string_view capability_kind() const = 0;

    // Capability identity used in serialized handle references.
    std::uint64_t capability_id() const noexcept { return capability_id_; }

    // Registration groups consult this so only supported primitives are installed.
    virtual bool supports(std::string_view operation) const { (void)operation; return true; }

    virtual Value invoke(Interpreter& interpreter, std::string_view operation,
                         const std::vector<Value>& arguments) = 0;

    // Called when a handle minted by this capability is closed or revoked.
    virtual void release(std::string_view kind, const std::shared_ptr<void>& token) {
        (void)kind; (void)token;
    }

private:
    std::uint64_t capability_id_ = next_capability_id();
    static std::uint64_t next_capability_id();
};

#define TOOLSCHEME_CAPABILITY(Name, KindText)                                    \
    class Name : public Capability {                                             \
    public:                                                                      \
        static constexpr const char* kind = KindText;                            \
        std::string_view capability_kind() const override { return KindText; }   \
    }

TOOLSCHEME_CAPABILITY(FileSystemCapability, "filesystem");
TOOLSCHEME_CAPABILITY(ProcessCapability, "process");
TOOLSCHEME_CAPABILITY(ShellCapability, "shell");
TOOLSCHEME_CAPABILITY(TerminalCapability, "terminal");
TOOLSCHEME_CAPABILITY(ClockCapability, "clock");
TOOLSCHEME_CAPABILITY(SystemCapability, "system");
TOOLSCHEME_CAPABILITY(ServiceCapability, "service");
TOOLSCHEME_CAPABILITY(ArchiveCapability, "archive");
TOOLSCHEME_CAPABILITY(CompressionCapability, "compression");
TOOLSCHEME_CAPABILITY(CryptoCapability, "crypto");
TOOLSCHEME_CAPABILITY(NetworkCapability, "network");
TOOLSCHEME_CAPABILITY(HttpCapability, "http");
TOOLSCHEME_CAPABILITY(RemoteShellCapability, "remote-shell");
TOOLSCHEME_CAPABILITY(EditorCapability, "editor");
TOOLSCHEME_CAPABILITY(LoggingCapability, "logging");
TOOLSCHEME_CAPABILITY(WatchCapability, "watch");
TOOLSCHEME_CAPABILITY(SqlCapability, "sql");
TOOLSCHEME_CAPABILITY(DesktopCapability, "desktop");

#undef TOOLSCHEME_CAPABILITY

// ---------------------------------------------------------------------------
// Registration groups
// ---------------------------------------------------------------------------

enum class PrimitiveGroup {
    Core, Text, Diff, Json, Encoding, Output, Path, File, Repository,
    Process, System, Terminal, Clock, Service, Archive, Compression,
    Crypto, Network, Http, RemoteShell, Editor, Shell, Desktop, Logging, Vcs,
    Watch, Sql
};

class Interpreter {
public:
    Interpreter();
    ~Interpreter();
    Interpreter(Interpreter&&) noexcept;
    Interpreter& operator=(Interpreter&&) noexcept;
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    // Per-process runtime identity embedded in serialized handle references.
    std::uint64_t runtime_id() const noexcept;

    Value read(std::string_view source, std::string_view file = "<string>");
    Value eval(Value expression);
    Value eval(std::string_view source, std::string_view file = "<string>");
    // Parses and evaluates one datum at a time instead of materializing the source.
    Value eval_stream(const std::function<int()>& next_byte, std::string_view file);
    std::string write(const Value& value) const;

    void define(std::string_view name, Value value);
    void define_native(std::string_view name, NativeFunction function);
    void define_capability(std::string_view name, std::shared_ptr<Capability> capability);

    // Installs a capability under a Scheme name, makes it the default for its kind
    // when no default exists, and registers the primitive groups it backs.
    void install(std::string_view name, std::shared_ptr<Capability> capability);
    std::shared_ptr<Capability> default_capability(std::string_view kind) const;
    void set_default_capability(std::string_view kind, std::shared_ptr<Capability> capability);

    void enable_group(PrimitiveGroup group);
    void enable_path_primitives();
    void enable_text_primitives();
    void enable_repository_primitives();
    void enable_process_primitives();
    void enable_system_primitives();
    void enable_archive_primitives();
    void enable_editor_primitives();
    void enable_shell_primitives();
    void enable_all_primitives();
    bool group_enabled(PrimitiveGroup group) const;

    Value cons(Value car, Value cdr);
    Value list(const std::vector<Value>& values);
    Value apply(const Value& procedure, const std::vector<Value>& arguments);
    std::vector<std::string> primitive_names() const;
    // Every top-level binding, including the Scheme library. What a caller
    // actually wants when asking "does this already exist?".
    std::vector<std::string> global_names() const;
    bool has_primitive(std::string_view name) const;

    // Mints an unforgeable handle whose written form is an evaluable reference.
    Value make_handle(const Capability& owner, std::string_view kind,
                      std::shared_ptr<void> token);
    // Looks a handle up by its serialized reference; returns a structured error list
    // for stale, revoked, or foreign references.
    Value resolve_handle(const Value& reference);
    bool revoke_handle(const Value& handle);
    std::shared_ptr<void> handle_token(const Value& handle, std::string_view kind) const;
    Capability* handle_owner(const Value& handle) const;

    // Per-call telemetry, off by default. Recording happens at the single point
    // every capability call passes through, so it cannot miss one.
    void set_telemetry(bool enabled);
    bool telemetry_enabled() const noexcept;
    void record_call(std::string_view name, std::int64_t nanoseconds, std::size_t result_bytes,
                     std::string_view code);
    Value telemetry_summary() const;
    void clear_telemetry();

    // Published tools: Scheme procedures with a typed signature and provenance,
    // discoverable by an agent over MCP.
    void publish_tool(std::string_view name, Value definition);
    Value tool_manifest() const;
    Value tool_definition(std::string_view name) const;

    // Breaks reference cycles among unreachable environments and closures. Returns
    // the number of environments reclaimed.
    std::size_t collect();
    std::size_t live_environments() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace toolscheme

#endif
