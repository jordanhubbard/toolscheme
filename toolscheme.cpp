#include "toolscheme.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace toolscheme {
namespace {

struct Environment;

struct NativeString {
    std::vector<unsigned char> storage;
};

// Proper lists are contiguous segments. `cons` onto a proper list normalizes back
// into one segment, so length and indexing stay O(1) in the common case.
struct List {
    std::shared_ptr<std::vector<Value>> values;
    std::size_t offset = 0;
    std::size_t limit = 0;
    Value tail;
};

struct Procedure {
    NativeFunction native;
    std::vector<std::string> parameters;
    std::string rest;
    std::vector<Value> body;
    std::shared_ptr<Environment> environment;
    std::string name;
};

struct CapabilityValue {
    std::shared_ptr<Capability> capability;
};

// Handles carry runtime identity, capability identity, kind, generation, and an
// unforgeable token. The host resource itself is never serialized.
struct HandleValue {
    std::uint64_t runtime = 0;
    std::uint64_t capability = 0;
    std::uint64_t index = 0;
    std::uint64_t generation = 0;
    std::string kind;
    std::string token;
    std::shared_ptr<void> resource;
    std::shared_ptr<Capability> owner;
};

using Payload = std::variant<std::monostate, NativeString, std::string, List,
                             Procedure, CapabilityValue, HandleValue>;

[[noreturn]] void fail(const std::string& message) { throw Error(message); }
[[noreturn]] void fail_at(const std::string& message, SourceLocation where) {
    throw Error(message, std::move(where));
}

} // namespace

struct ValueData {
    Payload payload;
};

struct ValueAccess {
    static const Payload& payload(const Value& value) { return value.data_->payload; }
    static Payload& payload(Value& value) { return value.data_->payload; }
    static Value make(Value::Type type, Payload payload) {
        auto data = std::make_shared<ValueData>();
        data->payload = std::move(payload);
        return Value(type, std::move(data));
    }
    static const void* identity(const Value& value) { return value.data_.get(); }
};

namespace {

const Payload& checked(const Value& value, Value::Type type, const char* expected) {
    if (value.type() != type) fail(std::string("expected ") + expected);
    return ValueAccess::payload(value);
}

const List& list_data(const Value& value) {
    return std::get<List>(checked(value, Value::Type::Pair, "pair"));
}

const Procedure& procedure_data(const Value& value) {
    return std::get<Procedure>(checked(value, Value::Type::Procedure, "procedure"));
}

const CapabilityValue& capability_data(const Value& value) {
    return std::get<CapabilityValue>(checked(value, Value::Type::Capability, "capability"));
}

const HandleValue& handle_data(const Value& value) {
    return std::get<HandleValue>(checked(value, Value::Type::Handle, "handle"));
}

// Borrowed view over a proper list's elements. Single-segment lists are inspected
// without copying; only improper or spliced lists materialize a vector.
struct Forms {
    const Value* data = nullptr;
    std::size_t count = 0;
    std::vector<Value> owned;

    bool empty() const noexcept { return count == 0; }
    std::size_t size() const noexcept { return count; }
    const Value& operator[](std::size_t index) const { return data[index]; }
    const Value* begin() const noexcept { return data; }
    const Value* end() const noexcept { return data + count; }
    const Value& back() const { return data[count - 1]; }
};

Forms forms_of(const Value& value, const char* context) {
    Forms forms;
    if (value.is_nil()) return forms;
    if (value.type() != Value::Type::Pair) fail(std::string(context) + " requires a proper list");
    const List& list = list_data(value);
    if (list.tail.is_nil()) {
        forms.data = list.values->data() + list.offset;
        forms.count = list.limit - list.offset;
        return forms;
    }
    forms.owned = value.to_vector();
    forms.data = forms.owned.data();
    forms.count = forms.owned.size();
    return forms;
}

std::string symbol_name(const Value& value, const char* context) {
    if (value.type() != Value::Type::Symbol) fail(std::string(context) + " requires a symbol");
    return std::string(value.as_symbol());
}

void arity(const std::vector<Value>& arguments, std::size_t count, const char* name) {
    if (arguments.size() != count)
        fail(std::string(name) + " expects " + std::to_string(count) + " arguments, got " +
             std::to_string(arguments.size()));
}

void arity_between(const std::vector<Value>& arguments, std::size_t low, std::size_t high,
                   const char* name) {
    if (arguments.size() < low || arguments.size() > high)
        fail(std::string(name) + " expects between " + std::to_string(low) + " and " +
             std::to_string(high) + " arguments, got " + std::to_string(arguments.size()));
}

void arity_least(const std::vector<Value>& arguments, std::size_t low, const char* name) {
    if (arguments.size() < low)
        fail(std::string(name) + " expects at least " + std::to_string(low) + " arguments");
}

bool numeric(const Value& value) {
    return value.type() == Value::Type::Integer || value.type() == Value::Type::Float;
}

double real_of(const Value& value) {
    if (value.type() == Value::Type::Integer) return static_cast<double>(value.as_integer());
    if (value.type() == Value::Type::Float) return value.as_float();
    fail("expected number");
}

std::int64_t checked_add(std::int64_t a, std::int64_t b, const char* op) {
    std::int64_t out = 0;
    if (__builtin_add_overflow(a, b, &out)) fail(std::string("integer overflow in ") + op);
    return out;
}
std::int64_t checked_sub(std::int64_t a, std::int64_t b, const char* op) {
    std::int64_t out = 0;
    if (__builtin_sub_overflow(a, b, &out)) fail(std::string("integer overflow in ") + op);
    return out;
}
std::int64_t checked_mul(std::int64_t a, std::int64_t b, const char* op) {
    std::int64_t out = 0;
    if (__builtin_mul_overflow(a, b, &out)) fail(std::string("integer overflow in ") + op);
    return out;
}

// Exact ordering between native integers and doubles. Converting the integer to a
// double would silently lose precision above 2^53, so integral doubles are compared
// against the integer directly and only the fractional part breaks ties.
int compare_numbers(const Value& a, const Value& b) {
    const bool integer_a = a.type() == Value::Type::Integer;
    const bool integer_b = b.type() == Value::Type::Integer;
    if (integer_a && integer_b) {
        const std::int64_t left = a.as_integer(), right = b.as_integer();
        return left < right ? -1 : left > right ? 1 : 0;
    }
    if (!integer_a && !integer_b) {
        const double left = a.as_float(), right = b.as_float();
        return left < right ? -1 : left > right ? 1 : 0;
    }
    const std::int64_t whole = integer_a ? a.as_integer() : b.as_integer();
    const double other = integer_a ? b.as_float() : a.as_float();
    int order;
    if (other >= 9223372036854775808.0) order = -1;
    else if (other < -9223372036854775808.0) order = 1;
    else {
        const double truncated = std::trunc(other);
        const std::int64_t rounded = static_cast<std::int64_t>(truncated);
        if (whole != rounded) order = whole < rounded ? -1 : 1;
        else {
            const double fraction = other - truncated;
            order = fraction > 0 ? -1 : fraction < 0 ? 1 : 0;
        }
    }
    return integer_a ? order : -order;
}

// Shortest decimal form that reads back as the same double.
std::string format_double(double value) {
    char buffer[64];
    for (int precision = 15; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value) break;
    }
    return std::string(buffer);
}

// Numeric token grammar, shared by the reader and `string->number`.
//
//   integer : optional sign, decimal digits, exact 64-bit range
//   float   : any decimal form carrying a period or exponent, including the
//             documented trailing-period spelling for integral doubles (`42.`)
//
// Returns true on a number, false when the token is an ordinary symbol, and sets
// `error` when the token is numeric-looking but malformed or out of range.
bool numeric_token(const std::string& token, Value& out, std::string& error) {
    error.clear();
    if (token.empty()) return false;
    const bool signed_token = token[0] == '+' || token[0] == '-';
    const std::size_t body = signed_token ? 1 : 0;
    if (body >= token.size()) return false;
    const bool leading_digit = std::isdigit(static_cast<unsigned char>(token[body])) != 0;
    const bool leading_point = token[body] == '.' && token.size() > body + 1 &&
                               std::isdigit(static_cast<unsigned char>(token[body + 1])) != 0;
    if (!leading_digit && !leading_point) return false;

    std::int64_t integer = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), integer);
    if (parsed.ec == std::errc() && parsed.ptr == token.data() + token.size()) {
        out = Value::integer(integer);
        return true;
    }
    if (parsed.ec == std::errc::result_out_of_range &&
        token.find_first_not_of("+-0123456789") == std::string::npos) {
        error = "integer literal out of range: " + token;
        return true;
    }
    // Hexadecimal and other strtod extensions are not part of the grammar.
    if (token.find('x') != std::string::npos || token.find('X') != std::string::npos) {
        error = "invalid numeric literal: " + token;
        return true;
    }
    std::string text = token;
    if (text.back() == '.') text.pop_back();
    if (text.empty() || text == "+" || text == "-") {
        error = "invalid float literal: " + token;
        return true;
    }
    const char* first = text.c_str();
    char* last = nullptr;
    errno = 0;
    const double value = std::strtod(first, &last);
    if (last != first + text.size()) {
        error = "invalid numeric literal: " + token;
        return true;
    }
    if (!std::isfinite(value)) {
        error = "float literal is not finite: " + token;
        return true;
    }
    out = Value::real(value);
    return true;
}

std::string to_hex(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

std::uint64_t random_nonce() {
    static std::mutex guard;
    static std::mt19937_64 engine([] {
        std::random_device device;
        const std::uint64_t high = (static_cast<std::uint64_t>(device()) << 32) ^ device();
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return high ^ static_cast<std::uint64_t>(now);
    }());
    std::lock_guard<std::mutex> lock(guard);
    std::uint64_t value = engine();
    return value ? value : 1;
}

// ---------------------------------------------------------------------------
// Environments
// ---------------------------------------------------------------------------

struct Environment {
    std::unordered_map<std::string, Value> values;
    std::shared_ptr<Environment> parent;

    Value get(const std::string& name) const {
        for (const Environment* scope = this; scope; scope = scope->parent.get()) {
            auto found = scope->values.find(name);
            if (found != scope->values.end()) return found->second;
        }
        fail("unbound symbol: " + name);
    }

    bool lookup(const std::string& name, Value& out) const {
        for (const Environment* scope = this; scope; scope = scope->parent.get()) {
            auto found = scope->values.find(name);
            if (found != scope->values.end()) { out = found->second; return true; }
        }
        return false;
    }

    void assign(const std::string& name, Value value) {
        for (Environment* scope = this; scope; scope = scope->parent.get()) {
            auto found = scope->values.find(name);
            if (found != scope->values.end()) { found->second = std::move(value); return; }
        }
        fail("cannot set unbound symbol: " + name);
    }
};

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

class Reader {
public:
    Reader(std::string_view source, std::string_view file) : source_(source), file_(file) {}

    bool done() { skip_space(); return position_ == source_.size(); }

    SourceLocation here() const {
        SourceLocation where;
        where.file = file_;
        where.offset = position_;
        where.line = line_;
        where.column = column_;
        where.known = true;
        return where;
    }

    Value datum() {
        skip_space();
        if (position_ == source_.size()) fail_at("unexpected end of input", here());
        const char c = source_[position_];
        if (c == '(') return read_list(')');
        if (c == ')') fail_at("unexpected closing delimiter", here());
        if (c == '\'' || c == '`' || c == ',') return read_abbreviation(c);
        if (c == '"') return read_string();
        if (c == '|') return read_quoted_symbol();
        return read_atom();
    }

private:
    std::string_view source_;
    std::string file_;
    std::size_t position_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;

    // Brackets are ordinary symbol characters: the coding-agent manifest requires a
    // primitive literally named `[`, so they cannot double as list delimiters.
    static bool delimiter(char c) {
        return std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' ||
               c == ';' || c == '"';
    }

    char take() {
        const char c = source_[position_++];
        if (c == '\n') { ++line_; column_ = 1; } else { ++column_; }
        return c;
    }

    void skip_space() {
        for (;;) {
            while (position_ < source_.size() &&
                   std::isspace(static_cast<unsigned char>(source_[position_])))
                take();
            if (position_ + 1 < source_.size() && source_[position_] == '#' &&
                source_[position_ + 1] == '|') {
                take(); take();
                int depth = 1;
                while (position_ < source_.size() && depth) {
                    if (position_ + 1 < source_.size() && source_[position_] == '|' &&
                        source_[position_ + 1] == '#') { take(); take(); --depth; }
                    else if (position_ + 1 < source_.size() && source_[position_] == '#' &&
                             source_[position_ + 1] == '|') { take(); take(); ++depth; }
                    else take();
                }
                if (depth) fail_at("unterminated block comment", here());
                continue;
            }
            if (position_ < source_.size() && source_[position_] == ';') {
                while (position_ < source_.size() && source_[position_] != '\n') take();
                continue;
            }
            return;
        }
    }

    Value read_abbreviation(char marker) {
        take();
        std::string name = marker == '\'' ? "quote" : marker == '`' ? "quasiquote" : "unquote";
        if (marker == ',' && position_ < source_.size() && source_[position_] == '@') {
            take();
            name = "unquote-splicing";
        }
        return Value::list({Value::symbol(name), datum()});
    }

    Value read_list(char closing) {
        const SourceLocation start = here();
        take();
        skip_space();
        std::vector<Value> values;
        // Growing from empty costs a reallocation per doubling, which dominates the
        // allocation count for exactly the short lists that make up most source.
        // One reservation up front covers them; longer lists still double from here.
        values.reserve(8);
        Value tail = Value::nil();
        while (position_ < source_.size() && source_[position_] != closing) {
            if (source_[position_] == '.' && position_ + 1 < source_.size() &&
                delimiter(source_[position_ + 1])) {
                if (values.empty()) fail_at("dotted list needs a leading element", here());
                take();
                tail = datum();
                skip_space();
                break;
            }
            values.push_back(datum());
            skip_space();
        }
        if (position_ == source_.size() || source_[position_] != closing)
            fail_at("unterminated list", start);
        take();
        if (values.empty()) return tail;
        return Value::improper(std::move(values), std::move(tail));
    }

    Value read_string() {
        const SourceLocation start = here();
        take();
        std::string out;
        while (position_ < source_.size() && source_[position_] != '"') {
            unsigned char c = static_cast<unsigned char>(take());
            if (c == '\\') {
                if (position_ == source_.size()) fail_at("unterminated string escape", start);
                c = static_cast<unsigned char>(take());
                switch (c) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'a': c = '\a'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'v': c = '\v'; break;
                case '0': c = 0; break;
                case '"': case '\\': break;
                case 'x': {
                    unsigned value = 0;
                    int digits = 0;
                    while (position_ < source_.size() && source_[position_] != ';') {
                        const char h = take();
                        value *= 16;
                        if (h >= '0' && h <= '9') value += static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') value += static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') value += static_cast<unsigned>(h - 'A' + 10);
                        else fail_at("invalid hex escape", here());
                        if (++digits > 2) fail_at("invalid hex escape", here());
                    }
                    if (!digits || position_ == source_.size() || value > 255)
                        fail_at("invalid hex escape", here());
                    take();
                    c = static_cast<unsigned char>(value);
                    break;
                }
                default: fail_at("unknown string escape", here());
                }
            }
            out.push_back(static_cast<char>(c));
        }
        if (position_ == source_.size()) fail_at("unterminated string", start);
        take();
        return Value::string(std::string_view(out.data(), out.size()));
    }

    // `|name|` carries symbols whose spelling would otherwise re-read as something
    // else: names containing delimiters, the empty name, a lone dot, or digits.
    Value read_quoted_symbol() {
        const SourceLocation start = here();
        take();
        std::string name;
        while (position_ < source_.size() && source_[position_] != '|') {
            char c = take();
            if (c == '\\' && position_ < source_.size()) c = take();
            name.push_back(c);
        }
        if (position_ == source_.size()) fail_at("unterminated |symbol|", start);
        take();
        return Value::symbol(name);
    }

    Value read_atom() {
        const SourceLocation start = here();
        const std::size_t begin = position_;
        // The character after `#\` belongs to the literal even when it is otherwise a
        // delimiter, so `#\"`, `#\;`, and `#\(` all read back.
        if (position_ + 1 < source_.size() && source_[position_] == '#' &&
            source_[position_ + 1] == '\\') {
            take();
            take();
            if (position_ < source_.size()) take();
        }
        while (position_ < source_.size() && !delimiter(source_[position_])) take();
        const std::string token(source_.substr(begin, position_ - begin));
        Value parsed;
        if (parse_atom(token, parsed, start)) return parsed;
        return Value::symbol(token);
    }

    static bool parse_character(const std::string& token, Value& out) {
        const std::string name = token.substr(2);
        if (name == "space") { out = Value::character(' '); return true; }
        if (name == "newline") { out = Value::character('\n'); return true; }
        if (name == "tab") { out = Value::character('\t'); return true; }
        if (name == "return") { out = Value::character('\r'); return true; }
        if (name == "nul" || name == "null") { out = Value::character('\0'); return true; }
        if (name == "delete") { out = Value::character('\x7f'); return true; }
        if (name.size() > 1 && name[0] == 'x') {
            unsigned value = 0;
            for (std::size_t i = 1; i < name.size(); ++i) {
                const char h = name[i];
                value *= 16;
                if (h >= '0' && h <= '9') value += static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f') value += static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') value += static_cast<unsigned>(h - 'A' + 10);
                else return false;
            }
            if (value > 255) return false;
            out = Value::character(static_cast<char>(value));
            return true;
        }
        if (name.size() == 1) { out = Value::character(name[0]); return true; }
        return false;
    }

    // Numeric-looking tokens are decided deterministically: an exact integer, a
    // float, or a symbol. Malformed and overflowing forms are errors rather than a
    // silent fallback to a symbol.
    static bool parse_atom(const std::string& token, Value& out, const SourceLocation& start) {
        if (token.empty()) return false;
        if (token == "#t" || token == "#true") { out = Value::boolean(true); return true; }
        if (token == "#f" || token == "#false") { out = Value::boolean(false); return true; }
        if (token == "#!unspecified") { out = Value::unspecified(); return true; }
        if (token.rfind("#\\", 0) == 0) {
            if (parse_character(token, out)) return true;
            fail_at("invalid character literal: " + token, start);
        }
        // A bare dot only ever introduces an improper tail.
        if (token == ".") fail_at("misplaced dot outside a dotted list", start);
        std::string error;
        if (!numeric_token(token, out, error)) return false;
        if (!error.empty()) fail_at(error, start);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Equality and writing
// ---------------------------------------------------------------------------

bool deep_equal(const Value& a, const Value& b);

bool equal_step(const Value& a, const Value& b) {
    if (numeric(a) && numeric(b)) {
        if ((a.type() == Value::Type::Float) != (b.type() == Value::Type::Float)) return false;
        return compare_numbers(a, b) == 0;
    }
    if (a.type() != b.type()) return false;
    switch (a.type()) {
    case Value::Type::Unspecified:
    case Value::Type::Nil: return true;
    case Value::Type::Boolean: return a.as_boolean() == b.as_boolean();
    case Value::Type::Character: return a.as_character() == b.as_character();
    case Value::Type::String: return a.as_string() == b.as_string();
    case Value::Type::Symbol: return a.as_symbol() == b.as_symbol();
    case Value::Type::Handle: {
        const HandleValue& left = handle_data(a);
        const HandleValue& right = handle_data(b);
        return left.runtime == right.runtime && left.index == right.index &&
               left.generation == right.generation && left.token == right.token;
    }
    default: return a.same_object(b);
    }
}

// Iterates the list spine so million-element lists compare without stack growth.
bool deep_equal(const Value& a, const Value& b) {
    Value left = a, right = b;
    for (;;) {
        if (left.type() == Value::Type::Pair && right.type() == Value::Type::Pair) {
            if (!deep_equal(left.car(), right.car())) return false;
            left = left.cdr();
            right = right.cdr();
            continue;
        }
        return equal_step(left, right);
    }
}

std::string write_string_literal(std::string_view bytes) {
    std::string out = "\"";
    for (const unsigned char c : bytes) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == 0) out += "\\x0;";
        else if (c < 32 || c == 127) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\x%x;", static_cast<unsigned>(c));
            out += buffer;
        }
        else out += static_cast<char>(c);
    }
    return out + '"';
}

std::string write_value(const Value& value);

// A symbol is written bare only when reading it back yields the same symbol.
std::string write_symbol(std::string_view name) {
    bool plain = !name.empty() && name != ".";
    for (const char c : name)
        if (std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == ';' ||
            c == '"' || c == '|' || c == '\'' || c == '`' || c == ',')
            plain = false;
    if (plain) {
        // A name that reads as a number, or that the reader rejects outright, must be
        // quoted too.
        Value parsed;
        std::string error;
        if (numeric_token(std::string(name), parsed, error)) plain = false;
        if (name.rfind("#", 0) == 0) plain = false;
    }
    if (plain) return std::string(name);
    std::string out = "|";
    for (const char c : name) {
        if (c == '|' || c == '\\') out += '\\';
        out += c;
    }
    return out + '|';
}

// Runtime-bound resources print as a validated reference expression rather than an
// unreadable diagnostic token, so every written result stays evaluable.
std::string write_handle(const HandleValue& handle) {
    // The fields are quoted so the whole form evaluates: `handle-ref` receives the
    // reference records themselves rather than trying to call `runtime`, `kind`, and
    // friends as procedures.
    std::string out = "(handle-ref (quote (runtime \"" + to_hex(handle.runtime) + "\"))";
    out += " (quote (capability \"" + to_hex(handle.capability) + "\"))";
    out += " (quote (kind " + handle.kind + "))";
    out += " (quote (index " + std::to_string(handle.index) + "))";
    out += " (quote (generation " + std::to_string(handle.generation) + "))";
    out += " (quote (token \"" + handle.token + "\")))";
    return out;
}

