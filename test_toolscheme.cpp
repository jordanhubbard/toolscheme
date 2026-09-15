// Toolscheme test suite.
//
// Checks are always enabled: nothing here depends on `assert` or on NDEBUG. Tests
// are named with the `task-NN` prefix from docs/roadmap/active-work.md so every
// roadmap item maps to executable coverage, and the registry-completeness pass
// fails if any registered primitive lacks a worked example.

#include "toolscheme.hpp"
#include "toolscheme_posix.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using toolscheme::Capability;
using toolscheme::Interpreter;
using toolscheme::ListBuilder;
using toolscheme::PrimitiveGroup;
using toolscheme::Value;

namespace {

int failures = 0;
int checks = 0;

// Stress sizes scale down under a sanitizer build. Instrumented allocation costs
// roughly 500us per procedure call, so the full-size loops would take hours and a
// gate nobody can run is not a gate. The shapes still hold at 1/100 scale: 10k
// tail calls prove the stack does not grow, and a 10k-element list still
// distinguishes O(1) access from O(n).
std::int64_t stress(std::int64_t full) {
    static const std::int64_t divisor = [] {
        const char* scale = std::getenv("TOOLSCHEME_STRESS_DIVISOR");
        const long value = scale ? std::strtol(scale, nullptr, 10) : 1;
        return value > 0 ? static_cast<std::int64_t>(value) : 1;
    }();
    const std::int64_t scaled = full / divisor;
    return scaled > 0 ? scaled : 1;
}

std::string with_stress(const std::string& source, std::int64_t full) {
    const std::string marker = "@N@";
    const std::size_t at = source.find(marker);
    if (at == std::string::npos) return source;
    return source.substr(0, at) + std::to_string(stress(full)) +
           source.substr(at + marker.size());
}

void check(bool condition, const std::string& name, const std::string& detail = {}) {
    ++checks;
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << name;
    if (!detail.empty()) std::cerr << ": " << detail;
    std::cerr << '\n';
}

// Evaluates `source` and compares its canonical written form with `expected`.
void equal(Interpreter& vm, std::string_view source, std::string_view expected,
           const std::string& name) {
    ++checks;
    try {
        const std::string actual = vm.eval(source).to_string();
        if (actual == expected) return;
        ++failures;
        std::cerr << "FAIL: " << name << "\n  source:   " << source << "\n  expected: " << expected
                  << "\n  actual:   " << actual << '\n';
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: " << name << "\n  source: " << source << "\n  raised: " << error.what()
                  << '\n';
    }
}

// Requires that evaluating `source` raises an error whose message contains `text`.
void raises(Interpreter& vm, std::string_view source, std::string_view text,
            const std::string& name) {
    ++checks;
    try {
        const Value result = vm.eval(source);
        ++failures;
        std::cerr << "FAIL: " << name << "\n  source: " << source
                  << "\n  expected an error, got: " << result.to_string() << '\n';
    } catch (const std::exception& error) {
        if (std::string(error.what()).find(text) != std::string::npos) return;
        ++failures;
        std::cerr << "FAIL: " << name << "\n  source: " << source << "\n  expected message with: "
                  << text << "\n  actual: " << error.what() << '\n';
    }
}

// Requires a structured error result carrying the given code symbol.
void error_code(Interpreter& vm, std::string_view source, std::string_view code,
                const std::string& name) {
    ++checks;
    try {
        const Value result = vm.eval(source);
        const Value actual = toolscheme::option(result, "code");
        if (actual.type() == Value::Type::Symbol && actual.as_symbol() == code) return;
        ++failures;
        std::cerr << "FAIL: " << name << "\n  source: " << source << "\n  expected code: " << code
                  << "\n  actual: " << result.to_string() << '\n';
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: " << name << "\n  source: " << source << "\n  raised: " << error.what()
                  << '\n';
    }
}

bool contains_runtime_reference(const Value& value) {
    switch (value.type()) {
    case Value::Type::Handle:
    case Value::Type::Procedure:
    case Value::Type::Capability: return true;
    case Value::Type::Pair: {
        Value at = value;
        while (at.type() == Value::Type::Pair) {
            if (contains_runtime_reference(at.car())) return true;
            at = at.cdr();
        }
        return contains_runtime_reference(at);
    }
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// Test capabilities
// ---------------------------------------------------------------------------

// Stands in for host subsystems the reference adapters delegate or refuse, so the
// dispatch, option, and result contracts are still exercised for those names.
class StubCapability : public Capability {
public:
    explicit StubCapability(std::string kind) : kind_(std::move(kind)) {}
    std::string_view capability_kind() const override { return kind_; }
    Value invoke(Interpreter&, std::string_view operation,
                 const std::vector<Value>& arguments) override {
        ListBuilder out(3);
        out.symbol_field("operation", operation);
        out.field("arguments", Value::list(arguments));
        out.field("stub", true);
        return out.build();
    }

private:
    std::string kind_;
};

// Refuses everything, which is what an embedder installs to keep a subsystem off.
class DenyingCapability : public toolscheme::FileSystemCapability {
public:
    Value invoke(Interpreter&, std::string_view operation, const std::vector<Value>&) override {
        ++calls;
        return toolscheme::denied_result(operation, "this capability denies every operation");
    }
    int calls = 0;
};

// Violates the result contract on purpose.
class MisbehavingCapability : public toolscheme::FileSystemCapability {
public:
    Value invoke(Interpreter&, std::string_view, const std::vector<Value>&) override {
        return Value::integer(42);
    }
};

// Advertises support for only one operation, so registration groups must skip the rest.
class NarrowCapability : public toolscheme::ArchiveCapability {
public:
    bool supports(std::string_view operation) const override { return operation == "tar"; }
    Value invoke(Interpreter&, std::string_view operation, const std::vector<Value>&) override {
        return toolscheme::ok_result({toolscheme::symbol_field("operation", operation)});
    }
};

// Mints a handle so handle forgery and revocation can be tested without the host.
class HandleMintingCapability : public toolscheme::EditorCapability {
public:
    Value invoke(Interpreter& vm, std::string_view operation,
                 const std::vector<Value>&) override {
        if (operation == "mint") {
            auto token = std::make_shared<int>(7);
            return toolscheme::ok_result({toolscheme::field(
                "handle", vm.make_handle(*this, "editor-session", token))});
        }
        return toolscheme::unsupported_result(operation, "only `mint` is available");
    }
    void release(std::string_view, const std::shared_ptr<void>&) override { ++releases; }
    int releases = 0;
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

std::string fixture_root;

bool run_shell(const std::string& command) { return std::system(command.c_str()) == 0; }

void build_fixture() {
    char pattern[] = "/tmp/toolscheme-testXXXXXX";
    const char* made = ::mkdtemp(pattern);
    if (!made) {
        std::cerr << "FATAL: cannot create a temporary fixture root\n";
        std::exit(2);
    }
    fixture_root = made;
    const std::string sub = fixture_root + "/sub";
    if (::mkdir(sub.c_str(), 0755) != 0) {
        std::cerr << "FATAL: cannot create the fixture subdirectory\n";
        std::exit(2);
    }
    const auto write_file = [](const std::string& path, const std::string& content) {
        std::FILE* handle = std::fopen(path.c_str(), "wb");
        if (!handle) { std::cerr << "FATAL: cannot write " << path << '\n'; std::exit(2); }
        std::fwrite(content.data(), 1, content.size(), handle);
        std::fclose(handle);
    };
    write_file(fixture_root + "/a.txt", "alpha\nbeta\ngamma\n");
    write_file(fixture_root + "/b.txt", "alpha\nBETA\ngamma\n");
    write_file(sub + "/c.txt", "nested\n");
    write_file(fixture_root + "/binary.bin", std::string("head\0tail", 9));
}

void remove_fixture() {
    if (fixture_root.empty()) return;
    run_shell("rm -rf '" + fixture_root + "'");
}

toolscheme::posix::Policy test_policy() {
    toolscheme::posix::Policy policy;
    policy.root = fixture_root;
    policy.allow_process = true;
    policy.default_timeout_ms = 4000;
    return policy;
}

// Installs the reference adapters for subsystems that are safe to exercise, and
// stubs for the ones that would touch the developer's machine (syslog, the desktop
// session, the service manager) or that this adapter set deliberately delegates.
void install_test_capabilities(Interpreter& vm) {
    const toolscheme::posix::Policy policy = test_policy();
    vm.install("filesystem", toolscheme::posix::make_filesystem(policy));
    const auto processes = toolscheme::posix::make_process(policy);
    vm.install("process", processes);
    vm.install("clock", toolscheme::posix::make_clock());
    vm.install("system", toolscheme::posix::make_system(policy));
    vm.install("crypto", toolscheme::posix::make_crypto());
    vm.install("shell", toolscheme::posix::make_shell(policy, processes));
    for (const char* kind : {"terminal", "service", "logging", "desktop", "archive",
                             "compression", "network", "http", "remote-shell", "editor"})
        vm.install(kind, std::make_shared<StubCapability>(kind));
    vm.enable_all_primitives();
}

const char* kFixtureDefinitions = R"scheme(
(define TEXT "alpha\nbeta\ngamma\n")
(define LINES '("alpha" "beta" "gamma"))
(define PATCH "--- a/a.txt\n+++ b/a.txt\n@@ -1,3 +1,3 @@\n alpha\n-beta\n+BETA\n gamma\n")
(define (with-file proc)
  (let* ((h (field-ref (file-open "a.txt") 'file)) (r (proc h))) (file-close h) r))
(define (with-dir proc)
  (let* ((h (field-ref (directory-open ".") 'directory)) (r (proc h))) (directory-close h) r))
(define (with-session proc)
  (let* ((h (field-ref (ed-open "a.txt") 'session)) (r (proc h))) (ed-close h) r))
(define (with-job proc)
  (let* ((h (field-ref (process-start '((program "echo") (arguments ("hi")))) 'job))
         (r (proc h)))
    (process-cancel h) r))
(define (with-open-job proc)
  (let* ((h (field-ref (process-start '((program "cat"))) 'job)) (r (proc h)))
    (process-cancel h) r))
)scheme";

struct Example {
    const char* name;
    const char* success;
    const char* failure;
};

const Example kExamples[] = {
#include "tests/primitive_examples.inc"
};

// Names whose documented success result is itself a structured refusal or a
// platform-specific unsupported report.
const std::set<std::string>& error_is_correct() {
    static const std::set<std::string> names = [] {
        std::set<std::string> out = {"procedure-ref"};
#if defined(__APPLE__)
        out.insert("systemctl");
#else
        out.insert("launchctl");
#endif
        return out;
    }();
    return names;
}

// Shell dialects and helper programs that may simply not be installed on the host,
// plus `git`, whose success additionally depends on the fixture being a repository.
const std::set<std::string>& host_dependent() {
    static const std::set<std::string> names = {"bash", "sh", "zsh", "csh", "tcsh",
                                                "ksh", "dash", "fish", "git"};
    return names;
}

} // namespace

// ---------------------------------------------------------------------------
// Roadmap coverage
// ---------------------------------------------------------------------------

static void task_01_published_semantics(Interpreter& vm) {
    // The frozen contract from the roadmap's language and storage section.
    equal(vm, "42", "42", "task-01 integers have no suffix");
    equal(vm, "42.", "42.", "task-01 floats carry a period");
    equal(vm, "(list-ref '(10 20 30) 1)", "10", "task-01 list indexing is 1-based");
    equal(vm, "(string-ref \"abc\" 1)", "#\\a", "task-01 string indexing is 1-based");
    equal(vm, "(if 0 'yes 'no)", "yes", "task-01 only #f is false");
    equal(vm, "(if '() 'yes 'no)", "yes", "task-01 nil is not false");
    equal(vm, "(if #f 'yes 'no)", "no", "task-01 #f is false");
    equal(vm, "(equal? (cons 1 '(2 3)) '(1 2 3))", "#t",
          "task-01 cons onto a proper list normalizes");
    equal(vm, "(list? (cons 1 2))", "#f", "task-01 improper pairs stay representable");
}

static void task_02_value_representation(Interpreter& vm) {
    // Scalars are immediates: constructing one allocates nothing and two equal
    // scalars are the same object without any interning table.
    check(sizeof(Value) <= 32, "task-02 value stays compact",
          "sizeof(Value)=" + std::to_string(sizeof(Value)));
    const Value a = Value::integer(7), b = Value::integer(7);
    check(a.same_object(b), "task-02 integers are immediate");
    check(Value::character('x').same_object(Value::character('x')), "task-02 characters are immediate");
    check(Value::nil().same_object(Value::nil()), "task-02 nil is immediate");
    check(Value::symbol("alpha").same_object(Value::symbol("alpha")),
          "task-02 symbols are interned");
    check(!Value::symbol("alpha").same_object(Value::symbol("beta")),
          "task-02 distinct symbols differ");
    equal(vm, "(eq? 'alpha 'alpha)", "#t", "task-02 symbol identity");

    vm.define_native("square", [](Interpreter&, const std::vector<Value>& a) {
        if (a.size() != 1) throw toolscheme::Error("square expects 1 argument");
        return Value::integer(a[0].as_integer() * a[0].as_integer());
    });
    equal(vm, "(square 9)", "81", "task-02 native value API");
    raises(vm, "(square)", "square expects", "task-02 native arity error");
}

static void task_03_ownership() {
    // Values outlive the interpreter that produced them, including nested aggregates.
    Value survivor;
    Value closure_result;
    {
        Interpreter temporary;
        survivor = temporary.eval("'(kept 42 \"text\" #\\c (nested (deep)))");
        closure_result = temporary.eval("((lambda (x) (list x x)) 'held)");
    }
    check(survivor.to_string() == "(kept 42 \"text\" #\\c (nested (deep)))",
          "task-03 aggregate values survive interpreter destruction", survivor.to_string());
    check(closure_result.to_string() == "(held held)",
          "task-03 closure results survive interpreter destruction");

    // Cycle-aware collection reclaims environments that only reference each other.
    Interpreter vm;
    const std::size_t before = vm.live_environments();
    vm.eval("(define (make-counter) (letrec ((n 0) (bump (lambda () (set! n (+ n 1)) bump))) bump))");
    for (int i = 0; i < 200; ++i) vm.eval("(make-counter)");
    const std::size_t grown = vm.live_environments();
    const std::size_t reclaimed = vm.collect();
    check(grown > before, "task-03 closures retain their environments");
    check(reclaimed >= 200, "task-03 cycle collection reclaims unreachable environments",
          "reclaimed=" + std::to_string(reclaimed));
    check(vm.live_environments() < grown, "task-03 live environment count drops after collection");

    // A closure the caller still holds must never be collected.
    Interpreter keeper;
    const Value held = keeper.eval("(let ((n 5)) (lambda () n))");
    keeper.collect();
    check(keeper.apply(held, {}).to_string() == "5",
          "task-03 externally held closures survive collection");
}

static void task_04_strings(Interpreter& vm) {
    const std::string large(4096, 'x');
    const Value text = Value::string(large);
    check(text.string_size() == 4096, "task-04 strings longer than 255 bytes");
    std::size_t header = 0;
    std::memcpy(&header, text.string_data(), sizeof header);
    check(header == 4096, "task-04 native size_t length header");
    check(text.string_data()[sizeof(std::size_t) + 4096] == 0, "task-04 trailing NUL terminator");

    const std::string embedded("a\0b", 3);
    const Value nulled = Value::string(embedded);
    check(nulled.string_size() == 3, "task-04 embedded NUL is preserved");
    check(nulled.as_string() == std::string_view(embedded.data(), 3),
          "task-04 as_string preserves embedded NUL");
    vm.define("embedded", nulled);
    equal(vm, "(string-length embedded)", "3", "task-04 embedded NUL counts toward length");
    equal(vm, "(equal? embedded (read-from-string (write-to-string embedded)))", "#t",
          "task-04 embedded NUL round trips");

    // UTF-8 is stored and indexed as bytes, exactly as the contract states.
    equal(vm, "(string-length \"\xc3\xa9\")", "2", "task-04 UTF-8 length counts bytes");
    equal(vm, "(char->integer (string-ref \"\xc3\xa9\" 1))", "195", "task-04 UTF-8 byte indexing");
    equal(vm, "(equal? \"\xe2\x9c\x93\" (read-from-string (write-to-string \"\xe2\x9c\x93\")))", "#t",
          "task-04 UTF-8 round trips");
    equal(vm, "(write-to-string \"a\\n\\\"\\\\\")", "\"\\\"a\\\\n\\\\\\\"\\\\\\\\\\\"\"",
          "task-04 canonical escaping");
    raises(vm, "(string-ref \"abc\" 0)", "out of range", "task-04 index 0 is out of range");
    raises(vm, "(string-ref \"abc\" 4)", "out of range", "task-04 index past the end");
}

static void task_05_lists(Interpreter& vm) {
    // Equivalent construction through literals, dotted forms, `list`, and `cons`.
    equal(vm, "(equal? '(1 2 3) (list 1 2 3))", "#t", "task-05 literal equals list");
    equal(vm, "(equal? '(1 2 3) (cons 1 (cons 2 (cons 3 '()))))", "#t", "task-05 cons chain");
    equal(vm, "(equal? '(1 2 3) '(1 . (2 . (3 . ()))))", "#t", "task-05 dotted spelling");
    equal(vm, "(equal? '(1 . 2) (cons 1 2))", "#t", "task-05 improper pair");
    equal(vm, "'(1 . 2)", "(1 . 2)", "task-05 improper pair writes with a dot");

    check(vm.eval("'()").list_size() == 0, "task-05 empty list length");
    check(vm.eval("'(1 2 3)").list_size() == 3, "task-05 length through the native API");
    check(vm.eval("'(1 2 3)").list_at(2).to_string() == "3", "task-05 zero-based native indexing");
    equal(vm, "(list-tail '(1 2 3 4) 2)", "(3 4)", "task-05 cdr views");
    equal(vm, "(sublist '(1 2 3 4 5) 2 4)", "(2 3 4)", "task-05 sublist view");

    // Random access over a large list, built in bulk.
    const std::int64_t big_size = stress(100000);
    vm.eval(with_stress("(define big (let loop ((n @N@) (acc '())) "
                        "(if (= n 0) acc (loop (- n 1) (cons n acc)))))", 100000));
    equal(vm, "(length big)", std::to_string(big_size), "task-05 large list length");
    equal(vm, "(list-ref big 1)", "1", "task-05 large list first element");
    equal(vm, "(list-ref big " + std::to_string(big_size) + ")", std::to_string(big_size),
          "task-05 large list last element");
    equal(vm, "(list-ref big " + std::to_string(big_size / 2) + ")",
          std::to_string(big_size / 2), "task-05 large list random index");
    raises(vm, "(list-ref big " + std::to_string(big_size + 1) + ")", "out of range",
           "task-05 large list out of range");
    // Deep structural comparison must not grow the C++ stack.
    equal(vm, "(equal? big (list-copy big))", "#t", "task-05 deep equality iterates the spine");
    raises(vm, "(length (cons 1 2))", "proper list", "task-05 length rejects improper lists");
}

static void task_06_reader(Interpreter& vm) {
    ++checks;
    try {
        vm.eval("(+ 1\n   (* 2\n      ))\n(bad", "example.scm");
        ++failures;
        std::cerr << "FAIL: task-06 unterminated list must raise\n";
    } catch (const toolscheme::Error& error) {
        check(error.where().known, "task-06 errors carry a source location");
        check(error.where().file == "example.scm", "task-06 location names the file",
              error.where().file);
        check(error.where().line == 4, "task-06 location names the line",
              "line=" + std::to_string(error.where().line));
    }
    equal(vm, "(read-from-string \"; comment\\n42\")", "42", "task-06 line comments");
    equal(vm, "(read-from-string \"#| block |# 42\")", "42", "task-06 block comments");
    equal(vm, "(read-from-string \"[\")", "[", "task-06 brackets are ordinary symbols");
    raises(vm, "(read-from-string \"(1 2\")", "unterminated list", "task-06 unterminated list");
    raises(vm, "(read-from-string \")\")", "unexpected closing", "task-06 stray close");
    raises(vm, "(read-from-string \"\\\"abc\")", "unterminated string", "task-06 unterminated string");
    raises(vm, "(read-from-string \"1 2\")", "trailing input", "task-06 trailing input");

    // Streaming evaluation parses one datum at a time.
    const std::string program = "(define a 1)\n(define b 2)\n(+ a b)\n";
    std::size_t cursor = 0;
    const Value streamed = vm.eval_stream(
        [&] { return cursor < program.size() ? static_cast<int>(program[cursor++]) : -1; },
        "stream.scm");
    check(streamed.to_string() == "3", "task-06 streaming evaluation", streamed.to_string());
}

static void task_07_numbers(Interpreter& vm) {
    equal(vm, "(+ 1 2 3)", "6", "task-07 integer addition");
    equal(vm, "(+ 1 2.)", "3.", "task-07 mixed arithmetic is inexact");
    raises(vm, "(+ 9223372036854775807 1)", "overflow", "task-07 checked addition");
    raises(vm, "(- -9223372036854775807 2)", "overflow", "task-07 checked subtraction");
    raises(vm, "(* 4294967296 4294967296)", "overflow", "task-07 checked multiplication");
    raises(vm, "(abs -9223372036854775808)", "overflow", "task-07 checked absolute value");
    raises(vm, "(quotient -9223372036854775808 -1)", "overflow", "task-07 checked quotient");
    equal(vm, "(remainder -9223372036854775808 -1)", "0", "task-07 remainder avoids the trap");
    raises(vm, "(read-from-string \"99999999999999999999\")", "out of range",
           "task-07 integer literal overflow");
    equal(vm, "9223372036854775807", "9223372036854775807", "task-07 maximum integer");
    equal(vm, "-9223372036854775808", "-9223372036854775808", "task-07 minimum integer");

    // Exact comparison above 2^53, where a double conversion would lose the answer.
    equal(vm, "(= 9007199254740993 9007199254740992.)", "#f", "task-07 exact mixed comparison");
    equal(vm, "(< 9007199254740992. 9007199254740993)", "#t", "task-07 exact mixed ordering");
    equal(vm, "(= 9223372036854775807 9223372036854775808.)", "#f",
          "task-07 comparison at the integer boundary");
    equal(vm, "(< 9223372036854775807 9223372036854775808.)", "#t",
          "task-07 ordering past the integer boundary");
    equal(vm, "(equal? 1 1.)", "#f", "task-07 exactness participates in equality");

    equal(vm, "0.1", "0.1", "task-07 fractional float reads and writes");
    equal(vm, "(equal? 0.1 (read-from-string (write-to-string 0.1)))", "#t",
          "task-07 fractional round trip");
    equal(vm, "(write-to-string 1e300)", "\"1e+300.\"", "task-07 exponent form carries a period");
    equal(vm, "(equal? 1e300 (read-from-string (write-to-string 1e300)))", "#t",
          "task-07 large float round trip");
    raises(vm, "(read-from-string \"1e400\")", "not finite", "task-07 overflowing float literal");
    raises(vm, "(read-from-string \"1.2.3\")", "invalid numeric", "task-07 malformed float");
    raises(vm, "(read-from-string \"0xFF\")", "invalid numeric", "task-07 hexadecimal is rejected");
    equal(vm, "(string->number \"nope\")", "#f", "task-07 string->number reports failure as #f");
    equal(vm, "(modulo -7 3)", "2", "task-07 modulo follows the divisor sign");
    equal(vm, "(remainder -7 3)", "-1", "task-07 remainder follows the dividend sign");
    raises(vm, "(/ 1 0)", "division by zero", "task-07 division by zero");
}

static void task_08_evaluator(Interpreter& vm) {
    equal(vm, with_stress("(begin (define (loop n a) (if (= n 0) a (loop (- n 1) (+ a 1))))"
                          " (loop @N@ 0))", 1000000),
          std::to_string(stress(1000000)), "task-08 one million tail calls");
    equal(vm, with_stress("(let loop ((n @N@) (a 0)) (if (= n 0) a (loop (- n 1) (+ a 1))))",
                          1000000),
          std::to_string(stress(1000000)), "task-08 one million named-let iterations");
    equal(vm, with_stress("(begin (define (even2 n) (if (= n 0) #t (odd2 (- n 1))))"
                          " (define (odd2 n) (if (= n 0) #f (even2 (- n 1)))) (even2 @N@))",
                          200000),
          "#t", "task-08 mutual recursion in tail position");
    equal(vm, "(and #t 4)", "4", "task-08 and returns the last value");
    equal(vm, "(or #f 'x)", "x", "task-08 or returns the first true value");
    equal(vm, "(cond ((= 1 2) 'a) ((= 1 1) 'b) (else 'c))", "b", "task-08 cond");
    equal(vm, "(cond ((assq 'b '((a 1) (b 2))) => cadr) (else 'none))", "2", "task-08 cond arrow");
    equal(vm, "(case 3 ((1 2) 'low) ((3 4) 'high) (else 'other))", "high", "task-08 case");
    equal(vm, "(when #t 1 2)", "2", "task-08 when");
    equal(vm, "(unless #f 1 2)", "2", "task-08 unless");
    equal(vm, "(let* ((x 1) (y (+ x 1))) (list x y))", "(1 2)", "task-08 let*");
    equal(vm, "(letrec ((e (lambda (n) (if (= n 0) #t (o (- n 1)))))"
              " (o (lambda (n) (if (= n 0) #f (e (- n 1)))))) (e 10))",
          "#t", "task-08 letrec");
    equal(vm, "(do ((i 0 (+ i 1)) (acc '() (cons i acc))) ((= i 4) (reverse acc)))", "(0 1 2 3)",
          "task-08 do");
    equal(vm, "`(1 ,(+ 1 1) ,@(list 3 4) 5)", "(1 2 3 4 5)", "task-08 quasiquote");
    equal(vm, "`(a `(b ,(c)))", "(a (quasiquote (b (unquote (c)))))", "task-08 nested quasiquote");
    equal(vm, "((lambda (a . rest) (list a rest)) 1 2 3)", "(1 (2 3))", "task-08 rest arguments");
    equal(vm, "(map + '(1 2) '(10 20))", "(11 22)", "task-08 map over several lists");
    equal(vm, "(apply + 1 '(2 3))", "6", "task-08 apply with leading arguments");
    equal(vm, "(list-sort '(3 1 2) <)", "(1 2 3)", "task-08 sort with a predicate");
    equal(vm, "(let ((x 1)) (set! x 2) x)", "2", "task-08 assignment");
    raises(vm, "(lambda (x x) x)", "duplicate", "task-08 duplicate parameters are rejected");
    raises(vm, "(set! never-bound 1)", "unbound", "task-08 assignment to an unbound name");
    raises(vm, "(undefined-name)", "unbound symbol", "task-08 unbound symbol");
    raises(vm, "(1 2)", "expected procedure", "task-08 calling a non-procedure");

    // Deep environments resolve without recursion.
    vm.eval("(define deep (let build ((n 0) (body ''done)) "
            "(if (= n 400) body (build (+ n 1) (list 'let (list (list 'v n)) body)))))");
    equal(vm, "(eval deep)", "done", "task-08 deeply nested environments");

    equal(vm, "(catch-errors (lambda () (error \"boom\" 42)) (lambda (e) (field-ref e 'code)))",
          "scheme-error", "task-08 error capture");
    equal(vm, "(eval '(+ 1 2))", "3", "task-08 eval with the interaction environment");
    equal(vm, "(eval '(+ 1 2) 'interaction-environment)", "3", "task-08 eval with an explicit environment");
    raises(vm, "(eval '(+ 1 2) 'nowhere)", "unknown evaluation environment",
           "task-08 unknown environment is rejected");
}

static void task_09_capabilities_and_handles(Interpreter& vm) {
    // A missing capability is a structured result, never a silent host call.
    Interpreter bare;
    bare.enable_all_primitives();
    error_code(bare, "(pwd)", "capability-missing", "task-09 missing capability");
    error_code(bare, "(bash '((command \"echo\")))", "capability-missing",
               "task-09 missing shell capability");

    auto denying = std::make_shared<DenyingCapability>();
    Interpreter denied;
    denied.install("filesystem", denying);
    error_code(denied, "(pwd)", "permission-denied", "task-09 denied capability");
    error_code(denied, "(read-file \"/etc/passwd\")", "permission-denied",
               "task-09 denied read cannot reach the host");
    check(denying->calls >= 2, "task-09 denial happens inside the capability");

    // The wrong kind of capability is refused before the host sees anything.
    error_code(vm, "(pwd clock)", "capability-kind", "task-09 capability kind is checked");

    Interpreter misbehaving;
    misbehaving.install("filesystem", std::make_shared<MisbehavingCapability>());
    raises(misbehaving, "(pwd)", "must return a proper list",
           "task-09 host result contract is enforced");

    // Handles: evaluable references, runtime validation, revocation, and forgery.
    Interpreter minting;
    auto minter = std::make_shared<HandleMintingCapability>();
    minting.install("editor", minter);
    const Value minted = minting.eval("(field-ref (capability-call editor 'mint) 'handle)");
    check(minted.type() == Value::Type::Handle, "task-09 capabilities mint handles");
    const std::string written = minting.write(minted);
    check(written.rfind("(handle-ref", 0) == 0,
          "task-09 handles write as an evaluable reference", written);
    check(written.find("#<") == std::string::npos,
          "task-09 handles never print as unreadable tokens", written);
    minting.define("minted", minted);
    equal(minting, "(equal? minted (eval (read-from-string (write-to-string minted))))", "#t",
          "task-09 handle reference resolves to the same handle");

    // A reference with a tampered token is refused.
    std::string forged = written;
    const std::size_t token_at = forged.find("(token \"");
    check(token_at != std::string::npos, "task-09 reference carries a token");
    forged.replace(token_at + 8, 16, "0000000000000000");
    minting.define("forged-source", Value::string(forged));
    error_code(minting, "(eval (read-from-string forged-source))", "invalid-handle",
               "task-09 forged handle token is refused");

    // A reference minted by a different runtime is refused.
    Interpreter other;
    other.enable_all_primitives();
    other.define("foreign-source", Value::string(written));
    error_code(other, "(eval (read-from-string foreign-source))", "invalid-handle",
               "task-09 foreign runtime handle is refused");

    // Closing revokes: the same reference must stop resolving and the host is told.
    minting.eval("(handle-close minted)");
    check(minter->releases == 1, "task-09 closing a handle releases the host resource");
    error_code(minting, "(eval (read-from-string (write-to-string minted)))", "invalid-handle",
               "task-09 stale handle is refused");
    error_code(minting, "(handle-close minted)", "invalid-handle",
               "task-09 double close is refused");

    // Procedures and capabilities report a structured refusal rather than a token.
    const std::string procedure_form = vm.write(vm.eval("car"));
    check(procedure_form.find("#<") == std::string::npos,
          "task-09 procedures avoid unreadable tokens", procedure_form);
    error_code(vm, "(eval (read-from-string (write-to-string car)))", "non-serializable",
               "task-09 procedure transfer is refused");
    equal(vm, "(field-ref (capability-kind (eval (read-from-string (write-to-string clock)))) 'kind)",
          "\"clock\"", "task-09 capability reference resolves in its own runtime");
    error_code(other, "(eval (read-from-string \"(capability-ref \\\"clock\\\" \\\"00\\\")\"))",
               "non-serializable", "task-09 foreign capability reference is refused");
}

static void task_10_processes(Interpreter& vm) {
    equal(vm,
          "(field-ref (with-job (lambda (j) (process-wait j))) 'stdout)", "\"hi\\n\"",
          "task-10 process output is captured");
    equal(vm, "(field-ref (with-job (lambda (j) (process-wait j))) 'exit-status)", "0",
          "task-10 exit status is reported");
    equal(vm,
          "(let* ((h (field-ref (process-start '((program \"cat\"))) 'job))"
          "       (ignore (process-write h \"round trip\"))"
          "       (ignore2 (process-close-input h)))"
          "  (field-ref (process-wait h) 'stdout))",
          "\"round trip\"", "task-10 stdin is delivered to the child");
    equal(vm,
          "(field-ref (let ((h (field-ref (process-start '((program \"false\"))) 'job)))"
          "             (process-wait h)) 'exit-status)",
          "1", "task-10 non-zero exit status");
    // A run that exceeds its deadline is killed and reported, not left behind.
    const Value timed = vm.eval(
        "(let ((h (field-ref (process-start '((program \"sleep\") (arguments (\"30\")))) 'job)))"
        "  (process-wait h '((timeout-ms 150))))");
    check(toolscheme::option(timed, "timed-out").truthy(), "task-10 timeout is reported",
          timed.to_string());
    const Value state = toolscheme::option(timed, "state");
    check(state.type() == Value::Type::Symbol && state.as_symbol() == "cancelled",
          "task-10 a timed-out job is cancelled");
    // Cancellation of a long-running child.
    const Value cancelled = vm.eval(
        "(let ((h (field-ref (process-start '((program \"sleep\") (arguments (\"30\")))) 'job)))"
        "  (process-cancel h) (process-wait h '((timeout-ms 2000))))");
    check(toolscheme::option(cancelled, "signal").as_integer() != 0 ||
              toolscheme::option(cancelled, "exit-status").as_integer() != 0,
          "task-10 cancellation terminates the child", cancelled.to_string());
    error_code(vm, "(process-start '((program \"no-such-program-anywhere\")))", "not-found",
               "task-10 unknown program");

    // Output flooding is bounded by the policy limit rather than exhausting memory.
    Interpreter bounded;
    toolscheme::posix::Policy policy = test_policy();
    policy.output_limit = 4096;
    bounded.install("process", toolscheme::posix::make_process(policy));
    bounded.enable_all_primitives();
    const Value flooded = bounded.eval(
        "(let ((h (field-ref (process-start '((program \"yes\"))) 'job)))"
        "  (process-wait h '((timeout-ms 300))))");
    check(toolscheme::option(flooded, "stdout-truncated").truthy(),
          "task-10 runaway output is truncated", "flood result was not truncated");
    check(toolscheme::option(flooded, "stdout").as_string().size() <= 4096,
          "task-10 truncation respects the configured limit");

    // A process capability that forbids children refuses without forking.
    Interpreter refused;
    toolscheme::posix::Policy closed = test_policy();
    closed.allow_process = false;
    refused.install("process", toolscheme::posix::make_process(closed));
    refused.enable_all_primitives();
    error_code(refused, "(process-start '((program \"echo\")))", "permission-denied",
               "task-10 child processes can be disabled");

    // An executable allowlist is enforced.
    Interpreter restricted;
    toolscheme::posix::Policy listed = test_policy();
    listed.allowed_programs = {"echo"};
    restricted.install("process", toolscheme::posix::make_process(listed));
    restricted.enable_all_primitives();
    check(toolscheme::option(restricted.eval("(process-start '((program \"echo\")))"), "pid")
              .type() == Value::Type::Integer,
          "task-10 allowlisted program runs");
    error_code(restricted, "(process-start '((program \"cat\")))", "permission-denied",
               "task-10 allowlist blocks other programs");
}

static void task_11_to_14_filesystem(Interpreter& vm) {
    equal(vm, "(field-ref (pwd) 'path)", "\".\"", "task-11 working directory");
    equal(vm, "(field-ref (read-file \"a.txt\") 'text)", "\"alpha\\nbeta\\ngamma\\n\"",
          "task-11 read-file");
    equal(vm, "(field-ref (stat \"a.txt\") 'kind)", "file", "task-11 stat reports the kind");
    equal(vm, "(field-ref (stat \"sub\") 'kind)", "directory", "task-11 directory kind");
    equal(vm, "(field-ref (read-file \"a.txt\" '((first-line 2) (last-line 2))) 'text)",
          "\"beta\\n\"", "task-13 line ranges");
    equal(vm, "(field-ref (read-file \"a.txt\" '((byte-offset 6) (byte-count 4))) 'text)",
          "\"beta\"", "task-13 byte ranges");
    error_code(vm, "(read-file \"binary.bin\")", "binary-file", "task-13 binary policy");
    equal(vm, "(field-ref (read-file \"binary.bin\" '((allow-binary #t))) 'bytes)", "9",
          "task-13 binary reads can be authorized");
    equal(vm, "(field-ref (glob \"**/*.txt\") 'count)", "3", "task-13 recursive glob");
    equal(vm, "(field-ref (glob \"*.txt\") 'count)", "2", "task-13 non-recursive glob");
    equal(vm, "(field-ref (glob \"*\" '((kind directory))) 'count)", "1", "task-13 glob by kind");
    equal(vm, "(field-ref (field-ref (tree \".\" '((max-depth 1))) 'tree) 'kind)", "directory",
          "task-13 tree structure");
    equal(vm, "(field-ref (search \"beta\" '(\".\") '((literal #t))) 'count)", "1",
          "task-14 literal search");
    equal(vm, "(field-ref (search \"beta\" '(\".\") '((ignore-case #t))) 'count)", "2",
          "task-14 case-insensitive search");
    equal(vm, "(field-ref (search \"^alpha$\" '(\".\")) 'count)", "2", "task-14 regular expressions");
    equal(vm, "(field-ref (rg \"beta\" '(\".\") '((include (\"a.txt\")))) 'count)", "1",
          "task-14 include filter");
    equal(vm, "(field-ref (rg \"alpha\" '(\".\") '((exclude (\"b.txt\")))) 'count)", "1",
          "task-14 exclude filter");
    equal(vm, "(field-ref (list-ref (field-ref (search \"beta\" '(\".\") '((literal #t))) 'matches) 1) 'line)",
          "2", "task-14 match records carry line numbers");
    error_code(vm, "(search \"[\" '(\".\"))", "invalid-pattern", "task-14 malformed pattern");

    // Round trip a file through the handle API.
    equal(vm,
          "(let* ((h (field-ref (file-open \"handle.txt\" '((mode write))) 'file))"
          "       (w (file-write h \"0123456789\")))"
          "  (file-close h)"
          "  (let* ((r (field-ref (file-open \"handle.txt\") 'file))"
          "         (part (field-ref (file-read-at r 3 '((count 4))) 'text)))"
          "    (file-close r) part))",
          "\"3456\"", "task-11 positioned file handle reads");
    error_code(vm, "(file-read 42)", "invalid-handle", "task-11 file operations validate handles");
}

static void task_15_16_text(Interpreter& vm) {
    equal(vm, "(field-ref (text-lines TEXT) 'count)", "3", "task-15 text-lines");
    equal(vm, "(field-ref (wc TEXT) 'words)", "3", "task-15 wc counts words");
    equal(vm, "(field-ref (wc TEXT) 'bytes)", "17", "task-15 wc counts bytes");
    equal(vm, "(field-ref (head LINES '((count 2))) 'lines)", "(\"alpha\" \"beta\")", "task-15 head");
    equal(vm, "(field-ref (tail LINES '((count 1))) 'lines)", "(\"gamma\")", "task-15 tail");
    equal(vm, "(field-ref (sort '(\"c\" \"a\" \"b\")) 'lines)", "(\"a\" \"b\" \"c\")", "task-15 sort");
    equal(vm, "(field-ref (sort '(\"10\" \"9\") '((numeric #t))) 'lines)", "(\"9\" \"10\")",
          "task-15 numeric sort");
    equal(vm, "(field-ref (uniq '(\"a\" \"a\" \"b\")) 'lines)", "(\"a\" \"b\")", "task-15 uniq");
    equal(vm, "(field-ref (cut '(\"a:b:c\") '((separator \":\") (fields (1 3)) (output-separator \",\"))) 'lines)",
          "(\"a,c\")", "task-15 cut by field");
    equal(vm, "(field-ref (tr \"hello\" \"l\" \"L\") 'text)", "\"heLLo\"", "task-15 tr");
    equal(vm, "(field-ref (tr \"hello\" \"l\" \"\" '((delete #t))) 'text)", "\"heo\"",
          "task-15 tr deletes");
    equal(vm, "(field-ref (text-replace \"a1b22c\" \"[0-9]+\" \"#\") 'replacements)", "2",
          "task-15 regular-expression replace");
    equal(vm, "(field-ref (text-replace \"aaa\" \"a\" \"b\" '((literal #t) (all #f))) 'text)",
          "\"baa\"", "task-15 single literal replace");
    equal(vm, "(field-ref (grep \"alpha\" LINES '((invert #t))) 'count)", "2", "task-15 inverted grep");
    equal(vm, "(field-ref (comm '(\"a\" \"b\") '(\"b\" \"c\")) 'both)", "(\"b\")", "task-15 comm");
    equal(vm, "(field-ref (fold '(\"abcdef\") '((width 2))) 'lines)", "(\"ab\" \"cd\" \"ef\")",
          "task-15 fold");
    equal(vm, "(field-ref (expand '(\"a\\tb\")) 'lines)", "(\"a       b\")", "task-15 expand");
    equal(vm, "(field-ref (xargs '(\"a\" \"b\" \"c\") '((max-arguments 2))) 'count)", "2",
          "task-15 xargs batching");
    equal(vm, "(field-ref (strings \"ab\\x0;cdefgh\") 'count)", "1", "task-15 strings");
    equal(vm, "(field-ref (cmp \"abc\" \"abc\") 'identical)", "#t", "task-15 cmp identical");
    equal(vm, "(field-ref (cmp \"abc\" \"abd\") 'offset)", "3", "task-15 cmp difference offset");

    equal(vm, "(field-ref (diff LINES LINES) 'identical)", "#t", "task-16 identical inputs");
    equal(vm, "(field-ref (diff '(\"a\" \"b\") '(\"a\" \"c\")) 'unified)",
          "\"--- a\\n+++ b\\n@@ -1,2 +1,2 @@\\n a\\n-b\\n+c\\n\"", "task-16 unified output");
    equal(vm, "(field-ref (diff '(\"a\" \"b\") '(\"a\" \"c\")) 'exact)", "#t",
          "task-16 shortest edit script");
    equal(vm, "(field-ref (diff3 '(\"a\") '(\"a\" \"b\") '(\"a\" \"c\")) 'conflicts)", "1",
          "task-16 conflicting three-way merge");
    equal(vm, "(field-ref (diff3 '(\"a\") '(\"a\" \"b\") '(\"a\")) 'clean)", "#t",
          "task-16 clean three-way merge");

    // Atomic patching: a dry run changes nothing, a good patch applies, and a bad
    // hunk is rejected without touching the file.
    equal(vm, "(field-ref (apply-patch PATCH '((dry-run #t))) 'clean)", "#t", "task-16 dry run");
    equal(vm, "(field-ref (read-file \"a.txt\") 'text)", "\"alpha\\nbeta\\ngamma\\n\"",
          "task-16 dry run leaves the file untouched");
    equal(vm, "(field-ref (apply-patch PATCH) 'clean)", "#t", "task-16 patch applies");
    equal(vm, "(field-ref (read-file \"a.txt\") 'text)", "\"alpha\\nBETA\\ngamma\\n\"",
          "task-16 patch changed the file");
    equal(vm, "(field-ref (apply-patch PATCH) 'clean)", "#f",
          "task-16 reapplying the same patch is rejected");
    equal(vm, "(field-ref (read-file \"a.txt\") 'text)", "\"alpha\\nBETA\\ngamma\\n\"",
          "task-16 a rejected patch leaves the file untouched");
    equal(vm,
          "(field-ref (list-ref (field-ref (apply-patch PATCH) 'rejects) 1) 'code)",
          "context-mismatch", "task-16 rejects name the reason");
    // Restore the fixture for later tests.
    vm.eval("(write-file \"a.txt\" \"alpha\\nbeta\\ngamma\\n\")");
}

static void task_17_18_output_and_system(Interpreter& vm) {
    equal(vm, "(field-ref (echo \"a\" \"b\") 'text)", "\"a b\"", "task-17 echo");
    equal(vm, "(field-ref (printf \"%s=%04d\" \"x\" 42) 'text)", "\"x=0042\"", "task-17 printf");
    equal(vm, "(field-ref (printf \"%5.2f|%x|%c\" 3.14159 255 #\\z) 'text)", "\" 3.14|ff|z\"",
          "task-17 printf conversions");
    raises(vm, "(printf \"%d\")", "more conversions than arguments", "task-17 printf arity");
    raises(vm, "(printf \"%q\" 1)", "unsupported printf conversion", "task-17 printf rejects unknown");
    equal(vm, "(field-ref (expr '+ 2 3) 'value)", "5", "task-17 expr arithmetic");
    equal(vm, "(field-ref (expr '< \"a\" \"b\") 'value)", "#t", "task-17 expr string comparison");
    equal(vm, "(field-ref (expr 'substr \"abcdef\" 2 3) 'value)", "\"bcd\"", "task-17 expr substr");
    equal(vm, "(field-ref (expr 'match \"foobar\" \"o+\") 'value)", "2", "task-17 expr match");
    error_code(vm, "(expr '/ 1 0)", "invalid-argument", "task-17 expr division by zero");
    equal(vm, "(field-ref (test 'integer-less 1 2) 'result)", "#t", "task-17 test integer predicate");
    equal(vm, "(field-ref (test 'string-equal \"a\" \"b\") 'result)", "#f", "task-17 test strings");
    equal(vm, "(field-ref (test 'file-exists \"a.txt\") 'result)", "#t",
          "task-17 test filesystem predicate");
    equal(vm, "(field-ref (test 'file-exists \"missing.txt\") 'result)", "#f",
          "task-17 test missing file");
    equal(vm, "(field-ref (test 'directory \"sub\") 'result)", "#t", "task-17 test directory");
    equal(vm, "(field-ref ([ 'string-nonempty \"a\" \"]\") 'result)", "#t",
          "task-17 bracket form shares the implementation");
    equal(vm, "(field-ref (yes \"y\" '((count 3))) 'count)", "3", "task-17 yes is bounded");
    raises(vm, "(yes \"y\" '((count 2000000)))", "one million line limit",
           "task-17 yes refuses unbounded output");

    equal(vm, "(field-ref (date '((epoch-seconds 0))) 'iso)", "\"1970-01-01T00:00:00Z\"",
          "task-17 date is deterministic when given an instant");
    equal(vm, "(field-ref (date '((epoch-seconds 86400) (format \"%Y-%m-%d\"))) 'text)",
          "\"1970-01-02\"", "task-17 date formatting");
    check(toolscheme::option(vm.eval("(uname)"), "system").type() == Value::Type::String,
          "task-18 uname");
    check(toolscheme::option(vm.eval("(id)"), "uid").type() == Value::Type::Integer, "task-18 id");
    equal(vm, "(ok? (which \"sh\"))", "#t", "task-18 which finds a program");
    error_code(vm, "(which \"no-such-program-anywhere\")", "not-found", "task-18 which reports misses");
    equal(vm, "(ok? (ps '((limit 2))))", "#t", "task-18 process listing");
    equal(vm, "(field-ref (wait4path \"a.txt\" '((timeout-ms 100))) 'present)", "#t",
          "task-18 wait4path finds an existing path");
    equal(vm, "(field-ref (wait4path \"never.txt\" '((timeout-ms 60))) 'timed-out)", "#t",
          "task-18 wait4path times out");
    error_code(vm, "(kill -1)", "invalid-argument", "task-18 kill refuses broadcast signals");

    // The environment is capability-filtered.
    Interpreter filtered;
    toolscheme::posix::Policy policy = test_policy();
    policy.environment_allowlist = {"PATH"};
    filtered.install("system", toolscheme::posix::make_system(policy));
    filtered.enable_all_primitives();
    equal(filtered, "(field-ref (env) 'filtered)", "#t", "task-18 environment filtering is reported");
    check(toolscheme::option(filtered.eval("(env)"), "count").as_integer() <= 1,
          "task-18 environment allowlist is enforced");

    // `stty` never silently reaches for the embedding process's terminal.
    Interpreter terminal;
    terminal.install("terminal", toolscheme::posix::make_terminal(test_policy()));
    terminal.enable_all_primitives();
    error_code(terminal, "(stty '())", "invalid-argument",
               "task-18 stty requires an explicit terminal handle");
}

static void task_19_23_adapters(Interpreter& vm) {
    equal(vm, "(field-ref (hash \"hello\" '((algorithm sha256))) 'hex)",
          "\"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\"",
          "task-19 sha256 test vector");
    equal(vm, "(field-ref (hash \"\" '((algorithm sha256))) 'hex)",
          "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"",
          "task-19 sha256 empty input");
    equal(vm, "(field-ref (md5 \"hello\") 'hex)", "\"5d41402abc4b2a76b9719d911017c592\"",
          "task-19 md5 test vector");
    equal(vm, "(field-ref (md5 \"\") 'hex)", "\"d41d8cd98f00b204e9800998ecf8427e\"",
          "task-19 md5 empty input");
    equal(vm, "(field-ref (hash \"abc\" '((algorithm sha1))) 'hex)",
          "\"a9993e364706816aba3e25717850c26c9cd0d89d\"", "task-19 sha1 test vector");
    equal(vm, "(length (field-ref (hash \"hello\" '((algorithm sha256))) 'bytes))", "32",
          "task-19 digests expose raw bytes");
    error_code(vm, "(hash \"x\" '((algorithm md6)))", "unsupported",
               "task-19 unavailable algorithms are refused");
    equal(vm, "(field-ref (base64 \"hello\") 'text)", "\"aGVsbG8=\"", "task-19 base64 encode");
    equal(vm, "(field-ref (base64 \"aGVsbG8=\" '((mode decode))) 'text)", "\"hello\"",
          "task-19 base64 decode");
    equal(vm, "(field-ref (cksum \"hello\") 'hex)", "\"3610a686\"", "task-19 crc32 test vector");

    equal(vm, "(field-ref (field-ref (json-parse \"{\\\"a\\\":[1,2]}\") 'value) \"a\")", "(1 2)",
          "task-20 objects become association lists");
    equal(vm, "(field-ref (json-parse \"{\\\"n\\\":null,\\\"t\\\":true}\") 'value)",
          "((\"n\" null) (\"t\" #t))", "task-20 null and booleans");
    equal(vm, "(field-ref (json-parse \"1.5\") 'value)", "1.5", "task-20 fractional numbers");
    equal(vm, "(field-ref (json-parse \"12345678901234567\") 'value)", "12345678901234567",
          "task-20 large integers stay exact");
    equal(vm, "(field-ref (json-write '((\"a\" (1 2)) (\"b\" null))) 'text)",
          "\"{\\\"a\\\":[1,2],\\\"b\\\":null}\"", "task-20 json-write");
    equal(vm, "(field-ref (json-write '(1 2 3)) 'text)", "\"[1,2,3]\"", "task-20 arrays");
    equal(vm, "(field-ref (json-parse \"\\\"\\\\u00e9\\\"\") 'value)", "\"\xc3\xa9\"",
          "task-20 unicode escapes become UTF-8");
    error_code(vm, "(json-parse \"{\")", "malformed-json", "task-20 malformed input");
    error_code(vm, "(json-parse \"[[[[[[1]]]]]]\" '((max-depth 3)))", "malformed-json",
               "task-20 nesting depth is bounded");

    equal(vm,
          "(let* ((s (field-ref (ed-open \"edit.txt\" '((create #t))) 'session))"
          "       (i (ed-command s '((operation insert) (line 1) (text \"one\\ntwo\"))))"
          "       (b (ed-buffer s)))"
          "  (ed-close s) (field-ref b 'lines))",
          "(\"one\" \"two\")", "task-21 editor insert");
    equal(vm,
          "(let* ((s (field-ref (ed-open \"a.txt\") 'session))"
          "       (r (ed-command s '((operation substitute) (pattern \"beta\") (replacement \"BETA\"))))"
          "       (b (ed-buffer s)))"
          "  (ed-close s) (field-ref r 'changed))",
          "1", "task-21 editor substitute");
    equal(vm,
          "(let* ((s (field-ref (ed-open \"a.txt\") 'session))"
          "       (r (ed-command s '((operation delete) (from 1) (to 1))))"
          "       (b (ed-buffer s)))"
          "  (ed-close s) (field-ref b 'total))",
          "2", "task-21 editor delete");
    equal(vm,
          "(let* ((s (field-ref (ed-open \"written.txt\" '((create #t))) 'session))"
          "       (i (ed-command s '((operation insert) (line 1) (text \"saved\"))))"
          "       (w (ed-write s)))"
          "  (ed-close s) (field-ref (read-file \"written.txt\") 'text))",
          "\"saved\\n\"", "task-21 editor writes through the filesystem capability");
    error_code(vm, "(ed-buffer 42)", "invalid-handle", "task-21 editor validates handles");
    equal(vm,
          "(let ((s (field-ref (ed-open \"a.txt\") 'session)))"
          "  (ed-command s '((operation insert) (line 1) (text \"x\")))"
          "  (field-ref (ed-close s) 'unsaved-changes))",
          "#t", "task-21 unsaved changes are reported on close");

    // Shell dispatch carries the dialect and never embeds a shell.
    const Value shell = vm.eval("(sh '((command \"echo shell-works\")))");
    if (toolscheme::option(shell, "error").type() == Value::Type::String) {
        check(toolscheme::option(shell, "code").as_symbol() == "not-found",
              "task-22 shell dispatch reports a missing interpreter", shell.to_string());
    } else {
        equal(vm, "(field-ref (sh '((command \"echo shell-works\"))) 'dialect)", "sh",
              "task-22 dialect identity is explicit");
        equal(vm,
              "(field-ref (process-wait (field-ref (sh '((command \"echo shell-works\"))) 'job)) 'stdout)",
              "\"shell-works\\n\"", "task-22 shell command output");
    }
    error_code(vm, "(bash '())", "invalid-argument", "task-22 shell requires a command");

    // Platform-specific names keep their real semantics on the other platform.
#if defined(__APPLE__)
    error_code(vm, "(systemctl '((operation list)))", "unsupported",
               "task-23 systemctl is unsupported on Darwin");
    equal(vm, "(ok? (launchctl '((operation list))))", "#t", "task-23 launchctl runs on Darwin");
#else
    error_code(vm, "(launchctl '((operation list)))", "unsupported",
               "task-23 launchctl is unsupported off Darwin");
    equal(vm, "(ok? (systemctl '((operation list))))", "#t", "task-23 systemctl runs on Linux");
#endif
}

static void task_24_registration_groups() {
    // A bare interpreter has only the core group.
    Interpreter core;
    check(core.group_enabled(PrimitiveGroup::Core), "task-24 core group is always enabled");
    check(!core.group_enabled(PrimitiveGroup::Path), "task-24 path group is off by default");
    check(!core.has_primitive("pwd"), "task-24 capability primitives are not registered by default");
    check(core.has_primitive("car"), "task-24 core primitives are registered");

    // Installing a capability registers exactly the groups it backs.
    Interpreter installed;
    installed.install("filesystem", toolscheme::posix::make_filesystem(test_policy()));
    check(installed.has_primitive("pwd"), "task-24 installing a capability registers its group");
    check(installed.has_primitive("glob"), "task-24 repository group follows the filesystem");
    check(installed.has_primitive("ed-open"), "task-24 editor sessions follow the filesystem");
    check(!installed.has_primitive("tar"), "task-24 unrelated groups stay unregistered");
    check(!installed.has_primitive("bash"), "task-24 shell group needs a shell capability");

    // A capability that supports only some operations registers only those names.
    Interpreter narrow;
    narrow.install("archive", std::make_shared<NarrowCapability>());
    check(narrow.has_primitive("tar"), "task-24 supported operations are registered");
    check(!narrow.has_primitive("zip"), "task-24 unsupported operations are skipped");
    check(!narrow.has_primitive("cpio"), "task-24 unsupported operations stay absent");

    // Explicitly enabling a group without a capability still yields the denial path.
    Interpreter forced;
    forced.enable_path_primitives();
    check(forced.has_primitive("pwd"), "task-24 groups can be enabled without a capability");
    const Value denied = forced.eval("(pwd)");
    check(toolscheme::option(denied, "code").as_symbol() == "capability-missing",
          "task-24 an enabled group without a capability reports the miss");

    // Group registration is idempotent.
    const std::size_t before = forced.primitive_names().size();
    forced.enable_path_primitives();
    forced.enable_path_primitives();
    check(forced.primitive_names().size() == before, "task-24 enabling a group twice is a no-op");
}

static void task_25_security(Interpreter& vm) {
    // Traversal, absolute escape, and symlink escape are all refused.
    error_code(vm, "(read-file \"../../etc/passwd\")", "outside-root", "task-25 relative traversal");
    error_code(vm, "(read-file \"/etc/passwd\")", "outside-root", "task-25 absolute escape");
    error_code(vm, "(read-file \"sub/../../../etc/passwd\")", "outside-root",
               "task-25 traversal through a real directory");
    error_code(vm, "(write-file \"../escape.txt\" \"x\")", "outside-root", "task-25 write traversal");
    error_code(vm, "(mkdir \"../escape\")", "outside-root", "task-25 mkdir traversal");
    error_code(vm, "(glob \"*\" '((directory \"../..\")))", "outside-root", "task-25 glob traversal");

    // A symlink pointing outside the root is refused even though the link is inside.
    const std::string link = fixture_root + "/escape-link";
    ::unlink(link.c_str());
    check(::symlink("/etc/passwd", link.c_str()) == 0, "task-25 test symlink created");
    error_code(vm, "(read-file \"escape-link\")", "outside-root", "task-25 symlink escape is refused");
    error_code(vm, "(stat \"escape-link\")", "outside-root",
               "task-25 following an escaping link is refused");
    // Naming the link without dereferencing it stays allowed, and reports the link.
    equal(vm, "(field-ref (stat \"escape-link\" '((follow-symlinks #f))) 'kind)", "symlink",
          "task-25 stat reports the link itself when not following");
    equal(vm, "(ok? (unlink \"escape-link\"))", "#t",
          "task-25 an escaping link can still be removed");
    ::unlink(link.c_str());

    // Embedded NULs never reach a host path.
    error_code(vm, "(read-file \"a.txt\\x0;/../../etc/passwd\")", "invalid-path",
               "task-25 embedded NUL in a path");

    // The capability root itself cannot be removed.
    error_code(vm, "(rmdir \".\")", "permission-denied", "task-25 the root cannot be removed");

    // A read-only filesystem capability refuses mutating operations by not offering them.
    Interpreter readonly;
    toolscheme::posix::Policy policy = test_policy();
    policy.writable = false;
    readonly.install("filesystem", toolscheme::posix::make_filesystem(policy));
    check(readonly.has_primitive("read-file"), "task-25 read-only capability keeps reads");
    check(!readonly.has_primitive("write-file"), "task-25 read-only capability drops writes");
    check(!readonly.has_primitive("rm"), "task-25 read-only capability drops removal");

    // Patch application refuses to escape its root.
    equal(vm,
          "(field-ref (list-ref (field-ref (apply-patch"
          " \"--- a/../../etc/passwd\\n+++ b/../../etc/passwd\\n@@ -1,1 +1,1 @@\\n-a\\n+b\\n\""
          " '((dry-run #t))) 'rejects) 1) 'code)",
          "unsafe-path", "task-25 patch traversal is rejected");

    // Pattern compilation errors are structured, not crashes.
    error_code(vm, "(search \"(\" '(\".\"))", "invalid-pattern", "task-25 malformed search pattern");
    raises(vm, "(grep \"(\" LINES)", "invalid regular expression", "task-25 malformed grep pattern");

    // Malformed data is rejected rather than misparsed.
    error_code(vm, "(json-parse \"[1,]\")", "malformed-json", "task-25 malformed JSON");
    error_code(vm, "(base64 \"@@@@\" '((mode decode)))", "malformed-input", "task-25 malformed base64");
    error_code(vm, "(apply-patch \"@@ -1,1 +1,1 @@\\n-a\\n+b\\n\")", "malformed-patch",
               "task-25 hunk without a file header");
}

static void task_26_documentation(Interpreter& vm) {
    // Snippets reproduced from README.md and docs/api.md must keep working.
    equal(vm, "(eval (read-from-string \"(+ 20 22)\"))", "42", "task-26 README embedding snippet");
    equal(vm, "(eval (read-from-string (write-to-string '(+ 20 22))))", "42",
          "task-26 README generated-code snippet");
    check(vm.write(vm.read("'(generated code)")) == "(quote (generated code))",
          "task-26 quote writes in its canonical form");
    equal(vm, "(field-ref (pwd '((output data))) 'path)", "\".\"", "task-26 roadmap data output mode");
    equal(vm, "(field-ref (pwd '((output source))) 'source)",
          "\"((path \\\".\\\") (root \\\"" + fixture_root + "\\\"))\"",
          "task-26 roadmap source output mode");
    equal(vm, "(field-ref (read-from-string (field-ref (pwd '((output source))) 'source)) 'path)",
          "\".\"", "task-26 roadmap source round trip");
    equal(vm, "(field-ref (eval (field-ref (pwd '((output expression))) 'expression)) 'path)",
          "\".\"", "task-26 expression output mode stays evaluable");
    equal(vm, "(rg \"needle\" '(\"missing-dir\") '((literal #t)))",
          "((error \"no such file or directory\") (code not-found) (operation rg) (path \"missing-dir\"))",
          "task-26 README capability snippet shape");
    raises(vm, "(pwd '((output nonsense)))", "unknown output mode", "task-26 output modes are closed");
}

static void task_28_generated_code(Interpreter& vm) {
    equal(vm, "(eval (read-from-string \"(+ 20 22)\"))", "42", "task-28 read and eval");
    equal(vm, "(equal? '(a (1 . 2)) (read-from-string (write-to-string '(a (1 . 2)))))", "#t",
          "task-28 improper list round trip");
    equal(vm, "(equal? '(1 \"two\" #\\3 4. five ()) "
              "(read-from-string (write-to-string '(1 \"two\" #\\3 4. five ()))))",
          "#t", "task-28 mixed data round trip");

    // Cross-interpreter handoff of pure data.
    Interpreter producer;
    Interpreter consumer;
    const std::string generated = producer.write(producer.eval("'(+ 40 2)"));
    check(consumer.eval(consumer.read(generated)).as_integer() == 42,
          "task-28 cross-interpreter evaluation");
    const std::string data = producer.write(producer.eval("'((path \"/workspace\") (size 12))"));
    consumer.define("transferred", consumer.read(data));
    equal(consumer, "(equal? transferred '((path \"/workspace\") (size 12)))", "#t",
          "task-28 cross-interpreter data transfer");

    // Every write/read/write cycle is stable, including over generated results.
    const Value result = vm.eval("(stat \"a.txt\")");
    check(vm.write(vm.read(vm.write(result))) == vm.write(result),
          "task-28 capability results are canonical");
}

// ---------------------------------------------------------------------------
// Milestone 0 -- the tools agents actually reach for
// ---------------------------------------------------------------------------

static void milestone_0_file_sources(Interpreter& vm) {
    // Counting assertions need a directory this test alone owns; the fixture root
    // accumulates artifacts from the task tests that ran before it.
    vm.eval("(mkdir \"m0/deep\" '((parents #t)))");
    vm.eval("(write-file \"m0/one.txt\" \"alpha\\nbeta\\ngamma\\n\")");
    vm.eval("(write-file \"m0/two.txt\" \"alpha\\ndelta\\n\")");
    vm.eval("(write-file \"m0/deep/three.txt\" \"gamma\\n\")");

    // A tagged source names files; a bare string or list of strings is still
    // inline data, exactly as before.
    equal(vm, "(field-ref (grep \"beta\" '(files \"a.txt\")) 'count)", "1",
          "m0 grep over a named file");
    equal(vm, "(field-ref (list-ref (field-ref (grep \"beta\" '(files \"a.txt\")) 'matches) 1) 'path)",
          "\"a.txt\"", "m0 matches carry their path");
    equal(vm, "(field-ref (grep \"alpha\" '(glob \"m0/*.txt\")) 'count)", "2",
          "m0 grep across a glob");
    equal(vm, "(field-ref (grep \"alpha\" '(glob \"m0/*.txt\")) 'files-scanned)", "2",
          "m0 glob search reports files scanned");
    // Line numbers are per file, not per concatenated stream.
    // `gamma` is line 1 of m0/deep/three.txt and line 3 of m0/one.txt. Both report
    // their own file's numbering, not an offset into a concatenated stream.
    equal(vm, "(map (lambda (m) (list (field-ref m 'path) (field-ref m 'line)))"
              "     (field-ref (grep \"gamma\" '(glob \"m0/**/*.txt\")) 'matches))",
          "((\"m0/deep/three.txt\" 1) (\"m0/one.txt\" 3))",
          "m0 line numbers are relative to their own file");
    equal(vm, "(field-ref (grep \"alpha\" '(glob \"m0/*.txt\") '((files-with-matches #t))) 'count)", "2",
          "m0 grep -l collapses to distinct files");
    equal(vm, "(field-ref (grep \"beta\" '(\"alpha\" \"beta\")) 'count)", "1",
          "m0 inline data still works");
    // One unreadable path is a record, not an aborted call.
    equal(vm, "(field-ref (grep \"x\" '(files \"missing.txt\")) 'count)", "0",
          "m0 a missing file does not abort the search");
    check(toolscheme::option(vm.eval("(grep \"x\" '(files \"missing.txt\"))"), "unreadable")
              .is_list(),
          "m0 unreadable paths are reported structurally");

    equal(vm, "(field-ref (head '(files \"a.txt\") '((count 2))) 'lines)", "(\"alpha\" \"beta\")",
          "m0 head over a file");
    equal(vm, "(field-ref (tail '(files \"a.txt\") '((count 1))) 'lines)", "(\"gamma\")",
          "m0 tail over a file");
    equal(vm, "(field-ref (sort '(files \"a.txt\")) 'lines)", "(\"alpha\" \"beta\" \"gamma\")",
          "m0 sort over a file");
    // wc counts real bytes, including the trailing newline, and breaks down per file.
    equal(vm, "(field-ref (wc '(files \"a.txt\")) 'lines)", "3", "m0 wc line count");
    equal(vm, "(field-ref (wc '(files \"a.txt\")) 'bytes)", "17", "m0 wc byte count");
    equal(vm, "(field-ref (list-ref (field-ref (wc '(glob \"m0/*.txt\")) 'files) 1) 'path)",
          "\"m0/one.txt\"", "m0 wc breaks down per file");
    equal(vm, "(field-ref (wc TEXT) 'bytes)", "17", "m0 wc on inline text is unchanged");
}

static void milestone_0_working_directory(Interpreter& vm) {
    equal(vm, "(field-ref (pwd) 'path)", "\".\"", "m0 starts at the root");
    equal(vm, "(field-ref (cd \"sub\") 'path)", "\"sub\"", "m0 cd moves the working directory");
    equal(vm, "(field-ref (pwd) 'path)", "\"sub\"", "m0 pwd follows cd");
    // Relative paths now resolve against the new directory.
    equal(vm, "(field-ref (read-file \"c.txt\") 'text)", "\"nested\\n\"",
          "m0 relative paths resolve against the working directory");
    equal(vm, "(field-ref (cd \"..\") 'path)", "\".\"", "m0 cd back up");
    error_code(vm, "(cd \"../../etc\")", "outside-root", "m0 cd cannot escape the root");
    error_code(vm, "(cd \"a.txt\")", "not-a-directory", "m0 cd rejects a file");
}

static void milestone_0_find_predicates(Interpreter& vm) {
    // -name matches the basename; the glob pattern matches the whole relative path.
    equal(vm, "(field-ref (find \"\" '((directory \"m0\") (name \"three.txt\"))) 'count)", "1",
          "m0 find -name");
    equal(vm, "(field-ref (find \"\" '((directory \"m0\") (name \"*.txt\") (kind file))) 'count)", "3",
          "m0 find -name with -type");
    equal(vm, "(field-ref (find \"\" '((directory \"m0\") (kind directory))) 'count)", "1",
          "m0 find -type d");
    equal(vm, "(field-ref (find \"\" '((directory \"m0\") (empty #t))) 'count)", "0",
          "m0 find -empty");
    equal(vm, "(field-ref (find \"\" '((directory \"m0\") (kind file) (max-size 8))) 'count)", "1",
          "m0 find bounded by size");
}

static void milestone_0_sed(Interpreter& vm) {
    equal(vm, "(field-ref (sed '(\"aaa\") '((operation substitute) (pattern \"a\") (replacement \"X\"))) 'lines)",
          "(\"XXX\")", "m0 sed substitutes every occurrence by default");
    equal(vm, "(field-ref (sed '(\"aaa\") '((operation substitute) (pattern \"a\") (replacement \"X\") (all #f))) 'lines)",
          "(\"Xaa\")", "m0 sed can substitute only the first");
    equal(vm, "(field-ref (sed LINES '((operation print) (from 2) (to 3))) 'lines)",
          "(\"beta\" \"gamma\")", "m0 sed -n range print");
    equal(vm, "(field-ref (sed LINES '((operation delete) (from 1) (to 1))) 'lines)",
          "(\"beta\" \"gamma\")", "m0 sed deletes a range");
    equal(vm, "(field-ref (sed '(\"one\") '((operation insert) (line 1) (text \"zero\"))) 'lines)",
          "(\"zero\" \"one\")", "m0 sed inserts before a line");
    // Steps compose in order.
    equal(vm, "(field-ref (sed '(\"a\" \"b\") (list '((operation substitute) (pattern \"a\") (replacement \"z\"))"
              " '((operation delete) (from 2) (to 2)))) 'lines)",
          "(\"z\")", "m0 sed applies steps in order");
    error_code(vm, "(sed LINES '((operation nope)))", "invalid-argument",
               "m0 sed rejects unknown operations");
    // In-place editing is only meaningful against a single file.
    error_code(vm, "(sed LINES '((operation delete) (from 1) (to 1)) '((in-place #t)))",
               "invalid-argument", "m0 sed refuses in-place edits on inline data");
    equal(vm,
          "(begin (write-file \"edit-me.txt\" \"one\\ntwo\\n\")"
          " (sed '(files \"edit-me.txt\") '((operation substitute) (pattern \"one\") (replacement \"ONE\"))"
          "      '((in-place #t)))"
          " (field-ref (read-file \"edit-me.txt\") 'text))",
          "\"ONE\\ntwo\\n\"", "m0 sed writes back in place");
}

static void milestone_0_stability(Interpreter& vm) {
    // The contract that keeps an agent's prompt cache warm: repeating a call must
    // produce byte-identical output.
    for (const char* call : {"(ls \"m0\")", "(stat \"a.txt\")", "(glob \"m0/*.txt\")",
                             "(tree \"m0\")", "(find \"\" '((directory \"m0\") (name \"*.txt\")))"}) {
        const std::string first = vm.write(vm.eval(call));
        const std::string second = vm.write(vm.eval(call));
        check(first == second, std::string("m0 stable output: ") + call);
    }
    // Volatile host metadata is omitted by default and available on request.
    check(toolscheme::option(vm.eval("(stat \"a.txt\")"), "modified").type() ==
              Value::Type::Unspecified,
          "m0 stat omits modification time by default");
    check(toolscheme::option(vm.eval("(stat \"a.txt\" '((volatile #t)))"), "modified").type() ==
              Value::Type::Integer,
          "m0 volatile fields are available on request");
    check(toolscheme::option(vm.eval("(stat \"a.txt\" '((volatile #t)))"), "inode").type() ==
              Value::Type::Integer,
          "m0 inode is available on request");
    // Projection narrows to exactly the named fields.
    equal(vm, "(stat \"a.txt\" '((fields (path kind size))))",
          "((path \"a.txt\") (kind file) (size 17))", "m0 fields projection");
    // Records nested inside list fields inherit the contract.
    check(vm.write(vm.eval("(ls \"m0\")")).find("(modified ") == std::string::npos,
          "m0 nested records are filtered too");
    check(vm.write(vm.eval("(ls \"m0\" '((volatile #t)))")).find("(modified ") != std::string::npos,
          "m0 nested records honour the volatile opt-in");
}

static void milestone_0_git(Interpreter& vm) {
    // The typed front end reports what it supports rather than passing bad input to git.
    error_code(vm, "(git 'nonsense)", "invalid-argument", "m0 git rejects unknown operations");
    check(vm.has_primitive("git"), "m0 git registers with the process capability");
    // Installing only a filesystem capability must not bring git along.
    Interpreter files_only;
    files_only.install("filesystem", toolscheme::posix::make_filesystem(test_policy()));
    check(!files_only.has_primitive("git"), "m0 git needs the process capability");
}

// ---------------------------------------------------------------------------
// Registry completeness (roadmap item 27)
// ---------------------------------------------------------------------------

static void registry_completeness(Interpreter& vm) {
    // The core group defines the language procedures; everything else is a utility
    // and must obey the proper-list result contract.
    Interpreter core_only;
    const std::vector<std::string> core_names = core_only.primitive_names();
    const std::set<std::string> core(core_names.begin(), core_names.end());

    std::map<std::string, const Example*> table;
    for (const Example& example : kExamples) {
        if (!table.emplace(example.name, &example).second)
            check(false, "task-27 duplicate example row", example.name);
    }

    const std::vector<std::string> registered = vm.primitive_names();
    const std::set<std::string> registry(registered.begin(), registered.end());
    for (const std::string& name : registered) {
        if (name == "square") continue; // defined by task-02
        if (!table.count(name))
            check(false, "task-27 every registered primitive needs an example", name);
    }
    for (const auto& entry : table)
        if (!registry.count(entry.first))
            check(false, "task-27 example names an unregistered primitive", entry.first);

    int without_failure = 0;
    for (const auto& entry : table) {
        const std::string& name = entry.first;
        const Example& example = *entry.second;
        if (!registry.count(name)) continue;
        const bool utility = core.count(name) == 0;

        // Success example.
        try {
            const Value result = vm.eval(example.success);
            if (utility)
                check(result.is_list(), "task-27 utility success result is a proper list",
                      name + " -> " + result.to_string());
            check(vm.write(vm.read(vm.write(result))) == vm.write(result),
                  "task-27 success result is canonical", name + " -> " + vm.write(result));
            if (!contains_runtime_reference(result))
                check(vm.read(vm.write(result)).to_string() == result.to_string(),
                      "task-27 success result round trips", name);
            const bool errored = toolscheme::option(result, "error").type() == Value::Type::String;
            const Value code = toolscheme::option(result, "code");
            const bool code_tolerated =
                code.type() == Value::Type::Symbol &&
                (code.as_symbol() == "not-found" || code.as_symbol() == "host-error");
            const bool tolerated = error_is_correct().count(name) ||
                                   (host_dependent().count(name) && code_tolerated);
            if (errored && !tolerated)
                check(false, "task-27 success example must succeed",
                      name + " -> " + result.to_string());
        } catch (const std::exception& error) {
            check(false, "task-27 success example must not raise",
                  name + ": " + error.what() + " [" + example.success + "]");
        }

        // Failure example.
        if (!example.failure) { ++without_failure; continue; }
        bool reported = false;
        try {
            const Value result = vm.eval(example.failure);
            reported = result.is_list() &&
                       toolscheme::option(result, "error").type() == Value::Type::String;
            if (reported)
                check(vm.write(vm.read(vm.write(result))) == vm.write(result),
                      "task-27 failure result is canonical", name);
        } catch (const toolscheme::Error&) {
            reported = true;
        } catch (const std::exception& error) {
            check(false, "task-27 failure example raised a foreign exception",
                  name + ": " + error.what());
            reported = true;
        }
        check(reported, "task-27 failure example must report an error",
              std::string(name) + " [" + example.failure + "]");
    }
    std::cout << "  registry: " << registered.size() << " primitives, " << table.size()
              << " examples, " << without_failure << " with no reachable failure mode\n";
}

// ---------------------------------------------------------------------------

int main() {
    build_fixture();

    Interpreter vm;
    install_test_capabilities(vm);
    vm.eval(kFixtureDefinitions, "fixture.scm");

    task_01_published_semantics(vm);
    task_02_value_representation(vm);
    task_03_ownership();
    task_04_strings(vm);
    task_05_lists(vm);
    task_06_reader(vm);
    task_07_numbers(vm);
    task_08_evaluator(vm);
    task_09_capabilities_and_handles(vm);
    task_10_processes(vm);
    task_11_to_14_filesystem(vm);
    task_15_16_text(vm);
    task_17_18_output_and_system(vm);
    task_19_23_adapters(vm);
    task_24_registration_groups();
    task_25_security(vm);
    task_26_documentation(vm);
    task_28_generated_code(vm);
    milestone_0_file_sources(vm);
    milestone_0_working_directory(vm);
    milestone_0_find_predicates(vm);
    milestone_0_sed(vm);
    milestone_0_stability(vm);
    milestone_0_git(vm);
    registry_completeness(vm);

    remove_fixture();
    if (failures) {
        std::cerr << failures << " of " << checks << " checks failed\n";
        return 1;
    }
    std::cout << "toolscheme: " << checks << " checks passed ("
              << vm.primitive_names().size() << " primitives)\n";
    return 0;
}
