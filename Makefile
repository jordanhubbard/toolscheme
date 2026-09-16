CXX ?= c++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -Wpedantic
SANFLAGS = -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined \
           -fno-omit-frame-pointer
SOURCES = toolscheme.cpp toolscheme_posix.cpp
HEADERS = toolscheme.hpp toolscheme_posix.hpp

.PHONY: all test sanitize fuzz bench loop synthesize check clean
all: toolscheme toolscheme_test

# The executable: a scripting front end and an MCP server.
toolscheme: $(SOURCES) $(HEADERS) main.cpp
	$(CXX) $(CXXFLAGS) -I. $(SOURCES) main.cpp -o $@

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

# The self-improvement loop, end to end. These cover the parts that fail silently
# rather than loudly: intake reading both transcript schemas, a derived MCP schema
# that is an object and not an array of pairs, and every synthesis response branch
# including a refusal -- all without an API key. The live call is `make synthesize`.
#
# Then the publication gate. A real fused tool and a deliberately lossy one are
# replayed against the shell commands they claim to replace; the real one must
# publish and the lossy one must be refused, even though it is stabler and cheaper.
# A gate that cannot reject is not a gate. Needs a shell and grep, so it stays out
# of the hermetic suite.
loop: toolscheme
	./toolscheme tests/intake-check.scm --lib lib | tee /dev/stderr | grep -q '(checks-hold #t)'
	./toolscheme tests/tools-check.scm --lib lib | tee /dev/stderr | grep -q '(checks-hold #t)'
	./toolscheme tests/mcp-check.scm --lib lib | tee /dev/stderr | grep -q '(checks-hold #t)'
	./toolscheme tests/synthesis-check.scm --lib lib | tee /dev/stderr | grep -q '(checks-hold #t)'
	./toolscheme tests/replay-check.scm --lib lib --allow-process \
	  --allow-program grep --allow-program head --allow-program sh --allow-program bash \
	  | tee /dev/stderr | grep -q '(gate-holds #t)'

# The full gate: warning-clean optimized build, sanitizers, fuzzing, benchmarks,
# and the loop.
check: test sanitize fuzz bench loop

# The live synthesis call. Needs ANTHROPIC_API_KEY; never a secret in the repo.
synthesize: toolscheme
	@test -n "$$ANTHROPIC_API_KEY" || { echo "set ANTHROPIC_API_KEY first"; exit 2; }
	./toolscheme tests/synthesize-live.scm --lib lib --allow-process --allow-program curl

clean:
	rm -f toolscheme toolscheme_test toolscheme_test_san toolscheme_fuzz toolscheme_bench
