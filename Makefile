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
#
# Detection is a link, not a header. `make vendor-duckdb` fetches a prebuilt
# shared object for one platform, and a header says nothing about whether that
# object can be linked here: building in an Alpine container against a vendored
# glibc build fails with `undefined reference to vtable for std::runtime_error`
# rather than falling back to no DuckDB, which is the behaviour this project
# promises. So the probe compiles and links a trivial program against it, and a
# library that cannot be used is treated exactly like one that is not there.
DUCKDB_DIR ?= vendor/duckdb
ifneq ($(wildcard $(DUCKDB_DIR)/duckdb.h),)
  # The probe has to *call* something, or it links vacuously: a `main` that
  # references no DuckDB symbol succeeds against a library the real build cannot
  # use, which is exactly what the first version of this did.
  DUCKDB_USABLE := $(shell printf '#include "duckdb.h"\nint main(void){return duckdb_library_version()==0;}\n' \
        > .duckdb-probe.cpp 2>/dev/null && \
      $(CXX) -std=c++17 .duckdb-probe.cpp -I$(DUCKDB_DIR) -L$(DUCKDB_DIR) -lduckdb \
        -o .duckdb-probe.out >/dev/null 2>&1 && echo yes; \
      rm -f .duckdb-probe.cpp .duckdb-probe.out)
endif
ifeq ($(DUCKDB_USABLE),yes)
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

.PHONY: all test sanitize fuzz bench loop adoption check install uninstall install-mcp uninstall-mcp install-codex-shim uninstall-codex-shim vendor-duckdb FORCE clean learning-test install-test package
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

# The parts that fail silently rather than loudly: intake reading both transcript
# schemas, the hook's decisions, the refusals, and the MCP handshake.
#
# This used to end with the publication gate -- a real fused tool and a
# deliberately lossy one replayed against the commands they claimed to replace.
# That whole stack was removed in 0.5.0: measured against the corpus, a
# substitution cannot win, because byte-identity makes the byte count equal by
# construction and a shell call composes where a tool call does not. See
# docs/relevance.md.

# Each check prints its report and then has to contain the passing marker. The
# report is captured and echoed rather than teed: `tee /dev/stderr` opens the
# target with O_TRUNC, so under `make check > log 2>&1` every earlier stage is
# erased and the log ends up one line long.
define check-scheme
out=$$(TOOLSCHEME_STATE= ./toolscheme $(1)); echo "$$out"; echo "$$out" | grep -q '$(2)'
endef

