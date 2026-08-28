#include "toolscheme.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>

namespace toolscheme {
namespace {

struct List {
    std::shared_ptr<std::vector<Value>> values;
    std::size_t offset = 0;
    Value tail;
};

struct Environment;

struct Procedure {
    NativeFunction native;
    std::vector<std::string> parameters;
    std::string rest_parameter;
    std::vector<Value> body;
    std::shared_ptr<Environment> environment;
};

struct NativeString {
    std::vector<unsigned char> bytes;
};

using Payload = std::variant<std::monostate, bool, std::int64_t, double, char,
                             NativeString, std::string, List, Procedure>;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("toolscheme: " + message);
}

} // namespace

struct ValueData {
    Value::Type type = Value::Type::Nil;
    Payload payload;
};

namespace {

Value make_value(Value::Type type, Payload payload = {}) {
    auto data = std::make_shared<ValueData>();
    data->type = type;
    data->payload = std::move(payload);
    return Value(std::move(data));
}

const ValueData& checked(const Value& value, Value::Type type, const char* expected) {
    if (value.type() != type) fail(std::string("expected ") + expected);
    return *value.storage();
}

struct Environment {
    std::unordered_map<std::string, Value> values;
    std::shared_ptr<Environment> parent;

    Value get(const std::string& name) const {
        auto found = values.find(name);
        if (found != values.end()) return found->second;
        if (parent) return parent->get(name);
        fail("unbound symbol: " + name);
    }

    void set(const std::string& name, Value value) {
        auto found = values.find(name);
        if (found != values.end()) {
            found->second = std::move(value);
            return;
        }
        if (parent) {
            parent->set(name, std::move(value));
            return;
        }
        fail("cannot set unbound symbol: " + name);
    }
};

std::vector<Value> unpack_list(Value value, const char* context) {
    if (value.is_nil()) return {};
    if (value.type() != Value::Type::Pair) fail(std::string(context) + " requires a proper list");
    const List& list = std::get<List>(value.storage()->payload);
    if (!list.tail.is_nil()) fail(std::string(context) + " requires a proper list");
    std::vector<Value> result(list.values->begin() + static_cast<std::ptrdiff_t>(list.offset), list.values->end());
    return result;
}

std::string symbol_name(const Value& value, const char* context) {
    if (value.type() != Value::Type::Symbol) fail(std::string(context) + " requires a symbol");
    return std::string(value.as_symbol());
}

class Parser {
public:
    explicit Parser(std::string_view source) : source_(source) {}

    std::vector<Value> parse_all() {
        std::vector<Value> forms;
        skip_space();
        while (position_ < source_.size()) {
            forms.push_back(parse());
            skip_space();
        }
        return forms;
    }

private:
    std::string_view source_;
    std::size_t position_ = 0;

    void skip_space() {
        for (;;) {
            while (position_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[position_]))) ++position_;
            if (position_ >= source_.size() || source_[position_] != ';') return;
            while (position_ < source_.size() && source_[position_] != '\n') ++position_;
        }
    }

    Value parse() {
        skip_space();
        if (position_ >= source_.size()) fail("unexpected end of input");
        char c = source_[position_];
        if (c == '(') return parse_list();
        if (c == ')') fail("unexpected ')'");
        if (c == '\'') {
            ++position_;
            return make_pair(Value::symbol("quote"), make_pair(parse(), Value::nil()));
        }
        if (c == '"') return parse_string();
        return parse_atom();
    }

    Value make_pair(Value car, Value cdr) {
        auto values = std::make_shared<std::vector<Value>>();
        values->push_back(std::move(car));
        return make_value(Value::Type::Pair, List{std::move(values), 0, std::move(cdr)});
    }

    Value parse_list() {
        ++position_;
        skip_space();
        if (position_ < source_.size() && source_[position_] == ')') {
            ++position_;
            return Value::nil();
        }
        std::vector<Value> values;
        Value tail = Value::nil();
        while (position_ < source_.size() && source_[position_] != ')') {
            if (source_[position_] == '.' && position_ + 1 < source_.size() &&
                std::isspace(static_cast<unsigned char>(source_[position_ + 1]))) {
                if (values.empty()) fail("invalid dotted list");
                ++position_;
                tail = parse();
                skip_space();
                break;
            }
            values.push_back(parse());
            skip_space();
        }
        if (position_ >= source_.size() || source_[position_] != ')') fail("unterminated list");
        ++position_;
        if (values.empty()) return tail;
        auto storage = std::make_shared<std::vector<Value>>(std::move(values));
        return make_value(Value::Type::Pair, List{std::move(storage), 0, std::move(tail)});
    }

    Value parse_string() {
        ++position_;
        std::string value;
        while (position_ < source_.size() && source_[position_] != '"') {
            char c = source_[position_++];
            if (c == '\\') {
                if (position_ >= source_.size()) fail("unterminated string escape");
                char escaped = source_[position_++];
                if (escaped == 'n') c = '\n';
                else if (escaped == 't') c = '\t';
                else if (escaped == 'r') c = '\r';
                else c = escaped;
            }
            value.push_back(c);
        }
        if (position_ >= source_.size()) fail("unterminated string");
        ++position_;
        return Value(value);
    }

    Value parse_atom() {
        std::size_t start = position_;
        while (position_ < source_.size() &&
               !std::isspace(static_cast<unsigned char>(source_[position_])) &&
               source_[position_] != '(' && source_[position_] != ')') ++position_;
        std::string token(source_.substr(start, position_ - start));
        if (token == "#t") return Value(true);
        if (token == "#f") return Value(false);
        if (token.rfind("#\\", 0) == 0) {
            std::string character = token.substr(2);
            if (character == "space") return Value(' ');
            if (character == "newline") return Value('\n');
            if (character.size() == 1) return Value(character[0]);
            fail("invalid character literal: " + token);
        }
        if (token.size() > 1 && token.back() == '.') {
            std::string number = token.substr(0, token.size() - 1);
            char* end = nullptr;
            double value = std::strtod(number.c_str(), &end);
            if (end == number.c_str() + number.size()) return Value(value);
        }
        std::int64_t integer = 0;
        auto converted = std::from_chars(token.data(), token.data() + token.size(), integer);
        if (converted.ec == std::errc() && converted.ptr == token.data() + token.size()) return Value(integer);
        return Value::symbol(token);
    }
};

