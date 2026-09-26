;;; codex-continue.scm -- one pass over every Codex thread on this machine.
;;;
;;; Reports what it saw either way, so a dry run and a live run read the same
;;; apart from the "queued" field. Nothing is queued unless the feature is on.

;;; Emitted as JSON rather than as a value, because the second stage --
;;; codex-record.scm, which writes the observation records this cannot reach
;;; from its own sandbox root -- is handed this report verbatim and must parse
;;; it. It stays a single readable line in the journal either way.

;;; `json-write` answers a record -- ((text "...") (bytes N)) -- rather than the
;;; string itself, so the text has to be taken out of it. Returned whole, the
;;; journal showed the wrapper and the shell's test for a queued thread matched
;;; nothing, because the JSON inside had been escaped a second time.
(field-ref
  (json-write
    (let ((enabled (codex-enabled?)))
      (list (list "enabled" enabled)
            (list "socket" (codex-socket))
            (list "idle-threshold" (codex-idle-seconds))
            (list "threads" (codex-scan enabled)))))
  'text "")
