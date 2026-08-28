#include "toolscheme.hpp"

#include <cassert>
#include <iostream>

int main() {
    toolscheme::Interpreter scheme;
    assert(scheme.eval("(+ 1 2 3)").as_integer() == 6);
    assert(scheme.eval("(+ 1 2.)").as_float() == 3.0);
    assert(scheme.eval("(begin (define x 4) (set! x (* x 3)) x)").as_integer() == 12);
    assert(scheme.eval("(begin (define (make-adder x) (lambda (y) (+ x y))) ((make-adder 7) 8))").as_integer() == 15);
    assert(scheme.eval("(begin (define (sum n acc) (if (= n 0) acc (sum (- n 1) (+ acc n)))) (sum 10000 0))").as_integer() == 50005000);
    assert(scheme.eval("(car '(10 20))").as_integer() == 10);
    auto list = scheme.eval("'(10 20 30)");
    assert(list.list_size() == 3);
    assert(list.list_at(2).as_integer() == 30);
    assert(list.cdr().list_at(1).as_integer() == 30);
    assert(scheme.eval("(list-ref '(10 20 30) 2)").as_integer() == 30);
    assert(scheme.eval("(length \"native\")").as_integer() == 6);
    assert(scheme.eval("(string-ref \"abc\" 1)").as_character() == 'a');

    auto string = scheme.eval("\"hello\"");
    assert(string.string_data()[0] == 5);
    assert(string.string_data()[1] == 'h');

    scheme.define_native("square", [](toolscheme::Interpreter&, const std::vector<toolscheme::Value>& args) {
        if (args.size() != 1) throw std::runtime_error("square expects one argument");
        auto value = args[0].as_integer();
        return toolscheme::Value(value * value);
    });
    assert(scheme.eval("(square 9)").as_integer() == 81);
    std::cout << "toolscheme tests passed\n";
}