bool numeric(const Value& value) {
    return value.type() == Value::Type::Integer || value.type() == Value::Type::Float;
}

double number(const Value& value) {
    if (value.type() == Value::Type::Integer) return static_cast<double>(value.as_integer());
    if (value.type() == Value::Type::Float) return value.as_float();
    fail("expected number");
}

bool equal(const Value& left, const Value& right) {
    if (numeric(left) && numeric(right)) return number(left) == number(right);
    if (left.type() != right.type()) return false;
    switch (left.type()) {
    case Value::Type::Nil: return true;
    case Value::Type::Boolean: return left.as_boolean() == right.as_boolean();
    case Value::Type::Character: return left.as_character() == right.as_character();
    case Value::Type::String: return left.as_string() == right.as_string();
    case Value::Type::Symbol: return left.as_symbol() == right.as_symbol();
    default: return left.storage() == right.storage();
    }
}

void arity(const std::vector<Value>& args, std::size_t count, const char* name) {
    if (args.size() != count) fail(std::string(name) + " expects " + std::to_string(count) + " arguments");
}

} // namespace

Value::Value() : data_(std::make_shared<ValueData>()) {}
Value::Value(std::shared_ptr<ValueData> data) : data_(std::move(data)) {}
Value::Value(std::int64_t value) : Value(make_value(Type::Integer, value)) {}
Value::Value(double value) : Value(make_value(Type::Float, value)) {}
Value::Value(bool value) : Value(make_value(Type::Boolean, value)) {}
Value::Value(char value) : Value(make_value(Type::Character, value)) {}
Value::Value(std::string_view value) {
    if (value.size() > 255) fail("strings are limited to 255 bytes");
    NativeString string;
    string.bytes.reserve(value.size() + 1);
    string.bytes.push_back(static_cast<unsigned char>(value.size()));
    string.bytes.insert(string.bytes.end(), value.begin(), value.end());
    *this = make_value(Type::String, std::move(string));
}
Value Value::nil() { return Value(); }
Value Value::symbol(std::string_view name) { return make_value(Type::Symbol, std::string(name)); }
Value::Type Value::type() const noexcept { return data_->type; }
bool Value::is_nil() const noexcept { return type() == Type::Nil; }
bool Value::truthy() const noexcept { return type() != Type::Boolean || std::get<bool>(data_->payload); }
std::int64_t Value::as_integer() const { return std::get<std::int64_t>(checked(*this, Type::Integer, "integer").payload); }
double Value::as_float() const { return std::get<double>(checked(*this, Type::Float, "float").payload); }
bool Value::as_boolean() const { return std::get<bool>(checked(*this, Type::Boolean, "boolean").payload); }
char Value::as_character() const { return std::get<char>(checked(*this, Type::Character, "character").payload); }
std::string_view Value::as_string() const {
    const auto& bytes = std::get<NativeString>(checked(*this, Type::String, "string").payload).bytes;
    return {reinterpret_cast<const char*>(bytes.data() + 1), bytes[0]};
}
std::string_view Value::as_symbol() const { return std::get<std::string>(checked(*this, Type::Symbol, "symbol").payload); }
Value Value::car() const {
    const List& list = std::get<List>(checked(*this, Type::Pair, "pair").payload);
    return (*list.values)[list.offset];
}
Value Value::cdr() const {
    const List& list = std::get<List>(checked(*this, Type::Pair, "pair").payload);
    if (list.offset + 1 == list.values->size()) return list.tail;
    return make_value(Type::Pair, List{list.values, list.offset + 1, list.tail});
}
std::size_t Value::list_size() const {
    const List& list = std::get<List>(checked(*this, Type::Pair, "pair").payload);
    if (!list.tail.is_nil()) fail("list_size requires a proper list");
    return list.values->size() - list.offset;
}
Value Value::list_at(std::size_t index) const {
    const List& list = std::get<List>(checked(*this, Type::Pair, "pair").payload);
    std::size_t size = list.values->size() - list.offset;
    if (index >= size) fail("list index out of range");
    return (*list.values)[list.offset + index];
}
const unsigned char* Value::string_data() const {
    return std::get<NativeString>(checked(*this, Type::String, "string").payload).bytes.data();
}
std::size_t Value::string_size() const { return string_data()[0]; }

