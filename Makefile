CXX ?= c++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -Wpedantic
SANFLAGS = -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined \
           -fno-omit-frame-pointer
SOURCES = toolscheme.cpp toolscheme_posix.cpp
HEADERS = toolscheme.hpp toolscheme_posix.hpp

.PHONY: all test sanitize fuzz bench check clean
all: toolscheme_test

toolscheme_test: $(SOURCES) $(HEADERS) test_toolscheme.cpp tests/primitive_examples.inc
	$(CXX) $(CXXFLAGS) -I. $(SOURCES) test_toolscheme.cpp -o $@

test: toolscheme_test
	./toolscheme_test

# AddressSanitizer, UndefinedBehaviorSanitizer, and leak checking over the same
# suite. Instrumented allocation costs roughly 500us per procedure call, so the
# stress loops run at 1/100 scale here: the invariants they prove (no stack growth
# under tail calls, O(1) list access) hold at that size, and the gate finishes in
# minutes instead of hours. Interned symbols live for the life of the process by
# design, so that one intentional retention is suppressed rather than reported.
sanitize: $(SOURCES) $(HEADERS) test_toolscheme.cpp tests/primitive_examples.inc
	$(CXX) $(SANFLAGS) -I. $(SOURCES) test_toolscheme.cpp -o toolscheme_test_san
	TOOLSCHEME_STRESS_DIVISOR=100 ASAN_OPTIONS=detect_leaks=1 \
	  LSAN_OPTIONS=suppressions=tests/leak-suppressions.txt ./toolscheme_test_san

toolscheme_fuzz: $(SOURCES) $(HEADERS) tests/fuzz_toolscheme.cpp
	$(CXX) $(SANFLAGS) -I. $(SOURCES) tests/fuzz_toolscheme.cpp -o $@

fuzz: toolscheme_fuzz
	./toolscheme_fuzz

toolscheme_bench: $(SOURCES) $(HEADERS) tests/bench_toolscheme.cpp
	$(CXX) $(CXXFLAGS) -I. $(SOURCES) tests/bench_toolscheme.cpp -o $@

bench: toolscheme_bench
	./toolscheme_bench

# The full gate: warning-clean optimized build, sanitizers, fuzzing, benchmarks.
check: test sanitize fuzz bench

clean:
	rm -f toolscheme_test toolscheme_test_san toolscheme_fuzz toolscheme_bench