std::string write_value(const Value& value) {
    switch (value.type()) {
    // A self-evaluating literal, so the write/read round trip is `equal?` without an
    // intervening evaluation.
    case Value::Type::Unspecified: return "#!unspecified";
    case Value::Type::Nil: return "()";
    case Value::Type::Boolean: return value.as_boolean() ? "#t" : "#f";
    case Value::Type::Integer: return std::to_string(value.as_integer());
    case Value::Type::Float: {
        // A period always appears so the float reading is unambiguous on re-read.
        std::string text = format_double(value.as_float());
        if (text.find('.') == std::string::npos) text += '.';
        return text;
    }
    case Value::Type::Character: {
        const unsigned char c = static_cast<unsigned char>(value.as_character());
        if (c == ' ') return "#\\space";
        if (c == '\n') return "#\\newline";
        if (c == '\t') return "#\\tab";
        if (c == '\r') return "#\\return";
        if (c == 0) return "#\\nul";
        if (c < 32 || c >= 127) {
            char buffer[16];
            std::snprintf(buffer, sizeof buffer, "#\\x%x", static_cast<unsigned>(c));
            return buffer;
        }
        return std::string("#\\") + static_cast<char>(c);
    }
    case Value::Type::String: return write_string_literal(value.as_string());
    case Value::Type::Symbol: return write_symbol(value.as_symbol());
    case Value::Type::Pair: {
        std::string out = "(";
        Value at = value;
        bool first = true;
        while (at.type() == Value::Type::Pair) {
            if (!first) out += ' ';
            out += write_value(at.car());
            at = at.cdr();
            first = false;
        }
        if (!at.is_nil()) out += " . " + write_value(at);
        return out + ')';
    }
    case Value::Type::Procedure: {
        const Procedure& procedure = procedure_data(value);
        return "(procedure-ref " + write_string_literal(procedure.name.empty() ? "anonymous"
                                                                               : procedure.name) + ")";
    }
    case Value::Type::Capability: {
        const CapabilityValue& capability = capability_data(value);
        return "(capability-ref " + write_string_literal(capability.capability->capability_kind()) +
               " \"" + to_hex(capability.capability->capability_id()) + "\")";
    }
    case Value::Type::Handle: return write_handle(handle_data(value));
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

Error::Error(const std::string& message) : std::runtime_error("toolscheme: " + message) {}

Error::Error(const std::string& message, SourceLocation where)
    : std::runtime_error("toolscheme: " + where.file + ":" + std::to_string(where.line) + ":" +
                         std::to_string(where.column) + ": " + message),
      where_(std::move(where)) {}

Value::Value() noexcept = default;
Value::Value(Type type, std::shared_ptr<ValueData> data) noexcept
    : type_(type), data_(std::move(data)) {}
Value::Value(std::int64_t value) noexcept : type_(Type::Integer) { immediate_.integer = value; }
Value::Value(double value) : type_(Type::Float) {
    if (!std::isfinite(value)) fail("non-finite float");
    immediate_.real = value;
}
Value::Value(bool value) noexcept : type_(Type::Boolean) { immediate_.boolean = value; }
Value::Value(char value) noexcept : type_(Type::Character) { immediate_.character = value; }
Value::Value(const char* value) : Value(std::string_view(value)) {}
Value::Value(std::string_view value) : Value(string(value)) {}

Value Value::unspecified() noexcept { Value out; out.type_ = Type::Unspecified; return out; }
Value Value::nil() noexcept { return Value(); }
Value Value::boolean(bool value) noexcept { return Value(value); }
Value Value::integer(std::int64_t value) noexcept { return Value(value); }
Value Value::real(double value) { return Value(value); }
Value Value::character(char value) noexcept { return Value(value); }

// Symbols are interned process-wide, so `eq?` is a pointer comparison and symbol
// values remain valid after the interpreter that produced them is destroyed.
Value Value::symbol(std::string_view name) {
    static std::mutex guard;
    static std::unordered_map<std::string, std::shared_ptr<ValueData>> table;
    const std::string key(name);
    std::lock_guard<std::mutex> lock(guard);
    auto found = table.find(key);
    if (found == table.end()) {
        auto data = std::make_shared<ValueData>();
        data->payload = key;
        found = table.emplace(key, std::move(data)).first;
    }
    return Value(Type::Symbol, found->second);
}

Value Value::string(const void* bytes, std::size_t size) {
    NativeString storage;
    storage.storage.resize(sizeof(std::size_t) + size + 1);
    std::memcpy(storage.storage.data(), &size, sizeof size);
    if (size) std::memcpy(storage.storage.data() + sizeof size, bytes, size);
    storage.storage.back() = 0;
    return ValueAccess::make(Type::String, std::move(storage));
}

Value Value::string(std::string_view bytes) { return string(bytes.data(), bytes.size()); }

Value Value::list(std::vector<Value> values) {
    if (values.empty()) return nil();
    const std::size_t count = values.size();
    auto storage = std::make_shared<std::vector<Value>>(std::move(values));
    return ValueAccess::make(Type::Pair, List{storage, 0, count, nil()});
}

Value Value::improper(std::vector<Value> values, Value tail) {
    if (values.empty()) return tail;
    // Splice a proper-list tail into the same segment so results stay normalized.
    if (tail.type() == Type::Pair && tail.is_list()) {
        std::vector<Value> rest = tail.to_vector();
        values.insert(values.end(), rest.begin(), rest.end());
        tail = nil();
    }
    const std::size_t count = values.size();
    auto storage = std::make_shared<std::vector<Value>>(std::move(values));
    return ValueAccess::make(Type::Pair, List{storage, 0, count, std::move(tail)});
}

Value::Type Value::type() const noexcept { return type_; }
bool Value::is_nil() const noexcept { return type_ == Type::Nil; }

bool Value::is_list() const noexcept {
    Value at = *this;
    while (at.type_ == Type::Pair) at = std::get<List>(at.data_->payload).tail;
    return at.is_nil();
}

bool Value::truthy() const noexcept { return type_ != Type::Boolean || immediate_.boolean; }

std::int64_t Value::as_integer() const {
    if (type_ != Type::Integer) fail("expected integer");
    return immediate_.integer;
}
double Value::as_float() const {
    if (type_ != Type::Float) fail("expected float");
    return immediate_.real;
}
bool Value::as_boolean() const {
    if (type_ != Type::Boolean) fail("expected boolean");
    return immediate_.boolean;
}
char Value::as_character() const {
    if (type_ != Type::Character) fail("expected character");
    return immediate_.character;
}
std::string_view Value::as_string() const {
    const auto& bytes = std::get<NativeString>(checked(*this, Type::String, "string")).storage;
    std::size_t size = 0;
    std::memcpy(&size, bytes.data(), sizeof size);
    return {reinterpret_cast<const char*>(bytes.data() + sizeof size), size};
}
std::string_view Value::as_symbol() const {
    return std::get<std::string>(checked(*this, Type::Symbol, "symbol"));
}

Value Value::car() const {
    const List& list = list_data(*this);
    return (*list.values)[list.offset];
}

Value Value::cdr() const {
    const List& list = list_data(*this);
    if (list.offset + 1 >= list.limit) return list.tail;
    return ValueAccess::make(Type::Pair, List{list.values, list.offset + 1, list.limit, list.tail});
}

std::size_t Value::list_size() const {
    std::size_t total = 0;
    Value at = *this;
    while (at.type_ == Type::Pair) {
        const List& list = list_data(at);
        total += list.limit - list.offset;
        at = list.tail;
    }
    if (!at.is_nil()) fail("length requires a proper list");
    return total;
}

Value Value::list_at(std::size_t index) const {
    Value at = *this;
    while (at.type_ == Type::Pair) {
        const List& list = list_data(at);
        const std::size_t count = list.limit - list.offset;
        if (index < count) return (*list.values)[list.offset + index];
        index -= count;
        at = list.tail;
    }
    fail("list index out of range");
}

Value Value::list_tail(std::size_t index) const {
    Value at = *this;
    while (at.type_ == Type::Pair) {
        const List& list = list_data(at);
        const std::size_t count = list.limit - list.offset;
        if (index < count)
            return ValueAccess::make(Type::Pair,
                                     List{list.values, list.offset + index, list.limit, list.tail});
        index -= count;
        at = list.tail;
    }
    if (index == 0) return at;
    fail("list index out of range");
}

std::vector<Value> Value::to_vector() const {
    std::vector<Value> out;
    Value at = *this;
    while (at.type_ == Type::Pair) {
        const List& list = list_data(at);
        out.insert(out.end(), list.values->begin() + static_cast<std::ptrdiff_t>(list.offset),
                   list.values->begin() + static_cast<std::ptrdiff_t>(list.limit));
        at = list.tail;
    }
    if (!at.is_nil()) fail("expected a proper list");
    return out;
}

const unsigned char* Value::string_data() const {
    return std::get<NativeString>(checked(*this, Type::String, "string")).storage.data();
}
std::size_t Value::string_size() const { return as_string().size(); }
std::string Value::to_string() const { return write_value(*this); }

bool Value::same_object(const Value& other) const noexcept {
    if (type_ != other.type_) return false;
    switch (type_) {
    case Type::Unspecified:
    case Type::Nil: return true;
    case Type::Boolean: return immediate_.boolean == other.immediate_.boolean;
    case Type::Integer: return immediate_.integer == other.immediate_.integer;
    case Type::Float: return immediate_.real == other.immediate_.real;
    case Type::Character: return immediate_.character == other.immediate_.character;
    default: return data_ == other.data_;
    }
}

std::uint64_t Capability::next_capability_id() {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1);
}

// ---------------------------------------------------------------------------
// Result helpers
// ---------------------------------------------------------------------------

ListBuilder& ListBuilder::field(std::string_view name, Value value) {
    return add(Value::list({Value::symbol(name), std::move(value)}));
}
ListBuilder& ListBuilder::field(std::string_view name, std::string_view text) {
    return field(name, Value::string(text));
}
ListBuilder& ListBuilder::field(std::string_view name, std::int64_t number) {
    return field(name, Value::integer(number));
}
ListBuilder& ListBuilder::field(std::string_view name, bool flag) {
    return field(name, Value::boolean(flag));
}
ListBuilder& ListBuilder::symbol_field(std::string_view name, std::string_view text) {
    return field(name, Value::symbol(text));
}

Value field(std::string_view name, Value value) {
    return Value::list({Value::symbol(name), std::move(value)});
}
Value field(std::string_view name, std::string_view text) {
    return field(name, Value::string(text));
}
Value field(std::string_view name, const char* text) {
    return field(name, Value::string(std::string_view(text)));
}
Value field(std::string_view name, std::int64_t number) {
    return field(name, Value::integer(number));
}
Value field(std::string_view name, bool flag) { return field(name, Value::boolean(flag)); }
Value symbol_field(std::string_view name, std::string_view text) {
    return field(name, Value::symbol(text));
}

Value ok_result(std::vector<Value> fields) { return Value::list(std::move(fields)); }

Value error_result(std::string_view message, std::string_view code, std::string_view operation,
                   std::vector<Value> extra) {
    ListBuilder out(4 + extra.size());
    out.field("error", message);
    out.symbol_field("code", code);
    out.symbol_field("operation", operation);
    out.add_all(extra);
    return out.build();
}

Value denied_result(std::string_view operation, std::string_view detail) {
    return error_result(detail, "permission-denied", operation);
}

Value unsupported_result(std::string_view operation, std::string_view detail) {
    return error_result(detail, "unsupported", operation);
}

// Keys may be symbols (option records and result fields) or strings (JSON objects,
// environment pairs), so one lookup serves both shapes.
Value option(const Value& options, std::string_view name, Value fallback) {
    if (options.type() != Value::Type::Pair || !options.is_list()) return fallback;
    Value at = options;
    while (at.type() == Value::Type::Pair) {
        const Value entry = at.car();
        if (entry.type() == Value::Type::Pair && entry.is_list() && entry.list_size() >= 2) {
            const Value key = entry.car();
            if ((key.type() == Value::Type::Symbol && key.as_symbol() == name) ||
                (key.type() == Value::Type::String && key.as_string() == name))
                return entry.list_at(1);
        }
        at = at.cdr();
    }
    return fallback;
}

bool has_option(const Value& options, std::string_view name) {
    return option(options, name, Value::unspecified()).type() != Value::Type::Unspecified;
}

// ---------------------------------------------------------------------------
// Evaluator
// ---------------------------------------------------------------------------

namespace {

enum class Form {
    None, Quote, Quasiquote, If, Begin, Define, Set, Lambda, NamedLambda, And, Or,
    Let, LetStar, LetRec, Cond, Case, When, Unless, Do, Eval, Delay
};

Form special_form(std::string_view name) {
    static const std::unordered_map<std::string_view, Form> table = {
        {"quote", Form::Quote}, {"quasiquote", Form::Quasiquote}, {"if", Form::If},
        {"begin", Form::Begin}, {"define", Form::Define}, {"set!", Form::Set},
        {"lambda", Form::Lambda}, {"named-lambda", Form::NamedLambda}, {"and", Form::And},
        {"or", Form::Or}, {"let", Form::Let}, {"let*", Form::LetStar},
        {"letrec", Form::LetRec}, {"letrec*", Form::LetRec}, {"cond", Form::Cond},
        {"case", Form::Case}, {"when", Form::When}, {"unless", Form::Unless},
        {"do", Form::Do}, {"eval", Form::Eval}
    };
    const auto found = table.find(name);
    return found == table.end() ? Form::None : found->second;
}

} // namespace

struct Interpreter::Impl {
    Interpreter* owner;
    std::shared_ptr<Environment> global = std::make_shared<Environment>();
    std::set<std::string> primitives;
    std::set<PrimitiveGroup> enabled;
    std::map<std::string, std::shared_ptr<Capability>> defaults;
    std::vector<std::shared_ptr<Capability>> installed;
    std::uint64_t runtime = random_nonce();

    struct HandleSlot {
        std::uint64_t generation = 0;
        std::string token;
        Value handle;
        bool live = false;
    };
    std::vector<HandleSlot> handles;

    // Weak registry backing cycle collection. Compacted as it grows so that long tail
    // loops do not accumulate dead entries.
    std::vector<std::weak_ptr<Environment>> environments;
    std::size_t compact_at = 1024;

    struct CallRecord {
        std::string name;
        std::int64_t nanoseconds = 0;
        std::size_t result_bytes = 0;
        std::string code;      // error code, empty on success
    };
    bool telemetry = false;
    std::vector<CallRecord> calls;

    std::map<std::string, Value> tools;

    explicit Impl(Interpreter* value) : owner(value) { track(global); }

    // A top-level `(define (f) ...)` stores a closure in the global environment,
    // and that closure holds a shared_ptr back to the same environment -- so the
    // global scope is a reference cycle with itself and survives its own
    // interpreter. Breaking every tracked scope at teardown releases it; without
    // this, any interpreter that ever defined a procedure leaks its whole global
    // environment and everything reachable from it.
    ~Impl() {
        for (const auto& entry : environments)
            if (auto scope = entry.lock()) {
                scope->values.clear();
                scope->parent.reset();
            }
        if (global) {
            global->values.clear();
            global->parent.reset();
        }
    }

    void track(const std::shared_ptr<Environment>& environment) {
        if (environments.size() >= compact_at) compact();
        environments.push_back(environment);
    }

    void compact() {
        environments.erase(std::remove_if(environments.begin(), environments.end(),
                                          [](const std::weak_ptr<Environment>& entry) {
                                              return entry.expired();
                                          }),
                           environments.end());
        compact_at = std::max<std::size_t>(1024, environments.size() * 2);
    }

    std::shared_ptr<Environment> child_of(const std::shared_ptr<Environment>& parent) {
        auto scope = std::make_shared<Environment>();
        scope->parent = parent;
        track(scope);
        return scope;
    }

    std::shared_ptr<Environment> bind(const Procedure& procedure,
                                      const std::vector<Value>& arguments) {
        if (arguments.size() < procedure.parameters.size() ||
            (procedure.rest.empty() && arguments.size() != procedure.parameters.size()))
            fail((procedure.name.empty() ? std::string("procedure") : procedure.name) +
                 " expects " + std::to_string(procedure.parameters.size()) +
                 (procedure.rest.empty() ? "" : " or more") + " arguments, got " +
                 std::to_string(arguments.size()));
        auto scope = child_of(procedure.environment);
        scope->values.reserve(procedure.parameters.size() + (procedure.rest.empty() ? 0 : 1));
        for (std::size_t i = 0; i < procedure.parameters.size(); ++i)
            scope->values[procedure.parameters[i]] = arguments[i];
        if (!procedure.rest.empty())
            scope->values[procedure.rest] = Value::list(
                {arguments.begin() + static_cast<std::ptrdiff_t>(procedure.parameters.size()),
                 arguments.end()});
        return scope;
    }

    Value make_lambda(const Forms& forms, const std::shared_ptr<Environment>& environment,
                      std::string name) {
        if (forms.size() < 2) fail("lambda expects parameters and a body");
        Procedure procedure;
        procedure.environment = environment;
        procedure.name = std::move(name);
        procedure.body.assign(forms.begin() + 1, forms.end());
        Value at = forms[0];
        std::set<std::string> seen;
        while (at.type() == Value::Type::Pair) {
            const std::string parameter = symbol_name(at.car(), "lambda");
            if (!seen.insert(parameter).second) fail("duplicate lambda parameter: " + parameter);
            procedure.parameters.push_back(parameter);
            at = at.cdr();
        }
        if (!at.is_nil()) {
            procedure.rest = symbol_name(at, "lambda");
            if (!seen.insert(procedure.rest).second)
                fail("duplicate lambda parameter: " + procedure.rest);
        }
        return ValueAccess::make(Value::Type::Procedure, std::move(procedure));
    }

    Value quasiquote(const Value& tpl, int depth, const std::shared_ptr<Environment>& environment) {
        if (tpl.type() != Value::Type::Pair) return tpl;
        const Value head = tpl.car();
        if (head.type() == Value::Type::Symbol) {
            const std::string_view name = head.as_symbol();
            if (name == "unquote" && tpl.is_list() && tpl.list_size() == 2) {
                if (depth == 1) return evaluate(tpl.list_at(1), environment);
                return Value::list({head, quasiquote(tpl.list_at(1), depth - 1, environment)});
            }
            if (name == "quasiquote" && tpl.is_list() && tpl.list_size() == 2)
                return Value::list({head, quasiquote(tpl.list_at(1), depth + 1, environment)});
        }
        std::vector<Value> out;
        Value at = tpl;
        while (at.type() == Value::Type::Pair) {
            const Value element = at.car();
            const Value rest = at.cdr();
            // A dotted `(a . ,b)` tail unquotes the tail itself rather than an element.
            if (element.type() == Value::Type::Symbol && element.as_symbol() == "unquote" &&
                !out.empty() && rest.type() == Value::Type::Pair && rest.cdr().is_nil()) {
                Value tail = depth == 1 ? evaluate(rest.car(), environment)
                                        : Value::list({element, quasiquote(rest.car(), depth - 1,
                                                                           environment)});
                return Value::improper(std::move(out), std::move(tail));
            }
            if (element.type() == Value::Type::Pair && element.car().type() == Value::Type::Symbol &&
                element.car().as_symbol() == "unquote-splicing" && element.is_list() &&
                element.list_size() == 2) {
                if (depth == 1) {
                    const Value spliced = evaluate(element.list_at(1), environment);
                    if (!spliced.is_nil()) {
                        if (!spliced.is_list()) fail("unquote-splicing requires a proper list");
                        const std::vector<Value> items = spliced.to_vector();
                        out.insert(out.end(), items.begin(), items.end());
                    }
                } else {
                    out.push_back(Value::list({element.car(),
                                               quasiquote(element.list_at(1), depth - 1,
                                                          environment)}));
                }
            } else {
                out.push_back(quasiquote(element, depth, environment));
            }
            at = rest;
        }
        if (!at.is_nil()) return Value::improper(std::move(out), quasiquote(at, depth, environment));
        return Value::list(std::move(out));
    }

    Value apply(const Value& procedure, const std::vector<Value>& arguments) {
        const Procedure& target = procedure_data(procedure);
        if (target.native) return target.native(*owner, arguments);
        auto scope = bind(target, arguments);
        for (std::size_t i = 0; i + 1 < target.body.size(); ++i) evaluate(target.body[i], scope);
        return evaluate(target.body.back(), scope);
    }

    Value evaluate(Value expression, std::shared_ptr<Environment> environment) {
        for (;;) {
            if (expression.type() == Value::Type::Symbol)
                return environment->get(std::string(expression.as_symbol()));
            if (expression.type() != Value::Type::Pair) return expression;

            const Value head = expression.car();
            const Value rest = expression.cdr();
            const Forms forms = forms_of(rest, "combination");

            if (head.type() == Value::Type::Symbol) {
                bool shadowed = false;
                Value ignored;
                switch (special_form(head.as_symbol())) {
                case Form::None: break;
                case Form::Quote:
                    if (forms.size() != 1) fail("quote expects 1 argument");
                    return forms[0];
                case Form::Quasiquote:
                    if (forms.size() != 1) fail("quasiquote expects 1 argument");
                    return quasiquote(forms[0], 1, environment);
                case Form::If:
                    if (forms.size() < 2 || forms.size() > 3) fail("if expects 2 or 3 arguments");
                    if (evaluate(forms[0], environment).truthy()) expression = forms[1];
                    else if (forms.size() == 3) expression = forms[2];
                    else return Value::unspecified();
                    continue;
                case Form::Begin:
                    if (forms.empty()) return Value::unspecified();
                    for (std::size_t i = 0; i + 1 < forms.size(); ++i)
                        evaluate(forms[i], environment);
                    expression = forms.back();
                    continue;
                case Form::Define: {
                    if (forms.empty()) fail("define expects a name");
                    if (forms[0].type() == Value::Type::Pair) {
                        const std::string name = symbol_name(forms[0].car(), "define");
                        Forms lambda_forms;
                        std::vector<Value> pieces{forms[0].cdr()};
                        pieces.insert(pieces.end(), forms.begin() + 1, forms.end());
                        lambda_forms.owned = std::move(pieces);
                        lambda_forms.data = lambda_forms.owned.data();
                        lambda_forms.count = lambda_forms.owned.size();
                        environment->values[name] =
                            make_lambda(lambda_forms, environment, name);
                        return Value::symbol(name);
                    }
                    if (forms.size() != 2) fail("define expects a name and one value");
                    const std::string name = symbol_name(forms[0], "define");
                    Value bound = evaluate(forms[1], environment);
                    if (bound.type() == Value::Type::Procedure) {
                        Procedure& procedure = std::get<Procedure>(ValueAccess::payload(bound));
                        if (procedure.name.empty()) procedure.name = name;
                    }
                    environment->values[name] = std::move(bound);
                    return Value::symbol(name);
                }
                case Form::Set: {
                    if (forms.size() != 2) fail("set! expects 2 arguments");
                    Value bound = evaluate(forms[1], environment);
                    environment->assign(symbol_name(forms[0], "set!"), bound);
                    return bound;
                }
                case Form::Lambda:
                    return make_lambda(forms, environment, {});
                case Form::NamedLambda: {
                    if (forms.size() < 2 || forms[0].type() != Value::Type::Pair)
                        fail("named-lambda expects (name . parameters) and a body");
                    Forms shifted;
                    std::vector<Value> pieces{forms[0].cdr()};
                    pieces.insert(pieces.end(), forms.begin() + 1, forms.end());
                    shifted.owned = std::move(pieces);
                    shifted.data = shifted.owned.data();
                    shifted.count = shifted.owned.size();
                    return make_lambda(shifted, environment, symbol_name(forms[0].car(),
                                                                        "named-lambda"));
                }
                case Form::And:
                case Form::Or: {
                    const bool conjunction = special_form(head.as_symbol()) == Form::And;
                    if (forms.empty()) return Value::boolean(conjunction);
                    for (std::size_t i = 0; i + 1 < forms.size(); ++i) {
                        Value step = evaluate(forms[i], environment);
                        if (conjunction ? !step.truthy() : step.truthy()) return step;
                    }
                    expression = forms.back();
                    continue;
                }
                case Form::Let: {
                    // Named let compiles to a self-recursive procedure applied in place.
                    if (!forms.empty() && forms[0].type() == Value::Type::Symbol) {
                        if (forms.size() < 3) fail("named let expects a name, bindings, and a body");
                        const std::string name = symbol_name(forms[0], "let");
                        std::vector<Value> parameters, initial;
                        for (const Value& binding : forms[1].to_vector()) {
                            const std::vector<Value> pair = binding.to_vector();
                            if (pair.size() != 2) fail("let binding expects a name and a value");
                            parameters.push_back(pair[0]);
                            initial.push_back(evaluate(pair[1], environment));
                        }
                        auto scope = child_of(environment);
                        Forms body;
                        std::vector<Value> pieces{Value::list(std::move(parameters))};
                        pieces.insert(pieces.end(), forms.begin() + 2, forms.end());
                        body.owned = std::move(pieces);
                        body.data = body.owned.data();
                        body.count = body.owned.size();
                        const Value procedure = make_lambda(body, scope, name);
                        scope->values[name] = procedure;
                        const Procedure& target = procedure_data(procedure);
                        auto call = bind(target, initial);
                        for (std::size_t i = 0; i + 1 < target.body.size(); ++i)
                            evaluate(target.body[i], call);
                        expression = target.body.back();
                        environment = call;
                        continue;
                    }
                    if (forms.size() < 2) fail("let expects bindings and a body");
                    auto scope = child_of(environment);
                    for (const Value& binding : forms[0].to_vector()) {
                        const std::vector<Value> pair = binding.to_vector();
                        if (pair.size() != 2) fail("let binding expects a name and a value");
                        scope->values[symbol_name(pair[0], "let")] = evaluate(pair[1], environment);
                    }
                    for (std::size_t i = 1; i + 1 < forms.size(); ++i) evaluate(forms[i], scope);
                    expression = forms.back();
                    environment = scope;
                    continue;
                }
                case Form::LetStar:
                case Form::LetRec: {
                    if (forms.size() < 2) fail("let* and letrec expect bindings and a body");
                    const bool recursive = special_form(head.as_symbol()) == Form::LetRec;
                    auto scope = child_of(environment);
                    const std::vector<Value> bindings = forms[0].to_vector();
                    if (recursive)
                        for (const Value& binding : bindings)
                            scope->values[symbol_name(binding.car(), "letrec")] =
                                Value::unspecified();
                    for (const Value& binding : bindings) {
                        const std::vector<Value> pair = binding.to_vector();
                        if (pair.size() != 2) fail("binding expects a name and a value");
                        scope->values[symbol_name(pair[0], "let*")] = evaluate(pair[1], scope);
                    }
                    for (std::size_t i = 1; i + 1 < forms.size(); ++i) evaluate(forms[i], scope);
                    expression = forms.back();
                    environment = scope;
                    continue;
                }
                case Form::Cond: {
                    bool taken = false;
                    for (std::size_t i = 0; i < forms.size() && !taken; ++i) {
                        const std::vector<Value> clause = forms[i].to_vector();
                        if (clause.empty()) fail("cond clause cannot be empty");
                        const bool otherwise = clause[0].type() == Value::Type::Symbol &&
                                               clause[0].as_symbol() == "else";
                        Value guard = otherwise ? Value::boolean(true)
                                                : evaluate(clause[0], environment);
                        if (!guard.truthy()) continue;
                        taken = true;
                        if (clause.size() == 1) return guard;
                        if (clause.size() == 3 && clause[1].type() == Value::Type::Symbol &&
                            clause[1].as_symbol() == "=>")
                            return apply(evaluate(clause[2], environment), {guard});
                        for (std::size_t j = 1; j + 1 < clause.size(); ++j)
                            evaluate(clause[j], environment);
                        expression = clause.back();
                    }
                    if (!taken) return Value::unspecified();
                    continue;
                }
                case Form::Case: {
                    if (forms.empty()) fail("case expects a key");
                    const Value key = evaluate(forms[0], environment);
                    bool taken = false;
                    for (std::size_t i = 1; i < forms.size() && !taken; ++i) {
                        const std::vector<Value> clause = forms[i].to_vector();
                        if (clause.size() < 2) fail("case clause expects data and a body");
                        if (clause[0].type() == Value::Type::Symbol &&
                            clause[0].as_symbol() == "else") {
                            taken = true;
                        } else {
                            for (const Value& candidate : clause[0].to_vector())
                                if (deep_equal(candidate, key)) { taken = true; break; }
                        }
                        if (!taken) continue;
                        for (std::size_t j = 1; j + 1 < clause.size(); ++j)
                            evaluate(clause[j], environment);
                        expression = clause.back();
                    }
                    if (!taken) return Value::unspecified();
                    continue;
                }
                case Form::When:
                case Form::Unless: {
                    if (forms.empty()) fail("when and unless expect a test");
                    const bool want = special_form(head.as_symbol()) == Form::When;
                    if (evaluate(forms[0], environment).truthy() != want)
                        return Value::unspecified();
                    if (forms.size() == 1) return Value::unspecified();
                    for (std::size_t i = 1; i + 1 < forms.size(); ++i)
                        evaluate(forms[i], environment);
                    expression = forms.back();
                    continue;
                }
                case Form::Do: {
                    if (forms.size() < 2) fail("do expects bindings and a termination clause");
                    struct Step { std::string name; Value step; bool has_step = false; };
                    std::vector<Step> steps;
                    auto scope = child_of(environment);
                    for (const Value& binding : forms[0].to_vector()) {
                        const std::vector<Value> pair = binding.to_vector();
                        if (pair.size() < 2) fail("do binding expects a name and an initial value");
                        Step step;
                        step.name = symbol_name(pair[0], "do");
                        scope->values[step.name] = evaluate(pair[1], environment);
                        if (pair.size() > 2) { step.step = pair[2]; step.has_step = true; }
                        steps.push_back(std::move(step));
                    }
                    const std::vector<Value> ending = forms[1].to_vector();
                    if (ending.empty()) fail("do expects a termination test");
                    for (;;) {
                        if (evaluate(ending[0], scope).truthy()) break;
                        for (std::size_t i = 2; i < forms.size(); ++i) evaluate(forms[i], scope);
                        std::vector<Value> next(steps.size());
                        for (std::size_t i = 0; i < steps.size(); ++i)
                            next[i] = steps[i].has_step ? evaluate(steps[i].step, scope)
                                                        : scope->values[steps[i].name];
                        auto rotated = child_of(environment);
                        for (std::size_t i = 0; i < steps.size(); ++i)
                            rotated->values[steps[i].name] = next[i];
                        scope = rotated;
                    }
                    if (ending.size() == 1) return Value::unspecified();
                    for (std::size_t i = 1; i + 1 < ending.size(); ++i) evaluate(ending[i], scope);
                    expression = ending.back();
                    environment = scope;
                    continue;
                }
                case Form::Eval: {
                    if (forms.empty() || forms.size() > 2) fail("eval expects 1 or 2 arguments");
                    // A second argument names the environment explicitly; with none, the
                    // documented interaction environment is used.
                    expression = evaluate(forms[0], environment);
                    if (forms.size() == 2) {
                        const Value which = evaluate(forms[1], environment);
                        const std::string name = which.type() == Value::Type::Symbol
                                                     ? std::string(which.as_symbol())
                                                     : "interaction-environment";
                        if (name == "interaction-environment") environment = global;
                        else if (name == "current-environment") { /* keep */ }
                        else fail("unknown evaluation environment: " + name);
                    } else {
                        environment = global;
                    }
                    continue;
                }
                case Form::Delay: break;
                }
                (void)shadowed;
                (void)ignored;
            }

            const Value callable = evaluate(head, environment);
            std::vector<Value> arguments;
            arguments.reserve(forms.size());
            for (const Value& form : forms) arguments.push_back(evaluate(form, environment));
            const Procedure& target = procedure_data(callable);
            if (target.native) return target.native(*owner, arguments);
            auto scope = bind(target, arguments);
            for (std::size_t i = 0; i + 1 < target.body.size(); ++i) evaluate(target.body[i], scope);
            expression = target.body.back();
            environment = scope;
        }
    }
};

// ---------------------------------------------------------------------------
// Cycle-aware collection
// ---------------------------------------------------------------------------

namespace {

// Records every environment a value can reach through a closure. List storage is
// visited once so shared and self-referential structures terminate.
void environment_edges(const Value& value, std::vector<Environment*>& out,
                       std::unordered_set<const void*>& seen) {
    switch (value.type()) {
    case Value::Type::Pair: {
        Value at = value;
        while (at.type() == Value::Type::Pair) {
            const List& segment = list_data(at);
            if (seen.insert(segment.values.get()).second)
                for (const Value& element : *segment.values) environment_edges(element, out, seen);
            at = segment.tail;
        }
        environment_edges(at, out, seen);
        return;
    }
    case Value::Type::Procedure: {
        if (!seen.insert(ValueAccess::identity(value)).second) return;
        const Procedure& procedure = procedure_data(value);
        if (procedure.environment) out.push_back(procedure.environment.get());
        for (const Value& form : procedure.body) environment_edges(form, out, seen);
        return;
    }
    default: return;
    }
}

} // namespace

std::size_t Interpreter::collect() {
    impl_->compact();
    std::vector<std::shared_ptr<Environment>> live;
    live.reserve(impl_->environments.size());
    for (const auto& entry : impl_->environments)
        if (auto scope = entry.lock()) live.push_back(std::move(scope));

    std::unordered_set<const Environment*> tracked;
    tracked.reserve(live.size() * 2);
    for (const auto& scope : live) tracked.insert(scope.get());

    // Count references held from inside the tracked graph. An environment whose total
    // reference count exceeds its internal count still has an external holder.
    std::unordered_map<const Environment*, long> internal;
    internal.reserve(live.size() * 2);
    std::unordered_map<const Environment*, std::vector<Environment*>> outgoing;
    outgoing.reserve(live.size() * 2);
    for (const auto& scope : live) {
        std::vector<Environment*> edges;
        std::unordered_set<const void*> seen;
        if (scope->parent) edges.push_back(scope->parent.get());
        for (const auto& binding : scope->values) environment_edges(binding.second, edges, seen);
        for (Environment* target : edges)
            if (tracked.count(target)) internal[target] += 1;
        outgoing.emplace(scope.get(), std::move(edges));
    }

    std::unordered_set<const Environment*> reachable;
    std::vector<const Environment*> pending;
    const auto visit = [&](const Environment* scope) {
        if (scope && tracked.count(scope) && reachable.insert(scope).second) pending.push_back(scope);
    };
    visit(impl_->global.get());
    for (const auto& scope : live) {
        const long total = static_cast<long>(scope.use_count()) - 1; // minus this snapshot
        if (total > internal[scope.get()]) visit(scope.get());
    }
    while (!pending.empty()) {
        const Environment* scope = pending.back();
        pending.pop_back();
        const auto found = outgoing.find(scope);
        if (found == outgoing.end()) continue;
        for (Environment* target : found->second) visit(target);
    }

    std::size_t reclaimed = 0;
    for (const auto& scope : live) {
        if (reachable.count(scope.get())) continue;
        scope->values.clear();
        scope->parent.reset();
        ++reclaimed;
    }
    live.clear();
    impl_->compact();
    return reclaimed;
}

void Interpreter::set_telemetry(bool enabled) { impl_->telemetry = enabled; }
bool Interpreter::telemetry_enabled() const noexcept { return impl_->telemetry; }
void Interpreter::clear_telemetry() { impl_->calls.clear(); }

void Interpreter::record_call(std::string_view name, std::int64_t nanoseconds,
                              std::size_t result_bytes, std::string_view code) {
    if (!impl_->telemetry) return;
    Impl::CallRecord record;
    record.name = std::string(name);
    record.nanoseconds = nanoseconds;
    record.result_bytes = result_bytes;
    record.code = std::string(code);
    impl_->calls.push_back(std::move(record));
}

// Aggregated per tool, because the question telemetry answers is which tools cost
// the most across a session, not what any single call did.
Value Interpreter::telemetry_summary() const {
    struct Totals {
        std::int64_t calls = 0, nanoseconds = 0, bytes = 0, errors = 0;
    };
    std::map<std::string, Totals> totals;
    for (const Impl::CallRecord& record : impl_->calls) {
        Totals& entry = totals[record.name];
        entry.calls += 1;
        entry.nanoseconds += record.nanoseconds;
        entry.bytes += static_cast<std::int64_t>(record.result_bytes);
        if (!record.code.empty()) entry.errors += 1;
    }
    std::vector<Value> rows;
    for (const auto& entry : totals)
        rows.push_back(ok_result({field("tool", entry.first),
                                  field("calls", entry.second.calls),
                                  field("total-ms", entry.second.nanoseconds / 1000000),
                                  field("bytes", entry.second.bytes),
                                  field("errors", entry.second.errors)}));
    return ok_result({field("tools", Value::list(std::move(rows))),
                      field("recorded", static_cast<std::int64_t>(impl_->calls.size())),
                      field("enabled", impl_->telemetry)});
}

void Interpreter::publish_tool(std::string_view name, Value definition) {
    impl_->tools[std::string(name)] = std::move(definition);
}

Value Interpreter::tool_definition(std::string_view name) const {
    const auto found = impl_->tools.find(std::string(name));
    return found == impl_->tools.end() ? Value::boolean(false) : found->second;
}

// The manifest omits the procedure itself: it describes what an agent may call,
// and a procedure has no transferable written form.
Value Interpreter::tool_manifest() const {
    std::vector<Value> rows;
    for (const auto& entry : impl_->tools) {
        ListBuilder row(11);
        // `shapes` and `proven` are data, not procedures, and they are the whole
        // basis on which a caller may substitute this tool for a command: what it
        // claims to replace, and the replay evidence behind the claim.
        for (const char* key : {"name", "description", "parameters", "provenance",
                                "stability", "shapes", "proven", "translate",
                                "legacy-form", "empty-status", "cases"}) {
            const Value value = option(entry.second, key);
            if (value.type() != Value::Type::Unspecified) row.field(key, value);
        }
        rows.push_back(row.build());
    }
    const std::int64_t count = static_cast<std::int64_t>(rows.size());
    return ok_result({field("tools", Value::list(std::move(rows))), field("count", count)});
}

std::size_t Interpreter::live_environments() const {
    std::size_t count = 0;
    for (const auto& entry : impl_->environments)
        if (!entry.expired()) ++count;
    return count;
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

Value Interpreter::make_handle(const Capability& owner, std::string_view kind,
                               std::shared_ptr<void> token) {
    HandleValue handle;
    handle.runtime = impl_->runtime;
    handle.capability = owner.capability_id();
    handle.kind = std::string(kind);
    handle.token = to_hex(random_nonce());
    handle.resource = std::move(token);
    for (const auto& installed : impl_->installed)
        if (installed->capability_id() == owner.capability_id()) handle.owner = installed;

    std::size_t slot = impl_->handles.size();
    for (std::size_t i = 0; i < impl_->handles.size(); ++i)
        if (!impl_->handles[i].live) { slot = i; break; }
    if (slot == impl_->handles.size()) impl_->handles.emplace_back();
    auto& entry = impl_->handles[slot];
    handle.index = slot;
    handle.generation = entry.generation + 1;
    Value value = ValueAccess::make(Value::Type::Handle, std::move(handle));
    entry.generation += 1;
    entry.token = std::get<HandleValue>(ValueAccess::payload(value)).token;
    entry.handle = value;
    entry.live = true;
    return value;
}

Value Interpreter::resolve_handle(const Value& reference) {
    const auto reject = [](std::string_view detail) {
        return error_result(detail, "invalid-handle", "handle-ref");
    };
    if (!reference.is_list()) return reject("handle reference must be a proper list");
    const Value runtime = option(reference, "runtime");
    const Value index = option(reference, "index");
    const Value generation = option(reference, "generation");
    const Value token = option(reference, "token");
    const Value kind = option(reference, "kind");
    if (runtime.type() != Value::Type::String || index.type() != Value::Type::Integer ||
        generation.type() != Value::Type::Integer || token.type() != Value::Type::String ||
        kind.type() != Value::Type::Symbol)
        return reject("handle reference is missing required fields");
    if (runtime.as_string() != to_hex(impl_->runtime))
        return reject("handle reference belongs to a different runtime");
    const std::int64_t slot = index.as_integer();
    if (slot < 0 || static_cast<std::size_t>(slot) >= impl_->handles.size())
        return reject("handle reference is out of range");
    const auto& entry = impl_->handles[static_cast<std::size_t>(slot)];
    if (!entry.live) return reject("handle has been closed");
    if (entry.generation != static_cast<std::uint64_t>(generation.as_integer()))
        return reject("handle reference is stale");
    // Constant-shape comparison of the unforgeable token defeats guessed references.
    if (entry.token.size() != token.as_string().size() ||
        std::memcmp(entry.token.data(), token.as_string().data(), entry.token.size()) != 0)
        return reject("handle token does not match");
    if (handle_data(entry.handle).kind != kind.as_symbol())
        return reject("handle kind does not match");
    return entry.handle;
}

bool Interpreter::revoke_handle(const Value& handle) {
    if (handle.type() != Value::Type::Handle) return false;
    const HandleValue& data = handle_data(handle);
    if (data.runtime != impl_->runtime) return false;
    if (data.index >= impl_->handles.size()) return false;
    auto& entry = impl_->handles[static_cast<std::size_t>(data.index)];
    if (!entry.live || entry.generation != data.generation) return false;
    if (data.owner) data.owner->release(data.kind, data.resource);
    entry.live = false;
    entry.token.clear();
    entry.handle = Value::nil();
    return true;
}

std::shared_ptr<void> Interpreter::handle_token(const Value& handle, std::string_view kind) const {
    if (handle.type() != Value::Type::Handle) return nullptr;
    const HandleValue& data = handle_data(handle);
    if (data.runtime != impl_->runtime || data.kind != kind) return nullptr;
    if (data.index >= impl_->handles.size()) return nullptr;
    const auto& entry = impl_->handles[static_cast<std::size_t>(data.index)];
    if (!entry.live || entry.generation != data.generation) return nullptr;
    return data.resource;
}

Capability* Interpreter::handle_owner(const Value& handle) const {
    if (handle.type() != Value::Type::Handle) return nullptr;
    return handle_data(handle).owner.get();
}

// ---------------------------------------------------------------------------
// Capability dispatch
// ---------------------------------------------------------------------------

namespace {

bool option_record_shape(const Value& value) {
    if (value.is_nil()) return true;
    if (value.type() != Value::Type::Pair || !value.is_list()) return false;
    for (const Value& entry : value.to_vector()) {
        if (entry.type() != Value::Type::Pair || !entry.is_list()) return false;
        if (entry.car().type() != Value::Type::Symbol) return false;
    }
    return true;
}

Value trailing_options(const std::vector<Value>& arguments) {
    if (arguments.empty()) return Value::nil();
    const Value& last = arguments.back();
    return option_record_shape(last) ? last : Value::nil();
}

// ---------------------------------------------------------------------------
// Output stability
// ---------------------------------------------------------------------------
//
// Host metadata that changes between otherwise identical calls -- modification
// times, inode numbers, process ids, elapsed milliseconds -- makes a result churn
// byte-for-byte even when nothing the caller asked about changed. For an agent
// that is expensive: a churning tool result invalidates the prompt-cache prefix
// and forces the whole suffix to be re-read.
//
// So results are *stable by default*: these fields are omitted unless the caller
// asks for them with `(volatile #t)`. `(fields (a b c))` narrows further, to just
// the named fields. Only symbol-keyed record fields are touched, so string-keyed
// data such as parsed JSON passes through untouched.

bool option_flag_default(const Value& options, std::string_view name, bool fallback) {
    const Value value = option(options, name);
    if (value.type() == Value::Type::Unspecified) return fallback;
    return value.truthy();
}

// Incidental metadata only. Identifiers the caller must act on -- a job's `pid`,
// a handle -- are never stripped, however volatile they are.
bool volatile_field(std::string_view name) {
    return name == "modified" || name == "accessed" || name == "changed" ||
           name == "inode" || name == "elapsed-ms" ||
           // Whether an answer came from the cache is incidental to the answer.
           // It has to be stripped by default or the stability contract breaks
           // on the very first repeat: the same call would return one field the
           // first time and two the second, which is exactly the churn the
           // contract exists to prevent.
           name == "cached";
}

Value filter_record(const Value& value, const std::set<std::string>& keep, bool drop_volatile);

// Rewrites a list: records inside it are filtered, everything else is preserved.
Value filter_list(const Value& value, const std::set<std::string>& keep, bool drop_volatile) {
    std::vector<Value> out;
    for (const Value& element : value.to_vector())
        out.push_back(filter_record(element, keep, drop_volatile));
    return Value::list(std::move(out));
}

Value filter_record(const Value& value, const std::set<std::string>& keep, bool drop_volatile) {
    if (value.type() != Value::Type::Pair || !value.is_list()) return value;
    const std::vector<Value> entries = value.to_vector();
    bool record = !entries.empty();
    for (const Value& entry : entries)
        if (entry.type() != Value::Type::Pair || !entry.is_list() || entry.list_size() < 2 ||
            entry.car().type() != Value::Type::Symbol) {
            record = false;
            break;
        }
    if (!record) return filter_list(value, keep, drop_volatile);

    std::vector<Value> out;
    for (const Value& entry : entries) {
        const std::string_view name = entry.car().as_symbol();
        if (drop_volatile && volatile_field(name)) continue;
        if (!keep.empty() && !keep.count(std::string(name))) continue;
        const Value inner = entry.list_at(1);
        // Nested records and lists of records inherit the same contract.
        const Value rewritten =
            (inner.type() == Value::Type::Pair && inner.is_list())
                ? filter_record(inner, {}, drop_volatile)
                : inner;
        out.push_back(Value::list({entry.car(), rewritten}));
    }
    return Value::list(std::move(out));
}

Value apply_stability(Value result, const Value& options) {
    const bool drop_volatile = !option_flag_default(options, "volatile", false);
    std::set<std::string> keep;
    const Value fields = option(options, "fields");
    if (fields.is_list() && !fields.is_nil())
        for (const Value& name : fields.to_vector()) {
            if (name.type() == Value::Type::Symbol) keep.insert(std::string(name.as_symbol()));
            else if (name.type() == Value::Type::String) keep.insert(std::string(name.as_string()));
        }
    if (!drop_volatile && keep.empty()) return result;
    return filter_record(result, keep, drop_volatile);
}

// Output-mode selection never changes the proper-list envelope; it only decides
// whether the payload is canonical data, evaluable source, or a quoted expression.
Value apply_output(Interpreter& interpreter, Value result, const Value& options) {
    result = apply_stability(std::move(result), options);
    const Value mode = option(options, "output", Value::symbol("data"));
    // The mode is selected by a symbol. A non-symbol `output` entry belongs to the
    // primitive's own option vocabulary and is left alone.
    if (mode.type() != Value::Type::Symbol) return result;
    const std::string_view name = mode.as_symbol();
    if (name == "data") return result;
    if (name == "source")
        return Value::list({field("source", Value::string(interpreter.write(result)))});
    if (name == "expression")
        return Value::list({field("expression",
                                  Value::list({Value::symbol("quote"), std::move(result)}))});
    fail("unknown output mode: " + std::string(name));
}

Value dispatch_capability(Interpreter& interpreter, const char* kind, const char* operation,
                          const std::vector<Value>& arguments) {
    std::shared_ptr<Capability> capability;
    std::size_t first = 0;
    if (!arguments.empty() && arguments[0].type() == Value::Type::Capability) {
        capability = capability_data(arguments[0]).capability;
        first = 1;
        if (capability->capability_kind() != kind)
            return apply_output(interpreter,
                                error_result("expected a " + std::string(kind) +
                                                 " capability, got " +
                                                 std::string(capability->capability_kind()),
                                             "capability-kind", operation),
                                Value::nil());
    } else {
        capability = interpreter.default_capability(kind);
        if (!capability)
            return apply_output(interpreter,
                                error_result("no " + std::string(kind) +
                                                 " capability is installed",
                                             "capability-missing", operation),
                                trailing_options(arguments));
    }
    const std::vector<Value> request(arguments.begin() + static_cast<std::ptrdiff_t>(first),
                                     arguments.end());
    const Value options = trailing_options(request);
    if (!capability->supports(operation))
        return apply_output(interpreter,
                            unsupported_result(operation, std::string(kind) +
                                                              " capability does not support " +
                                                              operation),
                            options);
    Value result;
    const auto started = std::chrono::steady_clock::now();
    try {
        result = capability->invoke(interpreter, operation, request);
    } catch (const Error& error) {
        result = error_result(error.what(), "host-error", operation);
    } catch (const std::exception& error) {
        result = error_result(error.what(), "host-error", operation);
    }
    if (!result.is_list())
        fail(std::string(operation) + " capability must return a proper list");
    result = apply_output(interpreter, std::move(result), options);
    // Every capability call passes through here, so this is the one place
    // telemetry cannot miss one.
    if (interpreter.telemetry_enabled()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
        // A failure is the presence of an `error` string, the same test every
        // other caller uses. Keying on `code` alone would miscount: a successful
        // HTTP result carries one too.
        const bool failed = option(result, "error").type() == Value::Type::String;
        const Value code = option(result, "code");
        interpreter.record_call(operation, static_cast<std::int64_t>(elapsed.count()),
                                interpreter.write(result).size(),
                                failed && code.type() == Value::Type::Symbol
                                    ? code.as_symbol()
                                    : std::string_view());
    }
    return result;
}

NativeFunction capability_primitive(const char* kind, const char* operation) {
    return [kind, operation](Interpreter& interpreter, const std::vector<Value>& arguments) {
        return dispatch_capability(interpreter, kind, operation, arguments);
    };
}

void register_core(Interpreter& interpreter);
void register_text(Interpreter& interpreter);
void register_path_names(Interpreter& interpreter);
void register_diff(Interpreter& interpreter);
void register_json(Interpreter& interpreter);
void register_encoding(Interpreter& interpreter);
void register_output(Interpreter& interpreter);
void register_editor(Interpreter& interpreter);
void register_vcs(Interpreter& interpreter);
void register_shell_parsing(Interpreter& interpreter);
void register_group(Interpreter& interpreter, PrimitiveGroup group);
const std::vector<PrimitiveGroup>& groups_for_kind(std::string_view kind);

} // namespace

// ---------------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------------

Interpreter::Interpreter() : impl_(std::make_unique<Impl>(this)) {
    register_core(*this);
    register_shell_parsing(*this);
    impl_->enabled.insert(PrimitiveGroup::Core);
}

Interpreter::~Interpreter() = default;
Interpreter::Interpreter(Interpreter&& other) noexcept : impl_(std::move(other.impl_)) {
    if (impl_) impl_->owner = this;
}
Interpreter& Interpreter::operator=(Interpreter&& other) noexcept {
    impl_ = std::move(other.impl_);
    if (impl_) impl_->owner = this;
    return *this;
}

std::uint64_t Interpreter::runtime_id() const noexcept { return impl_->runtime; }

Value Interpreter::read(std::string_view source, std::string_view file) {
    Reader reader(source, file);
    Value value = reader.datum();
    if (!reader.done()) fail_at("trailing input after datum", reader.here());
    return value;
}

Value Interpreter::eval(Value expression) {
    return impl_->evaluate(std::move(expression), impl_->global);
}

Value Interpreter::eval(std::string_view source, std::string_view file) {
    Reader reader(source, file);
    Value result = Value::unspecified();
    while (!reader.done()) result = impl_->evaluate(reader.datum(), impl_->global);
    return result;
}

// Buffers only up to one complete top-level datum, so whole files never need to be
// resident to be evaluated.
Value Interpreter::eval_stream(const std::function<int()>& next_byte, std::string_view file) {
    std::string buffer;
    Value result = Value::unspecified();
    int depth = 0;
    bool in_string = false, escaped = false, in_comment = false, started = false;
    for (;;) {
        const int next = next_byte();
        if (next < 0) break;
        const char c = static_cast<char>(next);
        buffer.push_back(c);
        if (in_comment) {
            if (c == '\n') in_comment = false;
            continue;
        }
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == ';') { in_comment = true; continue; }
        if (c == '"') { in_string = true; started = true; continue; }
        if (c == '(' || c == '[') { ++depth; started = true; continue; }
        if (c == ')' || c == ']') { if (depth > 0) --depth; started = true; }
        else if (!std::isspace(static_cast<unsigned char>(c))) { started = true; continue; }
        if (started && depth == 0 &&
            (std::isspace(static_cast<unsigned char>(c)) || c == ')' || c == ']')) {
            Reader reader(buffer, file);
            while (!reader.done()) result = impl_->evaluate(reader.datum(), impl_->global);
            buffer.clear();
            started = false;
        }
    }
    if (!buffer.empty()) {
        Reader reader(buffer, file);
        while (!reader.done()) result = impl_->evaluate(reader.datum(), impl_->global);
    }
    return result;
}

std::string Interpreter::write(const Value& value) const { return write_value(value); }

void Interpreter::define(std::string_view name, Value value) {
    impl_->global->values[std::string(name)] = std::move(value);
}

void Interpreter::define_native(std::string_view name, NativeFunction function) {
    Procedure procedure;
    procedure.native = std::move(function);
    procedure.name = std::string(name);
    define(name, ValueAccess::make(Value::Type::Procedure, std::move(procedure)));
    impl_->primitives.insert(std::string(name));
}

void Interpreter::define_capability(std::string_view name, std::shared_ptr<Capability> capability) {
    if (!capability) fail("capability must not be null");
    impl_->installed.push_back(capability);
    define(name, ValueAccess::make(Value::Type::Capability, CapabilityValue{capability}));
    const std::string kind(capability->capability_kind());
    if (!impl_->defaults.count(kind)) impl_->defaults[kind] = std::move(capability);
}

void Interpreter::install(std::string_view name, std::shared_ptr<Capability> capability) {
    if (!capability) fail("capability must not be null");
    const std::string kind(capability->capability_kind());
    define_capability(name, capability);
    for (const PrimitiveGroup group : groups_for_kind(kind)) enable_group(group);
}

std::shared_ptr<Capability> Interpreter::default_capability(std::string_view kind) const {
    const auto found = impl_->defaults.find(std::string(kind));
    return found == impl_->defaults.end() ? nullptr : found->second;
}

void Interpreter::set_default_capability(std::string_view kind,
                                         std::shared_ptr<Capability> capability) {
    if (!capability) impl_->defaults.erase(std::string(kind));
    else impl_->defaults[std::string(kind)] = std::move(capability);
}

void Interpreter::enable_group(PrimitiveGroup group) {
    if (!impl_->enabled.insert(group).second) return;
    register_group(*this, group);
}

bool Interpreter::group_enabled(PrimitiveGroup group) const {
    return impl_->enabled.count(group) != 0;
}

void Interpreter::enable_path_primitives() {
    enable_group(PrimitiveGroup::Path);
    enable_group(PrimitiveGroup::File);
}
void Interpreter::enable_text_primitives() {
    enable_group(PrimitiveGroup::Text);
    enable_group(PrimitiveGroup::Diff);
    enable_group(PrimitiveGroup::Output);
}
void Interpreter::enable_repository_primitives() { enable_group(PrimitiveGroup::Repository); }
void Interpreter::enable_process_primitives() { enable_group(PrimitiveGroup::Process); }
void Interpreter::enable_system_primitives() {
    enable_group(PrimitiveGroup::System);
    enable_group(PrimitiveGroup::Clock);
}
void Interpreter::enable_archive_primitives() {
    enable_group(PrimitiveGroup::Archive);
    enable_group(PrimitiveGroup::Compression);
}
void Interpreter::enable_editor_primitives() { enable_group(PrimitiveGroup::Editor); }
void Interpreter::enable_shell_primitives() { enable_group(PrimitiveGroup::Shell); }

void Interpreter::enable_all_primitives() {
    for (const PrimitiveGroup group :
         {PrimitiveGroup::Core, PrimitiveGroup::Text, PrimitiveGroup::Diff, PrimitiveGroup::Json,
          PrimitiveGroup::Encoding, PrimitiveGroup::Output, PrimitiveGroup::Path,
          PrimitiveGroup::File, PrimitiveGroup::Repository, PrimitiveGroup::Process,
          PrimitiveGroup::System, PrimitiveGroup::Terminal, PrimitiveGroup::Clock,
          PrimitiveGroup::Service, PrimitiveGroup::Archive, PrimitiveGroup::Compression,
          PrimitiveGroup::Crypto, PrimitiveGroup::Network, PrimitiveGroup::Http,
          PrimitiveGroup::RemoteShell, PrimitiveGroup::Editor, PrimitiveGroup::Shell,
          PrimitiveGroup::Desktop, PrimitiveGroup::Logging, PrimitiveGroup::Vcs})
        enable_group(group);
}

Value Interpreter::cons(Value car, Value cdr) {
    if (cdr.is_nil()) return Value::list({std::move(car)});
    if (cdr.type() == Value::Type::Pair && cdr.is_list()) {
        std::vector<Value> values;
        values.reserve(cdr.list_size() + 1);
        values.push_back(std::move(car));
        for (const Value& element : cdr.to_vector()) values.push_back(element);
        return Value::list(std::move(values));
    }
    return Value::improper({std::move(car)}, std::move(cdr));
}

Value Interpreter::list(const std::vector<Value>& values) { return Value::list(values); }

Value Interpreter::apply(const Value& procedure, const std::vector<Value>& arguments) {
    return impl_->apply(procedure, arguments);
}

std::vector<std::string> Interpreter::primitive_names() const {
    return {impl_->primitives.begin(), impl_->primitives.end()};
}

bool Interpreter::has_primitive(std::string_view name) const {
    return impl_->primitives.count(std::string(name)) != 0;
}

// ---------------------------------------------------------------------------
// Core procedures
// ---------------------------------------------------------------------------

namespace {

std::string_view want_string(const Value& value, const char* name) {
    if (value.type() != Value::Type::String) fail(std::string(name) + " expects a string");
    return value.as_string();
}

std::int64_t want_integer(const Value& value, const char* name) {
    if (value.type() != Value::Type::Integer) fail(std::string(name) + " expects an integer");
    return value.as_integer();
}

std::vector<Value> want_list(const Value& value, const char* name) {
    if (!value.is_list()) fail(std::string(name) + " expects a proper list");
    return value.to_vector();
}

// Total order used by `list-sort` and the text tools when no predicate is supplied.
int natural_order(const Value& a, const Value& b) {
    if (numeric(a) && numeric(b)) return compare_numbers(a, b);
    const auto rank = [](const Value& value) {
        switch (value.type()) {
        case Value::Type::Nil: return 0;
        case Value::Type::Boolean: return 1;
        case Value::Type::Integer:
        case Value::Type::Float: return 2;
        case Value::Type::Character: return 3;
        case Value::Type::String: return 4;
        case Value::Type::Symbol: return 5;
        default: return 6;
        }
    };
    if (rank(a) != rank(b)) return rank(a) < rank(b) ? -1 : 1;
    switch (a.type()) {
    case Value::Type::Boolean:
        return a.as_boolean() == b.as_boolean() ? 0 : (!a.as_boolean() ? -1 : 1);
    case Value::Type::Character:
        return a.as_character() == b.as_character() ? 0 : (a.as_character() < b.as_character() ? -1 : 1);
    case Value::Type::String: {
        const int order = a.as_string().compare(b.as_string());
        return order < 0 ? -1 : order > 0 ? 1 : 0;
    }
    case Value::Type::Symbol: {
        const int order = a.as_symbol().compare(b.as_symbol());
        return order < 0 ? -1 : order > 0 ? 1 : 0;
    }
    default: return 0;
    }
}

// Caller-supplied widths, counts, and tab stops drive allocation, so they are
// bounded. Without this a single option record could ask for an arbitrarily large
// buffer and take the embedding agent down with it.
constexpr std::int64_t kMaxGeneratedBytes = 1 << 24; // 16 MiB
constexpr std::int64_t kMaxLayoutWidth = 1 << 16;

std::int64_t bounded(std::int64_t value, std::int64_t limit, const char* name) {
    if (value < 0) fail(std::string(name) + " must not be negative");
    if (value > limit)
        fail(std::string(name) + " exceeds the maximum of " + std::to_string(limit));
    return value;
}

// `string->number` reports failure as #f rather than raising, so malformed input in
// data does not abort an evaluation.
Value parse_number(std::string_view text) {
    Value out;
    std::string error;
    if (!numeric_token(std::string(text), out, error) || !error.empty())
        return Value::boolean(false);
    return out;
}

void register_core(Interpreter& interpreter) {
    const auto add = [&interpreter](const char* name, NativeFunction function) {
        interpreter.define_native(name, std::move(function));
    };

    // -- numbers ------------------------------------------------------------
    add("+", [](Interpreter&, const std::vector<Value>& a) {
        bool inexact = false;
        std::int64_t whole = 0;
        double fraction = 0;
        for (const Value& value : a) {
            if (value.type() == Value::Type::Float) { inexact = true; fraction += value.as_float(); }
            else whole = checked_add(whole, want_integer(value, "+"), "+");
        }
        return inexact ? Value::real(fraction + static_cast<double>(whole)) : Value::integer(whole);
    });
    add("-", [](Interpreter&, const std::vector<Value>& a) {
        arity_least(a, 1, "-");
        const bool inexact = std::any_of(a.begin(), a.end(), [](const Value& v) {
            return v.type() == Value::Type::Float;
        });
        if (inexact) {
            double result = a.size() == 1 ? 0 : real_of(a[0]);
            for (std::size_t i = a.size() == 1 ? 0 : 1; i < a.size(); ++i) result -= real_of(a[i]);
            return Value::real(result);
        }
        std::int64_t result = a.size() == 1 ? 0 : want_integer(a[0], "-");
        for (std::size_t i = a.size() == 1 ? 0 : 1; i < a.size(); ++i)
            result = checked_sub(result, want_integer(a[i], "-"), "-");
        return Value::integer(result);
    });
    add("*", [](Interpreter&, const std::vector<Value>& a) {
        const bool inexact = std::any_of(a.begin(), a.end(), [](const Value& v) {
            return v.type() == Value::Type::Float;
        });
        if (inexact) {
            double result = 1;
            for (const Value& value : a) result *= real_of(value);
            return Value::real(result);
        }
        std::int64_t result = 1;
        for (const Value& value : a) result = checked_mul(result, want_integer(value, "*"), "*");
        return Value::integer(result);
    });
    add("/", [](Interpreter&, const std::vector<Value>& a) {
        arity_least(a, 1, "/");
        double result = a.size() == 1 ? 1 : real_of(a[0]);
        for (std::size_t i = a.size() == 1 ? 0 : 1; i < a.size(); ++i) {
            const double divisor = real_of(a[i]);
            if (divisor == 0) fail("division by zero");
            result /= divisor;
        }
        return Value::real(result);
    });
    add("quotient", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "quotient");
        const std::int64_t divisor = want_integer(a[1], "quotient");
        if (divisor == 0) fail("division by zero");
        const std::int64_t dividend = want_integer(a[0], "quotient");
        if (dividend == std::numeric_limits<std::int64_t>::min() && divisor == -1)
            fail("integer overflow in quotient");
        return Value::integer(dividend / divisor);
    });
    add("remainder", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "remainder");
        const std::int64_t divisor = want_integer(a[1], "remainder");
        if (divisor == 0) fail("division by zero");
        const std::int64_t dividend = want_integer(a[0], "remainder");
        if (dividend == std::numeric_limits<std::int64_t>::min() && divisor == -1)
            return Value::integer(0);
        return Value::integer(dividend % divisor);
    });
    add("modulo", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "modulo");
        const std::int64_t divisor = want_integer(a[1], "modulo");
        if (divisor == 0) fail("division by zero");
        const std::int64_t dividend = want_integer(a[0], "modulo");
        if (dividend == std::numeric_limits<std::int64_t>::min() && divisor == -1)
            return Value::integer(0);
        std::int64_t result = dividend % divisor;
        if (result != 0 && ((result < 0) != (divisor < 0))) result += divisor;
        return Value::integer(result);
    });
    add("abs", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "abs");
        if (a[0].type() == Value::Type::Float) return Value::real(std::fabs(a[0].as_float()));
        const std::int64_t value = want_integer(a[0], "abs");
        if (value == std::numeric_limits<std::int64_t>::min()) fail("integer overflow in abs");
        return Value::integer(value < 0 ? -value : value);
    });
    add("min", [](Interpreter&, const std::vector<Value>& a) {
        arity_least(a, 1, "min");
        Value best = a[0];
        for (const Value& value : a) if (compare_numbers(value, best) < 0) best = value;
        return best;
    });
    add("max", [](Interpreter&, const std::vector<Value>& a) {
        arity_least(a, 1, "max");
        Value best = a[0];
        for (const Value& value : a) if (compare_numbers(value, best) > 0) best = value;
        return best;
    });
    add("gcd", [](Interpreter&, const std::vector<Value>& a) {
        std::int64_t result = 0;
        for (const Value& value : a) {
            std::int64_t next = want_integer(value, "gcd");
            if (next < 0) next = -next;
            while (next) { const std::int64_t rest = result % next; result = next; next = rest; }
            if (result < 0) result = -result;
        }
        return Value::integer(result);
    });
    add("lcm", [](Interpreter&, const std::vector<Value>& a) {
        std::int64_t result = 1;
        for (const Value& value : a) {
            std::int64_t next = want_integer(value, "lcm");
            if (next == 0) return Value::integer(0);
            if (next < 0) next = -next;
            std::int64_t x = result, y = next;
            while (y) { const std::int64_t rest = x % y; x = y; y = rest; }
            result = checked_mul(result / x, next, "lcm");
        }
        return Value::integer(result < 0 ? -result : result);
    });
    add("expt", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "expt");
        if (a[0].type() == Value::Type::Integer && a[1].type() == Value::Type::Integer &&
            a[1].as_integer() >= 0) {
            std::int64_t result = 1, base = a[0].as_integer(), power = a[1].as_integer();
            while (power) {
                if (power & 1) result = checked_mul(result, base, "expt");
                power >>= 1;
                if (power) base = checked_mul(base, base, "expt");
            }
            return Value::integer(result);
        }
        return Value::real(std::pow(real_of(a[0]), real_of(a[1])));
    });
    add("sqrt", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "sqrt");
        const double value = real_of(a[0]);
        if (value < 0) fail("sqrt of a negative number");
        return Value::real(std::sqrt(value));
    });
    add("exp", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "exp");
        return Value::real(std::exp(real_of(a[0])));
    });
    add("log", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "log");
        const double value = real_of(a[0]);
        if (value <= 0) fail("log of a non-positive number");
        return Value::real(std::log(value));
    });
    const auto rounding = [&add](const char* name, double (*fn)(double)) {
        add(name, [name, fn](Interpreter&, const std::vector<Value>& a) {
            arity(a, 1, name);
            if (a[0].type() == Value::Type::Integer) return a[0];
            return Value::real(fn(real_of(a[0])));
        });
    };
    rounding("floor", [](double v) { return std::floor(v); });
    rounding("ceiling", [](double v) { return std::ceil(v); });
    rounding("truncate", [](double v) { return std::trunc(v); });
    rounding("round", [](double v) { return std::nearbyint(v); });
    add("exact->inexact", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "exact->inexact");
        return Value::real(real_of(a[0]));
    });
    add("inexact->exact", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "inexact->exact");
        if (a[0].type() == Value::Type::Integer) return a[0];
        const double value = std::trunc(real_of(a[0]));
        if (value >= 9223372036854775808.0 || value < -9223372036854775808.0)
            fail("integer overflow in inexact->exact");
        return Value::integer(static_cast<std::int64_t>(value));
    });
    const auto ordering = [&add](const char* name, int low, int high) {
        add(name, [name, low, high](Interpreter&, const std::vector<Value>& a) {
            arity_least(a, 2, name);
            for (std::size_t i = 0; i + 1 < a.size(); ++i) {
                if (!numeric(a[i]) || !numeric(a[i + 1])) fail(std::string(name) + " expects numbers");
                const int order = compare_numbers(a[i], a[i + 1]);
                if (order != low && order != high) return Value::boolean(false);
            }
            return Value::boolean(true);
        });
    };
    ordering("=", 0, 0);
    ordering("<", -1, -1);
    ordering(">", 1, 1);
    ordering("<=", -1, 0);
    ordering(">=", 1, 0);
    const auto sign_test = [&add](const char* name, int want) {
        add(name, [name, want](Interpreter&, const std::vector<Value>& a) {
            arity(a, 1, name);
            if (!numeric(a[0])) fail(std::string(name) + " expects a number");
            return Value::boolean(compare_numbers(a[0], Value::integer(0)) == want);
        });
    };
    sign_test("zero?", 0);
    sign_test("positive?", 1);
    sign_test("negative?", -1);
    add("even?", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "even?");
        return Value::boolean(want_integer(a[0], "even?") % 2 == 0);
    });
    add("odd?", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "odd?");
        return Value::boolean(want_integer(a[0], "odd?") % 2 != 0);
    });
    add("number->string", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 1, "number->string");
        if (!numeric(a[0])) fail("number->string expects a number");
        return Value::string(vm.write(a[0]));
    });
    add("string->number", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "string->number");
        return parse_number(want_string(a[0], "string->number"));
    });

    // -- pairs and lists ----------------------------------------------------
    add("cons", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 2, "cons");
        return vm.cons(a[0], a[1]);
    });
    add("car", [](Interpreter&, const std::vector<Value>& a) { arity(a, 1, "car"); return a[0].car(); });
    add("cdr", [](Interpreter&, const std::vector<Value>& a) { arity(a, 1, "cdr"); return a[0].cdr(); });
    const auto accessor = [&add](const char* name, const char* path) {
        add(name, [name, path](Interpreter&, const std::vector<Value>& a) {
            arity(a, 1, name);
            Value at = a[0];
            for (const char* step = path + std::strlen(path); step-- != path;)
                at = *step == 'a' ? at.car() : at.cdr();
            return at;
        });
    };
    accessor("caar", "aa");
    accessor("cadr", "ad");
    accessor("cdar", "da");
    accessor("cddr", "dd");
    accessor("caddr", "add");
    accessor("cdddr", "ddd");
    add("list", [](Interpreter&, const std::vector<Value>& a) { return Value::list(a); });
    add("length", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "length");
        if (a[0].type() == Value::Type::String)
            return Value::integer(static_cast<std::int64_t>(a[0].string_size()));
        return Value::integer(static_cast<std::int64_t>(a[0].list_size()));
    });
    add("list-ref", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "list-ref");
        const std::int64_t index = want_integer(a[1], "list-ref");
        if (index < 1) fail("list-ref index is 1-based");
        return a[0].list_at(static_cast<std::size_t>(index - 1));
    });
    add("list-tail", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "list-tail");
        const std::int64_t drop = want_integer(a[1], "list-tail");
        if (drop < 0) fail("list-tail expects a non-negative count");
        return a[0].list_tail(static_cast<std::size_t>(drop));
    });
    add("list-head", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "list-head");
        const std::int64_t keep = want_integer(a[1], "list-head");
        if (keep < 0) fail("list-head expects a non-negative count");
        std::vector<Value> values = want_list(a[0], "list-head");
        if (static_cast<std::size_t>(keep) > values.size()) fail("list-head count exceeds length");
        values.resize(static_cast<std::size_t>(keep));
        return Value::list(std::move(values));
    });
    add("sublist", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 3, "sublist");
        const std::vector<Value> values = want_list(a[0], "sublist");
        const std::int64_t from = want_integer(a[1], "sublist");
        const std::int64_t to = want_integer(a[2], "sublist");
        if (from < 1 || to < from - 1 || static_cast<std::size_t>(to) > values.size())
            fail("sublist range is out of bounds");
        return Value::list({values.begin() + (from - 1), values.begin() + to});
    });
    add("append", [](Interpreter&, const std::vector<Value>& a) {
        if (a.empty()) return Value::nil();
        std::vector<Value> values;
        for (std::size_t i = 0; i + 1 < a.size(); ++i) {
            const std::vector<Value> part = want_list(a[i], "append");
            values.insert(values.end(), part.begin(), part.end());
        }
        return Value::improper(std::move(values), a.back());
    });
    add("reverse", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "reverse");
        std::vector<Value> values = want_list(a[0], "reverse");
        std::reverse(values.begin(), values.end());
        return Value::list(std::move(values));
    });
    add("list-copy", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "list-copy");
        return Value::list(want_list(a[0], "list-copy"));
    });
    add("last", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "last");
        const std::vector<Value> values = want_list(a[0], "last");
        if (values.empty()) fail("last requires a non-empty list");
        return values.back();
    });
    const auto search = [&add](const char* name, bool identity, bool association) {
        add(name, [name, identity, association](Interpreter&, const std::vector<Value>& a) {
            arity(a, 2, name);
            Value at = a[1];
            while (at.type() == Value::Type::Pair) {
                const Value element = at.car();
                const Value key = association ? (element.type() == Value::Type::Pair ? element.car()
                                                                                     : element)
                                              : element;
                const bool hit = identity ? key.same_object(a[0]) || equal_step(key, a[0])
                                          : deep_equal(key, a[0]);
                if (hit) return association ? element : at;
                at = at.cdr();
            }
            return Value::boolean(false);
        });
    };
    search("memq", true, false);
    search("member", false, false);
    search("assq", true, true);
    search("assoc", false, true);
    add("list-index", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "list-index");
        const std::vector<Value> values = want_list(a[1], "list-index");
        for (std::size_t i = 0; i < values.size(); ++i)
            if (deep_equal(values[i], a[0])) return Value::integer(static_cast<std::int64_t>(i + 1));
        return Value::boolean(false);
    });
    add("apply", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_least(a, 2, "apply");
        std::vector<Value> arguments(a.begin() + 1, a.end() - 1);
        const std::vector<Value> spread = want_list(a.back(), "apply");
        arguments.insert(arguments.end(), spread.begin(), spread.end());
        return vm.apply(a[0], arguments);
    });
    add("map", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_least(a, 2, "map");
        std::vector<std::vector<Value>> columns;
        for (std::size_t i = 1; i < a.size(); ++i) columns.push_back(want_list(a[i], "map"));
        std::size_t rows = columns[0].size();
        for (const auto& column : columns) rows = std::min(rows, column.size());
        std::vector<Value> out;
        out.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row) {
            std::vector<Value> arguments;
            arguments.reserve(columns.size());
            for (const auto& column : columns) arguments.push_back(column[row]);
            out.push_back(vm.apply(a[0], arguments));
        }
        return Value::list(std::move(out));
    });
    add("for-each", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_least(a, 2, "for-each");
        std::vector<std::vector<Value>> columns;
        for (std::size_t i = 1; i < a.size(); ++i) columns.push_back(want_list(a[i], "for-each"));
        std::size_t rows = columns[0].size();
        for (const auto& column : columns) rows = std::min(rows, column.size());
        for (std::size_t row = 0; row < rows; ++row) {
            std::vector<Value> arguments;
            arguments.reserve(columns.size());
            for (const auto& column : columns) arguments.push_back(column[row]);
            vm.apply(a[0], arguments);
        }
        return Value::unspecified();
    });
    add("filter", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 2, "filter");
        std::vector<Value> out;
        for (const Value& element : want_list(a[1], "filter"))
            if (vm.apply(a[0], {element}).truthy()) out.push_back(element);
        return Value::list(std::move(out));
    });
    add("remove", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 2, "remove");
        std::vector<Value> out;
        for (const Value& element : want_list(a[1], "remove"))
            if (!vm.apply(a[0], {element}).truthy()) out.push_back(element);
        return Value::list(std::move(out));
    });
    add("fold-left", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 3, "fold-left");
        Value accumulator = a[1];
        for (const Value& element : want_list(a[2], "fold-left"))
            accumulator = vm.apply(a[0], {accumulator, element});
        return accumulator;
    });
    add("fold-right", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 3, "fold-right");
        const std::vector<Value> values = want_list(a[2], "fold-right");
        Value accumulator = a[1];
        for (std::size_t i = values.size(); i-- > 0;)
            accumulator = vm.apply(a[0], {values[i], accumulator});
        return accumulator;
    });
    add("reduce", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 3, "reduce");
        const std::vector<Value> values = want_list(a[2], "reduce");
        if (values.empty()) return a[1];
        Value accumulator = values[0];
        for (std::size_t i = 1; i < values.size(); ++i)
            accumulator = vm.apply(a[0], {accumulator, values[i]});
        return accumulator;
    });
    add("list-sort", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "list-sort");
        std::vector<Value> values = want_list(a[0], "list-sort");
        if (a.size() == 2) {
            const Value& predicate = a[1];
            std::stable_sort(values.begin(), values.end(),
                             [&vm, &predicate](const Value& x, const Value& y) {
                                 return vm.apply(predicate, {x, y}).truthy();
                             });
        } else {
            std::stable_sort(values.begin(), values.end(), [](const Value& x, const Value& y) {
                return natural_order(x, y) < 0;
            });
        }
        return Value::list(std::move(values));
    });

    // -- predicates ---------------------------------------------------------
    const auto predicate = [&add](const char* name, bool (*fn)(const Value&)) {
        add(name, [name, fn](Interpreter&, const std::vector<Value>& a) {
            arity(a, 1, name);
            return Value::boolean(fn(a[0]));
        });
    };
    predicate("null?", [](const Value& v) { return v.is_nil(); });
    predicate("pair?", [](const Value& v) { return v.type() == Value::Type::Pair; });
    predicate("list?", [](const Value& v) { return v.is_list(); });
    predicate("number?", [](const Value& v) { return numeric(v); });
    predicate("integer?", [](const Value& v) { return v.type() == Value::Type::Integer; });
    predicate("float?", [](const Value& v) { return v.type() == Value::Type::Float; });
    predicate("exact?", [](const Value& v) { return v.type() == Value::Type::Integer; });
    predicate("inexact?", [](const Value& v) { return v.type() == Value::Type::Float; });
    predicate("string?", [](const Value& v) { return v.type() == Value::Type::String; });
    predicate("symbol?", [](const Value& v) { return v.type() == Value::Type::Symbol; });
    predicate("char?", [](const Value& v) { return v.type() == Value::Type::Character; });
    predicate("boolean?", [](const Value& v) { return v.type() == Value::Type::Boolean; });
    predicate("procedure?", [](const Value& v) { return v.type() == Value::Type::Procedure; });
    predicate("capability?", [](const Value& v) { return v.type() == Value::Type::Capability; });
    predicate("handle?", [](const Value& v) { return v.type() == Value::Type::Handle; });
    predicate("unspecified?", [](const Value& v) { return v.type() == Value::Type::Unspecified; });
    add("not", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "not");
        return Value::boolean(!a[0].truthy());
    });
    add("eq?", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "eq?");
        return Value::boolean(a[0].same_object(a[1]) || equal_step(a[0], a[1]));
    });
    add("eqv?", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "eqv?");
        return Value::boolean(a[0].same_object(a[1]) || equal_step(a[0], a[1]));
    });
    add("equal?", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "equal?");
        return Value::boolean(deep_equal(a[0], a[1]));
    });

    // -- characters ---------------------------------------------------------
    add("char->integer", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "char->integer");
        if (a[0].type() != Value::Type::Character) fail("char->integer expects a character");
        return Value::integer(static_cast<unsigned char>(a[0].as_character()));
    });
    add("integer->char", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "integer->char");
        const std::int64_t code = want_integer(a[0], "integer->char");
        if (code < 0 || code > 255) fail("integer->char expects a byte value");
        return Value::character(static_cast<char>(code));
    });
    const auto char_class = [&add](const char* name, int (*fn)(int)) {
        add(name, [name, fn](Interpreter&, const std::vector<Value>& a) {
            arity(a, 1, name);
            if (a[0].type() != Value::Type::Character) fail(std::string(name) + " expects a character");
            return Value::boolean(fn(static_cast<unsigned char>(a[0].as_character())) != 0);
        });
    };
    char_class("char-alphabetic?", [](int c) { return std::isalpha(c); });
    char_class("char-numeric?", [](int c) { return std::isdigit(c); });
    char_class("char-whitespace?", [](int c) { return std::isspace(c); });
    add("char-upcase", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "char-upcase");
        if (a[0].type() != Value::Type::Character) fail("char-upcase expects a character");
        return Value::character(static_cast<char>(
            std::toupper(static_cast<unsigned char>(a[0].as_character()))));
    });
    add("char-downcase", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "char-downcase");
        if (a[0].type() != Value::Type::Character) fail("char-downcase expects a character");
        return Value::character(static_cast<char>(
            std::tolower(static_cast<unsigned char>(a[0].as_character()))));
    });
    const auto char_order = [&add](const char* name, int low, int high) {
        add(name, [name, low, high](Interpreter&, const std::vector<Value>& a) {
            arity(a, 2, name);
            if (a[0].type() != Value::Type::Character || a[1].type() != Value::Type::Character)
                fail(std::string(name) + " expects characters");
            const unsigned char left = static_cast<unsigned char>(a[0].as_character());
            const unsigned char right = static_cast<unsigned char>(a[1].as_character());
            const int order = left < right ? -1 : left > right ? 1 : 0;
            return Value::boolean(order == low || order == high);
        });
    };
    char_order("char=?", 0, 0);
    char_order("char<?", -1, -1);
    char_order("char>?", 1, 1);

    // -- strings ------------------------------------------------------------
    add("string-length", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "string-length");
        return Value::integer(static_cast<std::int64_t>(want_string(a[0], "string-length").size()));
    });
    add("string-ref", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "string-ref");
        const std::string_view text = want_string(a[0], "string-ref");
        const std::int64_t index = want_integer(a[1], "string-ref");
        if (index < 1 || static_cast<std::size_t>(index) > text.size())
            fail("string index out of range");
        return Value::character(text[static_cast<std::size_t>(index - 1)]);
    });
    add("substring", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 3, "substring");
        const std::string_view text = want_string(a[0], "substring");
        const std::int64_t from = want_integer(a[1], "substring");
        const std::int64_t to = want_integer(a[2], "substring");
        if (from < 1 || to < from - 1 || static_cast<std::size_t>(to) > text.size())
            fail("substring range is out of bounds");
        return Value::string(text.substr(static_cast<std::size_t>(from - 1),
                                         static_cast<std::size_t>(to - from + 1)));
    });
    add("string-append", [](Interpreter&, const std::vector<Value>& a) {
        std::string out;
        for (const Value& value : a) {
            const std::string_view part = want_string(value, "string-append");
            out.append(part.data(), part.size());
        }
        return Value::string(out);
    });
    add("string-copy", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "string-copy");
        return Value::string(want_string(a[0], "string-copy"));
    });
    add("make-string", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "make-string");
        const std::int64_t count = bounded(want_integer(a[0], "make-string"),
                                           kMaxGeneratedBytes, "make-string length");
        const char fill = a.size() == 2 ? a[1].as_character() : ' ';
        return Value::string(std::string(static_cast<std::size_t>(count), fill));
    });
    add("string-null?", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "string-null?");
        return Value::boolean(want_string(a[0], "string-null?").empty());
    });
    const auto string_order = [&add](const char* name, int low, int high, bool fold) {
        add(name, [name, low, high, fold](Interpreter&, const std::vector<Value>& a) {
            arity_least(a, 2, name);
            for (std::size_t i = 0; i + 1 < a.size(); ++i) {
                std::string left(want_string(a[i], name));
                std::string right(want_string(a[i + 1], name));
                if (fold) {
                    for (char& c : left) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    for (char& c : right) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                const int raw = left.compare(right);
                const int order = raw < 0 ? -1 : raw > 0 ? 1 : 0;
                if (order != low && order != high) return Value::boolean(false);
            }
            return Value::boolean(true);
        });
    };
    string_order("string=?", 0, 0, false);
    string_order("string<?", -1, -1, false);
    string_order("string>?", 1, 1, false);
    string_order("string<=?", -1, 0, false);
    string_order("string>=?", 1, 0, false);
    string_order("string-ci=?", 0, 0, true);
    const auto string_case = [&add](const char* name, int (*fn)(int)) {
        add(name, [name, fn](Interpreter&, const std::vector<Value>& a) {
            arity(a, 1, name);
            std::string out(want_string(a[0], name));
            for (char& c : out) c = static_cast<char>(fn(static_cast<unsigned char>(c)));
            return Value::string(out);
        });
    };
    string_case("string-upcase", [](int c) { return std::toupper(c); });
    string_case("string-downcase", [](int c) { return std::tolower(c); });
    add("string->list", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "string->list");
        const std::string_view text = want_string(a[0], "string->list");
        std::vector<Value> out;
        out.reserve(text.size());
        for (const char c : text) out.push_back(Value::character(c));
        return Value::list(std::move(out));
    });
    add("list->string", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "list->string");
        std::string out;
        for (const Value& element : want_list(a[0], "list->string")) {
            if (element.type() != Value::Type::Character)
                fail("list->string expects a list of characters");
            out.push_back(element.as_character());
        }
        return Value::string(out);
    });
    add("string->symbol", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "string->symbol");
        return Value::symbol(want_string(a[0], "string->symbol"));
    });
    add("symbol->string", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "symbol->string");
        if (a[0].type() != Value::Type::Symbol) fail("symbol->string expects a symbol");
        return Value::string(a[0].as_symbol());
    });
    add("string-index", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 2, 3, "string-index");
        const std::string_view text = want_string(a[0], "string-index");
        const std::string_view needle = want_string(a[1], "string-index");
        const std::int64_t from = a.size() == 3 ? want_integer(a[2], "string-index") : 1;
        if (from < 1) fail("string-index start is 1-based");
        if (static_cast<std::size_t>(from - 1) > text.size()) return Value::boolean(false);
        const std::size_t at = text.find(needle, static_cast<std::size_t>(from - 1));
        if (at == std::string_view::npos) return Value::boolean(false);
        return Value::integer(static_cast<std::int64_t>(at + 1));
    });
    add("string-contains?", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "string-contains?");
        return Value::boolean(want_string(a[0], "string-contains?")
                                  .find(want_string(a[1], "string-contains?")) !=
                              std::string_view::npos);
    });
    add("string-prefix?", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "string-prefix?");
        const std::string_view prefix = want_string(a[0], "string-prefix?");
        const std::string_view text = want_string(a[1], "string-prefix?");
        return Value::boolean(text.size() >= prefix.size() &&
                              text.compare(0, prefix.size(), prefix) == 0);
    });
    add("string-suffix?", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "string-suffix?");
        const std::string_view suffix = want_string(a[0], "string-suffix?");
        const std::string_view text = want_string(a[1], "string-suffix?");
        return Value::boolean(text.size() >= suffix.size() &&
                              text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0);
    });
    add("string-reverse", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "string-reverse");
        std::string out(want_string(a[0], "string-reverse"));
        std::reverse(out.begin(), out.end());
        return Value::string(out);
    });
    add("string-trim", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "string-trim");
        const std::string_view text = want_string(a[0], "string-trim");
        std::size_t begin = 0, end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
        return Value::string(text.substr(begin, end - begin));
    });
    const auto pad = [&add](const char* name, bool left) {
        add(name, [name, left](Interpreter&, const std::vector<Value>& a) {
            arity_between(a, 2, 3, name);
            const std::string_view text = want_string(a[0], name);
            const std::int64_t width = bounded(want_integer(a[1], name), kMaxGeneratedBytes,
                                               name);
            const char fill = a.size() == 3 ? a[2].as_character() : ' ';
            if (static_cast<std::size_t>(width) <= text.size())
                return Value::string(left ? text.substr(text.size() - static_cast<std::size_t>(width))
                                          : text.substr(0, static_cast<std::size_t>(width)));
            const std::string filler(static_cast<std::size_t>(width) - text.size(), fill);
            return Value::string(left ? filler + std::string(text) : std::string(text) + filler);
        });
    };
    pad("string-pad-left", true);
    pad("string-pad-right", false);
    add("string-split", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "string-split");
        const std::string_view text = want_string(a[0], "string-split");
        const std::string_view separator = want_string(a[1], "string-split");
        if (separator.empty()) fail("string-split expects a non-empty separator");
        std::vector<Value> out;
        std::size_t begin = 0;
        for (;;) {
            const std::size_t at = text.find(separator, begin);
            if (at == std::string_view::npos) break;
            out.push_back(Value::string(text.substr(begin, at - begin)));
            begin = at + separator.size();
        }
        out.push_back(Value::string(text.substr(begin)));
        return Value::list(std::move(out));
    });
    add("string-join", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "string-join");
        const std::string_view separator = a.size() == 2 ? want_string(a[1], "string-join")
                                                         : std::string_view();
        std::string out;
        bool first = true;
        for (const Value& element : want_list(a[0], "string-join")) {
            if (!first) out.append(separator.data(), separator.size());
            const std::string_view part = want_string(element, "string-join");
            out.append(part.data(), part.size());
            first = false;
        }
        return Value::string(out);
    });
    add("string-replace", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 3, "string-replace");
        const std::string_view text = want_string(a[0], "string-replace");
        const std::string_view from = want_string(a[1], "string-replace");
        const std::string_view to = want_string(a[2], "string-replace");
        if (from.empty()) fail("string-replace expects a non-empty pattern");
        std::string out;
        std::size_t begin = 0;
        for (;;) {
            const std::size_t at = text.find(from, begin);
            if (at == std::string_view::npos) break;
            out.append(text.substr(begin, at - begin));
            out.append(to);
            begin = at + from.size();
        }
        out.append(text.substr(begin));
        return Value::string(out);
    });

    // -- errors -------------------------------------------------------------
    add("error", [](Interpreter& vm, const std::vector<Value>& a) -> Value {
        arity_least(a, 1, "error");
        std::string message(a[0].type() == Value::Type::String ? std::string(a[0].as_string())
                                                               : vm.write(a[0]));
        for (std::size_t i = 1; i < a.size(); ++i) message += " " + vm.write(a[i]);
        fail(message);
    });
    add("catch-errors", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "catch-errors");
        try {
            return vm.apply(a[0], {});
        } catch (const Error& error) {
            const Value record = error_result(error.what(), "scheme-error", "catch-errors");
            if (a.size() == 2) return vm.apply(a[1], {record});
            return record;
        }
    });

    // -- reading, writing, and references -----------------------------------
    add("read-from-string", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "read-from-string");
        const std::string file = a.size() == 2 ? std::string(want_string(a[1], "read-from-string"))
                                               : std::string("<string>");
        return vm.read(want_string(a[0], "read-from-string"), file);
    });
    add("read", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "read");
        const std::string file = a.size() == 2 ? std::string(want_string(a[1], "read"))
                                               : std::string("<string>");
        return vm.read(want_string(a[0], "read"), file);
    });
    add("write-to-string", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 1, "write-to-string");
        return Value::string(vm.write(a[0]));
    });
    add("write", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 1, "write");
        return Value::string(vm.write(a[0]));
    });
    add("unspecified", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 0, "unspecified");
        return Value::unspecified();
    });
    add("primitive-names", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 0, "primitive-names");
        std::vector<Value> out;
        for (const std::string& name : vm.primitive_names()) out.push_back(Value::symbol(name));
        return Value::list(std::move(out));
    });
    // Results are association lists, so field lookup is the most common operation an
    // embedder performs on them.
    add("field-ref", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 2, 3, "field-ref");
        const Value fallback = a.size() == 3 ? a[2] : Value::boolean(false);
        const std::string name = a[1].type() == Value::Type::Symbol
                                     ? std::string(a[1].as_symbol())
                                     : std::string(want_string(a[1], "field-ref"));
        return option(a[0], name, fallback);
    });
    add("field-names", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "field-names");
        std::vector<Value> names;
        if (a[0].is_list())
            for (const Value& entry : a[0].to_vector())
                if (entry.type() == Value::Type::Pair && entry.is_list() &&
                    entry.car().type() == Value::Type::Symbol)
                    names.push_back(entry.car());
        return Value::list(std::move(names));
    });
    add("error?", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "error?");
        return Value::boolean(option(a[0], "error").type() == Value::Type::String);
    });
    add("ok?", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "ok?");
        return Value::boolean(a[0].is_list() &&
                              option(a[0], "error").type() != Value::Type::String);
    });
    add("handle-ref", [](Interpreter& vm, const std::vector<Value>& a) {
        return vm.resolve_handle(Value::list(a));
    });
    add("handle-close", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 1, "handle-close");
        const bool revoked = vm.revoke_handle(a[0]);
        if (!revoked)
            return error_result("handle is not live", "invalid-handle", "handle-close");
        return ok_result({field("closed", Value::boolean(true))});
    });
    add("handle-describe", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "handle-describe");
        if (a[0].type() != Value::Type::Handle)
            return error_result("not a handle", "invalid-handle", "handle-describe");
        const HandleValue& handle = handle_data(a[0]);
        ListBuilder out(4);
        out.symbol_field("kind", handle.kind);
        out.field("index", static_cast<std::int64_t>(handle.index));
        out.field("generation", static_cast<std::int64_t>(handle.generation));
        out.field("runtime", to_hex(handle.runtime));
        return out.build();
    });
    // Procedures are runtime-bound; a transfer attempt reports a structured refusal
    // instead of emitting an unreadable token.
    add("procedure-ref", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "procedure-ref");
        return error_result("procedures are not transferable between runtimes",
                            "non-serializable", "procedure-ref",
                            {field("name", a[0])});
    });
    add("capability-ref", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 2, "capability-ref");
        const std::string_view kind = want_string(a[0], "capability-ref");
        const std::string_view identity = want_string(a[1], "capability-ref");
        const std::shared_ptr<Capability> installed = vm.default_capability(kind);
        if (installed && to_hex(installed->capability_id()) == identity)
            return ValueAccess::make(Value::Type::Capability, CapabilityValue{installed});
        return error_result("capability reference is not resolvable in this runtime",
                            "non-serializable", "capability-ref",
                            {field("kind", Value::string(kind))});
    });
    add("capability-call", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_least(a, 2, "capability-call");
        if (a[0].type() != Value::Type::Capability)
            fail("capability-call expects a capability as its first argument");
        const std::shared_ptr<Capability>& capability = capability_data(a[0]).capability;
        const std::string operation = symbol_name(a[1], "capability-call");
        const std::vector<Value> request(a.begin() + 2, a.end());
        if (!capability->supports(operation))
            return unsupported_result(operation, "capability does not support this operation");
        Value result = capability->invoke(vm, operation, request);
        if (!result.is_list()) fail("capability must return a proper list");
        return apply_output(vm, std::move(result), trailing_options(request));
    });
    add("capability-kind", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "capability-kind");
        if (a[0].type() != Value::Type::Capability)
            return error_result("not a capability", "invalid-argument", "capability-kind");
        return ok_result({field("kind", Value::string(capability_data(a[0]).capability->capability_kind())),
                          field("identity", Value::string(
                              to_hex(capability_data(a[0]).capability->capability_id())))});
    });
    add("collect-garbage", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 0, "collect-garbage");
        return ok_result({field("reclaimed", static_cast<std::int64_t>(vm.collect())),
                          field("live", static_cast<std::int64_t>(vm.live_environments()))});
    });
}

