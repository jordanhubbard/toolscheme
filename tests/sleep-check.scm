;;; sleep-check.scm -- refusing a fixed wait, and the cases that must not be.
;;;
;;; The largest measured waste in the corpus: 1,168 fixed-timer sleeps, 11.9
;;; hours, against 22 seconds for the whole tool-substitution loop. Two milder
;;; mechanisms failed on it first -- the instruction was already in AGENTS.md and
;;; the sleeping continued, and the hook advice fired and was read past -- which
;;; is why this one refuses.
;;;
;;; Most of what is checked here is what it leaves alone. A rule that refuses a
;;; legitimate wait has no correct answer to offer, and gets switched off.

(define threshold 10000)

(define (shell-request command)
  (list (list "hook_event_name" "PreToolUse")
        (list "tool_name" "Bash")
        (list "cwd" "/w")
        (list "tool_input" (list (list "command" command)))))

;; Codex asks for a wait as a tool call with a duration, not as a shell command.
(define (codex-request ms)
  (list (list "hook_event_name" "PreToolUse")
        (list "tool_name" "clocksleep")
        (list "cwd" "/w")
        (list "turn_id" "t1")
        (list "tool_input" (list (list "duration_ms" ms)))))

(define (refused? request) (and (sleep-refusal request threshold) #t))

(define (reason request)
  (let ((d (sleep-refusal request threshold)))
    (if d (field-ref (field-ref d "hookSpecificOutput") "permissionDecisionReason" "") "")))

(define checks
  (list
    ;; Off unless asked for. Every mechanism in this project is.
    (list 'off-by-default (not (refuse-sleep-enabled?)))
    ;; The two spellings that make up the 11.9 hours.
    (list 'refuses-a-long-shell-sleep (refused? (shell-request "sleep 45")))
    (list 'refuses-a-codex-wait (refused? (codex-request 45000)))
    ;; A short wait is not the problem and refusing it would be noise.
    (list 'allows-a-short-sleep (not (refused? (shell-request "sleep 2"))))
    ;; `(sleep 25; touch READY) &` is the work being simulated, not the agent
    ;; waiting for it: it returns at once. Counting it would make every such
    ;; setup a false accusation.
    (list 'allows-backgrounded-work
          (not (refused? (shell-request "(sleep 25; touch READY) &"))))
    (list 'ignores-other-calls (not (refused? (shell-request "grep -n x f.c"))))
    ;; Only PreToolUse can refuse; a finished call cannot be taken back.
    (list 'ignores-post-events
          (not (refused? (list (list "hook_event_name" "PostToolUse")
                               (list "tool_name" "Bash")
                               (list "tool_input" (list (list "command" "sleep 45")))))))
    ;; A refusal has to leave a correct path open. 407 of the measured waits
    ;; were on something remote with no local condition to watch, and demanding
    ;; wait-for for those would be a rule with no right answer.
    (list 'offers-wait-for (string-contains? (reason (shell-request "sleep 45")) "wait-for"))
    (list 'leaves-polling-available
          (string-contains? (reason (shell-request "sleep 45")) "short increments"))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (not (refuse-sleep-enabled?))
                 (refused? (shell-request "sleep 45"))
                 (refused? (codex-request 45000))
                 (not (refused? (shell-request "sleep 2")))
                 (not (refused? (shell-request "(sleep 25; touch READY) &")))
                 (not (refused? (shell-request "grep -n x f.c")))
                 (string-contains? (reason (shell-request "sleep 45")) "wait-for")
                 (string-contains? (reason (shell-request "sleep 45")) "short increments"))))
