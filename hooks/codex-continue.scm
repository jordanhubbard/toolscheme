;;; codex-continue.scm -- one pass over every Codex thread on this machine.
;;;
;;; Reports what it saw either way, so a dry run and a live run read the same
;;; apart from the "queued" field. Nothing is queued unless the feature is on.

(let ((enabled (codex-enabled?)))
  (list (list "enabled" enabled)
        (list "socket" (codex-socket))
        (list "idle-threshold" (codex-idle-seconds))
        (list "threads" (codex-scan enabled))))
