// The toolscheme executable: a scripting front end and an MCP server.
//
// The C++ here is deliberately thin. It parses a command line, installs the host
// adapters a policy permits, loads the Scheme library, and pumps stdio. Everything
// with judgement in it -- the MCP protocol, transcript parsing, analysis, tool
// synthesis, the replay gate -- lives in lib/*.scm, so it can be read, changed and
// re-run without a rebuild.

#include "toolscheme.hpp"
#include "toolscheme_posix.hpp"
#if defined(TOOLSCHEME_DUCKDB)
#include "toolscheme_duckdb.hpp"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <algorithm>
#include <climits>
#include <dirent.h>
#include <cstdlib>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

using toolscheme::Interpreter;
using toolscheme::Value;

struct Options {
    std::string command;                  // eval | run | repl | mcp | analyze
    std::string expression;
    std::string script;
    std::vector<std::string> arguments;
    std::string library;
    toolscheme::posix::Policy policy;
    bool telemetry = false;
    bool quiet = false;
    bool read_standard_input = false;
    bool text_output = false;
};

void usage() {
    std::fprintf(stderr,
        "toolscheme -- a Scheme toolbox for coding agents\n"
        "\n"
        "  toolscheme -e <expr>         evaluate an expression and print the result\n"
        "  toolscheme <file.scm> [args] run a script\n"
        "  toolscheme repl              interactive read-eval-print loop\n"
        "  toolscheme mcp               serve the published tools over MCP on stdio\n"
        "  toolscheme analyze [paths]   rank tool-use opportunities from agent transcripts\n"
        "  toolscheme learn <command>   durable learning journal and periodic Git sync\n"
        "\n"
        "Options:\n"
        "  --root <dir>        filesystem sandbox root (default: the current directory)\n"
        "  --lib <dir>         Scheme library directory (default: $TOOLSCHEME_LIB or ./lib)\n"
        "  --allow-process     permit child processes (off by default)\n"
        "  --allow-program <p> add one program to the executable allowlist\n"
        "  --read-only         refuse every filesystem mutation\n"
        "  --telemetry         record per-call timing and result size\n"
        "  --stdin             bind standard input to `standard-input`\n"
        "  --text              print a string result verbatim instead of written form\n"
        "  --quiet             suppress the repl banner and a script's result\n");
}

// The library lives next to the binary in an installed tree and under the working
// directory in a source checkout; try both before giving up.
std::string locate_library(const std::string& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;
    if (const char* from_environment = std::getenv("TOOLSCHEME_LIB")) return from_environment;
    std::vector<std::string> candidates;
    // Finding one's own executable has no portable spelling: /proc is Linux-only
    // and silently absent on macOS, which the project also targets.
    std::string self;
    char buffer[4096];
#if defined(__APPLE__)
    std::uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) == 0) self = buffer;
#else
    const ssize_t got = ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (got > 0) { buffer[got] = '\0'; self = buffer; }
#endif
    if (!self.empty()) {
        const std::size_t slash = self.find_last_of('/');
        if (slash != std::string::npos) {
            const std::string bin = self.substr(0, slash);
            candidates.push_back(bin + "/lib");              // a source checkout
            candidates.push_back(bin + "/../lib");
            // An installed tree: the binary is in $PREFIX/bin and the library is in
            // $PREFIX/share/toolscheme/lib, so neither of the above finds it and
            // the interpreter starts with no library and only says so in a warning.
            candidates.push_back(bin + "/../share/toolscheme/lib");
        }
    }
    candidates.push_back("lib");
    for (const std::string& candidate : candidates)
        if (::access((candidate + "/mcp.scm").c_str(), R_OK) == 0) return candidate;
    return "lib";
}

bool load_library(Interpreter& vm, const std::string& directory, std::string& error) {
    // Order matters: later files build on earlier ones.
    static const char* files[] = {"prelude.scm", "agentlog.scm", "analysis.scm",
                                  "replay.scm", "synthesis.scm", "mcp.scm", "hooks.scm",
                                  "redirect.scm", "steer.scm", "continue.scm",
                                  "codex.scm"};
    for (const char* name : files) {
        const std::string path = directory + "/" + name;
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "cannot read " + path;
            return false;
        }
        std::ostringstream text;
        text << input.rdbuf();
        try {
            vm.eval(text.str(), path);
        } catch (const std::exception& failure) {
            error = failure.what();
            return false;
        }
    }
    // Published tools are ordinary Scheme files, loaded after the library they use.
    //
    // Read here rather than through the interpreter's own `glob`, because that
    // resolves inside the capability root: a tool library installed in
    // $PREFIX/share is invisible from any sandbox that does not contain it, which
    // is every workspace except this project's own. Tools silently failed to exist
    // everywhere they were actually wanted.
    const std::string tools = directory + "/tools";
    vm.define("tool-directory", Value::string(tools));
    std::vector<std::string> published;
    if (DIR* handle = ::opendir(tools.c_str())) {
        while (struct dirent* entry = ::readdir(handle)) {
            const std::string name = entry->d_name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".scm") == 0)
                published.push_back(tools + "/" + name);
        }
        ::closedir(handle);
    }
    std::sort(published.begin(), published.end());
    for (const std::string& path : published) {
        std::ifstream input(path, std::ios::binary);
        if (!input) continue;
        std::ostringstream text;
        text << input.rdbuf();
        try {
            vm.eval(text.str(), path);
        } catch (const std::exception& failure) {
            // One broken tool must not take the library with it.
            std::fprintf(stderr, "toolscheme: %s: %s\n", path.c_str(), failure.what());
        }
    }
    return true;
}

