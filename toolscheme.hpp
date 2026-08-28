#ifndef TOOLSCHEME_HPP
#define TOOLSCHEME_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace toolscheme {

class Interpreter;
struct ValueData;

class Value {
public:
    enum class Type {
        Nil,
        Boolean,
        Integer,
        Float,
        Character,
        String,
        Symbol,
        Pair,
        Procedure
    };

    Value();
    Value(std::int64_t value);
    Value(double value);
    Value(bool value);
    Value(char value);
    Value(std::string_view value);

    static Value nil();
    static Value symbol(std::string_view name);

    Type type() const noexcept;
    bool is_nil() const noexcept;
    bool truthy() const noexcept;
    std::int64_t as_integer() const;
    double as_float() const;
    bool as_boolean() const;
    char as_character() const;
    std::string_view as_string() const;
    std::string_view as_symbol() const;
    Value car() const;
    Value cdr() const;

    // Lists use shared contiguous storage. cdr() creates a constant-time view.
    std::size_t list_size() const;
    Value list_at(std::size_t index) const;

    // Native string layout: data()[0] is the byte length and data()[1..size()] is text.
    // Length is limited to 255 so it fits in one native character slot.
    const unsigned char* string_data() const;
    std::size_t string_size() const;
    std::string to_string() const;

    // Internal storage hook used by the single-file implementation.
    explicit Value(std::shared_ptr<ValueData> data);
    const std::shared_ptr<ValueData>& storage() const noexcept { return data_; }

private:
    std::shared_ptr<ValueData> data_;
    friend class Interpreter;
};

using NativeFunction = std::function<Value(Interpreter&, const std::vector<Value>&)>;

class Interpreter {
public:
    Interpreter();
    ~Interpreter();
    Interpreter(Interpreter&&) noexcept;
    Interpreter& operator=(Interpreter&&) noexcept;
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    Value eval(std::string_view source);
    Value eval_file(const std::string& path);
    void define(std::string_view name, Value value);
    void define_native(std::string_view name, NativeFunction function);

    Value cons(Value car, Value cdr);
    Value list(const std::vector<Value>& values);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace toolscheme

#endif
