CXX ?= c++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -Wpedantic
SANFLAGS = -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined \
           -fno-omit-frame-pointer
SOURCES = toolscheme.cpp toolscheme_posix.cpp
HEADERS = toolscheme.hpp toolscheme_posix.hpp
VERSION := $(shell cat VERSION.txt)
HOST_OS := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
  # Apple's sanitizer runtime supports ASan/UBSan, but not LeakSanitizer.
  DETECT_LEAKS = 0
else
  DETECT_LEAKS = 1
endif

# DuckDB is optional and detected, never required. The project has no external
# dependencies by design, and `toolscheme` has to build, run and keep collecting
# with nothing installed -- the log is a file, and that is deliberate. When the
# library is present the `sql-query` primitive appears; when it is not, it does
# not exist, which is exactly how every other capability-backed primitive behaves.
#
#   make vendor-duckdb     fetch the C library into vendor/ (about 38 MB)
DUCKDB_DIR ?= vendor/duckdb
ifneq ($(wildcard $(DUCKDB_DIR)/duckdb.h),)
  DUCKDB_FLAGS = -DTOOLSCHEME_DUCKDB -I$(DUCKDB_DIR)
  DUCKDB_SOURCES = toolscheme_duckdb.cpp
  # Two rpaths: the checkout for a development build, and a location relative to
  # the installed binary so `make install` does not leave it pointing at a source
  # tree that may later move or be deleted.
  ifeq ($(HOST_OS),Darwin)
    DUCKDB_RUNTIME = @loader_path/../share/toolscheme
  else
    DUCKDB_RUNTIME = $$ORIGIN/../share/toolscheme
  endif
  DUCKDB_LINK = -L$(DUCKDB_DIR) -lduckdb \
                -Wl,-rpath,$(abspath $(DUCKDB_DIR)) \
                -Wl,-rpath,'$(DUCKDB_RUNTIME)'
endif

.PHONY: all test sanitize fuzz bench loop synthesize adoption check install uninstall vendor-duckdb FORCE clean learning-test install-test package
all: toolscheme toolscheme_test

# The executable: a scripting front end and an MCP server.
# A stamp of the optional-feature configuration. Without it, gaining or losing
# DuckDB leaves the previous binary in place: nothing in the prerequisite list
# changes timestamp when a vendored directory appears, so make reports the target
# up to date and installs a binary built for the other configuration.
.build-config: FORCE
	@printf '%s\n' "$(DUCKDB_FLAGS)" | cmp -s - $@ 2>/dev/null || printf '%s\n' "$(DUCKDB_FLAGS)" > $@
FORCE:

toolscheme: $(SOURCES) $(HEADERS) main.cpp $(DUCKDB_SOURCES) Makefile VERSION.txt .build-config
	$(CXX) $(CXXFLAGS) -DTOOLSCHEME_VERSION='"$(VERSION)"' $(DUCKDB_FLAGS) -I. $(SOURCES) $(DUCKDB_SOURCES) main.cpp $(DUCKDB_LINK) -o $@

DUCKDB_VERSION ?= v1.5.5
DUCKDB_ARCH ?= $(shell uname -m | sed -e s/aarch64/arm64/ -e s/x86_64/amd64/)
vendor-duckdb:
	python3 scripts/vendor-duckdb.py $(DUCKDB_VERSION) "$(DUCKDB_DIR)"

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
	TOOLSCHEME_STRESS_DIVISOR=100 ASAN_OPTIONS=detect_leaks=$(DETECT_LEAKS) \
	  LSAN_OPTIONS=suppressions=tests/leak-suppressions.txt ./toolscheme_test_san

toolscheme_fuzz: $(SOURCES) $(HEADERS) tests/fuzz_toolscheme.cpp
	$(CXX) $(SANFLAGS) -I. $(SOURCES) tests/fuzz_toolscheme.cpp -o $@

fuzz: toolscheme_fuzz
	ASAN_OPTIONS=detect_leaks=$(DETECT_LEAKS) \
	  LSAN_OPTIONS=suppressions=tests/leak-suppressions.txt ./toolscheme_fuzz

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

# Each check prints its report and then has to contain the passing marker. The
# report is captured and echoed rather than teed: `tee /dev/stderr` opens the
# target with O_TRUNC, so under `make check > log 2>&1` every earlier stage is
# erased and the log ends up one line long.
define check-scheme
out=$$(TOOLSCHEME_STATE= ./toolscheme $(1)); echo "$$out"; echo "$$out" | grep -q '$(2)'
endef