// ---------------------------------------------------------------------------
// Text processing
// ---------------------------------------------------------------------------
//
// Text primitives are pure: they accept data and options and never touch the host.
// Input is either a single string, which is split on newlines, or an explicit list
// of line strings. Every result is a proper list of association-list fields.

std::vector<std::string> split_lines(std::string_view text, std::string_view separator) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t at = text.find(separator, begin);
        if (at == std::string_view::npos) break;
        lines.emplace_back(text.substr(begin, at - begin));
        begin = at + separator.size();
    }
    // A trailing separator terminates the last line rather than starting an empty one.
    if (begin < text.size()) lines.emplace_back(text.substr(begin));
    return lines;
}

std::vector<std::string> as_lines(const Value& value, const char* name) {
    if (value.type() == Value::Type::String) return split_lines(value.as_string(), "\n");
    if (value.is_list()) {
        std::vector<std::string> lines;
        for (const Value& element : value.to_vector()) {
            if (element.type() != Value::Type::String)
                fail(std::string(name) + " expects a string or a list of strings");
            lines.emplace_back(element.as_string());
        }
        return lines;
    }
    fail(std::string(name) + " expects a string or a list of strings");
}

std::string as_text(const Value& value, const char* name) {
    if (value.type() == Value::Type::String) return std::string(value.as_string());
    std::string out;
    bool first = true;
    for (const std::string& line : as_lines(value, name)) {
        if (!first) out += '\n';
        out += line;
        first = false;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Text input sources
// ---------------------------------------------------------------------------
//
// Inline data stays pure: a string is split on newlines, a list of strings is
// taken as lines. A *tagged source* instead names files, and is the form agents
// reach for -- `grep -rn needle src/` becomes
//
//   (grep "needle" '(glob "src/**/*.c") '((line-numbers #t) (limit 20)))
//
// Tagged sources read through the filesystem capability; inline data never
// touches the host. The tag is unambiguous because a list of lines always begins
// with a string, never a symbol.

struct TextInput {
    std::vector<std::string> lines;
    std::vector<std::string> paths;        // one per file; empty for inline data
    std::vector<std::size_t> file_of;      // parallel to lines: index into paths
    std::vector<std::size_t> line_in_file; // parallel to lines: 1-based within its file
    std::vector<Value> unreadable;         // structured per-path failures
    std::vector<std::size_t> file_bytes;   // parallel to paths
    std::size_t bytes = 0;                 // true byte count of the source
    bool from_files = false;
};

// Reads one file. A failure becomes a structured record rather than aborting the
// whole call, so one unreadable path does not lose the other results.
bool read_source_file(Interpreter& vm, const std::string& path, std::string& text, Value& error) {
    const Value result =
        dispatch_capability(vm, FileSystemCapability::kind, "read-file",
                            {Value::string(path), Value::nil()});
    const Value content = option(result, "text");
    if (content.type() != Value::Type::String) {
        error = result;
        return false;
    }
    text = std::string(content.as_string());
    return true;
}

// Expands a glob to regular-file paths, forwarding the traversal options the
// caller already passed to the text primitive.
std::vector<std::string> expand_source_glob(Interpreter& vm, const std::string& pattern,
                                            const Value& options, Value& error) {
    ListBuilder request(5);
    request.symbol_field("kind", "file");
    for (const char* name : {"directory", "hidden", "max-depth", "exclude"}) {
        const Value value = option(options, name);
        if (value.type() != Value::Type::Unspecified) request.field(name, value);
    }
    const Value result = dispatch_capability(vm, FileSystemCapability::kind, "glob",
                                             {Value::string(pattern), request.build()});
    if (option(result, "error").type() == Value::Type::String) {
        error = result;
        return {};
    }
    std::vector<std::string> paths;
    const Value entries = option(result, "entries");
    if (entries.is_list())
        for (const Value& entry : entries.to_vector()) {
            const Value path = option(entry, "path");
            if (path.type() == Value::Type::String) paths.emplace_back(path.as_string());
        }
    return paths;
}

// Returns the tag when `value` is a tagged source, empty otherwise.
std::string_view source_tag(const Value& value) {
    if (value.type() != Value::Type::Pair || !value.is_list()) return {};
    const Value head = value.car();
    if (head.type() != Value::Type::Symbol) return {};
    const std::string_view tag = head.as_symbol();
    if (tag == "files" || tag == "glob" || tag == "text" || tag == "lines") return tag;
    return {};
}

// Flattens the arguments of a tagged source: `(files "a" "b")` and
// `(files ("a" "b"))` mean the same thing.
std::vector<std::string> source_arguments(const Value& value, const char* name) {
    std::vector<std::string> out;
    const std::vector<std::string> nothing;
    Value at = value.cdr();
    while (at.type() == Value::Type::Pair) {
        const Value element = at.car();
        if (element.type() == Value::Type::String) out.emplace_back(element.as_string());
        else if (element.is_list())
            for (const Value& inner : element.to_vector()) {
                if (inner.type() != Value::Type::String)
                    fail(std::string(name) + " source expects strings");
                out.emplace_back(inner.as_string());
            }
        else fail(std::string(name) + " source expects strings");
        at = at.cdr();
    }
    return out;
}

TextInput resolve_input(Interpreter& vm, const Value& value, const Value& options,
                        const char* name) {
    TextInput input;
    const std::string_view tag = source_tag(value);
    if (tag.empty() || tag == "lines" || tag == "text") {
        if (tag == "text") {
            for (const std::string& part : source_arguments(value, name)) {
                input.bytes += part.size();
                for (std::string& line : split_lines(part, "\n")) input.lines.push_back(line);
            }
        } else if (tag == "lines") {
            input.lines = source_arguments(value, name);
        } else if (value.type() == Value::Type::String) {
            input.bytes = value.string_size();
            input.lines = split_lines(value.as_string(), "\n");
            return input;
        } else {
            input.lines = as_lines(value, name);
        }
        // A list of lines has no newline of its own; count them as joined.
        if (input.bytes == 0)
            for (std::size_t i = 0; i < input.lines.size(); ++i)
                input.bytes += input.lines[i].size() + (i + 1 < input.lines.size() ? 1 : 0);
        return input;
    }

    input.from_files = true;
    std::vector<std::string> paths;
    if (tag == "files") {
        paths = source_arguments(value, name);
    } else {
        for (const std::string& pattern : source_arguments(value, name)) {
            Value error;
            const std::vector<std::string> matched =
                expand_source_glob(vm, pattern, options, error);
            if (error.type() == Value::Type::Pair) input.unreadable.push_back(error);
            paths.insert(paths.end(), matched.begin(), matched.end());
        }
    }

    for (const std::string& path : paths) {
        std::string text;
        Value error;
        if (!read_source_file(vm, path, text, error)) {
            input.unreadable.push_back(error);
            continue;
        }
        const std::size_t index = input.paths.size();
        input.paths.push_back(path);
        input.file_bytes.push_back(text.size());
        input.bytes += text.size();
        std::size_t number = 0;
        for (std::string& line : split_lines(text, "\n")) {
            input.lines.push_back(std::move(line));
            input.file_of.push_back(index);
            input.line_in_file.push_back(++number);
        }
    }
    return input;
}

// Convenience for primitives that only need the lines.
std::vector<std::string> resolve_lines(Interpreter& vm, const Value& value, const Value& options,
                                       const char* name) {
    return resolve_input(vm, value, options, name).lines;
}

std::string resolve_text(Interpreter& vm, const Value& value, const Value& options,
                         const char* name) {
    if (source_tag(value).empty() && value.type() == Value::Type::String)
        return std::string(value.as_string());
    const std::vector<std::string> lines = resolve_lines(vm, value, options, name);
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out;
}

Value lines_value(const std::vector<std::string>& lines) {
    std::vector<Value> out;
    out.reserve(lines.size());
    for (const std::string& line : lines) out.push_back(Value::string(line));
    return Value::list(std::move(out));
}

Value line_result(const std::vector<std::string>& lines) {
    ListBuilder out(2);
    out.field("lines", lines_value(lines));
    out.field("count", static_cast<std::int64_t>(lines.size()));
    return out.build();
}

std::int64_t option_integer(const Value& options, std::string_view name, std::int64_t fallback) {
    const Value value = option(options, name);
    if (value.type() == Value::Type::Unspecified) return fallback;
    if (value.type() != Value::Type::Integer)
        fail(std::string(name) + " option expects an integer");
    return value.as_integer();
}

bool option_flag(const Value& options, std::string_view name, bool fallback = false) {
    const Value value = option(options, name);
    if (value.type() == Value::Type::Unspecified) return fallback;
    return value.truthy();
}

std::string option_text(const Value& options, std::string_view name, std::string_view fallback) {
    const Value value = option(options, name);
    if (value.type() == Value::Type::Unspecified) return std::string(fallback);
    if (value.type() == Value::Type::String) return std::string(value.as_string());
    if (value.type() == Value::Type::Character) return std::string(1, value.as_character());
    fail(std::string(name) + " option expects a string");
}

std::regex compile_pattern(std::string_view pattern, bool ignore_case, const char* name) {
    auto flags = std::regex::ECMAScript | std::regex::optimize;
    if (ignore_case) flags |= std::regex::icase;
    try {
        return std::regex(std::string(pattern), flags);
    } catch (const std::regex_error& error) {
        fail(std::string(name) + ": invalid regular expression: " + error.what());
    }
}

// Matches a pattern against one line either literally or as a regular expression.
struct Matcher {
    bool literal = false;
    bool ignore_case = false;
    std::string needle;
    std::regex expression;

    Matcher(std::string_view pattern, const Value& options, const char* name) {
        literal = option_flag(options, "literal");
        ignore_case = option_flag(options, "ignore-case");
        needle = std::string(pattern);
        if (literal && ignore_case)
            for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!literal) expression = compile_pattern(pattern, ignore_case, name);
    }

    // Returns the 1-based column of the first match, or 0 when the line does not match.
    std::size_t find(const std::string& line, std::size_t* length = nullptr) const {
        if (literal) {
            std::string haystack = line;
            if (ignore_case)
                for (char& c : haystack)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (needle.empty()) { if (length) *length = 0; return 1; }
            const std::size_t at = haystack.find(needle);
            if (at == std::string::npos) return 0;
            if (length) *length = needle.size();
            return at + 1;
        }
        std::smatch match;
        if (!std::regex_search(line, match, expression)) return 0;
        if (length) *length = static_cast<std::size_t>(match.length(0));
        return static_cast<std::size_t>(match.position(0)) + 1;
    }
};

// `input` supplies per-line provenance when the lines came from files, so each
// match can report its own path and its line number *within that file*.
Value grep_lines(const std::vector<std::string>& lines, const Matcher& matcher,
                 const Value& options, const std::string& source,
                 const TextInput* input = nullptr) {
    const bool invert = option_flag(options, "invert");
    const bool count_only = option_flag(options, "count-only");
    const std::int64_t limit = option_integer(options, "limit", 0);
    const std::int64_t before = option_integer(options, "before", 0);
    const std::int64_t after = option_integer(options, "after", 0);
    std::vector<Value> matches;
    std::int64_t total = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::size_t length = 0;
        const std::size_t column = matcher.find(lines[i], &length);
        const bool hit = invert ? column == 0 : column != 0;
        if (!hit) continue;
        ++total;
        if (count_only) continue;
        if (limit > 0 && static_cast<std::int64_t>(matches.size()) >= limit) continue;
        const bool traced = input && input->from_files && i < input->file_of.size();
        ListBuilder record(6);
        record.field("line", static_cast<std::int64_t>(traced ? input->line_in_file[i] : i + 1));
        record.field("column", static_cast<std::int64_t>(invert ? 1 : column));
        record.field("length", static_cast<std::int64_t>(length));
        record.field("text", lines[i]);
        if (traced) record.field("path", input->paths[input->file_of[i]]);
        else if (!source.empty()) record.field("path", source);
        if (before > 0) {
            std::vector<std::string> context;
            for (std::size_t j = i >= static_cast<std::size_t>(before) ? i - before : 0; j < i; ++j)
                context.push_back(lines[j]);
            record.field("before", lines_value(context));
        }
        if (after > 0) {
            std::vector<std::string> context;
            for (std::size_t j = i + 1; j < lines.size() && j <= i + static_cast<std::size_t>(after);
                 ++j)
                context.push_back(lines[j]);
            record.field("after", lines_value(context));
        }
        matches.push_back(record.build());
    }
    ListBuilder out(5);
    out.field("matches", Value::list(std::move(matches)));
    out.field("count", total);
    out.field("truncated", limit > 0 && total > limit);
    if (input && input->from_files) {
        out.field("files-scanned", static_cast<std::int64_t>(input->paths.size()));
        if (!input->unreadable.empty())
            out.field("unreadable", Value::list(input->unreadable));
    }
    return out.build();
}