// A local, bounded data snapshot, never executable Scheme or a network request.
// Missing or corrupt state cannot prevent startup. The sync worker alone reads
// Git history and publishes this file with atomic rename.
void load_learnings(Interpreter& vm) {
    vm.define("learning-notes", Value::string(""));
    vm.define("learning-snapshot", Value::list({}));
    std::string state;
    if (const char* p = std::getenv("TOOLSCHEME_STATE")) state = p;
    else if (const char* p = std::getenv("XDG_STATE_HOME")) state = std::string(p) + "/toolscheme";
    else if (const char* p = std::getenv("HOME")) state = std::string(p) + "/.local/state/toolscheme";
    if (state.empty()) return;
    std::ifstream input(state + "/learning-snapshot.json", std::ios::binary);
    if (!input) return;
    char buffer[65537];
    input.read(buffer, sizeof buffer);
    if (input.gcount() > 65536) return;
    try {
        vm.define("learning-json", Value::string(std::string(buffer, static_cast<std::size_t>(input.gcount()))));
        const Value parsed = vm.eval("(field-ref (json-parse learning-json) 'value '())");
        vm.define("learning-snapshot", parsed);
        if (!vm.eval("(equal? (field-ref learning-snapshot \"schema\" #f) 1)").truthy()) {
            vm.define("learning-snapshot", Value::list({}));
            return;
        }
        const Value notes = vm.eval("(field-ref learning-snapshot \"notes\" '())");
        std::string combined;
        if (notes.is_list()) {
            const std::size_t count = std::min<std::size_t>(notes.list_size(), 16);
            for (std::size_t i = 0; i < count; ++i) {
                const Value note = notes.list_at(i);
                if (note.type() != Value::Type::String || note.as_string().size() > 1024) continue;
                if (combined.size() + note.as_string().size() + 1 > 8192) break;
                if (!combined.empty()) combined += '\n';
                combined += note.as_string();
            }
        }
        vm.define("learning-notes", Value::string(combined));
    } catch (const std::exception&) {
        vm.define("learning-snapshot", Value::list({}));
    }
}

int run_repl(Interpreter& vm, const Options& options) {
    if (!options.quiet)
        std::fprintf(stderr, "toolscheme -- %zu primitives. Ctrl-D to exit.\n",
                     vm.primitive_names().size());
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            std::cout << vm.write(vm.eval(line, "<repl>")) << '\n';
        } catch (const std::exception& failure) {
            std::cout << failure.what() << '\n';
        }
        std::cout.flush();
    }
    return 0;
}

// MCP speaks line-delimited JSON-RPC on stdio. The C++ side only moves lines; the
// protocol itself is `(mcp-handle line)` in Scheme, which returns a response
// string or #f for a notification that needs no reply.
int run_mcp(Interpreter& vm) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::string reply;
        try {
            vm.define("mcp-request-line", Value::string(line));
            const Value response = vm.eval("(mcp-handle mcp-request-line)");
            if (response.type() != Value::Type::String) continue;  // notification
            reply = std::string(response.as_string());
        } catch (const std::exception& failure) {
            // A crash in the handler must still produce a protocol-legal reply.
            vm.define("mcp-failure", Value::string(failure.what()));
            try {
                reply = std::string(vm.eval("(mcp-internal-error mcp-failure)").as_string());
            } catch (const std::exception&) {
                continue;
            }
        }
        std::cout << reply << '\n';
        std::cout.flush();
    }
    return 0;
}

