// Deterministic property fuzzer.
//
// No external fuzzing engine is required: a seeded generator drives every target so
// a failure reproduces from its seed alone. Pass a seed and an iteration count to
// explore further, for example `./toolscheme_fuzz 12345 200000`.
//
// The invariant under test is always the same shape: malformed input must produce a
// structured error or a thrown toolscheme::Error, never a crash, a hang, or a silent
// misparse, and well-formed input must survive a write/read/write cycle unchanged.

#include "toolscheme.hpp"
#include "toolscheme_posix.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

using toolscheme::Interpreter;
using toolscheme::Value;

namespace {

int failures = 0;

void report(const std::string& target, const std::string& detail, const std::string& input) {
    ++failures;
    std::cerr << "FUZZ FAIL [" << target << "] " << detail << "\n  input: ";
    for (const unsigned char c : input) {
        if (c >= 32 && c < 127) std::cerr << static_cast<char>(c);
        else std::cerr << "\\x" << std::hex << static_cast<unsigned>(c) << std::dec << ';';
    }
    std::cerr << '\n';
}

std::string random_bytes(std::mt19937_64& engine, std::size_t limit) {
    // A mixed alphabet: Scheme punctuation appears often enough to reach deep parser
    // states, and raw bytes cover the rest.
    static const char* interesting = "()\"'`,.#\\;|[]{} \t\n0123456789abcdefxXeE+-";
    const std::size_t length = engine() % limit;
    std::string out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        if (engine() % 4) out += interesting[engine() % std::strlen(interesting)];
        else out += static_cast<char>(engine() % 256);
    }
    return out;
}

std::string random_json(std::mt19937_64& engine, std::size_t limit) {
    static const char* interesting = "{}[]\":,truefalsnl0123456789.eE+- \t\n\\u";
    const std::size_t length = engine() % limit;
    std::string out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) out += interesting[engine() % std::strlen(interesting)];
    return out;
}

// Builds a random readable datum directly, so the writer round trip is exercised on
// values the reader is guaranteed to accept.
Value random_datum(std::mt19937_64& engine, int depth) {
    switch (engine() % (depth > 3 ? 8u : 10u)) {
    case 0: return Value::integer(static_cast<std::int64_t>(engine()));
    case 1: {
        double bits;
        const std::uint64_t raw = engine();
        std::memcpy(&bits, &raw, sizeof bits);
        if (!std::isfinite(bits)) return Value::integer(0);
        return Value::real(bits);
    }
    case 2: return Value::boolean((engine() & 1) != 0);
    case 3: return Value::character(static_cast<char>(engine() % 256));
    case 4: return Value::nil();
    case 5: {
        std::string text;
        const std::size_t length = engine() % 24;
        for (std::size_t i = 0; i < length; ++i) text += static_cast<char>(engine() % 256);
        return Value::string(text);
    }
    case 6: {
        // Includes names that must be written in |...| form to read back correctly.
        static const char* names[] = {"alpha", "beta", "+", "a->b", "x1", "[", "...", "nil?",
                                      "", ".", "a b", "42", "1.5", "has(paren)", "semi;colon",
                                      "quote\"mark", "pipe|char", "#t", "#\\a", "'tick", "`back"};
        return Value::symbol(names[engine() % 21]);
    }
    case 7: return Value::unspecified();
    case 8: {
        std::vector<Value> items;
        const std::size_t count = engine() % 5;
        for (std::size_t i = 0; i < count; ++i) items.push_back(random_datum(engine, depth + 1));
        return Value::list(std::move(items));
    }
    default: {
        std::vector<Value> items;
        const std::size_t count = 1 + engine() % 4;
        for (std::size_t i = 0; i < count; ++i) items.push_back(random_datum(engine, depth + 1));
        return Value::improper(std::move(items), random_datum(engine, depth + 2));
    }
    }
}

std::vector<std::string> random_lines(std::mt19937_64& engine, std::size_t limit) {
    static const char* words[] = {"alpha", "beta", "gamma", "delta", "", "x", "long line of text"};
    const std::size_t count = engine() % limit;
    std::vector<std::string> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) out.emplace_back(words[engine() % 7]);
    return out;
}

Value lines_value(const std::vector<std::string>& lines) {
    std::vector<Value> out;
    out.reserve(lines.size());
    for (const std::string& line : lines) out.push_back(Value::string(line));
    return Value::list(std::move(out));
}

// ---------------------------------------------------------------------------

