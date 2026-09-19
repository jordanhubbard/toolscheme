// Benchmarks and enforced performance targets.
//
// Each roadmap performance target is asserted, not merely measured: allocation counts
// come from an interposed global operator new, complexity claims are checked by
// comparing timings across input sizes, and stack growth during tail recursion is
// measured from the address of a local in the deepest frame.

#include "toolscheme.hpp"
#include "toolscheme_posix.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <vector>
#include <unistd.h>

using toolscheme::Interpreter;
using toolscheme::Value;

namespace {

std::atomic<std::size_t> allocations{0};
bool counting = false;

} // namespace

void* operator new(std::size_t size) {
    if (counting) allocations.fetch_add(1, std::memory_order_relaxed);
    void* memory = std::malloc(size ? size : 1);
    if (!memory) throw std::bad_alloc();
    return memory;
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

int failures = 0;

void require(bool condition, const std::string& target, const std::string& detail) {
    if (condition) return;
    ++failures;
    std::cerr << "TARGET MISSED: " << target << ": " << detail << '\n';
}

std::size_t count_allocations(const std::function<void()>& body) {
    allocations.store(0);
    counting = true;
    body();
    counting = false;
    return allocations.load();
}

double milliseconds(const std::function<void()>& body) {
    const auto start = std::chrono::steady_clock::now();
    body();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

// Complexity comparisons need warm pages and more than one scheduler sample.
// Absolute timings below remain useful observations, but a single descheduling
// event must not be mistaken for a change in algorithmic complexity.
double median_milliseconds(const std::function<void()>& body) {
    body();
    double samples[] = {milliseconds(body), milliseconds(body), milliseconds(body)};
    std::sort(samples, samples + 3);
    return samples[1];
}

void report(const std::string& name, double ms, double operations) {
    const double per = operations > 0 ? ms * 1e6 / operations : 0;
    std::printf("  %-34s %9.3f ms  %10.1f ns/op\n", name.c_str(), ms, per);
}

// ---------------------------------------------------------------------------

void bench_scalars() {
    // Target: no heap allocation for immediate scalar values.
    const std::size_t scalar_allocations = count_allocations([] {
        volatile std::int64_t sink = 0;
        for (int i = 0; i < 100000; ++i) {
            const Value integer = Value::integer(i);
            const Value real = Value::real(i * 0.5);
            const Value flag = Value::boolean((i & 1) != 0);
            const Value character = Value::character(static_cast<char>(i));
            const Value empty = Value::nil();
            sink += integer.as_integer() + static_cast<std::int64_t>(real.as_float()) +
                    (flag.as_boolean() ? 1 : 0) + character.as_character() +
                    (empty.is_nil() ? 1 : 0);
        }
        (void)sink;
    });
    require(scalar_allocations == 0, "no heap allocation for immediate scalars",
            std::to_string(scalar_allocations) + " allocations for 500000 scalar values");

    const double ms = milliseconds([] {
        volatile std::int64_t sink = 0;
        for (int i = 0; i < 2000000; ++i) sink += Value::integer(i).as_integer();
        (void)sink;
    });
    report("scalar construction", ms, 2000000);
}

void bench_list_construction() {
    // Target: one aggregate allocation for a parsed proper list, and linear-time bulk
    // construction.
    Interpreter vm;
    const std::size_t parse_allocations = count_allocations([&vm] {
        vm.read("(1 2 3 4 5 6 7 8 9 10)");
    });
    require(parse_allocations <= 4, "parsed proper list uses few allocations",
            std::to_string(parse_allocations) + " allocations for a ten-element list");

    std::vector<Value> elements;
    for (int i = 0; i < 1000; ++i) elements.push_back(Value::integer(i));
    const std::size_t build_allocations = count_allocations([&elements] {
        const Value built = Value::list(elements);
        (void)built;
    });
    require(build_allocations <= 4, "bulk builder allocates once per list",
            std::to_string(build_allocations) + " allocations for a thousand-element list");

    const auto build = [](std::size_t count) {
        std::vector<Value> values;
        values.reserve(count);
        for (std::size_t i = 0; i < count; ++i) values.push_back(Value::integer(
            static_cast<std::int64_t>(i)));
        return median_milliseconds([&values] {
            for (int repeat = 0; repeat < 5; ++repeat) {
                const Value built = Value::list(values);
                (void)built;
            }
        });
    };
    // Both inputs exceed the cache of the Intel release runners. Comparing a
    // cache-resident input against a streaming one measures two memory regimes.
    const double small = build(1000000);
    const double large = build(4000000);
    report("bulk list construction (1M x5)", small, 5000000);
    report("bulk list construction (4M x5)", large, 20000000);
    // Four times the elements in at most eight times the wall clock is linear enough
    // to distinguish from any quadratic behaviour.
    require(large < small * 8 + 5, "bulk list construction is linear",
            "1M took " + std::to_string(small) + " ms, 4M took " + std::to_string(large) + " ms");
}

void bench_list_access() {
    // Targets: O(1) length, O(1) random access, O(1) cdr views.
    std::vector<Value> small_values, large_values;
    for (int i = 0; i < 1000; ++i) small_values.push_back(Value::integer(i));
    for (int i = 0; i < 1000000; ++i) large_values.push_back(Value::integer(i));
    const Value small = Value::list(small_values);
    const Value large = Value::list(large_values);

    const auto length_time = [](const Value& list) {
        return median_milliseconds([&list] {
            volatile std::size_t sink = 0;
            for (int i = 0; i < 200000; ++i) sink += list.list_size();
            (void)sink;
        });
    };
    const double small_length = length_time(small);
    const double large_length = length_time(large);
    report("list_size (1k elements)", small_length, 200000);
    report("list_size (1M elements)", large_length, 200000);
    require(large_length < small_length * 3 + 5, "list length is O(1)",
            "1k took " + std::to_string(small_length) + " ms, 1M took " +
                std::to_string(large_length) + " ms");

    const auto index_time = [](const Value& list, std::size_t span) {
        const std::size_t base = list.list_size() - span;
        return median_milliseconds([&list, span, base] {
            volatile std::int64_t sink = 0;
            for (std::size_t i = 0; i < 200000; ++i)
                sink += list.list_at(base + (i * 7919) % span).as_integer();
            (void)sink;
        });
    };
    const double small_index = index_time(small, 1000);
    // Equal working sets, at the end of each list. A traversal or a prefix copy
    // still scales with list length, but CPU cache misses no longer masquerade
    // as an O(n) implementation. Report full-span latency separately below.
    const double large_index = index_time(large, 1000);
    report("list_at (1k elements)", small_index, 200000);
    report("list_at (1M elements)", large_index, 200000);
    require(large_index < small_index * 4 + 5, "random access is O(1)",
            "1k took " + std::to_string(small_index) + " ms, 1M took " +
                std::to_string(large_index) + " ms");
    report("list_at (1M, full-span random)", index_time(large, 1000000), 200000);

    const double cdr_ms = milliseconds([&large] {
        Value at = large;
        for (int i = 0; i < 500000 && at.type() == Value::Type::Pair; ++i) at = at.cdr();
    });
    report("cdr view (500k steps)", cdr_ms, 500000);
    const std::size_t cdr_allocations = count_allocations([&large] {
        Value at = large;
        for (int i = 0; i < 1000; ++i) at = at.cdr();
    });
    require(cdr_allocations <= 2000, "cdr allocates only its view",
            std::to_string(cdr_allocations) + " allocations for 1000 cdr steps");

    // Target: no whole-list temporary copy merely to inspect arguments.
    Interpreter vm;
    vm.define("wide", large);
    vm.eval("(define (head-only a . rest) a)");
    const std::size_t inspect = count_allocations([&vm] { vm.eval("(car wide)"); });
    require(inspect < 1000, "inspecting a list does not copy it",
            std::to_string(inspect) + " allocations to take the car of a 1M-element list");
}

void bench_parsing_and_writing() {
    Interpreter vm;
    std::string source = "(";
    for (int i = 0; i < 20000; ++i) source += "(alpha " + std::to_string(i) + " \"text\") ";
    source += ")";
    const double read_ms = milliseconds([&vm, &source] {
        for (int i = 0; i < 5; ++i) { const Value parsed = vm.read(source); (void)parsed; }
    });
    report("read (20k records x5)", read_ms, 100000);

    const Value parsed = vm.read(source);
    const double write_ms = milliseconds([&vm, &parsed] {
        for (int i = 0; i < 5; ++i) { const std::string text = vm.write(parsed); (void)text; }
    });
    report("write (20k records x5)", write_ms, 100000);
}

void bench_strings() {
    const double build_ms = milliseconds([] {
        for (int i = 0; i < 200000; ++i) {
            const Value text = Value::string("a moderately sized string value");
            (void)text;
        }
    });
    report("string construction", build_ms, 200000);

    Interpreter vm;
    std::string big(1 << 20, 'x');
    vm.define("big-string", Value::string(big));
    const double index_ms = milliseconds([&vm] {
        vm.eval("(let loop ((i 1) (n 0)) (if (> i 200000) n (loop (+ i 1) (+ n 1))))");
    });
    report("interpreted loop (200k iterations)", index_ms, 200000);
}

// Target: no C++ stack growth during tail recursion.
volatile const char* deepest_frame = nullptr;
volatile const char* shallow_frame = nullptr;

void bench_tail_calls() {
    Interpreter vm;
    const char marker = 0;
    shallow_frame = &marker;
    vm.define_native("record-frame", [](Interpreter&, const std::vector<Value>&) {
        const char here = 0;
        deepest_frame = &here;
        return Value::nil();
    });
    vm.eval("(define (spin n) (if (= n 0) (record-frame) (spin (- n 1))))");
    const double ms = milliseconds([&vm] { vm.eval("(spin 1000000)"); });
    report("tail calls (1M)", ms, 1000000);

    const std::ptrdiff_t drift =
        shallow_frame > deepest_frame ? shallow_frame - deepest_frame : deepest_frame - shallow_frame;
    require(drift < 64 * 1024, "tail recursion does not grow the C++ stack",
            "stack moved " + std::to_string(drift) + " bytes across 1M tail calls");
}

void bench_text_and_diff() {
    Interpreter vm;
    vm.enable_text_primitives();
    std::vector<Value> lines;
    for (int i = 0; i < 20000; ++i)
        lines.push_back(Value::string("line " + std::to_string(i) + " of the corpus"));
    vm.define("corpus", Value::list(lines));
    report("grep over 20k lines", milliseconds([&vm] { vm.eval("(grep \"1234\" corpus)"); }), 20000);
    report("sort 20k lines", milliseconds([&vm] { vm.eval("(sort corpus)"); }), 20000);
    report("wc over 20k lines", milliseconds([&vm] { vm.eval("(wc corpus)"); }), 20000);

    std::vector<Value> changed = lines;
    for (std::size_t i = 0; i < changed.size(); i += 500) changed[i] = Value::string("changed");
    vm.define("changed", Value::list(changed));
    report("diff 20k vs 20k lines", milliseconds([&vm] { vm.eval("(diff corpus changed)"); }),
           20000);
}

void bench_filesystem(const std::string& root) {
    Interpreter vm;
    toolscheme::posix::Policy policy;
    policy.root = root;
    policy.allow_process = true;
    toolscheme::posix::install_all(vm, policy);

    // Build a small tree to traverse.
    vm.eval("(mkdir \"bench\" '((parents #t)))");
    for (int i = 0; i < 200; ++i) {
        vm.define("bench-name", Value::string("bench/file" + std::to_string(i) + ".txt"));
        vm.eval("(write-file bench-name \"alpha\\nbeta\\ngamma\\nneedle\\n\")");
    }
    report("stat", milliseconds([&vm] {
               for (int i = 0; i < 2000; ++i) vm.eval("(stat \"bench/file0.txt\")");
           }), 2000);
    report("glob over 200 files", milliseconds([&vm] {
               for (int i = 0; i < 20; ++i) vm.eval("(glob \"**/*.txt\")");
           }), 4000);
    report("tree traversal", milliseconds([&vm] {
               for (int i = 0; i < 20; ++i) vm.eval("(tree \".\")");
           }), 4000);
    report("search over 200 files", milliseconds([&vm] {
               for (int i = 0; i < 10; ++i) vm.eval("(search \"needle\" '(\".\") '((literal #t)))");
           }), 2000);
    report("ranged read", milliseconds([&vm] {
               for (int i = 0; i < 2000; ++i)
                   vm.eval("(read-file \"bench/file0.txt\" '((first-line 2) (last-line 3)))");
           }), 2000);
    report("directory listing", milliseconds([&vm] {
               for (int i = 0; i < 200; ++i) vm.eval("(ls \"bench\")");
           }), 200);

    vm.eval("(write-file \"patch-target.txt\" \"alpha\\nbeta\\ngamma\\n\")");
    vm.define("bench-patch",
              Value::string("--- a/patch-target.txt\n+++ b/patch-target.txt\n"
                            "@@ -1,3 +1,3 @@\n alpha\n-beta\n+BETA\n gamma\n"));
    vm.define("bench-revert",
              Value::string("--- a/patch-target.txt\n+++ b/patch-target.txt\n"
                            "@@ -1,3 +1,3 @@\n alpha\n-BETA\n+beta\n gamma\n"));
    report("apply-patch", milliseconds([&vm] {
               for (int i = 0; i < 200; ++i) {
                   vm.eval("(apply-patch bench-patch)");
                   vm.eval("(apply-patch bench-revert)");
               }
           }), 400);
    report("process launch", milliseconds([&vm] {
               for (int i = 0; i < 100; ++i)
                   vm.eval("(process-wait (field-ref (process-start"
                           " '((program \"true\"))) 'job))");
           }), 100);
}

} // namespace

int main() {
    char pattern[] = "/tmp/toolscheme-benchXXXXXX";
    const char* root = ::mkdtemp(pattern);
    if (!root) {
        std::cerr << "FATAL: cannot create a benchmark root\n";
        return 2;
    }
    std::cout << "toolscheme benchmarks\n";
    bench_scalars();
    bench_list_construction();
    bench_list_access();
    bench_parsing_and_writing();
    bench_strings();
    bench_tail_calls();
    bench_text_and_diff();
    bench_filesystem(root);

    std::string command = "rm -rf '";
    command += root;
    command += "'";
    if (std::system(command.c_str()) != 0) std::cerr << "warning: benchmark root not removed\n";

    if (failures) {
        std::cerr << failures << " performance targets missed\n";
        return 1;
    }
    std::cout << "all performance targets met\n";
    return 0;
}