// The result of a utility is an association list; an `error` field means failure,
// which is what the process exit status should report.
int status_of(Interpreter& vm, const Value& result) {
    (void)vm;
    return toolscheme::option(result, "error").type() == Value::Type::String ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::puts("toolscheme " TOOLSCHEME_VERSION);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "learn") {
        const std::string library = locate_library("");
        std::string script = library + "/../scripts/learning.py";
        if (::access(script.c_str(), R_OK) != 0) script = library + "/../learning.py";
        std::vector<char*> arguments{const_cast<char*>("python3"), const_cast<char*>(script.c_str())};
        for (int i = 2; i < argc; ++i) arguments.push_back(argv[i]);
        arguments.push_back(nullptr);
        ::execvp("python3", arguments.data());
        std::fprintf(stderr, "toolscheme learn requires Python 3 and Git\n");
        return 1;
    }
    Options options;
    options.policy.root = ".";
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "toolscheme: %s needs a value\n", argument.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (argument == "-e" || argument == "--eval") { options.command = "eval"; options.expression = next(); }
        else if (argument == "--root") options.policy.root = next();
        else if (argument == "--lib") options.library = next();
        else if (argument == "--allow-process") options.policy.allow_process = true;
        else if (argument == "--allow-program") options.policy.allowed_programs.push_back(next());
        else if (argument == "--read-only") options.policy.writable = false;
        else if (argument == "--telemetry") options.telemetry = true;
        else if (argument == "--stdin") options.read_standard_input = true;
        else if (argument == "--text") options.text_output = true;
        else if (argument == "--quiet") options.quiet = true;
        else if (argument == "-h" || argument == "--help") { usage(); return 0; }
        else if (!argument.empty() && argument[0] == '-') {
            std::fprintf(stderr, "toolscheme: unknown option %s\n", argument.c_str());
            return 2;
        }
        else positional.push_back(argument);
    }

    if (options.command.empty() && !positional.empty()) {
        if (positional[0] == "repl" || positional[0] == "mcp" || positional[0] == "analyze") {
            options.command = positional[0];
            options.arguments.assign(positional.begin() + 1, positional.end());
        } else {
            options.command = "run";
            options.script = positional[0];
            options.arguments.assign(positional.begin() + 1, positional.end());
        }
    }
    if (options.command.empty()) { usage(); return 2; }

    Interpreter vm;
    toolscheme::posix::install_all(vm, options.policy);
#if defined(TOOLSCHEME_DUCKDB)
    // Optional, and confined to the same root as everything else. Without it the
    // `sql-query` primitive simply does not exist, which is how every other
    // capability-backed primitive behaves when nothing backs it.
    vm.install("sql", toolscheme::duckdb::make_sql(options.policy.root));
#endif
    if (options.telemetry) vm.set_telemetry(true);

    // Script arguments are explicit values, never ambient process state.
    std::vector<Value> script_arguments;
    for (const std::string& argument : options.arguments)
        script_arguments.push_back(Value::string(argument));
    vm.define("command-arguments", Value::list(std::move(script_arguments)));

    // The sandbox root, resolved. Every primitive takes paths relative to it, but
    // a SQL query cannot -- DuckDB checks permission on the literal path before any
    // search path applies -- so a caller needs to be able to build an absolute one.
    {
        char resolved[PATH_MAX];
        const char* root = ::realpath(options.policy.root.c_str(), resolved)
                               ? resolved
                               : options.policy.root.c_str();
        vm.define("capability-root", Value::string(root));
    }

    // Standard input is bound as a value rather than exposed as a primitive that
    // reads it: a script that consumes stdin should say so on the command line, and
    // nothing else in the language should be able to reach ambient process state.
    // Off by default, because a script that does not ask must not block on a pipe.
    if (options.read_standard_input) {
        std::ostringstream text;
        text << std::cin.rdbuf();
        vm.define("standard-input", Value::string(text.str()));
    }

    const std::string library = locate_library(options.library);
    std::string library_error;
    const bool library_loaded = load_library(vm, library, library_error);
    if (library_loaded) load_learnings(vm);
    // A library that fails to parse used to be silent for -e and for scripts, so a
    // stray paren in one file made every procedure in the library "unbound" with
    // no hint as to why. It is fatal where the library is required and a warning
    // everywhere else, but it is never silent.
    if (!library_loaded) {
        if (options.command == "mcp" || options.command == "analyze") {
            std::fprintf(stderr, "toolscheme: %s requires the Scheme library: %s\n",
                         options.command.c_str(), library_error.c_str());
            return 2;
        }
        std::fprintf(stderr, "toolscheme: warning: library not loaded: %s\n",
                     library_error.c_str());
    }

    try {
        if (options.command == "eval") {
            const Value result = vm.eval(options.expression, "<argument>");
            // A tool standing in for a shell command has to emit the bytes that
            // command emitted, not a quoted and escaped Scheme string.
            if (options.text_output && result.type() == Value::Type::String)
                std::cout << result.as_string();
            else
                std::cout << vm.write(result) << '\n';
            return status_of(vm, result);
        }
        if (options.command == "run") {
            std::ifstream input(options.script, std::ios::binary);
            if (!input) {
                std::fprintf(stderr, "toolscheme: cannot read %s\n", options.script.c_str());
                return 2;
            }
            // Parse and evaluate one datum at a time rather than holding the file.
            const Value result = vm.eval_stream([&input] { return input.get(); }, options.script);
            // Primitives are pure -- `echo` and `printf` return records rather than
            // writing -- so a script's value is how it reports. Printing it is the
            // only way a script can say anything; `--quiet` is for when it should not.
            if (options.text_output && result.type() == Value::Type::String)
                std::cout << result.as_string();
            else if (!options.quiet && result.type() != Value::Type::Unspecified)
                std::cout << vm.write(result) << '\n';
            if (result.type() == Value::Type::Pair) return status_of(vm, result);
            return 0;
        }
        if (options.command == "repl") return run_repl(vm, options);
        if (options.command == "mcp") return run_mcp(vm);
        if (options.command == "analyze") {
            const Value result = vm.eval("(analyze-sessions command-arguments)");
            std::cout << vm.write(result) << '\n';
            return status_of(vm, result);
        }
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "%s\n", failure.what());
        return 1;
    }
    usage();
    return 2;
}
