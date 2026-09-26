;;; codex-wiring-check.scm -- the shell side of Codex continuation holds together.
;;;
;;; Two failures motivate every check here, and neither is the sort a unit test
;;; of the decision logic would catch, because both live in the plumbing.
;;;
;;; The first: the app-server socket path was written out in three files. Codex
;;; 0.156 began refusing to bind a socket whose directory another user could
;;; replace entries in, fixing one file left the other two pointing somewhere
;;; nothing was listening, and the symptom was silence -- sessions launched
;;; fine and were simply never reachable.
;;;
;;; The second: the shim resolves the real `codex` by scanning PATH, and is
;;; itself installed on PATH as `codex`. Skipping "the script I was invoked as"
;;; is not enough, because running the copy in a source tree while an installed
;;; copy sits earlier on PATH has each exec the other until the machine gives
;;; up. It skips anything carrying the shim's marker instead, so the marker has
;;; to actually be there.

(define (text-of path)
  (let ((r (catch-errors (lambda () (read-file path '((limit 262144)))))))
    (if (error? r) "" (field-ref r 'text ""))))

(define shim (text-of "hooks/codex-shim.sh"))
(define session (text-of "hooks/codex-session.sh"))
(define watcher (text-of "hooks/codex-continue.sh"))
(define unit (text-of "systemd/toolscheme-codex-continue.service"))

;; The one spelling of the socket, which every file must either use or defer to.
(define socket-path "$STATE/run/codex.sock")

(list
  (list 'checks-hold
        (and
          ;; Each file was found at all; an empty string would pass the
          ;; contains? checks below by vacuity in the negative cases.
          (not (string-null? shim))
          (not (string-null? session))
          (not (string-null? watcher))
          (not (string-null? unit))

          ;; Copies of the shim recognise each other by content, so the marker
          ;; must be present -- and must be the string the scan looks for.
          (string-contains? shim "TOOLSCHEME_CODEX_SHIM_MARKER")
          (string-contains? shim "looks_like_shim")

          ;; No hardcoded path to the real binary. /bin/codex was wrong on any
          ;; machine that installs it elsewhere, which is most of them.
          (not (string-contains? shim "REAL=/bin/codex"))
          (not (string-contains? shim "REAL=/usr/bin/codex"))

          ;; The shim owns the socket path and keeps its directory private.
          (string-contains? shim socket-path)
          (string-contains? shim "chmod 700")

          ;; The watcher must agree with it, since it is the side that connects.
          (string-contains? watcher socket-path)

          ;; The session wrapper defers rather than duplicating: it names the
          ;; shim and no socket of its own.
          (string-contains? session "codex-shim.sh")
          (not (string-contains? session "codex.sock"))

          ;; The timer runs the installed copy. A unit pointing into a checkout
          ;; works until the checkout moves, and then fails at 3am in silence.
          (string-contains? unit "/.local/share/toolscheme/hooks/codex-continue.sh")
          (not (string-contains? unit "/Src/")))))
