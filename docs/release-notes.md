ToolScheme 0.5.2 runs on FreeBSD, and tests two claims that had only ever been read.

Nothing here changes behaviour on Linux or macOS. It is a portability and
verification release, and every item in it was found by running the code on a
machine it had not been run on before.

## FreeBSD

FreeBSD 16.0-CURRENT failed 8 of 1,686 checks on first contact. All three causes
were real:

- **`df` did not work.** It reported "statvfs is unavailable on this platform".
  `statvfs` is POSIX.1-2001 and the header was already included unconditionally;
  the guard was an allowlist of the two platforms the code had been built on
  rather than of the platforms that support the call.
- **`ps`, `pgrep`, `pkill` and `uptime` were unimplemented.** FreeBSD uses the
  same `sysctl` interface as macOS -- `KERN_BOOTTIME` is identical, so `uptime`
  simply joins that branch -- but its `kinfo_proc` is flat, `ki_pid` and
  `ki_comm` rather than a nested `kp_proc`, so the process table needs its own.
- **Two tests assumed "not macOS" meant "Linux"**, and so required FreeBSD to
  produce a working `systemctl`. An init system belongs to one platform and on a
  third is neither, which cannot be written as an either/or.

FreeBSD now passes 1,680 checks and all 12 loop checks. `scripts/package.py`
knows the platform and uses `gmake`, since a BSD `make` cannot parse this
Makefile.

## Concurrency, now tested

`docs/agents.md` claimed concurrent sessions were safe because records are
clipped below `PIPE_BUF` and appended with `O_APPEND`. Nothing exercised it,
while the hook is installed globally and every session on the machine appends to
one file.

`tests/concurrent-hooks.sh` now runs dozens of hooks at once and checks that
every record parses, that none is lost, and that the clip lands where it is
supposed to. 480 appends from 48 writers on Linux, none torn.

The claim holds; the stated reason was wrong. `PIPE_BUF` bounds atomic writes to
pipes, not to files. What makes this safe is that `O_APPEND` on a regular file
makes the seek-to-end and the write one atomic step at any size. FreeBSD makes
the difference visible: its `PIPE_BUF` is 512 against Linux's 4096, and
2,808-byte records still arrive intact under 32 concurrent writers. Believing the
old explanation would have meant concluding FreeBSD was unsafe, or clipping
records to 512 bytes for no benefit.

## musl

Alpine/aarch64 passes 1,683 checks and 12 loop checks on overlayfs -- a different
C library than any previous test, and the strongest evidence so far for the
dependency-free claim.

It also found that DuckDB was enabled whenever `duckdb.h` existed, so a vendored
glibc build broke the link on musl instead of falling back to no DuckDB.
Detection now compiles and links a probe, and a library that cannot be used is
treated exactly like one that is not there.

## Known gap

The content cache's settle window compares a file's mtime against the local
clock. On a network filesystem the server stamps mtime, and a server clock
running behind its clients would defeat it. Untested: there is no NFS mount to
try it on.