std::string Value::to_string() const {
    switch (type()) {
    case Type::Nil: return "()";
    case Type::Boolean: return as_boolean() ? "#t" : "#f";
    case Type::Integer: return std::to_string(as_integer());
    case Type::Float: {
        std::ostringstream out;
        out << as_float();
        std::string result = out.str();
        if (result.find('.') == std::string::npos) result.push_back('.');
        return result;
    }
    case Type::Character: return std::string("#\\") + as_character();
    case Type::String: return "\"" + std::string(as_string()) + "\"";
    case Type::Symbol: return std::string(as_symbol());
    case Type::Procedure: return "#<procedure>";
    case Type::Pair: {
        std::string result = "(";
        Value current = *this;
        bool first = true;
        while (current.type() == Type::Pair) {
            if (!first) result += ' ';
            result += current.car().to_string();
            current = current.cdr();
            first = false;
        }
        if (!current.is_nil()) result += " . " + current.to_string();
        return result + ')';
    }
    }
    return {};
}

struct Interpreter::Impl {
    Interpreter* owner;
    std::shared_ptr<Environment> global = std::make_shared<Environment>();

    explicit Impl(Interpreter* interpreter) : owner(interpreter) {}

    Value eval(Value expression, std::shared_ptr<Environment> environment) {
        for (;;) {
            if (expression.type() == Value::Type::Symbol) return environment->get(std::string(expression.as_symbol()));
            if (expression.type() != Value::Type::Pair) return expression;

            Value head = expression.car();
            Value tail = expression.cdr();
            if (head.type() == Value::Type::Symbol) {
                std::string op(head.as_symbol());
                if (op == "quote") {
                    auto args = unpack_list(tail, "quote");
                    arity(args, 1, "quote");
                    return args[0];
                }
                if (op == "if") {
                    auto args = unpack_list(tail, "if");
                    if (args.size() < 2 || args.size() > 3) fail("if expects 2 or 3 arguments");
                    expression = eval(args[0], environment).truthy() ? args[1] : (args.size() == 3 ? args[2] : Value::nil());
                    continue;
                }
                if (op == "begin") {
                    auto args = unpack_list(tail, "begin");
                    if (args.empty()) return Value::nil();
                    for (std::size_t i = 0; i + 1 < args.size(); ++i) eval(args[i], environment);
                    expression = args.back();
                    continue;
                }
                if (op == "define") {
                    auto args = unpack_list(tail, "define");
                    if (args.size() < 2) fail("define expects a name and value");
                    if (args[0].type() == Value::Type::Pair) {
                        std::string name = symbol_name(args[0].car(), "define");
                        std::vector<Value> lambda_args{args[0].cdr()};
                        lambda_args.insert(lambda_args.end(), args.begin() + 1, args.end());
                        Value lambda = owner->cons(Value::symbol("lambda"), owner->list(lambda_args));
                        environment->values[name] = eval(lambda, environment);
                    } else {
                        arity(args, 2, "define");
                        environment->values[symbol_name(args[0], "define")] = eval(args[1], environment);
                    }
                    return Value::nil();
                }
                if (op == "set!") {
                    auto args = unpack_list(tail, "set!");
                    arity(args, 2, "set!");
                    Value value = eval(args[1], environment);
                    environment->set(symbol_name(args[0], "set!"), value);
                    return value;
                }
                if (op == "lambda") {
                    auto args = unpack_list(tail, "lambda");
                    if (args.size() < 2) fail("lambda expects parameters and a body");
                    Procedure procedure;
                    procedure.environment = environment;
                    procedure.body.assign(args.begin() + 1, args.end());
                    Value params = args[0];
                    while (params.type() == Value::Type::Pair) {
                        procedure.parameters.push_back(symbol_name(params.car(), "lambda"));
                        params = params.cdr();
                    }
                    if (!params.is_nil()) procedure.rest_parameter = symbol_name(params, "lambda");
                    return make_value(Value::Type::Procedure, std::move(procedure));
                }
                if (op == "and" || op == "or") {
                    auto args = unpack_list(tail, op.c_str());
                    Value result = Value(op == "and");
                    for (const Value& arg : args) {
                        result = eval(arg, environment);
                        if ((op == "and" && !result.truthy()) || (op == "or" && result.truthy())) return result;
                    }
                    return result;
                }
            }

            Value callable = eval(head, environment);
            std::vector<Value> arguments;
            for (const Value& argument : unpack_list(tail, "call")) arguments.push_back(eval(argument, environment));
            const Procedure& procedure = std::get<Procedure>(checked(callable, Value::Type::Procedure, "procedure").payload);
            if (procedure.native) return procedure.native(*owner, arguments);
            if (arguments.size() < procedure.parameters.size() ||
                (procedure.rest_parameter.empty() && arguments.size() != procedure.parameters.size())) fail("wrong procedure argument count");
            auto call_environment = std::make_shared<Environment>();
            call_environment->parent = procedure.environment;
            for (std::size_t i = 0; i < procedure.parameters.size(); ++i) call_environment->values[procedure.parameters[i]] = arguments[i];
            if (!procedure.rest_parameter.empty()) {
                std::vector<Value> rest(arguments.begin() + procedure.parameters.size(), arguments.end());
                call_environment->values[procedure.rest_parameter] = owner->list(rest);
            }
            for (std::size_t i = 0; i + 1 < procedure.body.size(); ++i) eval(procedure.body[i], call_environment);
            expression = procedure.body.back();
            environment = std::move(call_environment);
        }
    }
};