void fuzz_reader(Interpreter& vm, std::mt19937_64& engine) {
    const std::string input = random_bytes(engine, 200);
    Value parsed;
    try {
        parsed = vm.read(input, "fuzz");
    } catch (const toolscheme::Error&) {
        return; // A rejected malformed datum is the expected outcome.
    } catch (const std::exception& error) {
        report("reader", std::string("non-toolscheme exception: ") + error.what(), input);
        return;
    }
    // Anything the reader accepted must survive a write/read/write cycle unchanged.
    const std::string written = vm.write(parsed);
    try {
        if (vm.write(vm.read(written, "fuzz")) != written)
            report("reader", "write/read/write is unstable: " + written, input);
    } catch (const std::exception& error) {
        report("reader", std::string("canonical output failed to re-read: ") + error.what(),
               written);
    }
}

void fuzz_writer(Interpreter& vm, std::mt19937_64& engine) {
    const Value value = random_datum(engine, 0);
    const std::string written = vm.write(value);
    try {
        const Value again = vm.read(written, "fuzz");
        if (vm.write(again) != written)
            report("writer", "round trip changed the canonical form", written);
        vm.define("fuzz-left", value);
        vm.define("fuzz-right", again);
        if (!vm.eval("(equal? fuzz-left fuzz-right)").truthy())
            report("writer", "round trip is not equal?", written);
    } catch (const std::exception& error) {
        report("writer", std::string("writer produced unreadable output: ") + error.what(), written);
    }
}

void fuzz_evaluator(Interpreter& vm, std::mt19937_64& engine) {
    const std::string input = random_bytes(engine, 120);
    try {
        vm.eval(input, "fuzz");
    } catch (const toolscheme::Error&) {
    } catch (const std::exception& error) {
        report("evaluator", std::string("non-toolscheme exception: ") + error.what(), input);
    }
}

void fuzz_json(Interpreter& vm, std::mt19937_64& engine) {
    const std::string input = random_json(engine, 160);
    vm.define("fuzz-json", Value::string(input));
    Value result;
    try {
        result = vm.eval("(json-parse fuzz-json)");
    } catch (const std::exception& error) {
        report("json", std::string("parser raised: ") + error.what(), input);
        return;
    }
    if (!result.is_list()) { report("json", "result is not a proper list", input); return; }
    const Value parsed = toolscheme::option(result, "value");
    if (parsed.type() == Value::Type::Unspecified) return; // A structured rejection.
    // Anything accepted must serialize and parse back to the same value.
    vm.define("fuzz-value", parsed);
    try {
        if (!vm.eval("(equal? fuzz-value (field-ref (json-parse (field-ref (json-write fuzz-value)"
                     " 'text)) 'value))")
                 .truthy())
            report("json", "serialize/parse round trip changed the value", input);
    } catch (const std::exception& error) {
        report("json", std::string("round trip raised: ") + error.what(), input);
    }
}

void fuzz_structured_requests(Interpreter& vm, std::mt19937_64& engine) {
    // Random option records fed to primitives that accept them. Every outcome must be
    // a proper list or a thrown toolscheme::Error.
    static const char* targets[] = {"head", "tail", "sort", "uniq", "cut", "grep", "fold",
                                    "nl", "split", "strings", "xargs", "wc", "text-lines",
                                    "text-fields", "diff", "json-write", "base64", "cksum"};
    static const char* keys[] = {"count", "limit", "width", "lines", "separator", "fields",
                                 "reverse", "numeric", "ignore-case", "invert", "output",
                                 "max-arguments", "tab-stop", "context", "mode", "start"};
    std::vector<Value> options;
    const std::size_t entries = engine() % 4;
    for (std::size_t i = 0; i < entries; ++i)
        options.push_back(Value::list({Value::symbol(keys[engine() % 16]),
                                       random_datum(engine, 2)}));
    vm.define("fuzz-options", Value::list(std::move(options)));
    vm.define("fuzz-input", lines_value(random_lines(engine, 6)));
    const std::string target = targets[engine() % 18];
    const std::string call = "(" + target + " fuzz-input fuzz-options)";
    try {
        const Value result = vm.eval(call);
        if (!result.is_list()) report("requests", target + " returned a non-list", call);
        else if (vm.write(vm.read(vm.write(result), "fuzz")) != vm.write(result))
            report("requests", target + " result is not canonical", vm.write(result));
    } catch (const toolscheme::Error&) {
    } catch (const std::exception& error) {
        report("requests", target + " raised a foreign exception: " + error.what(), call);
    }
}

void fuzz_patch(Interpreter& vm, std::mt19937_64& engine) {
    // Random patch-shaped text must never be applied blindly.
    static const char* pieces[] = {"--- a/x\n", "+++ b/x\n", "@@ -1,1 +1,1 @@\n", "-a\n",
                                   "+b\n", " c\n", "\\ No newline\n", "@@\n", "--- /dev/null\n",
                                   "+++ /dev/null\n", "--- a/../../escape\n", "garbage\n"};
    std::string patch;
    const std::size_t count = engine() % 10;
    for (std::size_t i = 0; i < count; ++i) patch += pieces[engine() % 12];
    vm.define("fuzz-patch", Value::string(patch));
    try {
        const Value result = vm.eval("(apply-patch fuzz-patch '((dry-run #t)))");
        if (!result.is_list()) report("patch", "result is not a proper list", patch);
    } catch (const toolscheme::Error&) {
    } catch (const std::exception& error) {
        report("patch", std::string("foreign exception: ") + error.what(), patch);
    }
}

