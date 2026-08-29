#include "toolscheme.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <variant>

namespace toolscheme {
namespace {

struct Environment;

struct NativeString {
    std::vector<unsigned char> storage;
};

struct List {
    std::shared_ptr<std::vector<Value>> values;
    std::size_t offset = 0;
    Value tail;
};

struct Procedure {
    NativeFunction native;
    std::vector<std::string> parameters;
    std::string rest;
    std::vector<Value> body;
    std::shared_ptr<Environment> environment;
};

struct CapabilityValue {
    std::shared_ptr<Capability> capability;
};

using Payload = std::variant<std::monostate, bool, std::int64_t, double, char,
                             NativeString, std::string, List, Procedure, CapabilityValue>;

[[noreturn]] void fail(const std::string& message) { throw Error(message); }

} // namespace

struct ValueData {
    Value::Type type = Value::Type::Nil;
    Payload payload;
};

struct ValueAccess {
    static const ValueData& data(const Value& value) { return *value.data_; }
    static Value make(Value::Type type, Payload payload = {}) {
        auto data = std::make_shared<ValueData>();
        data->type = type;
        data->payload = std::move(payload);
        return Value(std::move(data));
    }
};

namespace {

const ValueData& checked(const Value& value, Value::Type type, const char* expected) {
    if (value.type() != type) fail(std::string("expected ") + expected);
    return ValueAccess::data(value);
}

struct Environment {
    std::unordered_map<std::string, Value> values;
    std::shared_ptr<Environment> parent;

    Value get(const std::string& name) const {
        for (const Environment* environment = this; environment; environment = environment->parent.get()) {
            auto found = environment->values.find(name);
            if (found != environment->values.end()) return found->second;
        }
        fail("unbound symbol: " + name);
    }

