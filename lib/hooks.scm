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

;; Strings here are native bytes, so cutting at a byte offset can slice a
;; multi-byte character in half and leave a JSON string no strict reader will
;; accept -- a box-drawing `-` in a command truncated after its first byte was
;; enough to make the log undecodable. A continuation byte is 10xxxxxx, so backing
;; up while the next byte is one lands on a character boundary.
(define (utf8-boundary text limit)
  (let loop ((n limit))
    (cond ((<= n 0) 0)
          ((>= n (string-length text)) (string-length text))
          (else
            (let ((next (char->integer (string-ref text (+ n 1)))))
              (if (and (>= next 128) (< next 192)) (loop (- n 1)) n))))))

;; Which tools actually run a shell. Both agents call it Bash; Codex code mode
;; wraps it in JavaScript and names the tool `exec`. Everything else that happens
;; to have a `command` field is something else wearing the same word.
(define (shell-tool? name input)
  (let ((lower (string-downcase name)))
    (or (string-contains? lower "bash")
        (string-contains? lower "shell")
        (string=? lower "exec")
        (string=? lower "exec_command")
        (and (string? input) (string-contains? input "tools.exec_command(")))))

(define (clip text limit)
  (if (<= (string-length text) limit)
      text
      (let ((cut (utf8-boundary text limit)))
        (if (<= cut 0) "..." (string-append (substring text 1 cut) "...")))))

;; The log lives in the state directory, which is the sandbox root the hook runs
;; under -- so the path is a bare filename and the hook has write access to that
;; directory and to nothing else. It observes every project and can touch none of
;; them.
(define (hook-log-path)
  (or (env-value "TOOLSCHEME_OBSERVATIONS") "observations.jsonl"))

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
         ;; A `command` field does not make something a shell command. Codex's
         ;; apply_patch carries the patch under that name, and shell-parsing a diff
         ;; invents commands called `+` and `***` -- which between them topped the
         ;; live corpus's ranking with nearly ten thousand calls that never happened.
         (command (if (shell-tool? (field-ref request "tool_name" "") input)
                      (hook-command-of input)
                      ""))
         (response (field-ref request "tool_response" #f)))
    (list (list "source" "toolscheme-hook")
          ;; Which agent this came from. `turn_id` is Codex's own documented
          ;; extension to the hook payload and Claude Code does not send it, which
          ;; is a more reliable marker than the tool name: both spell a shell call
          ;; "Bash". Worth keeping, because the two have measurably different
          ;; habits and a merged corpus that cannot tell them apart averages them.
          (list "agent" (cond ((not (absent? (field-ref request "turn_id" #f))) "codex")
                              ((not (absent? (field-ref request "prompt_id" #f))) "claude-code")
                              (else "unknown")))
          (list "event" (if finished "post" "pre"))
          (list "ok" (not (string=? event "PostToolUseFailure")))
          (list "session" (field-ref request "session_id" ""))
          (list "call" (field-ref request "tool_use_id" ""))
          (list "tool" (field-ref request "tool_name" ""))
          (list "cwd" (field-ref request "cwd" ""))
          (list "command" (clip command hook-command-limit))
          ;; A PostToolUse event carries the *rewritten* input, so once redirection
          ;; is on the log fills with toolscheme's own invocations presented as
          ;; commands the agent chose. Left unmarked they feed straight back into
          ;; the shape rankings that decide what to replace next -- the analyzer
          ;; measuring its own output and calling it demand.
          (list "rewritten" (and (string-contains? command "run-tool.scm") #t))
          ;; Enough of the input to tell one invocation from another, which is what
          ;; repeat detection needs; not enough to store the agent's whole payload.
          (list "input" (clip (write-to-string input) hook-input-limit))
          (list "at" (field-ref (time) 'epoch-milliseconds))
          (list "bytes" (if (absent? response) 0 (string-length (write-to-string response)))))))

;;; ---------------------------------------------------------------------------
;;; Rotation.
;;;
;;; The log grows by roughly 20MB a day under steady use and nothing was
;;; rotating it: the copy on this machine reached 156MB and 164,000 records
;;; before anyone looked. A hook that runs on every tool call cannot be allowed
;;; to fill a disk, and "the operator will notice" is not a bound.
;;;
;;; Generations rather than truncation, because the analysis wants history and
;;; discarding the oldest is the only part that loses anything. The size is
;;; checked after the append, so the cost is one stat per call and never a read
;;; of the log itself.

(define hook-default-log-bytes 67108864)
(define hook-default-generations 3)

(define (hook-setting-number name fallback)
  (let ((configured (setting name)))
    (if (string? configured)
        (let ((n (string->number configured)))
          (if (number? n) n fallback))
        fallback)))

(define (hook-log-bytes)
  (hook-setting-number "TOOLSCHEME_LOG_MAX_BYTES" hook-default-log-bytes))

(define (hook-generations)
  (hook-setting-number "TOOLSCHEME_LOG_KEEP" hook-default-generations))

(define (hook-generation-path path n)
  (string-append path "." (number->string n)))

;; Oldest first, so nothing is overwritten on the way down. The last generation
;; is removed rather than shifted, which is where the bound actually comes from.
(define (hook-rotate path keep)
  (let ((oldest (hook-generation-path path keep)))
    (catch-errors (lambda () (rm (list oldest))))
    (let loop ((n (- keep 1)))
      (if (< n 1)
          (catch-errors (lambda () (mv path (hook-generation-path path 1))))
          (begin
            (catch-errors (lambda () (mv (hook-generation-path path n)
                                         (hook-generation-path path (+ n 1)))))
            (loop (- n 1)))))))

;; Rotation is best-effort and deliberately silent. Two hooks finishing together
;; can race here, and the worst outcome is a generation that holds slightly more
;; or less than its share -- which costs nothing, where raising would cost the
;; session the hook is supposed to be measuring.
(define (hook-rotate-if-needed path)
  (let ((keep (hook-generations))
        (limit (hook-log-bytes)))
    (if (< keep 1)
        #f
        (let ((info (catch-errors (lambda () (stat path '((volatile #t)))))))
          (if (error? info)
              #f
              (if (>= (field-ref info 'size 0) limit)
                  (hook-rotate path keep)
                  #f))))))

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
        (begin (catch-errors (lambda () (hook-rotate-if-needed path)))
               written))))

(define (hook-observe)
  (catch-errors
    (lambda ()
      (let ((request (hook-request)))
        (if (null? request) #f (hook-append (hook-observation request))))))
  ;; Deliberately not a list: the CLI derives its exit status from a record's
  ;; `error` field, and this must always exit 0.
  #t)
