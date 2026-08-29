#include "toolscheme.hpp"

#include <cstring>
#include <iostream>
#include <set>

using toolscheme::Interpreter;
using toolscheme::Value;

namespace {
int failures = 0;
void check(bool condition, const char* name) { if (!condition) { std::cerr << "FAIL: " << name << '\n'; ++failures; } }
void equal(Interpreter& vm, std::string_view source, std::string_view expected, const char* name) { try { check(vm.eval(source).to_string() == expected, name); } catch (const std::exception& e) { std::cerr << "FAIL: " << name << ": " << e.what() << '\n'; ++failures; } }
void error(Interpreter& vm, std::string_view source, std::string_view text, const char* name) { try { vm.eval(source); std::cerr << "FAIL: " << name << ": no error\n"; ++failures; } catch (const std::exception& e) { check(std::string(e.what()).find(text) != std::string::npos, name); } }
void has(const std::set<std::string>& names, std::initializer_list<const char*> required, const char* test) {
    for (const char* name : required) check(names.count(name) == 1, test);
}

struct FakeCapability final : toolscheme::Capability {
    Value invoke(Interpreter& vm, std::string_view operation, const std::vector<Value>& arguments) override {
        if (operation == "pwd") return vm.list({vm.list({Value::symbol("path"), Value("/workspace")})});
        return vm.list({vm.list({Value::symbol("operation"), Value::symbol(operation)}), vm.list({Value::symbol("arguments"), vm.list(arguments)})});
    }
};
}