    void set(const std::string& name, Value value) {
        for (Environment* environment = this; environment; environment = environment->parent.get()) {
            auto found = environment->values.find(name);
            if (found != environment->values.end()) { found->second = std::move(value); return; }
        }
        fail("cannot set unbound symbol: " + name);
    }
};

const List& as_list_data(const Value& value) {
    return std::get<List>(checked(value, Value::Type::Pair, "pair").payload);
}

std::vector<Value> unpack(Value value, const char* context) {
    std::vector<Value> result;
    while (!value.is_nil()) {
        if (value.type() != Value::Type::Pair) fail(std::string(context) + " requires a proper list");
        const List& list = as_list_data(value);
        result.insert(result.end(), list.values->begin() + static_cast<std::ptrdiff_t>(list.offset), list.values->end());
        value = list.tail;
    }
    return result;
}

std::string symbol(const Value& value, const char* context) {
    if (value.type() != Value::Type::Symbol) fail(std::string(context) + " requires a symbol");
    return std::string(value.as_symbol());
}

void arity(const std::vector<Value>& args, std::size_t count, const char* name) {
    if (args.size() != count) fail(std::string(name) + " expects " + std::to_string(count) + " arguments");
}

bool numeric(const Value& value) {
    return value.type() == Value::Type::Integer || value.type() == Value::Type::Float;
}

double real(const Value& value) {
    if (value.type() == Value::Type::Integer) return static_cast<double>(value.as_integer());
    if (value.type() == Value::Type::Float) return value.as_float();
    fail("expected number");
}

std::int64_t checked_add(std::int64_t a, std::int64_t b, const char* op) {
    std::int64_t out;
    if (__builtin_add_overflow(a, b, &out)) fail(std::string("integer overflow in ") + op);
    return out;
}
std::int64_t checked_sub(std::int64_t a, std::int64_t b, const char* op) {
    std::int64_t out;
    if (__builtin_sub_overflow(a, b, &out)) fail(std::string("integer overflow in ") + op);
    return out;
}
std::int64_t checked_mul(std::int64_t a, std::int64_t b, const char* op) {
    std::int64_t out;
    if (__builtin_mul_overflow(a, b, &out)) fail(std::string("integer overflow in ") + op);
    return out;
}

class Reader {
public:
    explicit Reader(std::string_view source) : source_(source) {}
    bool done() { space(); return position_ == source_.size(); }
    Value datum() {
        space();
        if (position_ == source_.size()) fail("unexpected end of input");
        char c = source_[position_];
        if (c == '(') return list();
        if (c == ')') fail("unexpected ')'");
        if (c == '\'' || c == '`' || c == ',') {
            ++position_;
            std::string name = c == '\'' ? "quote" : c == '`' ? "quasiquote" : "unquote";
            if (c == ',' && position_ < source_.size() && source_[position_] == '@') { ++position_; name = "unquote-splicing"; }
            auto values = std::make_shared<std::vector<Value>>();
            values->push_back(Value::symbol(name)); values->push_back(datum());
            return ValueAccess::make(Value::Type::Pair, List{values, 0, Value::nil()});
        }
        if (c == '"') return string_value();
        return atom();
    }
private:
    std::string_view source_; std::size_t position_ = 0;
    static bool delimiter(char c) { return std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == ';'; }
    void space() {
        for (;;) {
            while (position_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[position_]))) ++position_;
            if (position_ == source_.size() || source_[position_] != ';') return;
            while (position_ < source_.size() && source_[position_] != '\n') ++position_;
        }
    }
    Value list() {
        ++position_; space();
        std::vector<Value> values; Value tail = Value::nil();
        while (position_ < source_.size() && source_[position_] != ')') {
            if (source_[position_] == '.' && position_ + 1 < source_.size() && delimiter(source_[position_ + 1])) {
                if (values.empty()) fail("invalid dotted list");
                ++position_; tail = datum(); space(); break;
            }
            values.push_back(datum()); space();
        }
        if (position_ == source_.size()) fail("unterminated list");
        ++position_;
        if (values.empty()) return tail;
        if (tail.type() == Value::Type::Pair && tail.is_list()) {
            auto rest = unpack(tail, "list"); values.insert(values.end(), rest.begin(), rest.end()); tail = Value::nil();
        }
        auto storage = std::make_shared<std::vector<Value>>(std::move(values));
        return ValueAccess::make(Value::Type::Pair, List{storage, 0, tail});
    }
    Value string_value() {
        ++position_; std::string out;
        while (position_ < source_.size() && source_[position_] != '"') {
            unsigned char c = static_cast<unsigned char>(source_[position_++]);
            if (c == '\\') {
                if (position_ == source_.size()) fail("unterminated string escape");
                c = static_cast<unsigned char>(source_[position_++]);
                if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t'; else if (c == '0') c = 0;
                else if (c == 'x') {
                    unsigned value = 0; int digits = 0;
                    while (position_ < source_.size() && source_[position_] != ';') {
                        char h = source_[position_++]; value *= 16;
                        if (h >= '0' && h <= '9') value += h - '0'; else if (h >= 'a' && h <= 'f') value += h - 'a' + 10; else if (h >= 'A' && h <= 'F') value += h - 'A' + 10; else fail("invalid hex escape");
                        ++digits;
                    }
                    if (!digits || position_ == source_.size() || value > 255) fail("invalid hex escape");
                    ++position_; c = static_cast<unsigned char>(value);
                }
            }
            out.push_back(static_cast<char>(c));
        }
        if (position_ == source_.size()) fail("unterminated string");
        ++position_; return Value(std::string_view(out.data(), out.size()));
    }
    Value atom() {
        std::size_t start = position_;
        while (position_ < source_.size() && !delimiter(source_[position_])) ++position_;
        std::string token(source_.substr(start, position_ - start));
        if (token == "#t") return Value(true); if (token == "#f") return Value(false);
        if (token.rfind("#\\", 0) == 0) {
            std::string c = token.substr(2); if (c == "space") return Value(' '); if (c == "newline") return Value('\n');
            if (c.size() == 1) return Value(c[0]); fail("invalid character literal: " + token);
        }
        if (token.size() > 1 && token.back() == '.') {
            std::string text = token.substr(0, token.size() - 1); char* end = nullptr; double value = std::strtod(text.c_str(), &end);
            if (end == text.c_str() + text.size() && std::isfinite(value)) return Value(value);
            fail("invalid float literal: " + token);
        }
        std::int64_t integer = 0; auto parsed = std::from_chars(token.data(), token.data() + token.size(), integer);
        if (parsed.ec == std::errc() && parsed.ptr == token.data() + token.size()) return Value(integer);
        if (parsed.ec == std::errc::result_out_of_range) fail("integer literal out of range: " + token);
        return Value::symbol(token);
    }
};

