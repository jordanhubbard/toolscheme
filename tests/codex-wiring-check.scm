;;; codex-wiring-check.scm -- the shell side of Codex continuation holds together.
;;;
;;; Most of what this file used to check is gone, and the reason is worth keeping.
;;;
;;; `codex queue` once reached only sessions launched with `--remote` against an
;;; app server, so toolscheme installed a `codex` shim earlier on PATH to add
;;; that flag, ran and supervised the server itself, kept its socket in a 0700
;;; directory because Codex 0.156 refused to bind anything looser, probed each
;;; new Codex build to confirm `--remote` still existed, and -- when Homebrew
;;; took over the PATH -- needed a line in the user's shell profile to be found
;;; at all. Five mechanisms, each guarding the one before it.
;;;
;;; Codex now runs its own shared managed daemon and `codex queue` addresses it
;;; by default. Verified against a session started with no special flags: the
;;; message queued, exit 0, and landed in that session's rollout. So all five
;;; are deleted, and what remains to check is that nothing still reaches for
;;; them.

(define (text-of path)
  (let ((r (catch-errors (lambda () (read-file path '((limit 262144)))))))
    (if (error? r) "" (field-ref r 'text ""))))

(define watcher (text-of "hooks/codex-continue.sh"))
(define library (text-of "lib/codex.scm"))
(define unit (text-of "systemd/toolscheme-codex-continue.service"))

(list
  (list 'checks-hold
        (and
          (not (string-null? watcher))
          (not (string-null? library))
          (not (string-null? unit))

          ;; Nothing addresses a socket of ours, because there is not one. A
          ;; stale `--remote unix://...` would point at a server no longer
          ;; running and fail every delivery -- silently, since a thread that
          ;; cannot be reached simply never continues.
          ;; Quoted, because that is how the flag appears as an argument. The
          ;; library's comment explains why it is gone, in backticks, and a
          ;; check that cannot tell code from prose fails on its own
          ;; documentation -- which this one did.
          (not (string-contains? library "\"--remote\""))
          (not (string-contains? watcher "TOOLSCHEME_CODEX_SOCKET"))
          ;; The test is that nothing *starts* a server, not that the word is
          ;; unmentionable -- the watcher's own comment explains that Codex runs
          ;; one, which is the whole point.
          (not (string-contains? watcher "app-server --listen"))
          (not (string-contains? library "app-server --listen"))

          ;; And the shim is gone rather than merely unused.
          (let ((orphan (catch-errors (lambda () (stat "hooks/codex-shim.sh")))))
            (error? orphan))
          (let ((orphan (catch-errors (lambda () (stat "hooks/codex-session.sh")))))
            (error? orphan))

          ;; The watcher must still find codex wherever it has moved to. A
          ;; systemd user unit inherits no login shell, so its PATH is whatever
          ;; the unit names -- and codex does not stay put: an npm global under
          ;; /usr/bin became a Homebrew install under /home/linuxbrew, and nine
          ;; delivery attempts failed against a directory that no longer held
          ;; one.
          (string-contains? watcher "command -v codex")
          (string-contains? watcher "linuxbrew")
          (string-contains? watcher "/opt/homebrew/bin/codex")

          ;; The timer runs the installed copy. A unit pointing into a checkout
          ;; works until the checkout moves, and then fails at 3am in silence.
          (string-contains? unit "/.local/share/toolscheme/hooks/codex-continue.sh")
          (not (string-contains? unit "/Src/")))))