// The strongest available property: a diff between two line sets, rendered as a
// unified patch, must apply cleanly and reproduce the target exactly.
void fuzz_diff_patch_roundtrip(Interpreter& vm, std::mt19937_64& engine) {
    const std::vector<std::string> before = random_lines(engine, 12);
    const std::vector<std::string> after = random_lines(engine, 12);
    vm.define("fuzz-before", lines_value(before));
    vm.define("fuzz-after", lines_value(after));
    try {
        const Value diff = vm.eval("(diff fuzz-before fuzz-after '((old-label \"a/r.txt\")"
                                   " (new-label \"b/r.txt\")))");
        if (toolscheme::option(diff, "identical").truthy()) return;
        if (!toolscheme::option(diff, "exact").truthy()) return; // Degraded to a replace.
        std::string original;
        for (const std::string& line : before) original += line + "\n";
        std::string expected;
        for (const std::string& line : after) expected += line + "\n";
        vm.define("fuzz-original", Value::string(original));
        vm.define("fuzz-unified", toolscheme::option(diff, "unified"));
        vm.eval("(write-file \"r.txt\" fuzz-original)");
        const Value applied = vm.eval("(apply-patch fuzz-unified)");
        if (!toolscheme::option(applied, "clean").truthy()) {
            report("diff-patch", "generated patch did not apply cleanly: " + vm.write(applied),
                   vm.write(toolscheme::option(diff, "unified")));
            return;
        }
        vm.define("fuzz-expected", Value::string(expected));
        if (!vm.eval("(equal? fuzz-expected (field-ref (read-file \"r.txt\") 'text))").truthy())
            report("diff-patch", "patched file does not match the target",
                   vm.write(toolscheme::option(diff, "unified")));
    } catch (const std::exception& error) {
        report("diff-patch", std::string("raised: ") + error.what(), "");
    }
}

void fuzz_handles(Interpreter& vm, std::mt19937_64& engine) {
    // Handle references assembled from random parts must never resolve.
    std::vector<Value> reference;
    static const char* names[] = {"runtime", "capability", "kind", "index", "generation", "token"};
    for (int i = 0; i < 6; ++i)
        reference.push_back(Value::list({Value::symbol(names[i]), random_datum(engine, 2)}));
    vm.define("fuzz-reference", Value::list(std::move(reference)));
    try {
        const Value result = vm.eval("(apply handle-ref fuzz-reference)");
        if (result.type() == Value::Type::Handle)
            report("handles", "a random reference resolved to a live handle",
                   vm.write(vm.eval("fuzz-reference")));
        else if (!result.is_list())
            report("handles", "rejection is not a proper list", "");
    } catch (const toolscheme::Error&) {
    } catch (const std::exception& error) {
        report("handles", std::string("foreign exception: ") + error.what(), "");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20260915u;
    const std::size_t rounds = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 20000u;

    char pattern[] = "/tmp/toolscheme-fuzzXXXXXX";
    const char* root = ::mkdtemp(pattern);
    if (!root) {
        std::cerr << "FATAL: cannot create a fuzzing root\n";
        return 2;
    }

    Interpreter vm;
    toolscheme::posix::Policy policy;
    policy.root = root;
    toolscheme::posix::install_all(vm, policy);

    std::mt19937_64 engine(seed);
    for (std::size_t i = 0; i < rounds && failures < 20; ++i) {
        fuzz_reader(vm, engine);
        fuzz_writer(vm, engine);
        fuzz_evaluator(vm, engine);
        fuzz_json(vm, engine);
        fuzz_structured_requests(vm, engine);
        fuzz_patch(vm, engine);
        fuzz_handles(vm, engine);
        if (i % 20 == 0) fuzz_diff_patch_roundtrip(vm, engine);
        // Keep the environment from growing without bound across rounds.
        if (i % 500 == 0) vm.collect();
    }

    std::string command = "rm -rf '";
    command += root;
    command += "'";
    if (std::system(command.c_str()) != 0) std::cerr << "warning: fuzzing root not removed\n";

    if (failures) {
        std::cerr << failures << " fuzz failures (seed " << seed << ")\n";
        return 1;
    }
    std::cout << "toolscheme fuzz: " << rounds << " rounds clean (seed " << seed << ")\n";
    return 0;
}