bool deep_equal(const Value& a, const Value& b) {
    if (numeric(a) && numeric(b)) {
        if (a.type() == Value::Type::Integer && b.type() == Value::Type::Integer) return a.as_integer() == b.as_integer();
        return static_cast<long double>(a.type() == Value::Type::Integer ? a.as_integer() : a.as_float()) == static_cast<long double>(b.type() == Value::Type::Integer ? b.as_integer() : b.as_float());
    }
    if (a.type() != b.type()) return false;
    switch (a.type()) {
    case Value::Type::Unspecified: case Value::Type::Nil: return true;
    case Value::Type::Boolean: return a.as_boolean() == b.as_boolean();
    case Value::Type::Character: return a.as_character() == b.as_character();
    case Value::Type::String: return a.as_string() == b.as_string();
    case Value::Type::Symbol: return a.as_symbol() == b.as_symbol();
    case Value::Type::Pair: return deep_equal(a.car(), b.car()) && deep_equal(a.cdr(), b.cdr());
    default: return &ValueAccess::data(a) == &ValueAccess::data(b);
    }
}

std::string write_value(const Value& value) {
    switch (value.type()) {
    case Value::Type::Unspecified: return "#<unspecified>";
    case Value::Type::Nil: return "()";
    case Value::Type::Boolean: return value.as_boolean() ? "#t" : "#f";
    case Value::Type::Integer: return std::to_string(value.as_integer());
    case Value::Type::Float: { std::ostringstream out; out << std::setprecision(std::numeric_limits<double>::max_digits10) << value.as_float(); return out.str() + "."; }
    case Value::Type::Character: { unsigned char c = static_cast<unsigned char>(value.as_character()); if (c == ' ') return "#\\space"; if (c == '\n') return "#\\newline"; return std::string("#\\") + static_cast<char>(c); }
    case Value::Type::String: {
        std::string out = "\"";
        for (unsigned char c : value.as_string()) {
            if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
            else if (c == '\n') out += "\\n"; else if (c == '\r') out += "\\r"; else if (c == '\t') out += "\\t"; else if (c == 0) out += "\\0";
            else if (c < 32 || c >= 127) { std::ostringstream h; h << "\\x" << std::hex << std::uppercase << static_cast<unsigned>(c) << ';'; out += h.str(); }
            else out += static_cast<char>(c);
        }
        return out + '"';
    }
    case Value::Type::Symbol: return std::string(value.as_symbol());
    case Value::Type::Pair: { std::string out = "("; Value at = value; bool first = true; while (at.type() == Value::Type::Pair) { if (!first) out += ' '; out += write_value(at.car()); at = at.cdr(); first = false; } if (!at.is_nil()) out += " . " + write_value(at); return out + ')'; }
    case Value::Type::Procedure: return "#<procedure>";
    case Value::Type::Capability: return "#<capability>";
    case Value::Type::Handle: return "#<handle>";
    }
    return {};
}

} // namespace