loop: toolscheme
	@$(call check-scheme,tests/intake-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/hook-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/tools-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/mcp-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/redirect-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/steer-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/sql-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/continue-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/codex-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/proven-check.scm --lib lib --allow-process \
	  --allow-program sh --allow-program grep --allow-program head,(checks-hold #t))
	@$(call check-scheme,tests/synthesis-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/replay-check.scm --lib lib --allow-process \
	  --allow-program grep --allow-program head --allow-program sh --allow-program bash,(gate-holds #t))

# The full gate: warning-clean optimized build, sanitizers, fuzzing, benchmarks,
# and the loop.
check: test sanitize fuzz bench loop learning-test install-test

learning-test: toolscheme
	python3 tests/learning_test.py

install-test: toolscheme
	python3 tests/install_test.py

package: toolscheme
	python3 scripts/package.py

# The live synthesis call against the NVIDIA inference gateway, which speaks the
# Anthropic Messages API natively on /v1/messages.
#
# The credential is resolved here and handed over as an environment variable rather
# than as a file the interpreter has to reach: the token lives outside the sandbox
# root, and widening the root to fetch it would trade a real boundary for a
# convenience. Never a secret in the repo.
#
# The replay side runs recorded commands, so the allowlist below is the complete
# set of programs the gate may execute. It matches replay-safe-programs in
# lib/analysis.scm, which is what decides a sample is offered at all; a command
# needing anything else is never replayed and its tool is never published.
SYNTHESIS_KEY_FILE ?= $(HOME)/Documents/API_KEYS/nvidia-inference.txt
# The nested schema is the default because only it records the working directory
# each command ran in, and a recorded command cannot be replayed without that.
TRANSCRIPTS ?= .claude/projects
# Optional: PATTERN='grep -n' points the loop at one opportunity instead of the
# highest ranked one.
REPLAY_PROGRAMS = sh grep egrep fgrep head tail cat wc ls find sort uniq cut nl \
                  basename dirname file stat du df which tr column
REPLAY_ALLOW = $(foreach p,$(REPLAY_PROGRAMS),--allow-program $(p))

synthesize: toolscheme
	@key="$${NVIDIA_INFERENCE_API_KEY:-$$(cat '$(SYNTHESIS_KEY_FILE)' 2>/dev/null)}"; \
	 if [ -z "$$key" ] && [ -z "$$ANTHROPIC_API_KEY" ]; then \
	   echo "no credential: export NVIDIA_INFERENCE_API_KEY, or put the token in"; \
	   echo "$(SYNTHESIS_KEY_FILE) (override with SYNTHESIS_KEY_FILE=...)"; \
	   exit 2; \
	 fi; \
	 NVIDIA_INFERENCE_API_KEY="$$key" ./toolscheme tests/synthesize-live.scm '$(TRANSCRIPTS)' $(PATTERN) \
	   --root "$(HOME)" --lib "$(CURDIR)/lib" --allow-process --allow-program curl $(REPLAY_ALLOW)

# Installing. A hook that only watches one repository can only ever report on that
# repository, so to observe every session the binary, the library and the hook have
# to live somewhere outside any checkout, and the agent configuration has to point
# at an absolute path rather than at ${CLAUDE_PROJECT_DIR}.
#
# Observations go to $XDG_STATE_HOME/toolscheme, which is also the sandbox root the
# hook runs under: it watches every project and can write to none of them.
PREFIX ?= $(HOME)/.local
BINDIR = $(PREFIX)/bin
SHAREDIR = $(PREFIX)/share/toolscheme
STATEDIR = $${XDG_STATE_HOME:-$(HOME)/.local/state}/toolscheme

install: toolscheme
	install -d "$(BINDIR)" "$(SHAREDIR)/lib/tools" "$(SHAREDIR)/hooks"
	install -m 755 toolscheme "$(BINDIR)/toolscheme"
	install -m 755 scripts/learning.py "$(SHAREDIR)/learning.py"
	install -m 644 VERSION.txt "$(SHAREDIR)/VERSION.txt"
	install -m 644 lib/*.scm "$(SHAREDIR)/lib/"
	@if ls lib/tools/*.scm >/dev/null 2>&1; then \
	   install -m 644 lib/tools/*.scm "$(SHAREDIR)/lib/tools/"; fi
	install -m 644 hooks/*.scm "$(SHAREDIR)/hooks/"
	install -m 755 hooks/*.sh "$(SHAREDIR)/hooks/"
	@if [ -f "$(DUCKDB_DIR)/libduckdb.so" ]; then \
	   install -m 644 "$(DUCKDB_DIR)/libduckdb.so" "$(SHAREDIR)/"; \
	   echo "installed libduckdb.so beside the library"; fi
	@if [ -f "$(DUCKDB_DIR)/libduckdb.dylib" ]; then \
	   install -m 755 "$(DUCKDB_DIR)/libduckdb.dylib" "$(SHAREDIR)/"; \
	   install_name_tool -id @rpath/libduckdb.dylib "$(SHAREDIR)/libduckdb.dylib"; \
	   codesign --force --sign - "$(SHAREDIR)/libduckdb.dylib"; fi
	@echo
	@echo "installed: $(BINDIR)/toolscheme and $(SHAREDIR)"
	@echo "observations will go to $(STATEDIR)"
	@echo
	@echo "To observe every session, add the handler to your agent configuration."
	@echo "Neither file is written for you; both are yours to review."
	@echo
	@echo "  ~/.claude/settings.json"
	@echo '    {"hooks": {"PreToolUse": [{"matcher": "*", "hooks":'
	@echo '      [{"type": "command", "command": "$(SHAREDIR)/hooks/observe.sh", "timeout": 5}]}],'
	@echo '               "PostToolUse": [{"matcher": "*", "hooks":'
	@echo '      [{"type": "command", "command": "$(SHAREDIR)/hooks/observe.sh", "timeout": 5}]}]}}'
	@echo
	@echo "  ~/.codex/config.toml"
	@echo '    [[hooks.PreToolUse]]'
	@echo '    matcher = "*"'
	@echo '    [[hooks.PreToolUse.hooks]]'
	@echo '    type = "command"'
	@echo '    command = "$(SHAREDIR)/hooks/observe.sh"'
	@echo
	@echo "Then: toolscheme analyze $(STATEDIR)"

uninstall:
	rm -f "$(BINDIR)/toolscheme"
	rm -rf "$(SHAREDIR)"
	@echo "removed the binary and $(SHAREDIR); observations in $(STATEDIR) are left alone"

# Did the instruction land? Claude Code does not record its loaded instructions in
# the transcript, so this is judged by behaviour instead: a session that waits
# without polling and without being advised is one that read the note.
adoption: toolscheme
	@./toolscheme tests/adoption-report.scm --root "$(STATEDIR)" --lib "$(CURDIR)/lib"

clean:
	rm -f toolscheme toolscheme_test toolscheme_test_san toolscheme_fuzz toolscheme_bench .build-config
