;;; decide-check.scm -- what the hook answers, and that it records having answered.
;;;
;;; This was redirect-check.scm, and almost all of it exercised the rewrite
;;; policy: which command shapes a published tool could claim, and when a claim
;;; was worth acting on. That policy is gone, so those cases are gone with it.
;;; What remains is the part that pays -- refuse, advise, bound -- and the record
;;; that says which of them happened.

(define (request-for command)
  (list (list "hook_event_name" "PreToolUse")
        (list "tool_name" "Bash")
        (list "cwd" "/w")
        (list "tool_input" (list (list "command" command)))))

;; Carries `turn_id`, which is how a request is known to have come from Codex.
(define decided-request
  (cons (list "turn_id" "t1") (cons (list "session_id" "s1") (request-for "sleep 40"))))

(define quiet-request (request-for "make test"))

(define (rule-for request) (car (tool-decision-with-rule request)))
(define (answer-for request) (cdr (tool-decision-with-rule request)))

;; A read the agent asked to make whole, and one it already bounded.
(define (read-request extra)
  (append (list (list "hook_event_name" "PreToolUse")
                (list "tool_name" "Read"))
          (list (list "tool_input" (append (list (list "file_path" "/w/a.txt")) extra)))))

(list
  (list 'checks
        (list
          ;; Nothing to say about an ordinary command.
          (list 'quiet-command-gets-no-rule (rule-for quiet-request))
          (list 'quiet-command-gets-no-answer (answer-for quiet-request))

          ;; The record names the rule, the agent and the command, and is built
          ;; without writing anything -- so a test can check it without leaving an
          ;; observation log in whatever directory it runs from.
          (list 'record-names-the-rule
                (field-ref (decision-record decided-request "refuse-sleep") "rule" ""))
          (list 'record-names-the-agent
                (field-ref (decision-record decided-request "refuse-sleep") "agent" ""))
          (list 'record-carries-the-command
                (field-ref (decision-record decided-request "refuse-sleep") "command" ""))

          ;; Reading is bounded only when asked for, and only when the agent did
          ;; not bound it itself.
          (list 'unbounded-read-left-alone-by-default (bound-read-decision (read-request '())))))

  (list 'checks-hold
        (and
          (equal? (rule-for quiet-request) "")
          (not (answer-for quiet-request))

          (equal? (field-ref (decision-record decided-request "refuse-sleep") "rule" "")
                  "refuse-sleep")
          (equal? (field-ref (decision-record decided-request "refuse-sleep") "agent" "")
                  "codex")
          (equal? (field-ref (decision-record decided-request "refuse-sleep") "command" "")
                  "sleep 40")

          ;; Nothing is written when no rule fired, or every call that passed
          ;; cleanly would be logged twice.
          (not (record-decision! decided-request ""))

          ;; Off unless TOOLSCHEME_READ_LIMIT says otherwise.
          (not (bound-read-decision (read-request '())))
          (not (read-bound)))))