Error::Error(const std::string& message) : std::runtime_error("toolscheme: " + message) {}
Value::Value() : Value(nil()) {}
Value::Value(std::shared_ptr<ValueData> data) : data_(std::move(data)) {}
Value::Value(std::int64_t value) : Value(ValueAccess::make(Type::Integer, value)) {}
Value::Value(double value) : Value(ValueAccess::make(Type::Float, value)) { if (!std::isfinite(value)) fail("non-finite float"); }
Value::Value(bool value) : Value(ValueAccess::make(Type::Boolean, value)) {}
Value::Value(char value) : Value(ValueAccess::make(Type::Character, value)) {}
Value::Value(const char* value) : Value(std::string_view(value)) {}
Value::Value(std::string_view value) {
    NativeString string; string.storage.resize(sizeof(std::size_t) + value.size() + 1);
    std::size_t size = value.size(); std::memcpy(string.storage.data(), &size, sizeof size);
    std::memcpy(string.storage.data() + sizeof size, value.data(), value.size()); string.storage.back() = 0;
    *this = ValueAccess::make(Type::String, std::move(string));
}
Value Value::unspecified() { return ValueAccess::make(Type::Unspecified); }
Value Value::nil() { static Value value = ValueAccess::make(Type::Nil); return value; }
Value Value::symbol(std::string_view name) { return ValueAccess::make(Type::Symbol, std::string(name)); }
Value::Type Value::type() const noexcept { return data_->type; }
bool Value::is_nil() const noexcept { return type() == Type::Nil; }
bool Value::is_list() const noexcept { if (is_nil()) return true; if (type() != Type::Pair) return false; Value at = *this; while (at.type() == Type::Pair) at = as_list_data(at).tail; return at.is_nil(); }
bool Value::truthy() const noexcept { return type() != Type::Boolean || std::get<bool>(data_->payload); }
std::int64_t Value::as_integer() const { return std::get<std::int64_t>(checked(*this, Type::Integer, "integer").payload); }
double Value::as_float() const { return std::get<double>(checked(*this, Type::Float, "float").payload); }
bool Value::as_boolean() const { return std::get<bool>(checked(*this, Type::Boolean, "boolean").payload); }
char Value::as_character() const { return std::get<char>(checked(*this, Type::Character, "character").payload); }
std::string_view Value::as_string() const { const auto& s = std::get<NativeString>(checked(*this, Type::String, "string").payload).storage; std::size_t n; std::memcpy(&n, s.data(), sizeof n); return {reinterpret_cast<const char*>(s.data() + sizeof n), n}; }
std::string_view Value::as_symbol() const { return std::get<std::string>(checked(*this, Type::Symbol, "symbol").payload); }
Value Value::car() const { const List& l = as_list_data(*this); return (*l.values)[l.offset]; }
Value Value::cdr() const { const List& l = as_list_data(*this); if (l.offset + 1 == l.values->size()) return l.tail; return ValueAccess::make(Type::Pair, List{l.values, l.offset + 1, l.tail}); }
std::size_t Value::list_size() const { if (is_nil()) return 0; return unpack(*this, "list_size").size(); }
Value Value::list_at(std::size_t index) const { auto values = unpack(*this, "list_at"); if (index >= values.size()) fail("list index out of range"); return values[index]; }
const unsigned char* Value::string_data() const { return std::get<NativeString>(checked(*this, Type::String, "string").payload).storage.data(); }
std::size_t Value::string_size() const { return as_string().size(); }
std::string Value::to_string() const { return write_value(*this); }

struct Interpreter::Impl {
    Interpreter* owner; std::shared_ptr<Environment> global = std::make_shared<Environment>(); std::set<std::string> primitives;
    explicit Impl(Interpreter* value) : owner(value) {}
    Value evaluate(Value expression, std::shared_ptr<Environment> environment) {
        for (;;) {
            if (expression.type() == Value::Type::Symbol) return environment->get(std::string(expression.as_symbol()));
            if (expression.type() != Value::Type::Pair) return expression;
            Value head = expression.car(); auto forms = unpack(expression.cdr(), "form");
            if (head.type() == Value::Type::Symbol) {
                std::string op(head.as_symbol());
                if (op == "quote") { arity(forms, 1, "quote"); return forms[0]; }
                if (op == "if") { if (forms.size() < 2 || forms.size() > 3) fail("if expects 2 or 3 arguments"); expression = evaluate(forms[0], environment).truthy() ? forms[1] : forms.size() == 3 ? forms[2] : Value::unspecified(); continue; }
                if (op == "begin") { if (forms.empty()) return Value::unspecified(); for (std::size_t i = 0; i + 1 < forms.size(); ++i) evaluate(forms[i], environment); expression = forms.back(); continue; }
                if (op == "define") { if (forms.size() < 2) fail("define expects a name and value"); if (forms[0].type() == Value::Type::Pair) { std::vector<Value> lambda{forms[0].cdr()}; lambda.insert(lambda.end(), forms.begin() + 1, forms.end()); environment->values[symbol(forms[0].car(), "define")] = evaluate(owner->cons(Value::symbol("lambda"), owner->list(lambda)), environment); } else { arity(forms, 2, "define"); environment->values[symbol(forms[0], "define")] = evaluate(forms[1], environment); } return Value::unspecified(); }
                if (op == "set!") { arity(forms, 2, "set!"); Value v = evaluate(forms[1], environment); environment->set(symbol(forms[0], "set!"), v); return v; }
                if (op == "lambda") { if (forms.size() < 2) fail("lambda expects parameters and body"); Procedure p; p.environment = environment; p.body.assign(forms.begin() + 1, forms.end()); Value at = forms[0]; std::set<std::string> names; while (at.type() == Value::Type::Pair) { auto name = symbol(at.car(), "lambda"); if (!names.insert(name).second) fail("duplicate lambda parameter: " + name); p.parameters.push_back(name); at = at.cdr(); } if (!at.is_nil()) p.rest = symbol(at, "lambda"); return ValueAccess::make(Value::Type::Procedure, std::move(p)); }
                if (op == "and" || op == "or") { if (forms.empty()) return Value(op == "and"); for (std::size_t i = 0; i + 1 < forms.size(); ++i) { Value v = evaluate(forms[i], environment); if ((op == "and" && !v.truthy()) || (op == "or" && v.truthy())) return v; } expression = forms.back(); continue; }
                if (op == "let") { if (forms.size() < 2) fail("let expects bindings and body"); auto child = std::make_shared<Environment>(); child->parent = environment; for (auto binding : unpack(forms[0], "let")) { auto b = unpack(binding, "let binding"); arity(b, 2, "let binding"); child->values[symbol(b[0], "let")] = evaluate(b[1], environment); } for (std::size_t i = 1; i + 1 < forms.size(); ++i) evaluate(forms[i], child); expression = forms.back(); environment = child; continue; }
                if (op == "eval") { if (forms.size() != 1) fail("eval expects 1 argument"); expression = evaluate(forms[0], environment); continue; }
            }
            Value callable = evaluate(head, environment); std::vector<Value> arguments; for (const auto& form : forms) arguments.push_back(evaluate(form, environment));
            const Procedure& p = std::get<Procedure>(checked(callable, Value::Type::Procedure, "procedure").payload);
            if (p.native) return p.native(*owner, arguments);
            if (arguments.size() < p.parameters.size() || (p.rest.empty() && arguments.size() != p.parameters.size())) fail("wrong procedure argument count");
            auto child = std::make_shared<Environment>(); child->parent = p.environment; for (std::size_t i = 0; i < p.parameters.size(); ++i) child->values[p.parameters[i]] = arguments[i];
            if (!p.rest.empty()) child->values[p.rest] = owner->list({arguments.begin() + static_cast<std::ptrdiff_t>(p.parameters.size()), arguments.end()});
            for (std::size_t i = 0; i + 1 < p.body.size(); ++i) evaluate(p.body[i], child); expression = p.body.back(); environment = child;
        }
    }
};

