ToolScheme 0.6.0 plugs into Codex's own app-server daemon instead of fighting it.

Breaking: `make install-codex-shim` and `make uninstall-codex-shim` are gone,
replaced by `make install-codex-continue` and `make uninstall-codex-continue`.
If you installed the shim, remove `~/.local/bin/codex` and any PATH ordering you
added for it.

## What changed

Codex 0.162 runs its own shared, managed app-server daemon that every session
joins, and `codex queue --thread <id> --message <text>` addresses it by default.
Verified against a session started with no special flags: the message queued,
exit 0, arrived in that session's rollout, and the session ran the step it had
named.

Until then, `codex queue` reached only sessions started with `--remote` against
an app server -- and nobody types a different command to start their editor. So
this project had built five mechanisms, each guarding the one before it:

- a `codex` shim installed earlier on PATH, to add the flag
- an app server toolscheme started and supervised
- a socket in a directory kept at 0700, because Codex 0.156 refused looser
- a probe checking `--remote` had survived the last Codex update
- a line in the shell profile, once Homebrew took the front of PATH and
  shadowed the shim

All five are deleted. The remaining mechanism is a supported CLI call against
the vendor's own daemon.

There is deliberately no version check. An older Codex fails the queue, which is
already handled the way every delivery failure is: recorded as `continue-failed`,
counted against the attempt bound, and the thread abandoned after three times the
cap. A version number would be one more thing to keep true.

## Also fixed

Every previously-installed or adopted tool had been failing with "unbound symbol:
define-tool" on **every invocation of the binary** since 0.5.0. The registry
those files call into was removed then; the loader that reads them was not. In a
hook, where stderr is discarded, that was three error lines per tool call that
nobody could see.

The loader is gone, and `make install` now clears the files an older install left
behind. Removing a feature is not finished while its artifacts are still on disk
being loaded.

## What this cost, measured

The hook taxes every tool call: 17ms per event, 23ms per call across the corpus.
The sleep refusal is still the only mechanism with a measured saving -- time in
fixed waits fell from 69ms per call before it to 4ms in the most recent week,
with long waits going 32 to 3. Net about 42ms saved per call, roughly 2.8 to 1.

Codex continuation has delivered two continuations in its existence, both in
controlled tests. It is simpler now than it has ever been, which makes the coming
weeks the first fair trial of whether it is worth having at all.

## Known gap

The content cache's settle window compares a file's mtime against the local
clock. On a network filesystem the server stamps mtime, and a server clock
running behind its clients would defeat it. Untested: there is no NFS mount to
try it on.
