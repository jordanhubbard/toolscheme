;;; codex-record.scm -- write the observation records for what the scan queued.
;;;
;;; The second half of a two-stage watcher. The scan runs rooted at the Codex
;;; rollouts, because that is what it reads; this runs rooted at the state
;;; directory, because that is what it writes. There is one filesystem root per
;;; run, and the observation log's path is relative to it -- so a single-stage
;;; version reported a successful append while putting the record in
;;; ~/.codex/sessions/observations.jsonl, where nothing would ever read it.
;;;
;;; Takes the scan's own report rather than re-deriving anything, so the two
;;; stages cannot disagree about what was queued.

(define report
  (let* ((raw (setting "TOOLSCHEME_CODEX_REPORT"))
         (parsed (if (string? raw)
                     (catch-errors (lambda () (json-parse raw)))
                     #f)))
    (if (or (not parsed) (error? parsed)) '() (field-ref parsed 'value '()))))

(define threads (field-ref report "threads" '()))

(list (list "recorded" (codex-record-all! threads))
      (list "of" (length threads)))