int main() {
    Interpreter vm;
    equal(vm, "(+ 1 2 3)", "6", "task-07 arithmetic");
    equal(vm, "(+ 1 2.)", "3.", "task-07 mixed arithmetic");
    error(vm, "(+ 9223372036854775807 1)", "overflow", "task-07 checked overflow");
    equal(vm, "(begin (define (loop n a) (if (= n 0) a (loop (- n 1) (+ a 1)))) (loop 100000 0))", "100000", "task-08 tail calls");
    equal(vm, "(let ((x 20) (y 22)) (+ x y))", "42", "task-08 let");
    equal(vm, "(and #t 4)", "4", "task-08 and tail");
    equal(vm, "(equal? (cons 1 (list 2 3)) '(1 2 3))", "#t", "task-05 normalized lists");
    equal(vm, "(list-ref '(10 20 30) 3)", "30", "task-05 one-based list access");
    check(vm.eval("'()").list_size() == 0, "task-05 empty list native access");
    std::string large(4096, 'x'); Value string{std::string_view(large)}; check(string.string_size() == 4096, "task-04 native-sized strings");
    std::size_t stored = 0; std::memcpy(&stored, string.string_data(), sizeof stored); check(stored == 4096, "task-04 native length header");
    equal(vm, "(string-ref \"abc\" 1)", "#\\a", "task-04 one-based strings");
    equal(vm, "(write-to-string \"a\\n\\\"\\\\\")", "\"\\\"a\\\\n\\\\\\\"\\\\\\\\\\\"\"", "task-06 canonical escaping");
    equal(vm, "(eval (read-from-string \"(+ 20 22)\"))", "42", "task-28 read write eval");
    equal(vm, "(equal? '(a (1 . 2)) (read-from-string (write-to-string '(a (1 . 2)))))", "#t", "task-28 round trip");
    vm.define_native("square", [](Interpreter&, const std::vector<Value>& a) { if (a.size()!=1) throw toolscheme::Error("square arity"); auto n=a[0].as_integer(); return Value(n*n); });
    equal(vm, "(square 9)", "81", "task-02 native value API");
    vm.define_capability("fake", std::make_shared<FakeCapability>());
    equal(vm, "(capability-call fake 'pwd)", "((path \"/workspace\"))", "task-09 capability result");
    equal(vm, "(pwd fake '((output data)))", "((path \"/workspace\"))", "task-11 path primitive");
    equal(vm, "(rg fake \"needle\" '(\"src\") '((literal #t)))", "((operation rg) (arguments (\"needle\" (\"src\") ((literal #t)))))", "task-14 search primitive");
    equal(vm, "(process-start fake '((program \"git\") (arguments (\"status\"))))", "((operation process-start) (arguments (((program \"git\") (arguments (\"status\"))))))", "task-10 process primitive");
    equal(vm, "(json-parse fake \"{\\\"x\\\":1}\")", "((operation json-parse) (arguments (\"{\\\"x\\\":1}\")))", "task-20 JSON primitive");
    equal(vm, "(read-from-string (write-to-string (capability-call fake 'pwd)))", "((path \"/workspace\"))", "task-28 evaluable capability result");
    check(vm.primitive_names().size() >= 175, "task-24 primitive registry");
    for (const auto& name : vm.primitive_names()) check(!name.empty(), "task-27 every primitive named");
    const std::set<std::string> core = {
        "+", "-", "*", "/", "=", "<", ">", "<=", ">=", "cons", "car", "cdr", "list",
        "length", "list-ref", "string-ref", "string-append", "null?", "pair?", "list?",
        "number?", "string?", "symbol?", "procedure?", "not", "eq?", "equal?",
        "read-from-string", "write-to-string", "primitive-names", "capability-call"
    };
    for (const auto& name : vm.primitive_names()) {
        if (core.count(name) || name == "square") continue;
        try {
            Value result = vm.eval("(" + name + " fake)");
            check(result.is_list(), "task-27 utility returns proper list");
            check(vm.read(vm.write(result)).is_list(), "task-28 utility result round trip");
        } catch (const std::exception& exception) {
            std::cerr << "FAIL: primitive example " << name << ": " << exception.what() << '\n';
            ++failures;
        }
    }
    const auto primitive_names = vm.primitive_names();
    const std::set<std::string> registered(primitive_names.begin(), primitive_names.end());
    has(registered, {"file-open", "file-read", "file-write", "file-stat"}, "task-11 file handle surface");
    has(registered, {"cp", "mv", "rm", "mkdir", "rmdir"}, "task-12 recursive filesystem surface");
    has(registered, {"glob", "tree", "read-file", "temp-file", "temp-directory"}, "task-13 traversal surface");
    has(registered, {"comm", "cut", "grep", "head", "sort", "tail", "wc"}, "task-15 text surface");
    has(registered, {"diff", "diff3", "apply-patch"}, "task-16 diff and patch surface");
    has(registered, {"echo", "expr", "printf", "test", "date", "env", "hostname"}, "task-17 output and system surface");
    has(registered, {"ps", "kill", "sleep", "timeout", "stty", "wait4path"}, "task-18 process and terminal surface");
    has(registered, {"hash", "base64", "gzip", "pax", "tar", "zip"}, "task-19 archive and crypto surface");
    has(registered, {"ed-open", "ed-command", "ed-buffer", "ed-write", "ed-close"}, "task-21 editor surface");
    has(registered, {"bash", "sh", "zsh", "csh", "tcsh", "ksh", "dash"}, "task-22 shell surface");
    has(registered, {"launchctl", "logger", "open"}, "task-23 platform surface");
    check(vm.write(vm.read("'(generated code)")) == "(quote (generated code))", "task-26 documented host API");

    Value survivor;
    {
        Interpreter temporary;
        survivor = temporary.eval("'(kept 42)");
    }
    check(survivor.to_string() == "(kept 42)", "task-03 value lifetime");
    Interpreter producer;
    std::string generated = producer.write(producer.eval("'(+ 40 2)"));
    Interpreter consumer;
    check(consumer.eval(consumer.read(generated)).as_integer() == 42, "task-28 cross-interpreter eval");
    if (failures) return 1;
    std::cout << "toolscheme tests passed (" << vm.primitive_names().size() << " primitives)\n";
}