std::vector<std::string> split_fields(const std::string& line, const Value& options) {
    const bool whitespace = option_flag(options, "whitespace");
    const std::string separator = option_text(options, "separator", whitespace ? "" : "\t");
    std::vector<std::string> fields;
    if (whitespace || separator.empty()) {
        std::size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            const std::size_t begin = i;
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (i > begin) fields.push_back(line.substr(begin, i - begin));
        }
        return fields;
    }
    return split_lines(line + separator, separator);
}

void register_text(Interpreter& interpreter) {
    const auto add = [&interpreter](const char* name, NativeFunction function) {
        interpreter.define_native(name, std::move(function));
    };

    add("text-lines", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "text-lines");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::string separator = option_text(options, "separator", "\n");
        const std::vector<std::string> lines =
            (source_tag(a[0]).empty() && a[0].type() == Value::Type::String)
                ? split_lines(a[0].as_string(), separator)
                : resolve_lines(vm, a[0], options, "text-lines");
        return line_result(lines);
    });
    add("text-fields", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "text-fields");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        std::vector<Value> rows;
        for (const std::string& line : resolve_lines(vm, a[0], options, "text-fields"))
            rows.push_back(lines_value(split_fields(line, options)));
        const std::int64_t count = static_cast<std::int64_t>(rows.size());
        ListBuilder out(2);
        out.field("rows", Value::list(std::move(rows)));
        out.field("count", count);
        return out.build();
    });
    add("text-select", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 2, "text-select");
        const std::vector<Value> wanted = want_list(a[1], "text-select");
        std::vector<Value> rows;
        for (const Value& row : want_list(a[0], "text-select")) {
            const std::vector<Value> fields = want_list(row, "text-select");
            std::vector<Value> picked;
            for (const Value& index : wanted) {
                const std::int64_t at = want_integer(index, "text-select");
                if (at < 1 || static_cast<std::size_t>(at) > fields.size())
                    picked.push_back(Value::string(""));
                else picked.push_back(fields[static_cast<std::size_t>(at - 1)]);
            }
            rows.push_back(Value::list(std::move(picked)));
        }
        const std::int64_t count = static_cast<std::int64_t>(rows.size());
        ListBuilder out(2);
        out.field("rows", Value::list(std::move(rows)));
        out.field("count", count);
        return out.build();
    });
    add("text-replace", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 3, 4, "text-replace");
        const Value options = a.size() == 4 ? a[3] : Value::nil();
        const std::string text = resolve_text(vm, a[0], options, "text-replace");
        const std::string_view pattern = want_string(a[1], "text-replace");
        const std::string replacement(want_string(a[2], "text-replace"));
        const bool all = option_flag(options, "all", true);
        const std::int64_t limit = option_integer(options, "limit", 0);
        std::string out;
        std::int64_t replacements = 0;
        if (option_flag(options, "literal")) {
            if (pattern.empty()) fail("text-replace expects a non-empty pattern");
            std::size_t begin = 0;
            for (;;) {
                const std::size_t at = text.find(pattern, begin);
                if (at == std::string::npos) break;
                if (!all && replacements == 1) break;
                if (limit > 0 && replacements >= limit) break;
                out.append(text, begin, at - begin);
                out.append(replacement);
                begin = at + pattern.size();
                ++replacements;
            }
            out.append(text, begin, std::string::npos);
        } else {
            const std::regex expression =
                compile_pattern(pattern, option_flag(options, "ignore-case"), "text-replace");
            auto begin = std::sregex_iterator(text.begin(), text.end(), expression);
            const auto end = std::sregex_iterator();
            std::size_t last = 0;
            for (auto it = begin; it != end; ++it) {
                if (!all && replacements == 1) break;
                if (limit > 0 && replacements >= limit) break;
                const std::smatch& match = *it;
                out.append(text, last, static_cast<std::size_t>(match.position(0)) - last);
                out.append(match.format(replacement));
                last = static_cast<std::size_t>(match.position(0) + match.length(0));
                ++replacements;
            }
            out.append(text, last, std::string::npos);
        }
        ListBuilder result(2);
        result.field("text", out);
        result.field("replacements", replacements);
        return result.build();
    });
    add("grep", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 2, 3, "grep");
        const Value options = a.size() == 3 ? a[2] : Value::nil();
        const Matcher matcher(want_string(a[0], "grep"), options, "grep");
        const TextInput input = resolve_input(vm, a[1], options, "grep");
        Value result = grep_lines(input.lines, matcher, options, {}, &input);
        // `grep -l`: collapse to the distinct files that matched.
        if (option_flag(options, "files-with-matches") && input.from_files) {
            std::vector<Value> names;
            std::set<std::string> seen;
            for (const Value& match : option(result, "matches").to_vector()) {
                const Value path = option(match, "path");
                if (path.type() == Value::Type::String && seen.insert(std::string(path.as_string())).second)
                    names.push_back(path);
            }
            const std::int64_t count = static_cast<std::int64_t>(names.size());
            return ok_result({field("files", Value::list(std::move(names))), field("count", count)});
        }
        return result;
    });
    add("head", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "head");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::int64_t bytes = option_integer(options, "bytes", 0);
        if (bytes > 0) {
            const std::string text = resolve_text(vm, a[0], options, "head");
            return ok_result({field("text", text.substr(0, static_cast<std::size_t>(bytes))),
                              field("bytes", std::min<std::int64_t>(
                                  bytes, static_cast<std::int64_t>(text.size())))});
        }
        std::vector<std::string> lines = resolve_lines(vm, a[0], options, "head");
        const std::int64_t count = option_integer(options, "count", 10);
        if (count < 0) fail("head count must not be negative");
        if (static_cast<std::size_t>(count) < lines.size())
            lines.resize(static_cast<std::size_t>(count));
        return line_result(lines);
    });
    add("tail", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "tail");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::int64_t bytes = option_integer(options, "bytes", 0);
        if (bytes > 0) {
            const std::string text = resolve_text(vm, a[0], options, "tail");
            const std::size_t keep = std::min<std::size_t>(static_cast<std::size_t>(bytes),
                                                           text.size());
            return ok_result({field("text", text.substr(text.size() - keep)),
                              field("bytes", static_cast<std::int64_t>(keep))});
        }
        std::vector<std::string> lines = resolve_lines(vm, a[0], options, "tail");
        const std::int64_t count = option_integer(options, "count", 10);
        if (count < 0) fail("tail count must not be negative");
        if (static_cast<std::size_t>(count) < lines.size())
            lines.erase(lines.begin(),
                        lines.end() - static_cast<std::ptrdiff_t>(count));
        return line_result(lines);
    });
    add("wc", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "wc");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const TextInput input = resolve_input(vm, a[0], options, "wc");
        const auto tally = [](const std::string& line, std::int64_t& words, std::int64_t& bytes) {
            bool inside = false;
            for (const char c : line) {
                if (std::isspace(static_cast<unsigned char>(c))) inside = false;
                else if (!inside) { inside = true; ++words; }
            }
            bytes += static_cast<std::int64_t>(line.size()) + 1; // the line's own newline
        };
        std::int64_t words = 0, bytes = 0;
        (void)bytes;
        std::vector<std::int64_t> per_lines(input.paths.size(), 0);
        std::vector<std::int64_t> per_words(input.paths.size(), 0);
        std::vector<std::int64_t> per_bytes(input.paths.size(), 0);
        for (std::size_t i = 0; i < input.lines.size(); ++i) {
            std::int64_t line_words = 0, line_bytes = 0;
            tally(input.lines[i], line_words, line_bytes);
            words += line_words;
            bytes += line_bytes;
            if (input.from_files && i < input.file_of.size()) {
                const std::size_t f = input.file_of[i];
                per_lines[f] += 1;
                per_words[f] += line_words;
                per_bytes[f] += line_bytes;
            }
        }
        const std::int64_t total_bytes = static_cast<std::int64_t>(input.bytes);
        ListBuilder out(5);
        out.field("lines", static_cast<std::int64_t>(input.lines.size()));
        out.field("words", words);
        out.field("bytes", total_bytes);
        out.field("characters", total_bytes);
        if (input.from_files) {
            std::vector<Value> per_file;
            for (std::size_t f = 0; f < input.paths.size(); ++f)
                per_file.push_back(ok_result({field("path", input.paths[f]),
                                              field("lines", per_lines[f]),
                                              field("words", per_words[f]),
                                              field("bytes", static_cast<std::int64_t>(
                                                  input.file_bytes[f]))}));
            out.field("files", Value::list(std::move(per_file)));
        }
        return out.build();
    });
    add("sort", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "sort");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        std::vector<std::string> lines = resolve_lines(vm, a[0], options, "sort");
        const bool numeric_order = option_flag(options, "numeric");
        const bool fold = option_flag(options, "ignore-case");
        const std::int64_t key = option_integer(options, "key", 0);
        const auto extract = [&](const std::string& line) {
            if (key <= 0) return line;
            const std::vector<std::string> fields = split_fields(line, options);
            if (static_cast<std::size_t>(key) > fields.size()) return std::string();
            return fields[static_cast<std::size_t>(key - 1)];
        };
        std::stable_sort(lines.begin(), lines.end(),
                         [&](const std::string& x, const std::string& y) {
                             std::string left = extract(x), right = extract(y);
                             if (numeric_order) {
                                 const double lv = std::strtod(left.c_str(), nullptr);
                                 const double rv = std::strtod(right.c_str(), nullptr);
                                 if (lv != rv) return lv < rv;
                                 return left < right;
                             }
                             if (fold) {
                                 for (char& c : left)
                                     c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                 for (char& c : right)
                                     c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                             }
                             return left < right;
                         });
        if (option_flag(options, "reverse")) std::reverse(lines.begin(), lines.end());
        if (option_flag(options, "unique")) lines.erase(std::unique(lines.begin(), lines.end()),
                                                        lines.end());
        return line_result(lines);
    });
    add("uniq", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "uniq");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::vector<std::string> lines = resolve_lines(vm, a[0], options, "uniq");
        const bool only_repeated = option_flag(options, "repeated");
        const bool only_unique = option_flag(options, "unique");
        std::vector<std::string> kept;
        std::vector<Value> counted;
        for (std::size_t i = 0; i < lines.size();) {
            std::size_t run = 1;
            while (i + run < lines.size() && lines[i + run] == lines[i]) ++run;
            const bool repeated = run > 1;
            if ((!only_repeated || repeated) && (!only_unique || !repeated)) {
                kept.push_back(lines[i]);
                counted.push_back(Value::list({Value::integer(static_cast<std::int64_t>(run)),
                                               Value::string(lines[i])}));
            }
            i += run;
        }
        ListBuilder out(3);
        out.field("lines", lines_value(kept));
        out.field("counts", Value::list(std::move(counted)));
        out.field("count", static_cast<std::int64_t>(kept.size()));
        return out.build();
    });
    add("cut", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "cut");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const Value fields = option(options, "fields");
        const Value characters = option(options, "characters");
        const std::string output_separator = option_text(options, "output-separator", "\t");
        std::vector<std::string> out;
        for (const std::string& line : resolve_lines(vm, a[0], options, "cut")) {
            std::string built;
            if (characters.type() != Value::Type::Unspecified) {
                for (const Value& index : want_list(characters, "cut")) {
                    const std::int64_t at = want_integer(index, "cut");
                    if (at >= 1 && static_cast<std::size_t>(at) <= line.size())
                        built += line[static_cast<std::size_t>(at - 1)];
                }
            } else if (fields.type() != Value::Type::Unspecified) {
                const std::vector<std::string> parts = split_fields(line, options);
                bool first = true;
                for (const Value& index : want_list(fields, "cut")) {
                    const std::int64_t at = want_integer(index, "cut");
                    if (at < 1 || static_cast<std::size_t>(at) > parts.size()) continue;
                    if (!first) built += output_separator;
                    built += parts[static_cast<std::size_t>(at - 1)];
                    first = false;
                }
            } else {
                built = line;
            }
            out.push_back(built);
        }
        return line_result(out);
    });
    add("comm", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 2, 3, "comm");
        std::vector<std::string> left = resolve_lines(vm, a[0], Value::nil(), "comm");
        std::vector<std::string> right = resolve_lines(vm, a[1], Value::nil(), "comm");
        std::sort(left.begin(), left.end());
        std::sort(right.begin(), right.end());
        std::vector<std::string> only_left, only_right, both;
        std::set_difference(left.begin(), left.end(), right.begin(), right.end(),
                            std::back_inserter(only_left));
        std::set_difference(right.begin(), right.end(), left.begin(), left.end(),
                            std::back_inserter(only_right));
        std::set_intersection(left.begin(), left.end(), right.begin(), right.end(),
                              std::back_inserter(both));
        ListBuilder out(3);
        out.field("only-first", lines_value(only_left));
        out.field("only-second", lines_value(only_right));
        out.field("both", lines_value(both));
        return out.build();
    });
    add("join", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 2, 3, "join");
        const Value options = a.size() == 3 ? a[2] : Value::nil();
        const std::int64_t left_key = option_integer(options, "first-key", 1);
        const std::int64_t right_key = option_integer(options, "second-key", 1);
        const std::string output_separator = option_text(options, "output-separator", " ");
        const auto key_of = [&](const std::vector<std::string>& fields, std::int64_t which) {
            if (which < 1 || static_cast<std::size_t>(which) > fields.size()) return std::string();
            return fields[static_cast<std::size_t>(which - 1)];
        };
        std::vector<std::pair<std::string, std::vector<std::string>>> right;
        for (const std::string& line : resolve_lines(vm, a[1], options, "join")) {
            std::vector<std::string> fields = split_fields(line, options);
            right.emplace_back(key_of(fields, right_key), std::move(fields));
        }
        std::vector<std::string> out;
        for (const std::string& line : resolve_lines(vm, a[0], options, "join")) {
            const std::vector<std::string> fields = split_fields(line, options);
            const std::string key = key_of(fields, left_key);
            for (const auto& candidate : right) {
                if (candidate.first != key) continue;
                std::string built = key;
                for (std::size_t i = 0; i < fields.size(); ++i)
                    if (static_cast<std::int64_t>(i + 1) != left_key)
                        built += output_separator + fields[i];
                for (std::size_t i = 0; i < candidate.second.size(); ++i)
                    if (static_cast<std::int64_t>(i + 1) != right_key)
                        built += output_separator + candidate.second[i];
                out.push_back(built);
            }
        }
        return line_result(out);
    });
    add("paste", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "paste");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::string separator = option_text(options, "separator", "\t");
        std::vector<std::vector<std::string>> columns;
        for (const Value& source : want_list(a[0], "paste"))
            columns.push_back(resolve_lines(vm, source, options, "paste"));
        std::size_t rows = 0;
        for (const auto& column : columns) rows = std::max(rows, column.size());
        std::vector<std::string> out;
        for (std::size_t row = 0; row < rows; ++row) {
            std::string built;
            for (std::size_t i = 0; i < columns.size(); ++i) {
                if (i) built += separator;
                if (row < columns[i].size()) built += columns[i][row];
            }
            out.push_back(built);
        }
        return line_result(out);
    });
    add("rev", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "rev");
        std::vector<std::string> lines = resolve_lines(vm, a[0], Value::nil(), "rev");
        for (std::string& line : lines) std::reverse(line.begin(), line.end());
        return line_result(lines);
    });
    add("nl", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "nl");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::int64_t start = option_integer(options, "start", 1);
        const std::int64_t width = bounded(option_integer(options, "width", 6),
                                           kMaxLayoutWidth, "nl width");
        const std::string separator = option_text(options, "separator", "\t");
        const bool skip_blank = option_flag(options, "skip-blank", true);
        std::vector<std::string> out;
        std::int64_t number = start;
        for (const std::string& line : resolve_lines(vm, a[0], options, "nl")) {
            if (skip_blank && line.empty()) { out.push_back(line); continue; }
            std::string label = std::to_string(number++);
            if (static_cast<std::int64_t>(label.size()) < width)
                label.insert(label.begin(),
                             static_cast<std::size_t>(width) - label.size(), ' ');
            out.push_back(label + separator + line);
        }
        return line_result(out);
    });
    add("fold", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "fold");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::int64_t width = bounded(option_integer(options, "width", 80),
                                           kMaxGeneratedBytes, "fold width");
        if (width < 1) fail("fold width must be positive");
        const bool break_words = !option_flag(options, "whole-words");
        std::vector<std::string> out;
        for (const std::string& line : resolve_lines(vm, a[0], options, "fold")) {
            if (line.empty()) { out.push_back(line); continue; }
            std::size_t begin = 0;
            while (begin < line.size()) {
                std::size_t take = std::min<std::size_t>(static_cast<std::size_t>(width),
                                                          line.size() - begin);
                if (!break_words && begin + take < line.size()) {
                    const std::size_t space = line.find_last_of(' ', begin + take);
                    if (space != std::string::npos && space > begin) take = space - begin;
                }
                out.push_back(line.substr(begin, take));
                begin += take;
                if (!break_words) while (begin < line.size() && line[begin] == ' ') ++begin;
            }
        }
        return line_result(out);
    });
    add("fmt", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "fmt");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::int64_t width = bounded(option_integer(options, "width", 75),
                                           kMaxGeneratedBytes, "fmt width");
        if (width < 1) fail("fmt width must be positive");
        std::vector<std::string> out;
        std::string current;
        const auto flush = [&] { if (!current.empty()) { out.push_back(current); current.clear(); } };
        for (const std::string& line : resolve_lines(vm, a[0], options, "fmt")) {
            if (line.empty()) { flush(); out.push_back(""); continue; }
            for (const std::string& word : split_fields(line, Value::list({field("whitespace",
                                                                                 Value::boolean(true))}))) {
                if (current.empty()) current = word;
                else if (static_cast<std::int64_t>(current.size() + 1 + word.size()) <= width)
                    current += " " + word;
                else { out.push_back(current); current = word; }
            }
        }
        flush();
        return line_result(out);
    });
    add("expand", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "expand");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::int64_t stop = bounded(option_integer(options, "tab-stop", 8),
                                          kMaxLayoutWidth, "expand tab stop");
        if (stop < 1) fail("expand tab stop must be positive");
        std::vector<std::string> out;
        for (const std::string& line : resolve_lines(vm, a[0], options, "expand")) {
            std::string built;
            for (const char c : line) {
                if (c != '\t') { built += c; continue; }
                do built += ' ';
                while (static_cast<std::int64_t>(built.size()) % stop != 0);
            }
            out.push_back(built);
        }
        return line_result(out);
    });
    add("unexpand", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "unexpand");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::int64_t stop = bounded(option_integer(options, "tab-stop", 8),
                                          kMaxLayoutWidth, "unexpand tab stop");
        if (stop < 1) fail("unexpand tab stop must be positive");
        std::vector<std::string> out;
        for (const std::string& line : resolve_lines(vm, a[0], options, "unexpand")) {
            std::size_t leading = 0;
            while (leading < line.size() && line[leading] == ' ') ++leading;
            const std::size_t tabs = leading / static_cast<std::size_t>(stop);
            out.push_back(std::string(tabs, '\t') +
                          line.substr(tabs * static_cast<std::size_t>(stop)));
        }
        return line_result(out);
    });
    add("tr", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 2, 4, "tr");
        const Value options = a.size() == 4 ? a[3] : Value::nil();
        const std::string text = resolve_text(vm, a[0], options, "tr");
        const std::string from(want_string(a[1], "tr"));
        const std::string to = a.size() >= 3 ? std::string(want_string(a[2], "tr")) : std::string();
        const bool remove = option_flag(options, "delete");
        const bool squeeze = option_flag(options, "squeeze");
        std::string out;
        char previous = 0;
        bool have_previous = false;
        for (const char c : text) {
            const std::size_t at = from.find(c);
            char mapped = c;
            if (at != std::string::npos) {
                if (remove) continue;
                if (!to.empty()) mapped = at < to.size() ? to[at] : to.back();
            }
            if (squeeze && have_previous && mapped == previous && at != std::string::npos) continue;
            out += mapped;
            previous = mapped;
            have_previous = true;
        }
        return ok_result({field("text", out),
                          field("bytes", static_cast<std::int64_t>(out.size()))});
    });
    add("split", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "split");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::int64_t per_chunk = option_integer(options, "lines", 1000);
        if (per_chunk < 1) fail("split requires a positive line count");
        const std::vector<std::string> lines = resolve_lines(vm, a[0], options, "split");
        std::vector<Value> chunks;
        for (std::size_t i = 0; i < lines.size(); i += static_cast<std::size_t>(per_chunk)) {
            const std::size_t end = std::min(lines.size(), i + static_cast<std::size_t>(per_chunk));
            chunks.push_back(lines_value({lines.begin() + static_cast<std::ptrdiff_t>(i),
                                          lines.begin() + static_cast<std::ptrdiff_t>(end)}));
        }
        const std::int64_t count = static_cast<std::int64_t>(chunks.size());
        ListBuilder out(2);
        out.field("chunks", Value::list(std::move(chunks)));
        out.field("count", count);
        return out.build();
    });
    add("strings", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "strings");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::int64_t minimum = option_integer(options, "min-length", 4);
        const std::string text = resolve_text(vm, a[0], options, "strings");
        std::vector<Value> found;
        std::string run;
        std::size_t begin = 0;
        const auto flush = [&](std::size_t at) {
            if (static_cast<std::int64_t>(run.size()) >= minimum)
                found.push_back(Value::list({field("offset", static_cast<std::int64_t>(begin)),
                                             field("text", run)}));
            run.clear();
            begin = at + 1;
        };
        for (std::size_t i = 0; i < text.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c >= 32 && c < 127) { if (run.empty()) begin = i; run += static_cast<char>(c); }
            else flush(i);
        }
        flush(text.size());
        const std::int64_t count = static_cast<std::int64_t>(found.size());
        ListBuilder out(2);
        out.field("strings", Value::list(std::move(found)));
        out.field("count", count);
        return out.build();
    });
    // Batching only. Execution stays explicit: feed the batches to `process-start`.
    add("xargs", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "xargs");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::int64_t per_batch = option_integer(options, "max-arguments", 128);
        const std::int64_t max_bytes = option_integer(options, "max-bytes", 131072);
        if (per_batch < 1) fail("xargs requires a positive batch size");
        const std::vector<std::string> items = resolve_lines(vm, a[0], options, "xargs");
        std::vector<Value> batches;
        std::vector<std::string> current;
        std::int64_t bytes = 0;
        const auto flush = [&] {
            if (!current.empty()) { batches.push_back(lines_value(current)); current.clear(); }
            bytes = 0;
        };
        for (const std::string& item : items) {
            if (!current.empty() &&
                (static_cast<std::int64_t>(current.size()) >= per_batch ||
                 bytes + static_cast<std::int64_t>(item.size()) > max_bytes))
                flush();
            current.push_back(item);
            bytes += static_cast<std::int64_t>(item.size()) + 1;
        }
        flush();
        const std::int64_t count = static_cast<std::int64_t>(batches.size());
        ListBuilder out(2);
        out.field("batches", Value::list(std::move(batches)));
        out.field("count", count);
        return out.build();
    });
    add("cmp", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 2, 3, "cmp");
        const std::string left = resolve_text(vm, a[0], Value::nil(), "cmp");
        const std::string right = resolve_text(vm, a[1], Value::nil(), "cmp");
        const std::size_t shared = std::min(left.size(), right.size());
        for (std::size_t i = 0; i < shared; ++i) {
            if (left[i] == right[i]) continue;
            const std::int64_t line =
                1 + static_cast<std::int64_t>(std::count(left.begin(), left.begin() +
                                                         static_cast<std::ptrdiff_t>(i), '\n'));
            ListBuilder out(5);
            out.field("identical", false);
            out.field("offset", static_cast<std::int64_t>(i + 1));
            out.field("line", line);
            out.field("first-byte", static_cast<std::int64_t>(
                static_cast<unsigned char>(left[i])));
            out.field("second-byte", static_cast<std::int64_t>(
                static_cast<unsigned char>(right[i])));
            return out.build();
        }
        if (left.size() == right.size()) return ok_result({field("identical", true)});
        ListBuilder out(3);
        out.field("identical", false);
        out.field("offset", static_cast<std::int64_t>(shared + 1));
        out.symbol_field("reason", left.size() < right.size() ? "first-is-shorter"
                                                              : "second-is-shorter");
        return out.build();
    });
    // A typed line editor over the same source layer, covering what agents reach
    // for with `sed`: substitute, delete, insert, append, print over a line range.
    // Deliberately not a programmable text language -- `awk` and full `sed` scripts
    // stay delegated to a shell capability.
    add("sed", [](Interpreter& vm, const std::vector<Value>& a) -> Value {
        arity_between(a, 2, 3, "sed");
        const Value options = a.size() == 3 ? a[2] : Value::nil();
        const TextInput input = resolve_input(vm, a[0], options, "sed");

        // One operation record, or a list of them applied in order.
        std::vector<Value> steps;
        if (option(a[1], "operation").type() != Value::Type::Unspecified) steps.push_back(a[1]);
        else steps = want_list(a[1], "sed");

        std::vector<std::string> lines = input.lines;
        std::int64_t changed = 0;
        std::vector<std::string> printed;
        bool printing = false;

        for (const Value& step : steps) {
            const Value kind = option(step, "operation");
            if (kind.type() != Value::Type::Symbol)
                return error_result("each sed step needs an operation symbol", "invalid-argument",
                                    "sed");
            const std::string_view op = kind.as_symbol();
            const std::int64_t total = static_cast<std::int64_t>(lines.size());
            std::int64_t from = option_integer(step, "from", 1);
            std::int64_t to = option_integer(step, "to", total);
            if (from < 1) from = 1;
            if (to > total) to = total;

            if (op == "substitute") {
                const Value pattern = option(step, "pattern");
                const Value replacement = option(step, "replacement");
                if (pattern.type() != Value::Type::String ||
                    replacement.type() != Value::Type::String)
                    return error_result("substitute needs a pattern and a replacement",
                                        "invalid-argument", "sed");
                const bool literal = option_flag(step, "literal");
                const bool all = option_flag(step, "all", true);
                std::regex expression;
                if (!literal)
                    expression = compile_pattern(pattern.as_string(),
                                                 option_flag(step, "ignore-case"), "sed");
                for (std::int64_t i = from; i <= to; ++i) {
                    std::string& line = lines[static_cast<std::size_t>(i - 1)];
                    std::string updated;
                    if (literal) {
                        const std::string_view needle = pattern.as_string();
                        if (needle.empty()) continue;
                        std::size_t begin = 0;
                        bool hit = false;
                        for (;;) {
                            const std::size_t at = line.find(needle, begin);
                            if (at == std::string::npos) break;
                            updated.append(line, begin, at - begin);
                            updated.append(replacement.as_string());
                            begin = at + needle.size();
                            hit = true;
                            if (!all) break;
                        }
                        if (!hit) continue;
                        updated.append(line, begin, std::string::npos);
                    } else {
                        updated = std::regex_replace(line, expression,
                                                     std::string(replacement.as_string()),
                                                     all ? std::regex_constants::format_default
                                                         : std::regex_constants::format_first_only);
                        if (updated == line) continue;
                    }
                    line = updated;
                    ++changed;
                }
            } else if (op == "delete") {
                if (to >= from) {
                    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(from - 1),
                                lines.begin() + static_cast<std::ptrdiff_t>(to));
                    changed += to - from + 1;
                }
            } else if (op == "insert" || op == "append") {
                const Value text = option(step, "text");
                if (text.type() == Value::Type::Unspecified)
                    return error_result("insert and append need text", "invalid-argument", "sed");
                const std::vector<std::string> added = as_lines(text, "sed");
                std::int64_t at = option_integer(step, "line", op == "insert" ? 1 : total);
                if (at < 0) at = 0;
                if (at > total) at = total;
                const std::size_t where =
                    static_cast<std::size_t>(op == "insert" ? (at > 0 ? at - 1 : 0) : at);
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(where), added.begin(),
                             added.end());
                changed += static_cast<std::int64_t>(added.size());
            } else if (op == "print") {
                printing = true;
                for (std::int64_t i = from; i <= to; ++i)
                    printed.push_back(lines[static_cast<std::size_t>(i - 1)]);
            } else {
                return error_result("unknown sed operation: " + std::string(op),
                                    "invalid-argument", "sed");
            }
        }

        // `sed -n '…p'`: report only what was selected.
        if (printing) {
            ListBuilder out(3);
            out.field("lines", lines_value(printed));
            out.field("count", static_cast<std::int64_t>(printed.size()));
            out.field("changed", changed);
            return out.build();
        }

        ListBuilder out(4);
        out.field("lines", lines_value(lines));
        out.field("count", static_cast<std::int64_t>(lines.size()));
        out.field("changed", changed);

        // `sed -i`: write the result back, but only when the source named files.
        if (option_flag(options, "in-place")) {
            if (!input.from_files || input.paths.size() != 1)
                return error_result("in-place editing needs exactly one file source",
                                    "invalid-argument", "sed");
            std::string content;
            for (const std::string& line : lines) content += line + "\n";
            const Value written =
                dispatch_capability(vm, FileSystemCapability::kind, "write-file",
                                    {Value::string(input.paths[0]), Value::string(content),
                                     Value::nil()});
            if (option(written, "error").type() == Value::Type::String) return written;
            out.field("path", input.paths[0]);
            out.field("written", true);
        }
        return out.build();
    });

    register_path_names(interpreter);
}