Interpreter::Interpreter() : impl_(std::make_unique<Impl>(this)) {
    auto native = [this](std::string name, NativeFunction fn) { define_native(name, std::move(fn)); };
    native("+", [](Interpreter&, const std::vector<Value>& args) {
        bool floating = false; std::int64_t integer = 0; double real = 0;
        for (const auto& arg : args) { floating |= arg.type() == Value::Type::Float; integer += arg.type() == Value::Type::Integer ? arg.as_integer() : 0; real += number(arg); }
        return floating ? Value(real) : Value(integer);
    });
    native("-", [](Interpreter&, const std::vector<Value>& args) {
        if (args.empty()) fail("- expects at least one argument");
        bool floating = false; for (const auto& arg : args) { number(arg); floating |= arg.type() == Value::Type::Float; }
        if (!floating) { std::int64_t result = args[0].as_integer(); if (args.size() == 1) result = -result; else for (std::size_t i = 1; i < args.size(); ++i) result -= args[i].as_integer(); return Value(result); }
        double result = number(args[0]); if (args.size() == 1) result = -result; else for (std::size_t i = 1; i < args.size(); ++i) result -= number(args[i]); return Value(result);
    });
    native("*", [](Interpreter&, const std::vector<Value>& args) {
        bool floating = false; std::int64_t integer = 1; double real = 1;
        for (const auto& arg : args) { floating |= arg.type() == Value::Type::Float; if (arg.type() == Value::Type::Integer) integer *= arg.as_integer(); real *= number(arg); }
        return floating ? Value(real) : Value(integer);
    });
    native("/", [](Interpreter&, const std::vector<Value>& args) {
        if (args.empty()) fail("/ expects at least one argument");
        double result = args.size() == 1 ? 1.0 : number(args[0]);
        std::size_t start = args.size() == 1 ? 0 : 1;
        for (std::size_t i = start; i < args.size(); ++i) { double divisor = number(args[i]); if (divisor == 0) fail("division by zero"); result /= divisor; }
        return Value(result);
    });
    auto comparison = [native](const char* name, auto predicate) mutable {
        native(name, [name, predicate](Interpreter&, const std::vector<Value>& args) { arity(args, 2, name); return Value(predicate(number(args[0]), number(args[1]))); });
    };
    comparison("=", [](double a, double b) { return a == b; });
    comparison("<", [](double a, double b) { return a < b; });
    comparison(">", [](double a, double b) { return a > b; });
    comparison("<=", [](double a, double b) { return a <= b; });
    comparison(">=", [](double a, double b) { return a >= b; });
    native("cons", [](Interpreter& vm, const std::vector<Value>& args) { arity(args, 2, "cons"); return vm.cons(args[0], args[1]); });
    native("car", [](Interpreter&, const std::vector<Value>& args) { arity(args, 1, "car"); return args[0].car(); });
    native("cdr", [](Interpreter&, const std::vector<Value>& args) { arity(args, 1, "cdr"); return args[0].cdr(); });
    native("list", [](Interpreter& vm, const std::vector<Value>& args) { return vm.list(args); });
    native("null?", [](Interpreter&, const std::vector<Value>& args) { arity(args, 1, "null?"); return Value(args[0].is_nil()); });
    native("pair?", [](Interpreter&, const std::vector<Value>& args) { arity(args, 1, "pair?"); return Value(args[0].type() == Value::Type::Pair); });
    native("number?", [](Interpreter&, const std::vector<Value>& args) { arity(args, 1, "number?"); return Value(numeric(args[0])); });
    native("string?", [](Interpreter&, const std::vector<Value>& args) { arity(args, 1, "string?"); return Value(args[0].type() == Value::Type::String); });
    native("procedure?", [](Interpreter&, const std::vector<Value>& args) { arity(args, 1, "procedure?"); return Value(args[0].type() == Value::Type::Procedure); });
    native("not", [](Interpreter&, const std::vector<Value>& args) { arity(args, 1, "not"); return Value(!args[0].truthy()); });
    native("eq?", [](Interpreter&, const std::vector<Value>& args) { arity(args, 2, "eq?"); return Value(equal(args[0], args[1])); });
    native("length", [](Interpreter&, const std::vector<Value>& args) { arity(args, 1, "length"); if (args[0].type() == Value::Type::String) return Value(static_cast<std::int64_t>(args[0].string_size())); return Value(static_cast<std::int64_t>(unpack_list(args[0], "length").size())); });
    native("list-ref", [](Interpreter&, const std::vector<Value>& args) {
        arity(args, 2, "list-ref");
        auto index = args[1].as_integer();
        if (index < 0) fail("list index out of range");
        return args[0].list_at(static_cast<std::size_t>(index));
    });
    native("string-ref", [](Interpreter&, const std::vector<Value>& args) { arity(args, 2, "string-ref"); auto index = args[1].as_integer(); if (index < 1 || static_cast<std::size_t>(index) > args[0].string_size()) fail("string index out of range"); return Value(static_cast<char>(args[0].string_data()[index])); });
    native("string-append", [](Interpreter&, const std::vector<Value>& args) { std::string out; for (const auto& arg : args) out += arg.as_string(); return Value(out); });
}

