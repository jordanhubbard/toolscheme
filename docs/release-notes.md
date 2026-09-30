ToolScheme 0.5.1 fixes a cache that could return the previous contents of a file.

Everyone on 0.3.0 or later should take this. The bug is in `read-file` and every
file-backed text tool built on it.

## The bug

Rewriting a 6-byte file from `alpha` to `BRAVO` and reading it back returned
`alpha`.

The content cache's fingerprint is device, inode, size and mtime, and the comment
beside it claimed "mtime to the nanosecond". The field holds nanoseconds; the
clock that fills it does not. A filesystem stamps mtime from a tick-granular
clock, so two writes a few hundred microseconds apart are identical in all four
fields -- and a same-length rewrite inside one tick is invisible to the check
that exists precisely to catch it.

It passed on arm64 because that machine is slow enough to cross a tick between
the write and the read. On x86_64 seven of 1,680 checks fail, six of which
disappear with `TOOLSCHEME_CACHE=0`. The project had never been tested on x86_64.

## The fix

A file modified within the last two seconds is read rather than recalled -- the
remedy rsync and make use. It costs only that a file just written is not served
from cache, which is the rare case in a corpus where agents mostly read what they
did not just write. `TOOLSCHEME_CACHE=0` still disables the cache entirely.

`touch` gains `(age-seconds N)`, which stamps a file into the past. Without it
the cache's hit path cannot be tested except by sleeping through the settle
window, and a cache whose hit path is untested is one that can silently stop
hitting.

## Verified

`make test` and `make loop` pass on linux-x86_64 (1,681 checks), linux-arm64
(1,680) and macos-arm64 (1,677); counts differ only by platform-gated checks.
