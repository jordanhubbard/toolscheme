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

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message);
};

class Value {
public:
    enum class Type {
        Unspecified, Nil, Boolean, Integer, Float, Character,
        String, Symbol, Pair, Procedure, Capability, Handle
    };

    Value();
    explicit Value(std::int64_t value);
    explicit Value(double value);
    explicit Value(bool value);
    explicit Value(char value);
    explicit Value(std::string_view value);
    explicit Value(const char* value);

    static Value unspecified();
    static Value nil();
    static Value symbol(std::string_view name);

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
    std::size_t list_size() const;
    Value list_at(std::size_t zero_based_index) const;

    // Layout begins with a native size_t, followed by bytes and a trailing NUL.
    const unsigned char* string_data() const;
    std::size_t string_size() const;
    std::string to_string() const;

private:
    explicit Value(std::shared_ptr<ValueData> data);
    std::shared_ptr<ValueData> data_;
    friend class Interpreter;
    friend struct ValueAccess;
};

using NativeFunction = std::function<Value(Interpreter&, const std::vector<Value>&)>;

class Capability {
public:
    virtual ~Capability() = default;
    virtual Value invoke(Interpreter&, std::string_view operation,
                         const std::vector<Value>& arguments) = 0;
};

class Interpreter {
public:
    Interpreter();
    ~Interpreter();
    Interpreter(Interpreter&&) noexcept;
    Interpreter& operator=(Interpreter&&) noexcept;
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    Value read(std::string_view source);
    Value eval(Value expression);
    Value eval(std::string_view source);
    std::string write(const Value& value) const;

    void define(std::string_view name, Value value);
    void define_native(std::string_view name, NativeFunction function);
    void define_capability(std::string_view name, std::shared_ptr<Capability> capability);

    Value cons(Value car, Value cdr);
    Value list(const std::vector<Value>& values);
    std::vector<std::string> primitive_names() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace toolscheme

#endif