// Pure path arithmetic: no capability is required to split a path string, so both the
// text and path groups provide these names.
void register_path_names(Interpreter& interpreter) {
    const auto add = [&interpreter](const char* name, NativeFunction function) {
        interpreter.define_native(name, std::move(function));
    };
    add("basename", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "basename");
        std::string path(want_string(a[0], "basename"));
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        const std::size_t slash = path.find_last_of('/');
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (name.empty()) name = "/";
        if (a.size() == 2) {
            const std::string_view suffix = want_string(a[1], "basename");
            if (name.size() > suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                name.resize(name.size() - suffix.size());
        }
        return ok_result({field("name", name)});
    });
    add("dirname", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 1, "dirname");
        std::string path(want_string(a[0], "dirname"));
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        const std::size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) return ok_result({field("path", ".")});
        if (slash == 0) return ok_result({field("path", "/")});
        return ok_result({field("path", path.substr(0, slash))});
    });
}

// ---------------------------------------------------------------------------
// Diff and patch
// ---------------------------------------------------------------------------

struct Edit {
    enum class Kind { Keep, Delete, Insert };
    Kind kind = Kind::Keep;
    std::size_t old_index = 0;
    std::size_t new_index = 0;
};

// Myers' greedy shortest-edit-script search. Each level stores only its own k-range,
// bounding the trace at O(d^2) entries, and `max_edits` caps the work so a pair of
// wholly unrelated files degrades to a replace instead of exhausting memory.
bool myers_script(const std::vector<std::string>& before, const std::vector<std::string>& after,
                  std::size_t max_edits, std::vector<Edit>& out) {
    const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(before.size());
    const std::ptrdiff_t m = static_cast<std::ptrdiff_t>(after.size());
    const std::ptrdiff_t bound =
        std::min<std::ptrdiff_t>(n + m, static_cast<std::ptrdiff_t>(max_edits));
    std::vector<std::vector<std::ptrdiff_t>> trace;
    std::vector<std::ptrdiff_t> front(static_cast<std::size_t>(2 * bound + 3), 0);
    const auto at = [bound](std::ptrdiff_t k) { return static_cast<std::size_t>(k + bound + 1); };

    for (std::ptrdiff_t d = 0; d <= bound; ++d) {
        std::vector<std::ptrdiff_t> level;
        level.reserve(static_cast<std::size_t>(d + 1));
        for (std::ptrdiff_t k = -d; k <= d; k += 2) {
            std::ptrdiff_t x;
            if (k == -d || (k != d && front[at(k - 1)] < front[at(k + 1)])) x = front[at(k + 1)];
            else x = front[at(k - 1)] + 1;
            std::ptrdiff_t y = x - k;
            while (x < n && y < m && before[static_cast<std::size_t>(x)] ==
                                         after[static_cast<std::size_t>(y)]) { ++x; ++y; }
            front[at(k)] = x;
            level.push_back(x);
            if (x >= n && y >= m) {
                trace.push_back(std::move(level));
                // Walk the trace backwards to recover the script.
                std::vector<Edit> reversed;
                std::ptrdiff_t px = n, py = m;
                for (std::ptrdiff_t step = d; step > 0; --step) {
                    const std::vector<std::ptrdiff_t>& previous = trace[static_cast<std::size_t>(step - 1)];
                    const std::ptrdiff_t pk = px - py;
                    const auto value = [&](std::ptrdiff_t key) -> std::ptrdiff_t {
                        const std::ptrdiff_t index = (key + (step - 1)) / 2;
                        if (index < 0 || index >= static_cast<std::ptrdiff_t>(previous.size()))
                            return -1;
                        return previous[static_cast<std::size_t>(index)];
                    };
                    const bool down = pk == -step ||
                                      (pk != step && value(pk - 1) < value(pk + 1));
                    const std::ptrdiff_t prev_k = down ? pk + 1 : pk - 1;
                    const std::ptrdiff_t prev_x = value(prev_k);
                    const std::ptrdiff_t prev_y = prev_x - prev_k;
                    while (px > prev_x && py > prev_y) {
                        --px; --py;
                        reversed.push_back({Edit::Kind::Keep, static_cast<std::size_t>(px),
                                            static_cast<std::size_t>(py)});
                    }
                    if (down) {
                        --py;
                        reversed.push_back({Edit::Kind::Insert, static_cast<std::size_t>(px),
                                            static_cast<std::size_t>(py)});
                    } else {
                        --px;
                        reversed.push_back({Edit::Kind::Delete, static_cast<std::size_t>(px),
                                            static_cast<std::size_t>(py)});
                    }
                }
                while (px > 0 && py > 0) {
                    --px; --py;
                    reversed.push_back({Edit::Kind::Keep, static_cast<std::size_t>(px),
                                        static_cast<std::size_t>(py)});
                }
                out.assign(reversed.rbegin(), reversed.rend());
                return true;
            }
        }
        trace.push_back(std::move(level));
    }
    return false;
}

