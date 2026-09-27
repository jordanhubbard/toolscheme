;;; codex-continue.scm -- one pass over every Codex thread on this machine.
;;;
;;; The middle of three stages: rooted at the rollouts, because that is what it
;;; reads, and the only stage allowed to run a process. It is handed the counts
;;; from codex-used.scm rather than deriving them, because the ledger it would
;;; need lives under a different filesystem root and because a bound that reads
;;; the same file the agent rewrites is not a bound.
;;;
;;; Reports what it saw either way, so a dry run and a live run read the same
;;; apart from the "queued" field. Nothing is queued unless the feature is on.
;;;
;;; Emitted as JSON rather than as a value, because the recording stage is handed
;;; this report verbatim and has to parse it. `json-write` answers a record --
;;; ((text "...") (bytes N)) -- rather than the string, so the text has to be
;;; taken out of it; returned whole, the report reached the next stage escaped a
;;; second time and it silently recorded nothing.

(define counts
  (let* ((raw (setting "TOOLSCHEME_CODEX_USED"))
         (parsed (if (string? raw) (catch-errors (lambda () (json-parse raw))) #f)))
    (if (or (not parsed) (error? parsed)) '() (field-ref parsed 'value '()))))

(field-ref
  (json-write
    (let ((enabled (codex-enabled?)))
      (list (list "enabled" enabled)
            (list "socket" (codex-socket))
            (list "idle-threshold" (codex-idle-seconds))
            (list "threads-known" (length counts))
            (list "threads" (codex-scan enabled counts)))))
  'text "")