Interpreter::~Interpreter() = default;
Interpreter::Interpreter(Interpreter&& other) noexcept : impl_(std::move(other.impl_)) { if (impl_) impl_->owner = this; }
Interpreter& Interpreter::operator=(Interpreter&& other) noexcept { impl_ = std::move(other.impl_); if (impl_) impl_->owner = this; return *this; }

Value Interpreter::eval(std::string_view source) {
    Value result = Value::nil();
    for (const Value& expression : Parser(source).parse_all()) result = impl_->eval(expression, impl_->global);
    return result;
}

Value Interpreter::eval_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) fail("cannot open file: " + path);
    std::ostringstream contents;
    contents << input.rdbuf();
    return eval(contents.str());
}

void Interpreter::define(std::string_view name, Value value) { impl_->global->values[std::string(name)] = std::move(value); }
void Interpreter::define_native(std::string_view name, NativeFunction function) {
    Procedure procedure;
    procedure.native = std::move(function);
    define(name, make_value(Value::Type::Procedure, std::move(procedure)));
}
Value Interpreter::cons(Value car, Value cdr) {
    auto values = std::make_shared<std::vector<Value>>();
    values->push_back(std::move(car));
    return make_value(Value::Type::Pair, List{std::move(values), 0, std::move(cdr)});
}
Value Interpreter::list(const std::vector<Value>& values) {
    if (values.empty()) return Value::nil();
    auto storage = std::make_shared<std::vector<Value>>(values);
    return make_value(Value::Type::Pair, List{std::move(storage), 0, Value::nil()});
}

} // namespace toolscheme