std::vector<Edit> replace_script(const std::vector<std::string>& before,
                                 const std::vector<std::string>& after) {
    std::vector<Edit> out;
    out.reserve(before.size() + after.size());
    for (std::size_t i = 0; i < before.size(); ++i) out.push_back({Edit::Kind::Delete, i, 0});
    for (std::size_t i = 0; i < after.size(); ++i)
        out.push_back({Edit::Kind::Insert, before.size(), i});
    return out;
}

struct Hunk {
    std::size_t old_start = 0, old_count = 0, new_start = 0, new_count = 0;
    std::vector<Edit> edits;
};

std::vector<Hunk> group_hunks(const std::vector<Edit>& script, std::size_t context) {
    std::vector<Hunk> hunks;
    std::size_t index = 0;
    while (index < script.size()) {
        if (script[index].kind == Edit::Kind::Keep) { ++index; continue; }
        std::size_t begin = index;
        std::size_t leading = 0;
        while (begin > 0 && script[begin - 1].kind == Edit::Kind::Keep && leading < context) {
            --begin;
            ++leading;
        }
        std::size_t end = index;
        std::size_t run = 0;
        while (end < script.size()) {
            if (script[end].kind != Edit::Kind::Keep) { run = 0; ++end; continue; }
            if (run >= 2 * context) break;
            ++run;
            ++end;
        }
        if (run > context) end -= run - context;
        Hunk hunk;
        hunk.edits.assign(script.begin() + static_cast<std::ptrdiff_t>(begin),
                          script.begin() + static_cast<std::ptrdiff_t>(end));
        hunk.old_start = script[begin].old_index + 1;
        hunk.new_start = script[begin].new_index + 1;
        for (const Edit& edit : hunk.edits) {
            if (edit.kind != Edit::Kind::Insert) ++hunk.old_count;
            if (edit.kind != Edit::Kind::Delete) ++hunk.new_count;
        }
        if (hunk.old_count == 0) hunk.old_start = script[begin].old_index;
        if (hunk.new_count == 0) hunk.new_start = script[begin].new_index;
        hunks.push_back(std::move(hunk));
        index = end;
    }
    return hunks;
}

std::string unified_text(const std::vector<Hunk>& hunks, const std::vector<std::string>& before,
                         const std::vector<std::string>& after, const std::string& old_label,
                         const std::string& new_label) {
    if (hunks.empty()) return {};
    std::string out = "--- " + old_label + "\n+++ " + new_label + "\n";
    for (const Hunk& hunk : hunks) {
        out += "@@ -" + std::to_string(hunk.old_start) + "," + std::to_string(hunk.old_count) +
               " +" + std::to_string(hunk.new_start) + "," + std::to_string(hunk.new_count) + " @@\n";
        for (const Edit& edit : hunk.edits) {
            switch (edit.kind) {
            case Edit::Kind::Keep: out += " " + before[edit.old_index] + "\n"; break;
            case Edit::Kind::Delete: out += "-" + before[edit.old_index] + "\n"; break;
            case Edit::Kind::Insert: out += "+" + after[edit.new_index] + "\n"; break;
            }
        }
    }
    return out;
}

// One parsed file section of a unified diff.
struct PatchFile {
    std::string old_path, new_path;
    bool creates = false, deletes = false;
    struct Line { char kind = ' '; std::string text; };
    struct Section { std::size_t old_start = 1, new_start = 1; std::vector<Line> lines; };
    std::vector<Section> sections;
};

bool parse_unified_patch(const std::vector<std::string>& lines, std::vector<PatchFile>& out,
                         std::string& error) {
    PatchFile current;
    bool open = false;
    const auto strip_prefix = [](std::string path) {
        if (path.rfind("a/", 0) == 0 || path.rfind("b/", 0) == 0) return path.substr(2);
        return path;
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.rfind("--- ", 0) == 0) {
            if (open) out.push_back(current);
            current = PatchFile();
            open = true;
            std::string path = line.substr(4);
            const std::size_t tab = path.find('\t');
            if (tab != std::string::npos) path.resize(tab);
            current.deletes = false;
            current.creates = path == "/dev/null";
            current.old_path = current.creates ? std::string() : strip_prefix(path);
            continue;
        }
        if (line.rfind("+++ ", 0) == 0) {
            if (!open) { error = "patch has +++ without ---"; return false; }
            std::string path = line.substr(4);
            const std::size_t tab = path.find('\t');
            if (tab != std::string::npos) path.resize(tab);
            current.deletes = path == "/dev/null";
            current.new_path = current.deletes ? std::string() : strip_prefix(path);
            continue;
        }
        if (line.rfind("@@", 0) == 0) {
            if (!open) { error = "patch hunk appears before a file header"; return false; }
            std::size_t minus = line.find('-');
            std::size_t plus = line.find('+', minus == std::string::npos ? 0 : minus);
            if (minus == std::string::npos || plus == std::string::npos) {
                error = "malformed hunk header: " + line;
                return false;
            }
            PatchFile::Section section;
            section.old_start = static_cast<std::size_t>(std::strtoll(line.c_str() + minus + 1,
                                                                      nullptr, 10));
            section.new_start = static_cast<std::size_t>(std::strtoll(line.c_str() + plus + 1,
                                                                      nullptr, 10));
            if (section.old_start == 0) section.old_start = 1;
            if (section.new_start == 0) section.new_start = 1;
            current.sections.push_back(std::move(section));
            continue;
        }
        if (!open || current.sections.empty()) continue;
        if (line.rfind("\\ No newline", 0) == 0) continue;
        if (line.empty()) { current.sections.back().lines.push_back({' ', ""}); continue; }
        const char kind = line[0];
        if (kind != ' ' && kind != '+' && kind != '-') {
            // Ignore index/mode metadata lines between hunks.
            continue;
        }
        current.sections.back().lines.push_back({kind, line.substr(1)});
    }
    if (open) out.push_back(current);
    if (out.empty()) { error = "patch contains no file sections"; return false; }
    return true;
}

// Rejects absolute paths, traversal, and embedded NULs before any host call.
bool safe_relative_path(const std::string& path, std::string& reason) {
    if (path.empty()) { reason = "empty path"; return false; }
    if (path.find('\0') != std::string::npos) { reason = "path contains an embedded NUL"; return false; }
    if (path[0] == '/') { reason = "absolute paths are not allowed"; return false; }
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t slash = path.find('/', begin);
        const std::string part = path.substr(begin, slash == std::string::npos
                                                        ? std::string::npos : slash - begin);
        if (part == "..") { reason = "path escapes the patch root"; return false; }
        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    return true;
}

void register_diff(Interpreter& interpreter) {
    const auto add = [&interpreter](const char* name, NativeFunction function) {
        interpreter.define_native(name, std::move(function));
    };

    add("diff", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 2, 3, "diff");
        const Value options = a.size() == 3 ? a[2] : Value::nil();
        const std::vector<std::string> before = resolve_lines(vm, a[0], options, "diff");
        const std::vector<std::string> after = resolve_lines(vm, a[1], options, "diff");
        const std::size_t context =
            static_cast<std::size_t>(std::max<std::int64_t>(0, option_integer(options, "context", 3)));
        const std::size_t max_edits =
            static_cast<std::size_t>(std::max<std::int64_t>(1, option_integer(options, "max-edits", 4000)));
        std::vector<Edit> script;
        const bool exact = myers_script(before, after, max_edits, script);
        if (!exact) script = replace_script(before, after);
        const std::vector<Hunk> hunks = group_hunks(script, context);
        std::vector<Value> encoded;
        for (const Hunk& hunk : hunks) {
            std::vector<Value> body;
            for (const Edit& edit : hunk.edits) {
                switch (edit.kind) {
                case Edit::Kind::Keep:
                    body.push_back(field("context", Value::string(before[edit.old_index])));
                    break;
                case Edit::Kind::Delete:
                    body.push_back(field("delete", Value::string(before[edit.old_index])));
                    break;
                case Edit::Kind::Insert:
                    body.push_back(field("insert", Value::string(after[edit.new_index])));
                    break;
                }
            }
            ListBuilder record(5);
            record.field("old-start", static_cast<std::int64_t>(hunk.old_start));
            record.field("old-count", static_cast<std::int64_t>(hunk.old_count));
            record.field("new-start", static_cast<std::int64_t>(hunk.new_start));
            record.field("new-count", static_cast<std::int64_t>(hunk.new_count));
            record.field("lines", Value::list(std::move(body)));
            encoded.push_back(record.build());
        }
        const std::string old_label = option_text(options, "old-label", "a");
        const std::string new_label = option_text(options, "new-label", "b");
        ListBuilder out(5);
        out.field("identical", hunks.empty());
        out.field("hunks", Value::list(std::move(encoded)));
        out.field("unified", unified_text(hunks, before, after, old_label, new_label));
        out.field("exact", exact);
        return out.build();
    });

    add("diff3", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 3, 4, "diff3");
        const std::vector<std::string> base = as_lines(a[0], "diff3");
        const std::vector<std::string> mine = as_lines(a[1], "diff3");
        const std::vector<std::string> theirs = as_lines(a[2], "diff3");
        std::vector<Edit> left, right;
        if (!myers_script(base, mine, 4000, left)) left = replace_script(base, mine);
        if (!myers_script(base, theirs, 4000, right)) right = replace_script(base, theirs);
        // Project both edit scripts onto base line numbers, then walk them together.
        const auto project = [&](const std::vector<Edit>& script, const std::vector<std::string>& side) {
            std::vector<std::vector<std::string>> slots(base.size() + 1);
            std::vector<bool> kept(base.size(), false);
            std::size_t cursor = 0;
            for (const Edit& edit : script) {
                if (edit.kind == Edit::Kind::Keep) { kept[edit.old_index] = true; cursor = edit.old_index + 1; }
                else if (edit.kind == Edit::Kind::Delete) { cursor = edit.old_index + 1; }
                else slots[cursor].push_back(side[edit.new_index]);
            }
            return std::make_pair(slots, kept);
        };
        const auto mine_view = project(left, mine);
        const auto theirs_view = project(right, theirs);
        std::vector<std::string> merged;
        std::int64_t conflicts = 0;
        for (std::size_t i = 0; i <= base.size(); ++i) {
            const std::vector<std::string>& added_mine = mine_view.first[i];
            const std::vector<std::string>& added_theirs = theirs_view.first[i];
            if (added_mine == added_theirs) {
                merged.insert(merged.end(), added_mine.begin(), added_mine.end());
            } else if (added_mine.empty()) {
                merged.insert(merged.end(), added_theirs.begin(), added_theirs.end());
            } else if (added_theirs.empty()) {
                merged.insert(merged.end(), added_mine.begin(), added_mine.end());
            } else {
                ++conflicts;
                merged.push_back("<<<<<<< mine");
                merged.insert(merged.end(), added_mine.begin(), added_mine.end());
                merged.push_back("=======");
                merged.insert(merged.end(), added_theirs.begin(), added_theirs.end());
                merged.push_back(">>>>>>> theirs");
            }
            if (i == base.size()) break;
            const bool in_mine = mine_view.second[i];
            const bool in_theirs = theirs_view.second[i];
            if (in_mine && in_theirs) merged.push_back(base[i]);
            else if (in_mine != in_theirs) { /* one side deleted the line */ }
            else { /* both deleted */ }
        }
        ListBuilder out(3);
        out.field("merged", lines_value(merged));
        out.field("conflicts", conflicts);
        out.field("clean", conflicts == 0);
        return out.build();
    });

    // Atomic unified-diff application. Every hunk is verified against the current file
    // contents before any write happens, so a rejected hunk leaves the tree untouched.
    add("apply-patch", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 3, "apply-patch");
        std::size_t first = 0;
        std::shared_ptr<Capability> filesystem;
        if (!a.empty() && a[0].type() == Value::Type::Capability) {
            filesystem = capability_data(a[0]).capability;
            first = 1;
            if (filesystem->capability_kind() != FileSystemCapability::kind)
                return error_result("apply-patch expects a filesystem capability",
                                    "capability-kind", "apply-patch");
        } else {
            filesystem = vm.default_capability(FileSystemCapability::kind);
        }
        if (a.size() <= first) fail("apply-patch expects a patch");
        const Value options = a.size() > first + 1 ? a[first + 1] : Value::nil();
        const bool dry_run = option_flag(options, "dry-run");
        const bool allow_outside = option_flag(options, "allow-outside");
        if (!filesystem && !dry_run)
            return error_result("no filesystem capability is installed", "capability-missing",
                                "apply-patch");

        std::vector<PatchFile> files;
        std::string parse_error;
        if (!parse_unified_patch(as_lines(a[first], "apply-patch"), files, parse_error))
            return error_result(parse_error, "malformed-patch", "apply-patch");

        struct Plan { std::string path; std::string content; bool remove = false; bool create = false; };
        std::vector<Plan> plans;
        std::vector<Value> rejects;

        for (const PatchFile& file : files) {
            const std::string path = file.deletes ? file.old_path : file.new_path;
            std::string reason;
            if (!allow_outside && !safe_relative_path(path, reason)) {
                rejects.push_back(ok_result({field("path", path), symbol_field("code", "unsafe-path"),
                                             field("reason", reason)}));
                continue;
            }
            std::vector<std::string> original;
            if (!file.creates) {
                if (!filesystem) {
                    rejects.push_back(ok_result({field("path", path),
                                                 symbol_field("code", "capability-missing"),
                                                 field("reason", "cannot read the original file")}));
                    continue;
                }
                const Value read = filesystem->invoke(vm, "read-file",
                                                      {Value::string(path), Value::nil()});
                if (!read.is_list()) fail("read-file capability must return a proper list");
                const Value text = option(read, "text");
                if (text.type() != Value::Type::String) {
                    rejects.push_back(ok_result({field("path", path),
                                                 symbol_field("code", "unreadable"),
                                                 field("detail", read)}));
                    continue;
                }
                original = split_lines(text.as_string(), "\n");
            }
            // Apply sections in order against a working copy of the original lines.
            std::vector<std::string> working = original;
            std::ptrdiff_t drift = 0;
            bool rejected = false;
            for (const PatchFile::Section& section : file.sections) {
                std::ptrdiff_t cursor = static_cast<std::ptrdiff_t>(section.old_start) - 1 + drift;
                if (cursor < 0) cursor = 0;
                std::vector<std::string> replacement;
                std::size_t consumed = 0;
                bool matched = true;
                for (const PatchFile::Line& line : section.lines) {
                    if (line.kind == '+') { replacement.push_back(line.text); continue; }
                    const std::size_t index = static_cast<std::size_t>(cursor) + consumed;
                    if (index >= working.size() || working[index] != line.text) {
                        matched = false;
                        break;
                    }
                    ++consumed;
                    if (line.kind == ' ') replacement.push_back(line.text);
                }
                if (!matched) {
                    rejects.push_back(ok_result({field("path", path),
                                                 symbol_field("code", "context-mismatch"),
                                                 field("old-start",
                                                       static_cast<std::int64_t>(section.old_start))}));
                    rejected = true;
                    break;
                }
                working.erase(working.begin() + cursor,
                              working.begin() + cursor + static_cast<std::ptrdiff_t>(consumed));
                working.insert(working.begin() + cursor, replacement.begin(), replacement.end());
                drift += static_cast<std::ptrdiff_t>(replacement.size()) -
                         static_cast<std::ptrdiff_t>(consumed);
            }
            if (rejected) continue;
            Plan plan;
            plan.path = path;
            plan.remove = file.deletes;
            plan.create = file.creates;
            if (!plan.remove) {
                for (const std::string& line : working) plan.content += line + "\n";
            }
            plans.push_back(std::move(plan));
        }

        std::vector<Value> applied;
        const bool clean = rejects.empty();
        if (clean && !dry_run) {
            for (const Plan& plan : plans) {
                const Value response =
                    plan.remove
                        ? filesystem->invoke(vm, "remove", {Value::string(plan.path), Value::nil()})
                        : filesystem->invoke(vm, "write-file",
                                             {Value::string(plan.path), Value::string(plan.content),
                                              Value::nil()});
                if (!response.is_list())
                    fail("filesystem capability must return a proper list");
                if (option(response, "error").type() == Value::Type::String) {
                    rejects.push_back(ok_result({field("path", plan.path),
                                                 symbol_field("code", "write-failed"),
                                                 field("detail", response)}));
                    continue;
                }
                applied.push_back(ok_result({field("path", plan.path),
                                             symbol_field("operation",
                                                          plan.remove ? "delete"
                                                                      : plan.create ? "add" : "update"),
                                             field("bytes",
                                                   static_cast<std::int64_t>(plan.content.size()))}));
            }
        } else if (clean) {
            for (const Plan& plan : plans)
                applied.push_back(ok_result({field("path", plan.path),
                                             symbol_field("operation",
                                                          plan.remove ? "delete"
                                                                      : plan.create ? "add" : "update"),
                                             field("bytes",
                                                   static_cast<std::int64_t>(plan.content.size()))}));
        }
        ListBuilder out(5);
        out.field("applied", Value::list(std::move(applied)));
        out.field("rejects", Value::list(rejects));
        out.field("clean", rejects.empty());
        out.field("dry-run", dry_run);
        out.field("files", static_cast<std::int64_t>(files.size()));
        return out.build();
    });
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
//
// Objects become ordered association lists with string keys, arrays become proper
// lists, and `null` becomes the symbol `null`.

class JsonReader {
public:
    JsonReader(std::string_view text, std::size_t depth_limit)
        : text_(text), depth_limit_(depth_limit) {}

    bool parse(Value& out, std::string& error) {
        skip();
        if (!value(out, 0, error)) return false;
        skip();
        if (position_ != text_.size()) { error = "trailing JSON input"; return false; }
        return true;
    }

private:
    std::string_view text_;
    std::size_t position_ = 0;
    std::size_t depth_limit_;

    void skip() {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_])))
            ++position_;
    }

    bool literal(std::string_view word) {
        if (text_.compare(position_, word.size(), word) != 0) return false;
        position_ += word.size();
        return true;
    }

    bool string_value(std::string& out, std::string& error) {
        if (position_ >= text_.size() || text_[position_] != '"') {
            error = "expected a JSON string";
            return false;
        }
        ++position_;
        while (position_ < text_.size() && text_[position_] != '"') {
            char c = text_[position_++];
            if (c != '\\') { out += c; continue; }
            if (position_ >= text_.size()) { error = "unterminated JSON escape"; return false; }
            c = text_[position_++];
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '/': out += '/'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'u': {
                if (position_ + 4 > text_.size()) { error = "truncated \\u escape"; return false; }
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = text_[position_++];
                    code *= 16;
                    if (h >= '0' && h <= '9') code += static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') code += static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code += static_cast<unsigned>(h - 'A' + 10);
                    else { error = "invalid \\u escape"; return false; }
                }
                // Encode the code point as UTF-8; surrogate halves pass through as-is.
                if (code < 0x80) out += static_cast<char>(code);
                else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                break;
            }
            default: error = "unknown JSON escape"; return false;
            }
        }
        if (position_ >= text_.size()) { error = "unterminated JSON string"; return false; }
        ++position_;
        return true;
    }

    bool value(Value& out, std::size_t depth, std::string& error) {
        if (depth > depth_limit_) { error = "JSON nesting is too deep"; return false; }
        skip();
        if (position_ >= text_.size()) { error = "unexpected end of JSON input"; return false; }
        const char c = text_[position_];
        if (c == '{') {
            ++position_;
            std::vector<Value> entries;
            skip();
            if (position_ < text_.size() && text_[position_] == '}') { ++position_; }
            else {
                for (;;) {
                    skip();
                    std::string key;
                    if (!string_value(key, error)) return false;
                    skip();
                    if (position_ >= text_.size() || text_[position_] != ':') {
                        error = "expected ':' in JSON object";
                        return false;
                    }
                    ++position_;
                    Value member;
                    if (!value(member, depth + 1, error)) return false;
                    entries.push_back(Value::list({Value::string(key), std::move(member)}));
                    skip();
                    if (position_ < text_.size() && text_[position_] == ',') { ++position_; continue; }
                    if (position_ < text_.size() && text_[position_] == '}') { ++position_; break; }
                    error = "expected ',' or '}' in JSON object";
                    return false;
                }
            }
            out = Value::list(std::move(entries));
            return true;
        }
        if (c == '[') {
            ++position_;
            std::vector<Value> items;
            skip();
            if (position_ < text_.size() && text_[position_] == ']') { ++position_; }
            else {
                for (;;) {
                    Value item;
                    if (!value(item, depth + 1, error)) return false;
                    items.push_back(std::move(item));
                    skip();
                    if (position_ < text_.size() && text_[position_] == ',') { ++position_; continue; }
                    if (position_ < text_.size() && text_[position_] == ']') { ++position_; break; }
                    error = "expected ',' or ']' in JSON array";
                    return false;
                }
            }
            out = Value::list(std::move(items));
            return true;
        }
        if (c == '"') {
            std::string text;
            if (!string_value(text, error)) return false;
            out = Value::string(text);
            return true;
        }
        if (literal("true")) { out = Value::boolean(true); return true; }
        if (literal("false")) { out = Value::boolean(false); return true; }
        if (literal("null")) { out = Value::symbol("null"); return true; }
        const std::size_t begin = position_;
        if (position_ < text_.size() && text_[position_] == '+') {
            error = "JSON numbers may not carry a leading '+'";
            return false;
        }
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        bool fractional = false;
        while (position_ < text_.size()) {
            const char digit = text_[position_];
            if (std::isdigit(static_cast<unsigned char>(digit))) { ++position_; continue; }
            if (digit == '.' || digit == 'e' || digit == 'E' || digit == '+' || digit == '-') {
                fractional = true;
                ++position_;
                continue;
            }
            break;
        }
        if (begin == position_) { error = "unexpected JSON token"; return false; }
        const std::string token(text_.substr(begin, position_ - begin));
        if (!fractional) {
            std::int64_t integer = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), integer);
            if (parsed.ec == std::errc() && parsed.ptr == token.data() + token.size()) {
                out = Value::integer(integer);
                return true;
            }
        }
        const char* first = token.c_str();
        char* last = nullptr;
        const double number = std::strtod(first, &last);
        if (last != first + token.size() || !std::isfinite(number)) {
            error = "invalid JSON number: " + token;
            return false;
        }
        out = Value::real(number);
        return true;
    }
};

bool json_object_shape(const Value& value) {
    if (value.is_nil() || value.type() != Value::Type::Pair || !value.is_list()) return false;
    for (const Value& entry : value.to_vector()) {
        if (entry.type() != Value::Type::Pair || !entry.is_list()) return false;
        if (entry.list_size() != 2) return false;
        if (entry.car().type() != Value::Type::String) return false;
    }
    return true;
}

void json_escape(std::string_view text, std::string& out) {
    out += '"';
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                out += buffer;
            } else out += static_cast<char>(c);
        }
    }
    out += '"';
}

void json_write_value(const Value& value, std::string& out, bool pretty, std::size_t indent) {
    const std::string pad(pretty ? (indent + 1) * 2 : 0, ' ');
    const std::string closing(pretty ? indent * 2 : 0, ' ');
    switch (value.type()) {
    case Value::Type::Nil: out += "[]"; return;
    case Value::Type::Boolean: out += value.as_boolean() ? "true" : "false"; return;
    case Value::Type::Integer: out += std::to_string(value.as_integer()); return;
    case Value::Type::Float: {
        // JSON has no exactness marker, so a decimal point is what distinguishes a
        // float from an integer on the way back in.
        std::string text = format_double(value.as_float());
        if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
            text.find('E') == std::string::npos)
            text += ".0";
        out += text;
        return;
    }
    case Value::Type::String: json_escape(value.as_string(), out); return;
    case Value::Type::Character: json_escape(std::string(1, value.as_character()), out); return;
    case Value::Type::Symbol:
        if (value.as_symbol() == "null") { out += "null"; return; }
        json_escape(value.as_symbol(), out);
        return;
    case Value::Type::Pair: {
        const std::vector<Value> items = value.is_list() ? value.to_vector()
                                                         : std::vector<Value>{value.car(), value.cdr()};
        const bool object = json_object_shape(value);
        out += object ? '{' : '[';
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i) out += ',';
            if (pretty) { out += '\n'; out += pad; }
            if (object) {
                json_escape(items[i].car().as_string(), out);
                out += pretty ? ": " : ":";
                json_write_value(items[i].list_at(1), out, pretty, indent + 1);
            } else {
                json_write_value(items[i], out, pretty, indent + 1);
            }
        }
        if (pretty && !items.empty()) { out += '\n'; out += closing; }
        out += object ? '}' : ']';
        return;
    }
    default:
        json_escape(write_value(value), out);
        return;
    }
}

void register_json(Interpreter& interpreter) {
    interpreter.define_native("json-parse", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "json-parse");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::size_t depth =
            static_cast<std::size_t>(std::max<std::int64_t>(1, option_integer(options, "max-depth", 200)));
        Value parsed;
        std::string error;
        JsonReader reader(want_string(a[0], "json-parse"), depth);
        if (!reader.parse(parsed, error))
            return error_result(error, "malformed-json", "json-parse");
        return ok_result({field("value", std::move(parsed))});
    });
    interpreter.define_native("json-write", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "json-write");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        std::string out;
        json_write_value(a[0], out, option_flag(options, "pretty"), 0);
        return ok_result({field("text", out),
                          field("bytes", static_cast<std::int64_t>(out.size()))});
    });
}

