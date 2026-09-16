;;; hooks.scm -- observe what a coding agent actually does, while it does it.
;;;
;;; A PreToolUse/PostToolUse hook sees every tool call live: the command, the
;;; working directory it runs in, and -- joining the two events by tool_use_id --
;;; how long it took and how many bytes came back. Transcripts give none of that
;;; reliably: the flat schema has no directory at all, and neither has timings.
;;;
;;; This observes only. It never denies, never rewrites, and never fails in a way
;;; the agent can notice: every path is wrapped, and the script returns a non-list
;;; so the process exits 0 whatever happened. A hook that breaks the session it is
;;; measuring is worse than no measurement.

;; One record has to stay under PIPE_BUF for O_APPEND to be atomic, because tool
;; calls run in parallel batches and two appends can interleave otherwise.
(define hook-record-limit 3500)
(define hook-command-limit 2000)
(define hook-input-limit 600)

(define (clip text limit)
  (if (<= (string-length text) limit)
      text
      (string-append (substring text 1 limit) "...")))

(define (hook-log-path)
  (or (env-value "TOOLSCHEME_OBSERVATIONS") ".toolscheme/observations.jsonl"))

(define (parent-directory path)
  (let loop ((i (string-length path)))
    (cond ((< i 1) "")
          ((char=? (string-ref path i) #\/) (substring path 1 (- i 1)))
          (else (loop (- i 1))))))

(define (hook-request)
  (let ((parsed (json-parse standard-input)))
    (if (error? parsed) '() (field-ref parsed 'value))))

;; The shape of a record is chosen so the analyzer can read it as a third
;; transcript schema rather than needing a separate pipeline.
(define (hook-observation request)
  (let* ((event (field-ref request "hook_event_name" ""))
         ;; A failed call fires PostToolUseFailure, not PostToolUse. Testing only
         ;; for the latter files the failure as a second `pre`, which leaves the
         ;; original call unpaired and quietly drops it from every timing.
         (finished (or (string=? event "PostToolUse")
                       (string=? event "PostToolUseFailure")))
         (input (field-ref request "tool_input" '()))
         (command (let ((c (field-ref input "command" #f))) (if (string? c) c "")))
         (response (field-ref request "tool_response" #f)))
    (list (list "source" "toolscheme-hook")
          (list "event" (if finished "post" "pre"))
          (list "ok" (not (string=? event "PostToolUseFailure")))
          (list "session" (field-ref request "session_id" ""))
          (list "call" (field-ref request "tool_use_id" ""))
          (list "tool" (field-ref request "tool_name" ""))
          (list "cwd" (field-ref request "cwd" ""))
          (list "command" (clip command hook-command-limit))
          ;; Enough of the input to tell one invocation from another, which is what
          ;; repeat detection needs; not enough to store the agent's whole payload.
          (list "input" (clip (write-to-string input) hook-input-limit))
          (list "at" (field-ref (time) 'epoch-milliseconds))
          (list "bytes" (if (absent? response) 0 (string-length (write-to-string response)))))))

(define (hook-append record)
  (let* ((path (hook-log-path))
         (line (clip (field-ref (json-write record) 'text) hook-record-limit))
         (written (write-file path (string-append line "\n") '((append #t)))))
    (if (error? written)
        ;; The log directory may not exist yet. Make it once and try again; if that
        ;; fails too, the observation is dropped rather than raised.
        (let ((parent (parent-directory path)))
          (if (string-null? parent)
              written
              (begin (mkdir parent '((parents #t)))
                     (write-file path (string-append line "\n") '((append #t))))))
        written)))

(define (hook-observe)
  (catch-errors
    (lambda ()
      (let ((request (hook-request)))
        (if (null? request) #f (hook-append (hook-observation request))))))
  ;; Deliberately not a list: the CLI derives its exit status from a record's
  ;; `error` field, and this must always exit 0.
  #t)