Interpreter::Interpreter() : impl_(std::make_unique<Impl>(this)) {
    auto add = [this](const char* name, NativeFunction fn) { define_native(name, std::move(fn)); };
    add("+", [](Interpreter&, const std::vector<Value>& a) { bool f=false; std::int64_t i=0; double d=0; for(auto&v:a){if(v.type()==Value::Type::Float){f=true;d+=v.as_float();}else{i=checked_add(i,v.as_integer(),"+");}} return f?Value(d+static_cast<double>(i)):Value(i); });
    add("-", [](Interpreter&, const std::vector<Value>& a) { if(a.empty())fail("- expects arguments"); bool f=std::any_of(a.begin(),a.end(),[](auto&v){return v.type()==Value::Type::Float;}); if(f){double r=a.size()==1?0:real(a[0]);for(std::size_t n=a.size()==1?0:1;n<a.size();++n)r-=real(a[n]);return Value(r);} std::int64_t r=a.size()==1?0:a[0].as_integer();for(std::size_t n=a.size()==1?0:1;n<a.size();++n)r=checked_sub(r,a[n].as_integer(),"-");return Value(r); });
    add("*", [](Interpreter&, const std::vector<Value>& a) { bool f=std::any_of(a.begin(),a.end(),[](auto&v){return v.type()==Value::Type::Float;});if(f){double r=1;for(auto&v:a)r*=real(v);return Value(r);}std::int64_t r=1;for(auto&v:a)r=checked_mul(r,v.as_integer(),"*");return Value(r); });
    add("/", [](Interpreter&, const std::vector<Value>& a) {if(a.empty())fail("/ expects arguments");double r=a.size()==1?1:real(a[0]);for(std::size_t n=a.size()==1?0:1;n<a.size();++n){double v=real(a[n]);if(v==0)fail("division by zero");r/=v;}return Value(r);});
    auto compare=[add](const char*n,auto fn)mutable{add(n,[n,fn](Interpreter&,const std::vector<Value>&a){arity(a,2,n);if(a[0].type()==Value::Type::Integer&&a[1].type()==Value::Type::Integer)return Value(fn(static_cast<long double>(a[0].as_integer()),static_cast<long double>(a[1].as_integer())));return Value(fn(static_cast<long double>(real(a[0])),static_cast<long double>(real(a[1]))));});};
    compare("=",[](auto a,auto b){return a==b;});compare("<",[](auto a,auto b){return a<b;});compare(">",[](auto a,auto b){return a>b;});compare("<=",[](auto a,auto b){return a<=b;});compare(">=",[](auto a,auto b){return a>=b;});
    add("cons",[](Interpreter&i,const std::vector<Value>&a){arity(a,2,"cons");return i.cons(a[0],a[1]);});add("car",[](Interpreter&,const std::vector<Value>&a){arity(a,1,"car");return a[0].car();});add("cdr",[](Interpreter&,const std::vector<Value>&a){arity(a,1,"cdr");return a[0].cdr();});add("list",[](Interpreter&i,const std::vector<Value>&a){return i.list(a);});
    add("length",[](Interpreter&,const std::vector<Value>&a){arity(a,1,"length");return Value(static_cast<std::int64_t>(a[0].type()==Value::Type::String?a[0].string_size():a[0].list_size()));});add("list-ref",[](Interpreter&,const std::vector<Value>&a){arity(a,2,"list-ref");auto n=a[1].as_integer();if(n<1)fail("list index out of range");return a[0].list_at(static_cast<std::size_t>(n-1));});
    add("string-ref",[](Interpreter&,const std::vector<Value>&a){arity(a,2,"string-ref");auto n=a[1].as_integer();if(n<1||static_cast<std::size_t>(n)>a[0].string_size())fail("string index out of range");return Value(a[0].as_string()[static_cast<std::size_t>(n-1)]);});add("string-append",[](Interpreter&,const std::vector<Value>&a){std::string s;for(auto&v:a)s+=v.as_string();return Value(std::string_view(s.data(),s.size()));});
    add("null?",[](Interpreter&,const std::vector<Value>&a){arity(a,1,"null?");return Value(a[0].is_nil());});add("pair?",[](Interpreter&,const std::vector<Value>&a){arity(a,1,"pair?");return Value(a[0].type()==Value::Type::Pair);});add("list?",[](Interpreter&,const std::vector<Value>&a){arity(a,1,"list?");return Value(a[0].is_list());});add("number?",[](Interpreter&,const std::vector<Value>&a){arity(a,1,"number?");return Value(numeric(a[0]));});add("string?",[](Interpreter&,const std::vector<Value>&a){arity(a,1,"string?");return Value(a[0].type()==Value::Type::String);});add("symbol?",[](Interpreter&,const std::vector<Value>&a){arity(a,1,"symbol?");return Value(a[0].type()==Value::Type::Symbol);});add("procedure?",[](Interpreter&,const std::vector<Value>&a){arity(a,1,"procedure?");return Value(a[0].type()==Value::Type::Procedure);});add("not",[](Interpreter&,const std::vector<Value>&a){arity(a,1,"not");return Value(!a[0].truthy());});add("eq?",[](Interpreter&,const std::vector<Value>&a){arity(a,2,"eq?");return Value(deep_equal(a[0],a[1]));});add("equal?",[](Interpreter&,const std::vector<Value>&a){arity(a,2,"equal?");return Value(deep_equal(a[0],a[1]));});
    add("read-from-string",[](Interpreter&i,const std::vector<Value>&a){arity(a,1,"read-from-string");return i.read(a[0].as_string());});add("write-to-string",[](Interpreter&i,const std::vector<Value>&a){arity(a,1,"write-to-string");return Value(i.write(a[0]));});add("primitive-names",[](Interpreter&i,const std::vector<Value>&a){arity(a,0,"primitive-names");std::vector<Value>v;for(auto&n:i.primitive_names())v.push_back(Value::symbol(n));return i.list(v);});
    add("capability-call",[](Interpreter&i,const std::vector<Value>&a){if(a.size()<2)fail("capability-call expects capability and operation");auto&c=std::get<CapabilityValue>(checked(a[0],Value::Type::Capability,"capability").payload);std::vector<Value>rest(a.begin()+2,a.end());Value result=c.capability->invoke(i,a[1].as_symbol(),rest);if(!result.is_list())fail("capability must return a proper list");return result;});

    // Coding-agent utilities are thin typed capability calls. Their host behavior
    // remains policy-controlled while their Scheme signatures stay portable.
    static const char* capability_primitives[] = {
        "[", "test", "basename", "cat", "chmod", "cp", "dd", "df", "dirname", "du",
        "file", "find", "link", "ln", "ls", "mkdir", "mktemp", "mv", "pwd", "readlink",
        "realpath", "rm", "rmdir", "stat", "sync", "touch", "truncate", "unlink",
        "file-open", "file-close", "file-read", "file-read-at", "file-write", "file-write-at",
        "file-seek", "file-stat", "file-truncate", "file-flush", "directory-open", "directory-read",
        "directory-close", "glob", "tree", "read-file", "search", "rg", "apply-patch", "temp-file",
        "temp-directory", "comm", "cut", "diff", "diff3", "expand", "fmt", "fold", "grep", "head",
        "join", "nl", "paste", "rev", "sort", "split", "strings", "tail", "tr", "unexpand", "uniq",
        "wc", "xargs", "text-lines", "text-fields", "text-select", "text-replace", "cksum", "cmp",
        "md5", "shasum", "sum", "which", "whereis", "base64", "hash", "date", "env", "hostname",
        "id", "kill", "nice", "pgrep", "pkill", "ps", "sleep", "time", "timeout", "tty", "uname",
        "uptime", "users", "wait4path", "who", "whoami", "echo", "expr", "printf", "yes", "bzip2",
        "compress", "cpio", "gunzip", "gzip", "pax", "tar", "uncompress", "unzip", "zip", "launchctl",
        "logger", "open", "stty", "process-start", "process-poll", "process-wait", "process-cancel",
        "process-write", "process-close-input", "process-read-output", "process-read-errors", "job-poll",
        "job-wait", "job-cancel", "job-input", "job-close-input", "job-output", "job-error-output",
        "job-status", "ed", "ed-open", "ed-command", "ed-buffer", "ed-write", "ed-close", "bash", "sh",
        "zsh", "csh", "tcsh", "ksh", "dash", "json-parse", "json-write"
    };
    for (const char* primitive : capability_primitives) {
        std::string operation(primitive);
        add(primitive, [operation](Interpreter& interpreter, const std::vector<Value>& arguments) {
            if (arguments.empty()) fail(operation + " expects an explicit capability argument");
            const auto& capability = std::get<CapabilityValue>(
                checked(arguments[0], Value::Type::Capability, "capability").payload);
            std::vector<Value> request(arguments.begin() + 1, arguments.end());
            Value result = capability.capability->invoke(interpreter, operation, request);
            if (!result.is_list()) fail(operation + " capability must return a proper list");
            return result;
        });
    }
}
Interpreter::~Interpreter()=default; Interpreter::Interpreter(Interpreter&&o)noexcept:impl_(std::move(o.impl_)){if(impl_)impl_->owner=this;} Interpreter& Interpreter::operator=(Interpreter&&o)noexcept{impl_=std::move(o.impl_);if(impl_)impl_->owner=this;return*this;}
Value Interpreter::read(std::string_view source){Reader r(source);Value value=r.datum();if(!r.done())fail("trailing input");return value;} Value Interpreter::eval(Value expression){return impl_->evaluate(std::move(expression),impl_->global);} Value Interpreter::eval(std::string_view source){Reader r(source);Value result=Value::unspecified();while(!r.done())result=impl_->evaluate(r.datum(),impl_->global);return result;} std::string Interpreter::write(const Value&v)const{return write_value(v);}
void Interpreter::define(std::string_view n,Value v){impl_->global->values[std::string(n)]=std::move(v);} void Interpreter::define_native(std::string_view n,NativeFunction f){Procedure p;p.native=std::move(f);define(n,ValueAccess::make(Value::Type::Procedure,std::move(p)));impl_->primitives.insert(std::string(n));} void Interpreter::define_capability(std::string_view n,std::shared_ptr<Capability> c){define(n,ValueAccess::make(Value::Type::Capability,CapabilityValue{std::move(c)}));}
Value Interpreter::cons(Value car,Value cdr){if(cdr.is_nil())return list({car});if(cdr.is_list()){auto v=unpack(cdr,"cons");v.insert(v.begin(),car);return list(v);}auto s=std::make_shared<std::vector<Value>>();s->push_back(car);return ValueAccess::make(Value::Type::Pair,List{s,0,cdr});} Value Interpreter::list(const std::vector<Value>&v){if(v.empty())return Value::nil();auto s=std::make_shared<std::vector<Value>>(v);return ValueAccess::make(Value::Type::Pair,List{s,0,Value::nil()});} std::vector<std::string> Interpreter::primitive_names()const{return{impl_->primitives.begin(),impl_->primitives.end()};}

} // namespace toolscheme