// ---------------------------------------------------------------------------
// Encoding and checksums
// ---------------------------------------------------------------------------

const char* const kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(bytes[i]);
        const unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        const unsigned triple = (a << 16) | (b << 8) | c;
        out += kBase64Alphabet[(triple >> 18) & 0x3F];
        out += kBase64Alphabet[(triple >> 12) & 0x3F];
        out += i + 1 < bytes.size() ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=';
        out += i + 2 < bytes.size() ? kBase64Alphabet[triple & 0x3F] : '=';
    }
    return out;
}

bool base64_decode(std::string_view text, std::string& out) {
    // Unsigned: the accumulator is shifted left by six per symbol, which overflows
    // a signed int -- undefined behaviour -- once four symbols have been read.
    std::uint32_t accumulated = 0;
    int bits = 0;
    for (const char raw : text) {
        if (std::isspace(static_cast<unsigned char>(raw))) continue;
        if (raw == '=') break;
        const char* found = std::strchr(kBase64Alphabet, raw);
        if (!found || raw == 0) return false;
        accumulated = (accumulated << 6) | static_cast<std::uint32_t>(found - kBase64Alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((accumulated >> bits) & 0xFF);
        }
    }
    return true;
}

std::uint32_t crc32_of(std::string_view bytes) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
            table[i] = value;
        }
        ready = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char c : bytes)
        crc = table[(crc ^ static_cast<unsigned char>(c)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

Value byte_list(std::string_view bytes) {
    std::vector<Value> out;
    out.reserve(bytes.size());
    for (const char c : bytes)
        out.push_back(Value::integer(static_cast<unsigned char>(c)));
    return Value::list(std::move(out));
}

void register_encoding(Interpreter& interpreter) {
    const auto add = [&interpreter](const char* name, NativeFunction function) {
        interpreter.define_native(name, std::move(function));
    };
    add("base64", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "base64");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const Value mode = option(options, "mode", Value::symbol("encode"));
        const std::string input = as_text(a[0], "base64");
        if (mode.type() == Value::Type::Symbol && mode.as_symbol() == "decode") {
            std::string decoded;
            if (!base64_decode(input, decoded))
                return error_result("input is not valid base64", "malformed-input", "base64");
            return ok_result({field("text", decoded), field("bytes", byte_list(decoded)),
                              field("length", static_cast<std::int64_t>(decoded.size()))});
        }
        const std::string encoded = base64_encode(input);
        return ok_result({field("text", encoded),
                          field("length", static_cast<std::int64_t>(encoded.size()))});
    });
    add("cksum", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "cksum");
        const std::string input = as_text(a[0], "cksum");
        const std::uint32_t crc = crc32_of(input);
        char buffer[16];
        std::snprintf(buffer, sizeof buffer, "%08x", crc);
        return ok_result({symbol_field("algorithm", "crc32"),
                          field("checksum", static_cast<std::int64_t>(crc)),
                          field("hex", buffer),
                          field("bytes", static_cast<std::int64_t>(input.size()))});
    });
    add("sum", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "sum");
        const std::string input = as_text(a[0], "sum");
        std::uint32_t checksum = 0;
        for (const char c : input) {
            checksum = (checksum >> 1) | ((checksum & 1) << 15);
            checksum = (checksum + static_cast<unsigned char>(c)) & 0xFFFF;
        }
        return ok_result({symbol_field("algorithm", "bsd"),
                          field("checksum", static_cast<std::int64_t>(checksum)),
                          field("blocks", static_cast<std::int64_t>((input.size() + 1023) / 1024)),
                          field("bytes", static_cast<std::int64_t>(input.size()))});
    });
}

// ---------------------------------------------------------------------------
// Output, expressions, and predicates
// ---------------------------------------------------------------------------

std::string render_argument(Interpreter& interpreter, const Value& value) {
    if (value.type() == Value::Type::String) return std::string(value.as_string());
    if (value.type() == Value::Type::Symbol) return std::string(value.as_symbol());
    if (value.type() == Value::Type::Character) return std::string(1, value.as_character());
    return interpreter.write(value);
}

// A typed formatter: conversions read their argument from the Scheme argument list
// and no part of the format string is ever evaluated.
std::string format_text(Interpreter& interpreter, std::string_view format,
                        const std::vector<Value>& arguments) {
    std::string out;
    std::size_t next = 0;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') { out += format[i]; continue; }
        if (i + 1 >= format.size()) fail("printf format ends with '%'");
        if (format[i + 1] == '%') { out += '%'; ++i; continue; }
        std::string spec = "%";
        ++i;
        while (i < format.size() && std::strchr("-+ #0", format[i])) spec += format[i++];
        while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])))
            spec += format[i++];
        if (i < format.size() && format[i] == '.') {
            spec += format[i++];
            while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])))
                spec += format[i++];
        }
        if (i >= format.size()) fail("printf format ends inside a conversion");
        const char conversion = format[i];
        if (next >= arguments.size() && conversion != '%')
            fail("printf has more conversions than arguments");
        const Value& argument = arguments[next++];
        char buffer[512];
        switch (conversion) {
        case 'd': case 'i': {
            spec += "lld";
            std::snprintf(buffer, sizeof buffer, spec.c_str(),
                          static_cast<long long>(want_integer(argument, "printf")));
            out += buffer;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            spec += "ll";
            spec += conversion;
            std::snprintf(buffer, sizeof buffer, spec.c_str(),
                          static_cast<unsigned long long>(want_integer(argument, "printf")));
            out += buffer;
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': {
            spec += conversion;
            std::snprintf(buffer, sizeof buffer, spec.c_str(), real_of(argument));
            out += buffer;
            break;
        }
        case 'c': {
            spec += 'c';
            std::snprintf(buffer, sizeof buffer, spec.c_str(),
                          argument.type() == Value::Type::Character ? argument.as_character()
                                                                    : ' ');
            out += buffer;
            break;
        }
        case 's': {
            const std::string text = render_argument(interpreter, argument);
            spec += 's';
            if (spec == "%s") out += text;
            else {
                std::vector<char> wide(text.size() + 512);
                std::snprintf(wide.data(), wide.size(), spec.c_str(), text.c_str());
                out += wide.data();
            }
            break;
        }
        default: fail(std::string("unsupported printf conversion: %") + conversion);
        }
    }
    return out;
}

void register_output(Interpreter& interpreter) {
    const auto add = [&interpreter](const char* name, NativeFunction function) {
        interpreter.define_native(name, std::move(function));
    };

    add("echo", [](Interpreter& vm, const std::vector<Value>& a) {
        std::string out;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (i) out += ' ';
            out += render_argument(vm, a[i]);
        }
        return ok_result({field("text", out),
                          field("bytes", static_cast<std::int64_t>(out.size()))});
    });
    add("printf", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_least(a, 1, "printf");
        const std::vector<Value> rest(a.begin() + 1, a.end());
        const std::string out = format_text(vm, want_string(a[0], "printf"), rest);
        return ok_result({field("text", out),
                          field("bytes", static_cast<std::int64_t>(out.size()))});
    });
    add("yes", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 0, 2, "yes");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::string text = a.empty() ? "y" : render_argument(vm, a[0]);
        const std::int64_t count = option_integer(options, "count", 1);
        if (count < 0) fail("yes count must not be negative");
        // Bounded by an explicit count: there is no ambient output stream to flood.
        if (count > 1000000) fail("yes count exceeds the one million line limit");
        if (count * static_cast<std::int64_t>(text.size() + 1) > kMaxGeneratedBytes)
            fail("yes would generate more than " + std::to_string(kMaxGeneratedBytes) + " bytes");
        return ok_result({field("lines", lines_value(std::vector<std::string>(
                              static_cast<std::size_t>(count), text))),
                          field("count", count)});
    });

    add("expr", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_least(a, 1, "expr");
        const std::string op = symbol_name(a[0], "expr");
        const std::vector<Value> rest(a.begin() + 1, a.end());
        const auto binary_numbers = [&](const char* name) {
            if (rest.size() != 2 || !numeric(rest[0]) || !numeric(rest[1]))
                fail(std::string("expr ") + name + " expects two numbers");
        };
        if (op == "+" || op == "-" || op == "*") {
            binary_numbers(op.c_str());
            if (rest[0].type() == Value::Type::Integer && rest[1].type() == Value::Type::Integer) {
                const std::int64_t x = rest[0].as_integer(), y = rest[1].as_integer();
                const std::int64_t value = op == "+" ? checked_add(x, y, "expr")
                                          : op == "-" ? checked_sub(x, y, "expr")
                                                      : checked_mul(x, y, "expr");
                return ok_result({field("value", Value::integer(value))});
            }
            const double x = real_of(rest[0]), y = real_of(rest[1]);
            return ok_result({field("value", Value::real(op == "+" ? x + y
                                                        : op == "-" ? x - y : x * y))});
        }
        if (op == "/" || op == "%") {
            binary_numbers(op.c_str());
            const std::int64_t divisor = want_integer(rest[1], "expr");
            if (divisor == 0) return error_result("division by zero", "invalid-argument", "expr");
            const std::int64_t dividend = want_integer(rest[0], "expr");
            return ok_result({field("value", Value::integer(op == "/" ? dividend / divisor
                                                                      : dividend % divisor))});
        }
        if (op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (rest.size() != 2) fail("expr comparison expects two operands");
            int order;
            if (numeric(rest[0]) && numeric(rest[1])) order = compare_numbers(rest[0], rest[1]);
            else order = natural_order(rest[0], rest[1]);
            const bool value = op == "=" ? order == 0 : op == "!=" ? order != 0
                               : op == "<" ? order < 0 : op == "<=" ? order <= 0
                               : op == ">" ? order > 0 : order >= 0;
            return ok_result({field("value", Value::boolean(value))});
        }
        if (op == "and" || op == "or") {
            bool value = op == "and";
            for (const Value& operand : rest)
                value = op == "and" ? (value && operand.truthy()) : (value || operand.truthy());
            return ok_result({field("value", Value::boolean(value))});
        }
        if (op == "not") {
            if (rest.size() != 1) fail("expr not expects one operand");
            return ok_result({field("value", Value::boolean(!rest[0].truthy()))});
        }
        if (op == "length") {
            if (rest.size() != 1) fail("expr length expects one operand");
            return ok_result({field("value", Value::integer(static_cast<std::int64_t>(
                                 want_string(rest[0], "expr").size())))});
        }
        if (op == "index") {
            if (rest.size() != 2) fail("expr index expects two strings");
            const std::size_t at = want_string(rest[0], "expr").find(want_string(rest[1], "expr"));
            return ok_result({field("value", Value::integer(
                at == std::string_view::npos ? 0 : static_cast<std::int64_t>(at + 1)))});
        }
        if (op == "substr") {
            if (rest.size() != 3) fail("expr substr expects a string, a position, and a length");
            const std::string_view text = want_string(rest[0], "expr");
            const std::int64_t from = want_integer(rest[1], "expr");
            const std::int64_t length = want_integer(rest[2], "expr");
            if (from < 1 || length < 0 || static_cast<std::size_t>(from - 1) > text.size())
                return ok_result({field("value", Value::string(""))});
            return ok_result({field("value", Value::string(text.substr(
                static_cast<std::size_t>(from - 1), static_cast<std::size_t>(length))))});
        }
        if (op == "match") {
            if (rest.size() != 2) fail("expr match expects a string and a pattern");
            const std::string text(want_string(rest[0], "expr"));
            const std::regex expression = compile_pattern(want_string(rest[1], "expr"), false, "expr");
            std::smatch match;
            if (!std::regex_search(text, match, expression))
                return ok_result({field("value", Value::integer(0))});
            if (match.size() > 1)
                return ok_result({field("value", Value::string(match[1].str()))});
            return ok_result({field("value", Value::integer(static_cast<std::int64_t>(
                                 match.length(0))))});
        }
        (void)vm;
        return error_result("unknown expr operator: " + op, "invalid-argument", "expr");
    });

    // `test` and `[` share one typed predicate implementation. Filesystem predicates
    // require an explicit filesystem capability as the first argument.
    const auto predicate_test = [](const char* name) {
        return [name](Interpreter& vm, const std::vector<Value>& a) -> Value {
            std::vector<Value> args(a.begin(), a.end());
            // `[` accepts the conventional trailing bracket for symmetry with the shell.
            if (!args.empty() && args.back().type() == Value::Type::String &&
                args.back().as_string() == "]")
                args.pop_back();
            std::shared_ptr<Capability> filesystem;
            if (!args.empty() && args[0].type() == Value::Type::Capability) {
                filesystem = capability_data(args[0]).capability;
                args.erase(args.begin());
                if (filesystem->capability_kind() != FileSystemCapability::kind)
                    return error_result("expected a filesystem capability", "capability-kind", name);
            }
            if (args.empty()) fail(std::string(name) + " expects a predicate");
            const std::string predicate = symbol_name(args[0], name);
            const std::vector<Value> operands(args.begin() + 1, args.end());
            const auto answer = [](bool value) {
                return ok_result({field("result", Value::boolean(value))});
            };
            if (predicate == "string-empty")
                return answer(operands.size() == 1 && want_string(operands[0], name).empty());
            if (predicate == "string-nonempty")
                return answer(operands.size() == 1 && !want_string(operands[0], name).empty());
            if (predicate == "string-equal" || predicate == "string-not-equal") {
                if (operands.size() != 2) fail(std::string(name) + " expects two strings");
                const bool same = want_string(operands[0], name) == want_string(operands[1], name);
                return answer(predicate == "string-equal" ? same : !same);
            }
            static const std::map<std::string, int> numeric_tests = {
                {"integer-equal", 0}, {"integer-not-equal", 1}, {"integer-less", 2},
                {"integer-less-or-equal", 3}, {"integer-greater", 4},
                {"integer-greater-or-equal", 5}
            };
            const auto found = numeric_tests.find(predicate);
            if (found != numeric_tests.end()) {
                if (operands.size() != 2) fail(std::string(name) + " expects two integers");
                const std::int64_t x = want_integer(operands[0], name);
                const std::int64_t y = want_integer(operands[1], name);
                switch (found->second) {
                case 0: return answer(x == y);
                case 1: return answer(x != y);
                case 2: return answer(x < y);
                case 3: return answer(x <= y);
                case 4: return answer(x > y);
                default: return answer(x >= y);
                }
            }
            static const std::set<std::string> file_tests = {
                "file-exists", "regular-file", "directory", "readable", "writable",
                "executable", "symlink", "non-empty"
            };
            if (file_tests.count(predicate)) {
                if (operands.size() != 1) fail(std::string(name) + " expects one path");
                if (!filesystem) filesystem = vm.default_capability(FileSystemCapability::kind);
                if (!filesystem)
                    return error_result("no filesystem capability is installed",
                                        "capability-missing", name);
                const Value stat = filesystem->invoke(vm, "stat", {operands[0], Value::nil()});
                if (!stat.is_list()) fail("stat capability must return a proper list");
                if (option(stat, "error").type() == Value::Type::String)
                    return answer(false);
                if (predicate == "file-exists") return answer(true);
                const Value kind = option(stat, "kind");
                const Value size = option(stat, "size");
                if (predicate == "regular-file")
                    return answer(kind.type() == Value::Type::Symbol && kind.as_symbol() == "file");
                if (predicate == "directory")
                    return answer(kind.type() == Value::Type::Symbol && kind.as_symbol() == "directory");
                if (predicate == "symlink")
                    return answer(kind.type() == Value::Type::Symbol && kind.as_symbol() == "symlink");
                if (predicate == "non-empty")
                    return answer(size.type() == Value::Type::Integer && size.as_integer() > 0);
                const Value permissions = option(stat, predicate);
                return answer(permissions.truthy() &&
                              permissions.type() != Value::Type::Unspecified);
            }
            return error_result("unknown predicate: " + predicate, "invalid-argument", name);
        };
    };
    add("test", predicate_test("test"));
    add("[", predicate_test("["));
}

// ---------------------------------------------------------------------------
// Editor sessions
// ---------------------------------------------------------------------------
//
// A small typed session API covering the automated edits agents actually make.
// Exact `ed` compatibility stays delegated to the editor capability.

struct EdSession {
    std::string path;
    std::vector<std::string> lines;
    bool modified = false;
    bool created = false;
    std::shared_ptr<Capability> filesystem;
};

std::shared_ptr<EdSession> session_of(Interpreter& interpreter, const Value& handle) {
    const std::shared_ptr<void> token = interpreter.handle_token(handle, "editor-session");
    return std::static_pointer_cast<EdSession>(token);
}

std::pair<std::size_t, std::size_t> line_range(const Value& options, std::size_t count,
                                               const char* name) {
    std::int64_t from = option_integer(options, "from", 1);
    std::int64_t to = option_integer(options, "to", static_cast<std::int64_t>(count));
    if (from < 1) fail(std::string(name) + " line numbers are 1-based");
    if (to < from - 1) fail(std::string(name) + " range is inverted");
    if (static_cast<std::size_t>(to) > count) to = static_cast<std::int64_t>(count);
    return {static_cast<std::size_t>(from), static_cast<std::size_t>(to)};
}

void register_editor(Interpreter& interpreter) {
    const auto add = [&interpreter](const char* name, NativeFunction function) {
        interpreter.define_native(name, std::move(function));
    };

    add("ed-open", [](Interpreter& vm, const std::vector<Value>& a) -> Value {
        arity_between(a, 1, 3, "ed-open");
        std::size_t first = 0;
        std::shared_ptr<Capability> filesystem;
        if (!a.empty() && a[0].type() == Value::Type::Capability) {
            filesystem = capability_data(a[0]).capability;
            first = 1;
            if (filesystem->capability_kind() != FileSystemCapability::kind)
                return error_result("ed-open expects a filesystem capability", "capability-kind",
                                    "ed-open");
        } else {
            filesystem = vm.default_capability(FileSystemCapability::kind);
        }
        if (!filesystem)
            return error_result("no filesystem capability is installed", "capability-missing",
                                "ed-open");
        if (a.size() <= first) fail("ed-open expects a path");
        const std::string path(want_string(a[first], "ed-open"));
        const Value options = a.size() > first + 1 ? a[first + 1] : Value::nil();
        auto session = std::make_shared<EdSession>();
        session->path = path;
        session->filesystem = filesystem;
        const Value read = filesystem->invoke(vm, "read-file", {Value::string(path), Value::nil()});
        if (!read.is_list()) fail("read-file capability must return a proper list");
        const Value text = option(read, "text");
        if (text.type() == Value::Type::String) {
            session->lines = split_lines(text.as_string(), "\n");
        } else if (option_flag(options, "create")) {
            session->created = true;
        } else {
            return error_result("cannot open the file for editing", "unreadable", "ed-open",
                                {field("path", path), field("detail", read)});
        }
        const Value handle = vm.make_handle(*filesystem, "editor-session", session);
        return ok_result({field("session", handle), field("path", path),
                          field("lines", static_cast<std::int64_t>(session->lines.size())),
                          field("created", session->created)});
    });

    add("ed-command", [](Interpreter& vm, const std::vector<Value>& a) -> Value {
        arity(a, 2, "ed-command");
        const std::shared_ptr<EdSession> session = session_of(vm, a[0]);
        if (!session)
            return error_result("editor session is not live", "invalid-handle", "ed-command");
        const Value request = a[1];
        const Value operation = option(request, "operation");
        if (operation.type() != Value::Type::Symbol)
            return error_result("ed-command requires an operation symbol", "invalid-argument",
                                "ed-command");
        const std::string_view op = operation.as_symbol();
        const auto text_lines = [&] {
            const Value text = option(request, "text");
            if (text.type() == Value::Type::Unspecified) return std::vector<std::string>{};
            return as_lines(text, "ed-command");
        };
        if (op == "insert" || op == "append") {
            const std::int64_t line = option_integer(request, "line",
                                                     op == "insert" ? 1
                                                     : static_cast<std::int64_t>(session->lines.size()));
            if (line < 0 || static_cast<std::size_t>(line) > session->lines.size() + 1)
                return error_result("line is out of range", "invalid-argument", "ed-command");
            const std::vector<std::string> added = text_lines();
            const std::size_t at = op == "insert" ? static_cast<std::size_t>(line ? line - 1 : 0)
                                                  : static_cast<std::size_t>(line);
            session->lines.insert(session->lines.begin() +
                                      static_cast<std::ptrdiff_t>(std::min(at, session->lines.size())),
                                  added.begin(), added.end());
            session->modified = true;
            return ok_result({symbol_field("operation", op),
                              field("inserted", static_cast<std::int64_t>(added.size())),
                              field("lines", static_cast<std::int64_t>(session->lines.size()))});
        }
        if (op == "delete" || op == "replace") {
            const auto range = line_range(request, session->lines.size(), "ed-command");
            if (range.first > session->lines.size())
                return error_result("range is out of bounds", "invalid-argument", "ed-command");
            const std::size_t begin = range.first - 1;
            const std::size_t end = std::min(range.second, session->lines.size());
            const std::size_t removed = end >= begin ? end - begin : 0;
            session->lines.erase(session->lines.begin() + static_cast<std::ptrdiff_t>(begin),
                                 session->lines.begin() + static_cast<std::ptrdiff_t>(begin + removed));
            std::int64_t inserted = 0;
            if (op == "replace") {
                const std::vector<std::string> added = text_lines();
                session->lines.insert(session->lines.begin() + static_cast<std::ptrdiff_t>(begin),
                                      added.begin(), added.end());
                inserted = static_cast<std::int64_t>(added.size());
            }
            session->modified = true;
            return ok_result({symbol_field("operation", op),
                              field("deleted", static_cast<std::int64_t>(removed)),
                              field("inserted", inserted),
                              field("lines", static_cast<std::int64_t>(session->lines.size()))});
        }
        if (op == "substitute") {
            const auto range = line_range(request, session->lines.size(), "ed-command");
            const Value pattern = option(request, "pattern");
            const Value replacement = option(request, "replacement");
            if (pattern.type() != Value::Type::String || replacement.type() != Value::Type::String)
                return error_result("substitute needs a pattern and a replacement",
                                    "invalid-argument", "ed-command");
            const bool literal = option_flag(request, "literal");
            std::int64_t changed = 0;
            std::regex expression;
            if (!literal)
                expression = compile_pattern(pattern.as_string(), option_flag(request, "ignore-case"),
                                             "ed-command");
            for (std::size_t i = range.first; i <= range.second && i <= session->lines.size(); ++i) {
                std::string& line = session->lines[i - 1];
                std::string updated;
                if (literal) {
                    const std::string_view needle = pattern.as_string();
                    if (needle.empty()) continue;
                    std::size_t begin = 0;
                    bool hit = false;
                    for (;;) {
                        const std::size_t at = line.find(needle, begin);
                        if (at == std::string::npos) break;
                        updated.append(line, begin, at - begin);
                        updated.append(replacement.as_string());
                        begin = at + needle.size();
                        hit = true;
                        if (!option_flag(request, "all", true)) break;
                    }
                    if (!hit) continue;
                    updated.append(line, begin, std::string::npos);
                } else {
                    updated = std::regex_replace(line, expression,
                                                 std::string(replacement.as_string()),
                                                 option_flag(request, "all", true)
                                                     ? std::regex_constants::format_default
                                                     : std::regex_constants::format_first_only);
                    if (updated == line) continue;
                }
                line = updated;
                ++changed;
            }
            if (changed) session->modified = true;
            return ok_result({symbol_field("operation", "substitute"), field("changed", changed),
                              field("lines", static_cast<std::int64_t>(session->lines.size()))});
        }
        if (op == "search") {
            const Value pattern = option(request, "pattern");
            if (pattern.type() != Value::Type::String)
                return error_result("search needs a pattern", "invalid-argument", "ed-command");
            const Matcher matcher(pattern.as_string(), request, "ed-command");
            return grep_lines(session->lines, matcher, request, session->path);
        }
        if (op == "print") {
            const auto range = line_range(request, session->lines.size(), "ed-command");
            std::vector<std::string> picked;
            for (std::size_t i = range.first; i <= range.second && i <= session->lines.size(); ++i)
                picked.push_back(session->lines[i - 1]);
            return line_result(picked);
        }
        return error_result("unknown editor operation: " + std::string(op), "invalid-argument",
                            "ed-command");
    });

    add("ed-buffer", [](Interpreter& vm, const std::vector<Value>& a) -> Value {
        arity_between(a, 1, 2, "ed-buffer");
        const std::shared_ptr<EdSession> session = session_of(vm, a[0]);
        if (!session)
            return error_result("editor session is not live", "invalid-handle", "ed-buffer");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const auto range = line_range(options, session->lines.size(), "ed-buffer");
        std::vector<std::string> picked;
        for (std::size_t i = range.first; i <= range.second && i <= session->lines.size(); ++i)
            picked.push_back(session->lines[i - 1]);
        ListBuilder out(4);
        out.field("lines", lines_value(picked));
        out.field("count", static_cast<std::int64_t>(picked.size()));
        out.field("total", static_cast<std::int64_t>(session->lines.size()));
        out.field("modified", session->modified);
        return out.build();
    });

    add("ed-write", [](Interpreter& vm, const std::vector<Value>& a) -> Value {
        arity_between(a, 1, 2, "ed-write");
        const std::shared_ptr<EdSession> session = session_of(vm, a[0]);
        if (!session)
            return error_result("editor session is not live", "invalid-handle", "ed-write");
        const std::string path = a.size() == 2 ? std::string(want_string(a[1], "ed-write"))
                                               : session->path;
        std::string content;
        for (const std::string& line : session->lines) content += line + "\n";
        const Value response = session->filesystem->invoke(
            vm, "write-file", {Value::string(path), Value::string(content), Value::nil()});
        if (!response.is_list()) fail("write-file capability must return a proper list");
        if (option(response, "error").type() == Value::Type::String) return response;
        session->modified = false;
        return ok_result({field("path", path),
                          field("bytes", static_cast<std::int64_t>(content.size())),
                          field("lines", static_cast<std::int64_t>(session->lines.size()))});
    });

    add("ed-close", [](Interpreter& vm, const std::vector<Value>& a) -> Value {
        arity(a, 1, "ed-close");
        const std::shared_ptr<EdSession> session = session_of(vm, a[0]);
        if (!session)
            return error_result("editor session is not live", "invalid-handle", "ed-close");
        const bool modified = session->modified;
        vm.revoke_handle(a[0]);
        return ok_result({field("closed", true), field("unsaved-changes", modified)});
    });
}

// ---------------------------------------------------------------------------
// Capability-backed primitive groups
// ---------------------------------------------------------------------------

const char* host_platform() {
#if defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "posix";
#endif
}

// Platform-specific names keep their real semantics: on the wrong platform they
// report a structured unsupported result rather than silently aliasing.
NativeFunction platform_primitive(const char* kind, const char* operation, const char* platform) {
    return [kind, operation, platform](Interpreter& vm, const std::vector<Value>& arguments) -> Value {
        if (std::strcmp(platform, host_platform()) != 0)
            return unsupported_result(operation, std::string(operation) + " is specific to " +
                                                     platform + "; this host is " + host_platform());
        return dispatch_capability(vm, kind, operation, arguments);
    };
}

struct GroupEntry {
    PrimitiveGroup group;
    const char* kind;
    const char* operation;
    const char* platform; // nullptr when the name is portable
};

const std::vector<GroupEntry>& group_table() {
    static const std::vector<GroupEntry> table = {
        // Files and paths
        {PrimitiveGroup::Path, FileSystemCapability::kind, "cat", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "chmod", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "cp", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "dd", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "df", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "du", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "file", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "find", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "link", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "ln", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "ls", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "mkdir", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "mktemp", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "mv", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "pwd", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "cd", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "readlink", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "realpath", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "rm", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "rmdir", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "stat", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "sync", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "touch", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "truncate", nullptr},
        {PrimitiveGroup::Path, FileSystemCapability::kind, "unlink", nullptr},

        // Typed file and directory handles
        {PrimitiveGroup::File, FileSystemCapability::kind, "file-open", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "file-close", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "file-read", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "file-read-at", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "file-write", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "file-write-at", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "file-seek", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "file-stat", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "file-truncate", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "file-flush", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "directory-open", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "directory-read", nullptr},
        {PrimitiveGroup::File, FileSystemCapability::kind, "directory-close", nullptr},

        // Repository inspection
        {PrimitiveGroup::Repository, FileSystemCapability::kind, "glob", nullptr},
        {PrimitiveGroup::Repository, FileSystemCapability::kind, "tree", nullptr},
        {PrimitiveGroup::Repository, FileSystemCapability::kind, "read-file", nullptr},
        {PrimitiveGroup::Repository, FileSystemCapability::kind, "write-file", nullptr},
        {PrimitiveGroup::Repository, FileSystemCapability::kind, "search", nullptr},
        {PrimitiveGroup::Repository, FileSystemCapability::kind, "rg", nullptr},
        {PrimitiveGroup::Repository, FileSystemCapability::kind, "temp-file", nullptr},
        {PrimitiveGroup::Repository, FileSystemCapability::kind, "temp-directory", nullptr},

        // Processes and jobs
        {PrimitiveGroup::Process, ProcessCapability::kind, "process-start", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "process-poll", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "process-wait", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "process-expect", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "process-cancel", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "process-write", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "process-close-input", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "process-read-output", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "process-read-errors", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "job-poll", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "job-wait", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "job-cancel", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "job-input", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "job-close-input", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "job-output", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "job-error-output", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "job-status", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "ps", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "pgrep", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "pkill", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "kill", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "nice", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "timeout", nullptr},
        {PrimitiveGroup::Process, ProcessCapability::kind, "wait4path", nullptr},

        // Waiting for something to happen rather than guessing how long it takes
        {PrimitiveGroup::Watch, WatchCapability::kind, "wait-for", nullptr},

        // Analytical queries over the observation log
        {PrimitiveGroup::Sql, SqlCapability::kind, "sql-query", nullptr},

        // Clock
        {PrimitiveGroup::Clock, ClockCapability::kind, "date", nullptr},
        {PrimitiveGroup::Clock, ClockCapability::kind, "sleep", nullptr},
        {PrimitiveGroup::Clock, ClockCapability::kind, "time", nullptr},
        {PrimitiveGroup::Clock, ClockCapability::kind, "uptime", nullptr},

        // System identity and environment
        {PrimitiveGroup::System, SystemCapability::kind, "env", nullptr},
        {PrimitiveGroup::System, SystemCapability::kind, "hostname", nullptr},
        {PrimitiveGroup::System, SystemCapability::kind, "id", nullptr},
        {PrimitiveGroup::System, SystemCapability::kind, "uname", nullptr},
        {PrimitiveGroup::System, SystemCapability::kind, "users", nullptr},
        {PrimitiveGroup::System, SystemCapability::kind, "who", nullptr},
        {PrimitiveGroup::System, SystemCapability::kind, "whoami", nullptr},
        {PrimitiveGroup::System, SystemCapability::kind, "which", nullptr},
        {PrimitiveGroup::System, SystemCapability::kind, "whereis", nullptr},

        // Terminal
        {PrimitiveGroup::Terminal, TerminalCapability::kind, "stty", nullptr},
        {PrimitiveGroup::Terminal, TerminalCapability::kind, "tty", nullptr},

        // Digests
        {PrimitiveGroup::Crypto, CryptoCapability::kind, "hash", nullptr},
        {PrimitiveGroup::Crypto, CryptoCapability::kind, "md5", nullptr},
        {PrimitiveGroup::Crypto, CryptoCapability::kind, "shasum", nullptr},

        // Archives and compression
        {PrimitiveGroup::Archive, ArchiveCapability::kind, "tar", nullptr},
        {PrimitiveGroup::Archive, ArchiveCapability::kind, "pax", nullptr},
        {PrimitiveGroup::Archive, ArchiveCapability::kind, "cpio", nullptr},
        {PrimitiveGroup::Archive, ArchiveCapability::kind, "zip", nullptr},
        {PrimitiveGroup::Archive, ArchiveCapability::kind, "unzip", nullptr},
        {PrimitiveGroup::Compression, CompressionCapability::kind, "gzip", nullptr},
        {PrimitiveGroup::Compression, CompressionCapability::kind, "gunzip", nullptr},
        {PrimitiveGroup::Compression, CompressionCapability::kind, "bzip2", nullptr},
        {PrimitiveGroup::Compression, CompressionCapability::kind, "compress", nullptr},
        {PrimitiveGroup::Compression, CompressionCapability::kind, "uncompress", nullptr},

        // Shell dialects: the operation name carries the dialect identity.
        {PrimitiveGroup::Shell, ShellCapability::kind, "bash", nullptr},
        {PrimitiveGroup::Shell, ShellCapability::kind, "sh", nullptr},
        {PrimitiveGroup::Shell, ShellCapability::kind, "zsh", nullptr},
        {PrimitiveGroup::Shell, ShellCapability::kind, "csh", nullptr},
        {PrimitiveGroup::Shell, ShellCapability::kind, "tcsh", nullptr},
        {PrimitiveGroup::Shell, ShellCapability::kind, "ksh", nullptr},
        {PrimitiveGroup::Shell, ShellCapability::kind, "dash", nullptr},
        {PrimitiveGroup::Shell, ShellCapability::kind, "fish", nullptr},

        // Delegated editor entry point; the typed session API is implemented natively.
        {PrimitiveGroup::Editor, EditorCapability::kind, "ed", nullptr},

        // Services and platform operations
        {PrimitiveGroup::Service, ServiceCapability::kind, "launchctl", "darwin"},
        {PrimitiveGroup::Service, ServiceCapability::kind, "systemctl", "linux"},
        {PrimitiveGroup::Service, ServiceCapability::kind, "service-list", nullptr},
        {PrimitiveGroup::Desktop, DesktopCapability::kind, "open", nullptr},
        {PrimitiveGroup::Logging, LoggingCapability::kind, "logger", nullptr},

        // Network and remote access
        {PrimitiveGroup::Network, NetworkCapability::kind, "net-resolve", nullptr},
        {PrimitiveGroup::Network, NetworkCapability::kind, "nc", nullptr},
        {PrimitiveGroup::Network, NetworkCapability::kind, "ping", nullptr},
        {PrimitiveGroup::Http, HttpCapability::kind, "http-request", nullptr},
        {PrimitiveGroup::Http, HttpCapability::kind, "curl", nullptr},
        {PrimitiveGroup::RemoteShell, RemoteShellCapability::kind, "ssh", nullptr},
        {PrimitiveGroup::RemoteShell, RemoteShellCapability::kind, "scp", nullptr},
    };
    return table;
}

