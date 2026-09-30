;;; decide.scm -- what the hook answers, and the record of having answered.
;;;
;;; This was redirect.scm, and most of it was the rewrite path: turn a shell
;;; command into the tool proven to replace it, via the `updatedInput` a
;;; PreToolUse hook may return. That is gone. Measured against 22,006 recorded
;;; calls it could not win, for two reasons that are structural rather than
;;; tuning, and both are written up in docs/relevance.md:
;;;
;;;   A shell call is a program in a language and a tool call is one operation.
;;;   44.2% of real calls compose more than one program, averaging 1.8 and
;;;   reaching 12, so matching one takes as many tool calls as it has stages.
;;;
;;;   The gate required byte-identity, which is the right rule -- an agent cannot
;;;   see that a rewrite happened, so it must not be able to tell -- but that
;;;   makes the byte count equal by construction and leaves latency as the only
;;;   axis, where a fresh interpreter loses to fork. The project's own
;;;   synthesized tools recorded returning 2.8% and 3.8% *more* bytes than the
;;;   commands they replaced, and passed anyway, because the gate never asked for
;;;   a win.
;;;
;;; What remains is the part that pays: refuse, advise, bound, and record which
;;; of those happened.

(define (advice-only-decision text)
  (list (list "hookSpecificOutput"
              (list (list "hookEventName" "PreToolUse")
                    (list "additionalContext" text)))))

(define (session-decision request)
  (let ((note (catch-errors (lambda () (session-start-advice request)))))
    (if (or (error? note) (not (string? note)))
        #f
        (list (list "hookSpecificOutput"
                    (list (list "hookEventName" "SessionStart")
                          (list "additionalContext" note)))))))

;; Bounding a read the agent asked to make whole.
;;
;; This is substitution rather than advice, which is the only mechanism measured
;; to remove a step: told to "prefer" a tool, an agent adds it and carries on.
;; The cost is that a bare read carries no statement of what is wanted -- by the
;; time the agent issues Read(path), the pattern it searched for is gone -- so
;; nothing downstream can bound it except by guessing that the answer comes
;; early. That guess is the whole experiment; it is not a safe default and is off
;; unless asked for.
;;
;; It survives the removal of the rewrite path because it is a different claim.
;; A rewrite asserts that a different call returns the same answer; this asserts
;; only that the agent wanted less than it asked for, and the agent can see the
;; bound in the result.
(define (read-bound) (let ((n (setting "TOOLSCHEME_READ_LIMIT")))
                       (if (string? n) (string->number n) #f)))

(define (bound-read-decision request)
  (let* ((input (field-ref request "tool_input" '()))
         (path (field-ref input "file_path" #f))
         (limit (read-bound)))
    (if (or (not limit)
            (not (string=? (field-ref request "tool_name" "") "Read"))
            (not (string? path))
            ;; Only an unbounded read: one the agent already bounded is its own.
            (not (absent? (field-ref input "limit" #f)))
            (not (absent? (field-ref input "offset" #f))))
        #f
        (list (list "hookSpecificOutput"
                    (list (list "hookEventName" "PreToolUse")
                          (list "permissionDecision" "allow")
                          (list "updatedInput"
                                (append input (list (list "limit" limit))))))))))

(define (hook-decision request)
  (cdr (hook-decision-with-rule request)))

;; Which rule answered, paired with the answer. `hook-run` records the name,
;; because a rule that fires and leaves no trace cannot be judged afterwards --
;; and the redirect was in exactly that position: switched on in production for
;; days, with no way to tell whether it had ever rewritten a single call. The
;; observation was logged; the decision about it was not.
;;
;; `hook-decision` stays as it was so that nothing which only wants the answer
;; has to know about this.
(define (hook-decision-with-rule request)
  (let ((event (field-ref request "hook_event_name" "")))
    (cond ((equal? event "SessionStart") (cons "session" (session-decision request)))
          ((equal? event "Stop")
           (let ((decision (catch-errors (lambda () (continue-decision request)))))
             (cons "continue" (if (or (error? decision) (not decision)) #f decision))))
          (else (tool-decision-with-rule request)))))

(define (tool-decision request)
  (cdr (tool-decision-with-rule request)))

(define (tool-decision-with-rule request)
  (let* ((waited (catch-errors (lambda () (sleep-decision request))))
         (refused (catch-errors (lambda () (dogfood-decision request))))
         (bounded (catch-errors (lambda () (bound-read-decision request))))
         (advice (catch-errors (lambda () (steer-advice request))))
         (advising (and (not (error? advice)) (string? advice) advice)))
    (cond
      ;; A refusal ends it: there is nothing to advise about a call that will
      ;; not run. The wait is checked first because it is the larger waste and
      ;; the more specific complaint.
      ((and (not (error? waited)) waited) (cons "refuse-sleep" waited))
      ((and (not (error? refused)) refused) (cons "dogfood" refused))
      ;; A bounded read is a complete decision on its own.
      ((and (not (error? bounded)) bounded) (cons "bound-read" bounded))
      ((not advising) (cons "" #f))
      (else (cons "steer" (advice-only-decision advising))))))

;; Small on purpose. The question this answers is "which rule fired, on what,
;; how often" -- the decision itself is reconstructible from the rule and the
;; command, and copying it in would put a refusal's whole explanatory paragraph
;; into the log on every sleep.
;;
;; Built separately from being written, so a test can check the record without
;; putting an observation log in whatever directory it runs from. That
;; separation is also what makes the names testable: `record-decision!` is
;; wrapped in `catch-errors` at its only call site -- correctly, since
;; bookkeeping must not cost the agent a call -- and that wrapper silently
;; swallowed two misspelled names while this was being written. A recorder that
;; records nothing is precisely the failure it exists to prevent.
(define (decision-record request rule)
  (list (list "source" "toolscheme-hook")
        (list "event" "decided")
        (list "rule" rule)
        (list "agent" (hook-agent-of request))
        (list "session" (field-ref request "session_id" ""))
        (list "tool" (field-ref request "tool_name" ""))
        (list "command" (clip (hook-command-of (field-ref request "tool_input" '()))
                              hook-command-limit))
        (list "at" (field-ref (time) 'epoch-milliseconds))
        (list "bytes" 0)))

(define (record-decision! request rule)
  (if (string-null? rule)
      #f
      (hook-append (decision-record request rule))))

(define (hook-run)
  (let ((request (catch-errors (lambda () (hook-request)))))
    (if (error? request)
        ""
        (begin
          (catch-errors
            (lambda () (if (null? request) #f (hook-append (hook-observation request)))))
          (let* ((answered (catch-errors (lambda () (hook-decision-with-rule request))))
                 (rule (if (error? answered) "" (car answered)))
                 (decision (if (error? answered) #f (cdr answered))))
            (if (not decision)
                ""
                (begin
                  ;; Recorded before the decision is handed back, and wrapped,
                  ;; because bookkeeping must not be able to cost the agent a
                  ;; call it was going to get.
                  (catch-errors (lambda () (record-decision! request rule)))
                  (field-ref (json-write decision) 'text))))))))
