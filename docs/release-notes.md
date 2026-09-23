ToolScheme 0.3.0 adds automatic agent hook setup, bounded observation logs, and configurable stall classification.

Changes since 0.2.0:

- `make install` now configures Claude Code and Codex observation hooks automatically. It preserves unrelated settings, backs up changed files, and avoids duplicate hooks on repeated installs. Set `CONFIGURE_HOOKS=0` for a files-only install; `CLAUDE_CONFIG_DIR` and `CODEX_HOME` select alternate settings directories.
- Observation logs rotate at 64 MB by default, retaining three generations. `TOOLSCHEME_LOG_MAX_BYTES` and `TOOLSCHEME_LOG_KEEP` control retention.
- Settings live under `$XDG_CONFIG_HOME/toolscheme/config` (normally `~/.config/toolscheme/config`), with the old state-directory configuration retained as a fallback. Credentials can be read from configuration as well as the environment.
- Optional model-based stall classification supports chat and typed System One backends. Output-aware rewrite decisions use recorded command output sizes. Experiment scripts and results document the measured tradeoffs.
- Platform facts include accelerator information.

Automatic configuration from source requires Python 3.11+. The interpreter remains free of external library dependencies; durable Git learning requires Python 3.9+ and Git 2.28+. Classification and command rewriting remain opt-in.

Download the archive for your operating system and architecture, verify its SHA-256 checksum, and extract it to a permanent directory. Add its `bin` directory to PATH and keep `bin` and `share` together. Extracting a release archive does not modify agent settings; automatic hook setup runs through `make install` in a source checkout.

Release archives support macOS and Linux on ARM64 and x86-64. Linux binaries target Ubuntu 22.04 or compatible systems with glibc 2.35+; macOS binaries target macOS 15+. Native Windows support is not included.