// ---------------------------------------------------------------------------
// Shell parsing
// ---------------------------------------------------------------------------
//
// Working out which programs an agent actually ran means parsing the command it
// sent, and that cannot be done with pattern matching. A regex sweep over a real
// transcript reports `e`, `if`, and `out.field` among the top "commands", because
// it reads heredoc bodies and quoted source as shell. So: a real tokenizer that
// tracks quoting, escapes, and heredoc bodies, and reports the pipeline structure.

struct ShellToken {
    std::string text;
    bool quoted = false;    // any part of it was inside quotes
};

struct ShellCommand {
    std::vector<std::pair<std::string, std::string>> assignments; // VAR=value prefixes
    std::string program;
    std::vector<std::string> arguments;
    std::vector<Value> redirections;
    std::string connector;  // how this command joins the next: | && || ; &
    bool header = false;    // a `for`/`case` word list, not a command anyone ran
    bool background = false; // ended with `&`, or inside a group that did
};

// Shell grammar words that occupy command position without being programs. The
// first set precedes a real command the way a VAR=value assignment does, so the
// program is the next word: `if grep -q x f` runs grep. The second set introduces
// a word list -- `for f in a b` names no program at all -- so the whole clause is
// discarded. Getting this wrong is what made a naive scan report `do` and `done`
// among the busiest tools on a real transcript corpus.
bool shell_keyword_precedes_command(const std::string& word) {
    static const std::set<std::string> words = {
        "if", "then", "else", "elif", "fi", "do", "done", "while", "until", "esac",
        "time", "!", "{", "}", "[[", "]]"};
    return words.count(word) != 0;
}

bool shell_keyword_opens_word_list(const std::string& word) {
    static const std::set<std::string> words = {"for", "case", "select"};
    return words.count(word) != 0;
}

// Reads a heredoc introducer and returns its terminator, or empty if this is not
// one. `<<-` strips leading tabs on the terminator line; quoting the delimiter
// suppresses expansion, which does not matter for structure.
std::string heredoc_terminator(const std::string& token, bool& strip_tabs) {
    if (token.rfind("<<", 0) != 0 || token.rfind("<<<", 0) == 0) return {};
    std::size_t at = 2;
    strip_tabs = at < token.size() && token[at] == '-';
    if (strip_tabs) ++at;
    std::string name = token.substr(at);
    if (name.size() >= 2 && ((name.front() == '\'' && name.back() == '\'') ||
                             (name.front() == '"' && name.back() == '"')))
        name = name.substr(1, name.size() - 2);
    return name;
}

bool shell_operator_at(const std::string& text, std::size_t at, std::string& found) {
    static const char* operators[] = {"&&", "||", ">>", "2>", "&>", "|", ";", "&", ">", "<"};
    for (const char* candidate : operators)
        if (text.compare(at, std::strlen(candidate), candidate) == 0) {
            found = candidate;
            return true;
        }
    return false;
}

// Splits one command line into commands, honouring quotes, escapes, comments and
// heredoc bodies. Substitutions -- $(...) and `...` -- are kept as single opaque
// tokens rather than recursed into.
std::vector<ShellCommand> shell_split(const std::string& source,
                                      std::vector<std::string>& heredoc_bodies) {
    std::vector<ShellCommand> commands;
    ShellCommand current;
    std::string token;
    bool have_token = false, token_quoted = false;
    std::vector<std::pair<std::string, bool>> pending_heredocs;
    std::string pending_redirection;
    std::vector<std::size_t> group_starts;
    std::size_t closed_group = static_cast<std::size_t>(-1);

    const auto flush_token = [&] {
        if (!have_token) return;
        if (!pending_redirection.empty()) {
            current.redirections.push_back(
                ok_result({symbol_field("operator", pending_redirection), field("target", token)}));
            pending_redirection.clear();
        } else if (current.header) {
            // Everything left in a `for`/`case` clause is part of its word list.
        } else if (current.program.empty()) {
            // A leading NAME=value is an assignment, not the program, and a leading
            // grammar word is neither.
            const std::size_t equals = token.find('=');
            if (!token_quoted && shell_keyword_precedes_command(token)) {
                // The program is the next word.
            } else if (!token_quoted && shell_keyword_opens_word_list(token)) {
                current.header = true;
            } else if (equals != std::string::npos && !token_quoted && equals > 0 &&
                       token.find_first_of("/ ") > equals) {
                current.assignments.emplace_back(token.substr(0, equals), token.substr(equals + 1));
            } else {
                current.program = token;
            }
        } else {
            current.arguments.push_back(token);
        }
        token.clear();
        have_token = false;
        token_quoted = false;
    };
    const auto flush_command = [&](const std::string& connector) {
        flush_token();
        if (!current.header && (!current.program.empty() || !current.assignments.empty())) {
            current.connector = connector;
            commands.push_back(current);
        }
        current = ShellCommand();
    };

    std::size_t i = 0;
    while (i < source.size()) {
        const char c = source[i];

        // A newline ends the command, and any heredocs it opened consume the
        // following lines up to their terminators.
        if (c == '\n') {
            flush_command(";");
            ++i;
            for (const auto& pending : pending_heredocs) {
                std::string body;
                for (;;) {
                    const std::size_t end = source.find('\n', i);
                    std::string line = source.substr(i, end == std::string::npos
                                                            ? std::string::npos : end - i);
                    std::string trimmed = line;
                    if (pending.second)
                        while (!trimmed.empty() && trimmed.front() == '\t')
                            trimmed.erase(trimmed.begin());
                    i = end == std::string::npos ? source.size() : end + 1;
                    if (trimmed == pending.first || end == std::string::npos) break;
                    body += line;
                    body += '\n';
                }
                heredoc_bodies.push_back(body);
            }
            pending_heredocs.clear();
            continue;
        }
        // A subshell is a boundary, not a word. Without this `(sleep 25; touch x) &`
        // parses its first command as `(sleep`, so nothing inside a grouping is
        // ever seen -- which hid a polling loop from the analysis that was looking
        // for exactly that.
        if (c == '(' || c == ')') {
            // The token has to be flushed first or the closing paren sticks to the
            // last word: `(b; c)` reported a program called `c)`.
            flush_token();
            flush_command(c == ')' ? ";" : "");
            if (c == '(') {
                group_starts.push_back(commands.size());
                closed_group = static_cast<std::size_t>(-1);
            } else if (!group_starts.empty()) {
                closed_group = group_starts.back();
                group_starts.pop_back();
            }
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) { flush_token(); ++i; continue; }
        if (c == '#' && !have_token) {  // comment to end of line
            while (i < source.size() && source[i] != '\n') ++i;
            continue;
        }
        if (c == '\\') {
            if (i + 1 < source.size()) {
                if (source[i + 1] == '\n') { i += 2; continue; }  // line continuation
                token += source[i + 1];
                have_token = true;
                i += 2;
                continue;
            }
            ++i;
            continue;
        }
        if (c == '\'' || c == '"') {
            const char quote = c;
            ++i;
            have_token = true;
            token_quoted = true;
            while (i < source.size() && source[i] != quote) {
                if (quote == '"' && source[i] == '\\' && i + 1 < source.size()) {
                    token += source[i + 1];
                    i += 2;
                    continue;
                }
                token += source[i++];
            }
            if (i < source.size()) ++i;
            continue;
        }
        // Substitutions stay opaque: their contents are a separate shell, and
        // splitting them here would attribute inner commands to the outer one.
        if (c == '$' && i + 1 < source.size() && source[i + 1] == '(') {
            int depth = 0;
            const std::size_t start = i;
            while (i < source.size()) {
                if (source[i] == '(') ++depth;
                else if (source[i] == ')' && --depth == 0) { ++i; break; }
                ++i;
            }
            token += source.substr(start, i - start);
            have_token = true;
            token_quoted = true;
            continue;
        }
        if (c == '`') {
            const std::size_t start = i++;
            while (i < source.size() && source[i] != '`') ++i;
            if (i < source.size()) ++i;
            token += source.substr(start, i - start);
            have_token = true;
            token_quoted = true;
            continue;
        }

        std::string found;
        if (!token_quoted && shell_operator_at(source, i, found)) {
            // `<<` introduces a heredoc; the delimiter is the next token.
            if (found == "<" && source.compare(i, 2, "<<") == 0) {
                std::size_t end = i + 2;
                if (end < source.size() && source[end] == '-') ++end;
                while (end < source.size() && std::isspace(static_cast<unsigned char>(source[end])) &&
                       source[end] != '\n')
                    ++end;
                std::size_t name_end = end;
                while (name_end < source.size() &&
                       !std::isspace(static_cast<unsigned char>(source[name_end])) &&
                       source[name_end] != ';' && source[name_end] != '|')
                    ++name_end;
                bool strip = false;
                const std::string introducer =
                    source.substr(i, 2) + (source[i + 2] == '-' ? "-" : "") +
                    source.substr(end, name_end - end);
                const std::string terminator = heredoc_terminator(introducer, strip);
                if (!terminator.empty()) pending_heredocs.emplace_back(terminator, strip);
                current.redirections.push_back(
                    ok_result({symbol_field("operator", "heredoc"), field("target", terminator)}));
                i = name_end;
                continue;
            }
            flush_token();
            if (found == "|" || found == "&&" || found == "||" || found == ";" || found == "&") {
                flush_command(found);
                // `&` backgrounds whatever preceded it: the command itself, or the
                // whole group when it closed just before. Without this a setup like
                // `(sleep 25; touch x) &` is indistinguishable from waiting 25
                // seconds, and every such line looks like the mistake it is not.
                if (found == "&") {
                    const std::size_t from =
                        closed_group == static_cast<std::size_t>(-1)
                            ? (commands.empty() ? 0 : commands.size() - 1)
                            : closed_group;
                    for (std::size_t n = from; n < commands.size(); ++n)
                        commands[n].background = true;
                }
                closed_group = static_cast<std::size_t>(-1);
            } else {
                pending_redirection = found;
            }
            i += found.size();
            continue;
        }
        token += c;
        have_token = true;
        ++i;
    }
    flush_command("");
    return commands;
}

void register_shell_parsing(Interpreter& interpreter) {
    interpreter.define_native("shell-parse", [](Interpreter&, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "shell-parse");
        const std::string source(want_string(a[0], "shell-parse"));
        std::vector<std::string> heredocs;
        const std::vector<ShellCommand> commands = shell_split(source, heredocs);

        std::vector<Value> records, programs;
        std::set<std::string> seen;
        for (const ShellCommand& command : commands) {
            std::vector<Value> arguments, flags, assignments;
            for (const std::string& argument : command.arguments) {
                arguments.push_back(Value::string(argument));
                if (argument.size() > 1 && argument[0] == '-') flags.push_back(Value::string(argument));
            }
            for (const auto& assignment : command.assignments)
                assignments.push_back(Value::list({Value::string(assignment.first),
                                                   Value::string(assignment.second)}));
            // The basename is what identifies the tool: ./a/b/grep is still grep.
            std::string base = command.program;
            const std::size_t slash = base.find_last_of('/');
            if (slash != std::string::npos) base = base.substr(slash + 1);
            if (!base.empty() && seen.insert(base).second) programs.push_back(Value::string(base));

            ListBuilder record(7);
            record.field("program", command.program);
            record.field("name", base);
            record.field("arguments", Value::list(std::move(arguments)));
            record.field("flags", Value::list(std::move(flags)));
            if (!assignments.empty()) record.field("assignments", Value::list(std::move(assignments)));
            if (!command.redirections.empty())
                record.field("redirections", Value::list(command.redirections));
            if (!command.connector.empty()) record.field("connector", command.connector);
            if (command.background) record.field("background", true);
            records.push_back(record.build());
        }
        std::vector<Value> bodies;
        for (const std::string& body : heredocs) bodies.push_back(Value::string(body));

        const std::int64_t count = static_cast<std::int64_t>(records.size());
        ListBuilder out(4);
        out.field("commands", Value::list(std::move(records)));
        out.field("count", count);
        out.field("programs", Value::list(std::move(programs)));
        out.field("heredocs", Value::list(std::move(bodies)));
        return out.build();
    });

    // Telemetry: which tools a session actually spent its budget on.
    interpreter.define_native("telemetry", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 0, 1, "telemetry");
        if (!a.empty()) {
            const std::string action = symbol_name(a[0], "telemetry");
            if (action == "start") { vm.set_telemetry(true); }
            else if (action == "stop") { vm.set_telemetry(false); }
            else if (action == "clear") { vm.clear_telemetry(); }
            else if (action != "summary")
                return error_result("unknown telemetry action: " + action, "invalid-argument",
                                    "telemetry");
        }
        return vm.telemetry_summary();
    });

    // A published tool is an ordinary Scheme procedure plus the metadata an agent
    // needs to discover and call it, and the provenance that says why it exists.
    interpreter.define_native("define-tool", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 1, "define-tool");
        const Value name = option(a[0], "name");
        const Value procedure = option(a[0], "procedure");
        if (name.type() != Value::Type::String)
            return error_result("a tool needs a name", "invalid-argument", "define-tool");
        if (procedure.type() != Value::Type::Procedure)
            return error_result("a tool needs a procedure", "invalid-argument", "define-tool");
        vm.publish_tool(name.as_string(), a[0]);
        return ok_result({field("name", name), field("published", true)});
    });
    interpreter.define_native("tool-manifest", [](Interpreter& vm, const std::vector<Value>& a) {
        arity(a, 0, "tool-manifest");
        return vm.tool_manifest();
    });
    interpreter.define_native("tool-invoke", [](Interpreter& vm, const std::vector<Value>& a) {
        arity_between(a, 1, 2, "tool-invoke");
        const Value definition = vm.tool_definition(want_string(a[0], "tool-invoke"));
        if (!definition.is_list() || definition.is_nil())
            return error_result("no such tool: " + std::string(want_string(a[0], "tool-invoke")),
                                "not-found", "tool-invoke");
        const Value procedure = option(definition, "procedure");
        if (procedure.type() != Value::Type::Procedure)
            return error_result("tool has no procedure", "invalid-argument", "tool-invoke");
        return vm.apply(procedure, {a.size() == 2 ? a[1] : Value::nil()});
    });

    // What the host can actually do, so a generated tool can branch on it rather
    // than guessing from the platform name.
    interpreter.define_native("platform-facts", [](Interpreter&, const std::vector<Value>& a) {
        arity(a, 0, "platform-facts");
        ListBuilder out(8);
        out.symbol_field("platform", host_platform());
        out.field("path-separator", "/");
        out.field("case-sensitive-paths",
#if defined(__APPLE__)
                  false
#else
                  true
#endif
        );
        out.field("pointer-bits", static_cast<std::int64_t>(sizeof(void*) * 8));
        bool wsl = false;
        std::string kernel;
#if defined(__linux__)
        // WSL identifies itself in the kernel release string; a tool that shells
        // out needs to know it is crossing a Windows boundary.
        if (std::FILE* version = std::fopen("/proc/version", "rb")) {
            char buffer[512];
            const std::size_t got = std::fread(buffer, 1, sizeof buffer - 1, version);
            std::fclose(version);
            buffer[got] = '\0';
            kernel = buffer;
            std::string lowered = kernel;
            for (char& c : lowered) c = static_cast<char>(std::tolower(
                static_cast<unsigned char>(c)));
            wsl = lowered.find("microsoft") != std::string::npos;
        }
#endif
        out.field("wsl", wsl);
        if (!kernel.empty()) {
            while (!kernel.empty() && (kernel.back() == '\n' || kernel.back() == '\r'))
                kernel.pop_back();
            out.field("kernel", kernel);
        }
        // What kind of model this host could run locally at all. Read from the
        // filesystem rather than by shelling out to nvidia-smi, so it stays
        // available to a tool holding no process capability -- which is the
        // situation every hook is in.
        //
        // This answers "what is feasible here", and nothing more. It is not a
        // way to choose a model: which one is actually better at a given
        // decision is a question for a corpus, not for the hardware.
        std::string accelerator = "none";
#if defined(__APPLE__)
        accelerator = "apple";
#elif defined(__linux__)
        // Every probe here is an fopen, so this file keeps to the standard
        // library and the rest of the tree keeps its one POSIX boundary. The
        // vendor is all that is wanted: the GPU's marketing name would need
        // directory enumeration and answers no question this asks.
        if (std::FILE* driver = std::fopen("/proc/driver/nvidia/version", "rb")) {
            std::fclose(driver);
            accelerator = "nvidia";
        } else {
            // No NVIDIA driver: read the PCI vendor of each render node instead.
            for (int card = 0; card < 8 && accelerator == "none"; ++card) {
                const std::string path =
                    "/sys/class/drm/card" + std::to_string(card) + "/device/vendor";
                if (std::FILE* handle = std::fopen(path.c_str(), "rb")) {
                    char buffer[32] = {};
                    const std::size_t got =
                        std::fread(buffer, 1, sizeof buffer - 1, handle);
                    std::fclose(handle);
                    buffer[got] = '\0';
                    const std::string vendor(buffer);
                    if (vendor.find("0x10de") != std::string::npos) accelerator = "nvidia";
                    else if (vendor.find("0x1002") != std::string::npos) accelerator = "amd";
                    else if (vendor.find("0x8086") != std::string::npos) accelerator = "intel";
                }
            }
        }
#endif
        out.symbol_field("accelerator", accelerator);
        return out.build();
    });
}

// ---------------------------------------------------------------------------
// Version control
// ---------------------------------------------------------------------------
//
// `git` is a typed front end over the generic process API, not a reimplementation.
// It runs git with machine-readable flags and parses the result into records, so
// callers never scrape porcelain text. Any ProcessCapability backs it.

struct ProgramResult {
    std::string out;
    std::string err;
    std::int64_t status = -1;
    Value failure;              // set when the process could not be run at all
    bool ran = false;
};

ProgramResult run_program(Interpreter& vm, const std::string& program,
                          const std::vector<std::string>& args, const Value& options) {
    ProgramResult result;
    std::vector<Value> argv;
    argv.reserve(args.size());
    for (const std::string& argument : args) argv.push_back(Value::string(argument));

    ListBuilder request(5);
    request.field("program", program);
    request.field("arguments", Value::list(std::move(argv)));
    const Value directory = option(options, "directory");
    if (directory.type() == Value::Type::String) request.field("directory", directory);
    const Value timeout = option(options, "timeout-ms");
    if (timeout.type() == Value::Type::Integer) request.field("timeout-ms", timeout);

    const Value started = dispatch_capability(vm, ProcessCapability::kind, "process-start",
                                              {request.build()});
    if (option(started, "error").type() == Value::Type::String) {
        result.failure = started;
        return result;
    }
    const Value job = option(started, "job");
    if (job.type() != Value::Type::Handle) {
        result.failure = error_result("process capability returned no job handle", "host-error",
                                      program);
        return result;
    }
    const Value finished = dispatch_capability(vm, ProcessCapability::kind, "process-wait", {job});
    if (option(finished, "error").type() == Value::Type::String) {
        result.failure = finished;
        return result;
    }
    const Value out = option(finished, "stdout");
    const Value err = option(finished, "stderr");
    const Value status = option(finished, "exit-status");
    if (out.type() == Value::Type::String) result.out = std::string(out.as_string());
    if (err.type() == Value::Type::String) result.err = std::string(err.as_string());
    if (status.type() == Value::Type::Integer) result.status = status.as_integer();
    result.ran = true;
    return result;
}

// Splits on a separator that cannot appear in the fields git emits.
std::vector<std::string> split_on(const std::string& text, char separator) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == separator) { out.push_back(current); current.clear(); }
        else current += c;
    }
    out.push_back(current);
    return out;
}

// `git status --porcelain=v1` status letters, as their own fields rather than a
// two-character code the caller has to decode.
const char* porcelain_state(char code) {
    switch (code) {
    case ' ': return "unchanged";
    case 'M': return "modified";
    case 'A': return "added";
    case 'D': return "deleted";
    case 'R': return "renamed";
    case 'C': return "copied";
    case 'U': return "unmerged";
    case '?': return "untracked";
    case '!': return "ignored";
    default: return "unknown";
    }
}

void register_vcs(Interpreter& interpreter) {
    interpreter.define_native("git", [](Interpreter& vm, const std::vector<Value>& a) -> Value {
        arity_between(a, 1, 2, "git");
        const std::string operation = symbol_name(a[0], "git");
        const Value options = a.size() == 2 ? a[1] : Value::nil();
        const std::int64_t limit = option_integer(options, "limit", 20);
        const std::vector<std::string> paths = [&] {
            std::vector<std::string> out;
            const Value value = option(options, "paths");
            if (value.type() == Value::Type::String) out.emplace_back(value.as_string());
            else if (value.is_list())
                for (const Value& element : value.to_vector())
                    if (element.type() == Value::Type::String) out.emplace_back(element.as_string());
            return out;
        }();
        const std::string revision = option_text(options, "revision", "HEAD");

        std::vector<std::string> args;
        if (operation == "status") {
            args = {"status", "--porcelain=v1", "--branch"};
        } else if (operation == "log") {
            args = {"log", "--no-color", "-n", std::to_string(limit),
                    "--format=%H\x1f%h\x1f%an\x1f%ae\x1f%at\x1f%s"};
            if (option(options, "revision").type() == Value::Type::String) args.push_back(revision);
        } else if (operation == "diff") {
            args = {"diff", "--no-color"};
            if (option_flag(options, "staged")) args.push_back("--staged");
            if (option_flag(options, "stat")) args.push_back("--numstat");
            if (!paths.empty()) args.push_back("--");
            for (const std::string& path : paths) args.push_back(path);
        } else if (operation == "show") {
            args = {"show", "--no-color", "--format=%H\x1f%h\x1f%an\x1f%ae\x1f%at\x1f%s",
                    revision};
        } else if (operation == "branch") {
            args = {"branch", "--format=%(refname:short)\x1f%(HEAD)"};
        } else if (operation == "add") {
            if (paths.empty())
                return error_result("add needs paths", "invalid-argument", "git");
            args = {"add", "--"};
            for (const std::string& path : paths) args.push_back(path);
        } else if (operation == "commit") {
            const Value message = option(options, "message");
            if (message.type() != Value::Type::String)
                return error_result("commit needs a message", "invalid-argument", "git");
            args = {"commit", "-m", std::string(message.as_string())};
            if (option_flag(options, "all")) args.insert(args.begin() + 1, "--all");
        } else if (operation == "rev-parse") {
            args = {"rev-parse", revision};
        } else {
            return error_result("unsupported git operation: " + operation, "invalid-argument",
                                "git", {field("supported",
                                              Value::list({Value::symbol("status"),
                                                           Value::symbol("log"),
                                                           Value::symbol("diff"),
                                                           Value::symbol("show"),
                                                           Value::symbol("branch"),
                                                           Value::symbol("add"),
                                                           Value::symbol("commit"),
                                                           Value::symbol("rev-parse")}))});
        }

        const ProgramResult run = run_program(vm, "git", args, options);
        if (!run.ran) return run.failure;
        if (run.status != 0)
            return error_result(run.err.empty() ? "git exited non-zero" : run.err, "host-error",
                                "git", {field("exit-status", run.status),
                                        symbol_field("git-operation", operation)});

        if (operation == "status") {
            std::string branch;
            std::vector<Value> entries;
            for (const std::string& line : split_lines(run.out, "\n")) {
                if (line.rfind("## ", 0) == 0) {
                    branch = line.substr(3);
                    const std::size_t cut = branch.find("...");
                    if (cut != std::string::npos) branch.resize(cut);
                    continue;
                }
                if (line.size() < 3) continue;
                entries.push_back(ok_result(
                    {field("path", line.substr(3)),
                     symbol_field("index", porcelain_state(line[0])),
                     symbol_field("worktree", porcelain_state(line[1]))}));
            }
            const bool clean = entries.empty();
            const std::int64_t count = static_cast<std::int64_t>(entries.size());
            return ok_result({field("branch", branch), field("files", Value::list(std::move(entries))),
                              field("count", count), field("clean", clean)});
        }
        if (operation == "log" || operation == "show") {
            std::vector<Value> commits;
            for (const std::string& line : split_lines(run.out, "\n")) {
                if (line.find('\x1f') == std::string::npos) continue;
                const std::vector<std::string> parts = split_on(line, '\x1f');
                if (parts.size() < 6) continue;
                commits.push_back(ok_result(
                    {field("hash", parts[0]), field("short", parts[1]), field("author", parts[2]),
                     field("email", parts[3]),
                     field("timestamp", static_cast<std::int64_t>(std::strtoll(parts[4].c_str(),
                                                                              nullptr, 10))),
                     field("subject", parts[5])}));
            }
            const std::int64_t count = static_cast<std::int64_t>(commits.size());
            ListBuilder out(3);
            out.field("commits", Value::list(std::move(commits)));
            out.field("count", count);
            if (operation == "show") out.field("diff", run.out);
            return out.build();
        }
        if (operation == "branch") {
            std::vector<Value> branches;
            std::string current;
            for (const std::string& line : split_lines(run.out, "\n")) {
                const std::vector<std::string> parts = split_on(line, '\x1f');
                if (parts.empty() || parts[0].empty()) continue;
                const bool head = parts.size() > 1 && parts[1] == "*";
                if (head) current = parts[0];
                branches.push_back(ok_result({field("name", parts[0]), field("head", head)}));
            }
            const std::int64_t count = static_cast<std::int64_t>(branches.size());
            return ok_result({field("branches", Value::list(std::move(branches))),
                              field("current", current), field("count", count)});
        }
        if (operation == "diff") {
            const std::vector<std::string> lines = split_lines(run.out, "\n");
            ListBuilder out(3);
            out.field("unified", run.out);
            out.field("lines", static_cast<std::int64_t>(lines.size()));
            out.field("empty", run.out.empty());
            return out.build();
        }
        std::string trimmed = run.out;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r'))
            trimmed.pop_back();
        return ok_result({symbol_field("operation", operation), field("output", trimmed)});
    });
}

void register_group(Interpreter& interpreter, PrimitiveGroup group) {
    switch (group) {
    case PrimitiveGroup::Core: register_core(interpreter); register_shell_parsing(interpreter); return;
    case PrimitiveGroup::Text: register_text(interpreter); return;
    case PrimitiveGroup::Diff: register_diff(interpreter); return;
    case PrimitiveGroup::Json: register_json(interpreter); return;
    case PrimitiveGroup::Encoding: register_encoding(interpreter); return;
    case PrimitiveGroup::Output: register_output(interpreter); return;
    case PrimitiveGroup::Editor: register_editor(interpreter); break;
    case PrimitiveGroup::Vcs: register_vcs(interpreter); return;
    default: break;
    }
    if (group == PrimitiveGroup::Path) register_path_names(interpreter);
    for (const GroupEntry& entry : group_table()) {
        if (entry.group != group) continue;
        // Only register names the installed capability actually supports; with no
        // capability installed the name is still registered and reports the denial.
        const std::shared_ptr<Capability> capability = interpreter.default_capability(entry.kind);
        if (capability && !capability->supports(entry.operation)) continue;
        interpreter.define_native(entry.operation,
                                  entry.platform
                                      ? platform_primitive(entry.kind, entry.operation, entry.platform)
                                      : capability_primitive(entry.kind, entry.operation));
    }
}

const std::vector<PrimitiveGroup>& groups_for_kind(std::string_view kind) {
    static std::map<std::string, std::vector<PrimitiveGroup>> table = [] {
        std::map<std::string, std::vector<PrimitiveGroup>> built;
        for (const GroupEntry& entry : group_table()) {
            std::vector<PrimitiveGroup>& groups = built[entry.kind];
            if (std::find(groups.begin(), groups.end(), entry.group) == groups.end())
                groups.push_back(entry.group);
        }
        // The typed editor sessions are driven by a filesystem capability.
        built[FileSystemCapability::kind].push_back(PrimitiveGroup::Editor);
        built[FileSystemCapability::kind].push_back(PrimitiveGroup::Diff);
        // `git` is a typed front end over the generic process API.
        built[ProcessCapability::kind].push_back(PrimitiveGroup::Vcs);
        return built;
    }();
    static const std::vector<PrimitiveGroup> none;
    const auto found = table.find(std::string(kind));
    return found == table.end() ? none : found->second;
}

} // namespace

} // namespace toolscheme