loop: toolscheme
	@$(call check-scheme,tests/intake-check.scm --lib lib,(checks-hold #t))
	@rm -rf .check-root && mkdir -p .check-root
	@$(call check-scheme,tests/hook-check.scm --lib lib --root .check-root,(checks-hold #t))
	@$(call check-scheme,tests/mcp-check.scm --lib lib,(checks-hold #t))
	@rm -rf .check-root && mkdir -p .check-root
	@$(call check-scheme,tests/decide-check.scm --lib lib --root .check-root,(checks-hold #t))
	@rm -rf .check-root
	@$(call check-scheme,tests/steer-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/sql-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/continue-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/codex-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/codex-wiring-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/classify-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/dogfood-check.scm --lib lib,(checks-hold #t))
	@$(call check-scheme,tests/sleep-check.scm --lib lib,(checks-hold #t))

# The full gate: warning-clean optimized build, sanitizers, fuzzing, benchmarks,
# and the loop.
check: test sanitize fuzz bench loop learning-test install-test

learning-test: toolscheme
	python3 tests/learning_test.py

install-test: toolscheme
	python3 tests/configure_hooks_test.py
	python3 tests/install_test.py

package: toolscheme
	python3 scripts/package.py

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

# Set CONFIGURE_HOOKS=0 for staging/package installs. Configuration requires Python 3.11+.
CONFIGURE_HOOKS ?= 1
CLAUDE_CONFIG_DIR ?= $(HOME)/.claude
CODEX_HOME ?= $(HOME)/.codex

install: toolscheme
	install -d "$(BINDIR)" "$(SHAREDIR)/lib" "$(SHAREDIR)/hooks"
	install -m 755 toolscheme "$(BINDIR)/toolscheme"
	install -m 755 scripts/learning.py "$(SHAREDIR)/learning.py"
	install -m 644 VERSION.txt "$(SHAREDIR)/VERSION.txt"
	install -m 644 lib/*.scm "$(SHAREDIR)/lib/"
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
	@if [ "$(CONFIGURE_HOOKS)" != "0" ]; then \
	   python3 scripts/configure-hooks.py --hook "$(SHAREDIR)/hooks/observe.sh" \
	     --claude-dir "$(CLAUDE_CONFIG_DIR)" --codex-dir "$(CODEX_HOME)"; fi
	@echo "Then: toolscheme analyze $(STATEDIR)"

uninstall: uninstall-codex-shim uninstall-mcp
	rm -f "$(BINDIR)/toolscheme"
	rm -rf "$(SHAREDIR)"
	@echo "removed the binary and $(SHAREDIR); observations in $(STATEDIR) are left alone"

# Deliberately not part of `install`. Everything else here observes; this one
# puts a file named `codex` ahead of the real one on someone's PATH, and that is
# a decision to make on purpose rather than to inherit from a make target. It is
# a symlink so that a later `make install` updates it, and the shim recognises
# other copies of itself by content, so linking it as `codex` cannot loop.
install-codex-shim: install
	ln -sf "$(SHAREDIR)/hooks/codex-shim.sh" "$(BINDIR)/codex"
	@# The shim only makes a session reachable; the timer is what reaches it.
	@# Installing it is safe whatever the setting says -- the watcher exits
	@# immediately unless TOOLSCHEME_CODEX_CONTINUE is on -- so the setting stays
	@# the single switch rather than one of two that have to agree.
	@if command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then \
	   install -d "$(HOME)/.config/systemd/user"; \
	   install -m 644 systemd/toolscheme-codex-continue.service systemd/toolscheme-codex-continue.timer \
	     "$(HOME)/.config/systemd/user/"; \
	   systemctl --user daemon-reload; \
	   systemctl --user enable --now toolscheme-codex-continue.timer >/dev/null 2>&1 \
	     && echo "watcher scheduled: toolscheme-codex-continue.timer, every minute" \
	     || echo "warning: could not enable toolscheme-codex-continue.timer"; \
	 else \
	   echo "no systemd user session -- run $(SHAREDIR)/hooks/codex-continue.sh from cron or launchd"; \
	 fi
	@echo
	@echo "installed: $(BINDIR)/codex -> $(SHAREDIR)/hooks/codex-shim.sh"
	@if [ "$$(command -v codex)" = "$(BINDIR)/codex" ]; then \
	   echo "codex on your PATH is now the shim"; \
	 else \
	   echo "warning: $(BINDIR) is not early enough on PATH -- codex still resolves to $$(command -v codex)"; \
	 fi
	@echo "it passes through unchanged unless TOOLSCHEME_CODEX_CONTINUE=1 is set"

# Serve the published tools to the agents in-process, which is the delivery
# mechanism this project's own measurements favour. Four separate attempts to
# make substitution pay -- rewriting a command into a toolscheme call from the
# hook -- all failed on the same arithmetic: a fresh interpreter costs 13-20ms
# against roughly 1ms to fork the real program, so reproducing a command exactly
# leaves nothing to win on. A server that is already running has no such start
# cost, and can return a structured result instead of bytes to re-parse.
#
# Registered through each agent's own CLI rather than by editing its config,
# because both ship one and hand-editing someone's primary tool's configuration
# is how it gets corrupted. Removing first makes it idempotent.
#
# Not folded into `install`: it changes what tools an agent has, which is a
# decision to take deliberately.
install-mcp: install
	@if command -v claude >/dev/null 2>&1; then \
	   claude mcp remove -s user toolscheme >/dev/null 2>&1 || true; \
	   claude mcp add -s user toolscheme -- "$(BINDIR)/toolscheme" mcp --lib "$(SHAREDIR)/lib" \
	     >/dev/null 2>&1 \
	     && echo "registered with Claude Code (user scope)" \
	     || echo "warning: could not register with Claude Code"; \
	 else echo "claude not found; skipped Claude Code"; fi
	@if command -v codex >/dev/null 2>&1; then \
	   codex mcp remove toolscheme >/dev/null 2>&1 || true; \
	   codex mcp add toolscheme -- "$(BINDIR)/toolscheme" mcp --lib "$(SHAREDIR)/lib" \
	     >/dev/null 2>&1 \
	     && echo "registered with Codex (global)" \
	     || echo "warning: could not register with Codex"; \
	 else echo "codex not found; skipped Codex"; fi
	@echo "the server is sandboxed to the directory the agent starts it in"

uninstall-mcp:
	@if command -v claude >/dev/null 2>&1; then \
	   claude mcp remove -s user toolscheme >/dev/null 2>&1 || true; fi
	@if command -v codex >/dev/null 2>&1; then \
	   codex mcp remove toolscheme >/dev/null 2>&1 || true; fi
	@echo "unregistered toolscheme from any agent that had it"

uninstall-codex-shim:
	@if [ -L "$(BINDIR)/codex" ]; then rm -f "$(BINDIR)/codex"; \
	   echo "removed $(BINDIR)/codex"; fi
	@if command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then \
	   systemctl --user disable --now toolscheme-codex-continue.timer >/dev/null 2>&1 || true; \
	   rm -f "$(HOME)/.config/systemd/user/toolscheme-codex-continue.service" \
	         "$(HOME)/.config/systemd/user/toolscheme-codex-continue.timer"; \
	   systemctl --user daemon-reload || true; fi

# Did the instruction land? Claude Code does not record its loaded instructions in
# the transcript, so this is judged by behaviour instead: a session that waits
# without polling and without being advised is one that read the note.
adoption: toolscheme
	@./toolscheme tests/adoption-report.scm --root "$(STATEDIR)" --lib "$(CURDIR)/lib"

clean:
	rm -f toolscheme toolscheme_test toolscheme_test_san toolscheme_fuzz toolscheme_bench .build-config
