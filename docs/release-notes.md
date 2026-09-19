ToolScheme 0.2.0 ships the CLI, stdio MCP server, observation hooks, and durable learning store as relocatable macOS and Linux installations for ARM64 and x86-64.

Learning capture stays local. A separate periodic job commits session summaries and steering revisions to a dedicated Git repository, fetches other hosts' records, and atomically refreshes a bounded startup snapshot. Remote outages retain local commits for retry and never put Git on a tool hook's execution path. Shared notes are advisory data, not executable Scheme; disabling a note propagates through the same Git history.

The interpreter requires no external library. Git synchronization requires Python 3.9+ and Git 2.28+. Optional DuckDB builds support both macOS and Linux. See `docs/learning.md` for setup, scheduling, conflict handling, and the data contract.

Download the archive for your operating system and architecture, verify its SHA-256 checksum, and extract it to a permanent directory. Add its `bin` directory to PATH. The `bin` and `share` directories must remain together. Agent configuration is explicit; installing the archive does not modify any agent's settings.

Linux release binaries target Ubuntu 22.04 or compatible systems with glibc 2.35+; macOS release binaries target macOS 15+. Windows native support is not included. This release does not automatically execute synthesized shared tools or turn on transparent command rewriting.
