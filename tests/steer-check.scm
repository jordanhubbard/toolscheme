;;; steer-check.scm -- advice: what is said, when, and how often.
;;;
;;; The bar for advice is different in kind from the bar for a rewrite. A rewrite
;;; must be proven byte-identical because the agent cannot see it happened. Advice
;;; changes nothing about what the call returns, so it cannot be wrong about the
;;; answer -- only about whether it was worth the context it costs. That makes
;;; "when do we stay quiet" the property worth testing.

(define (bash-request session command)
  (list (list "hook_event_name" "PreToolUse")
        (list "session_id" session)
        (list "tool_name" "Bash")
        (list "tool_input" (list (list "command" command)))))

(define (tool-request session tool input)
  (list (list "hook_event_name" "PreToolUse")
        (list "session_id" session)
        (list "tool_name" tool)
        (list "tool_input" input)))

(define (said request keys)
  (let ((found (advice-for request keys)))
    (if found (field-ref found 'text) #f)))

;; Recognizing the wait, in both spellings the corpus contains.
(define long-shell (sleeping-for (bash-request "s" "sleep 55")))
(define long-codex (sleeping-for (tool-request "s" "sleep" '(("duration_ms" 55000)))))
(define short (sleeping-for (bash-request "s" "sleep 2")))
(define piped (sleeping-for (bash-request "s" "make build && sleep 30")))

(define sleep-advice (said (bash-request "s" "sleep 55") '()))
(define sleep-again (said (bash-request "s" "sleep 45") '("\tADVISED-SLEEP")))
(define short-advice (said (bash-request "s" "sleep 2") '()))

;; A repeat is judged on the whole invocation: two greps for different patterns are
;; not the same work, and saying they are would be advice that is simply false.
(define key-a (steer-key (tool-request "s" "Read" '(("filePath" "/a.md")))))
(define key-b (steer-key (tool-request "s" "Read" '(("filePath" "/b.md")))))
(define repeat-advice (said (tool-request "s" "Read" '(("filePath" "/a.md"))) (list key-a)))
(define no-repeat (said (tool-request "s" "Read" '(("filePath" "/b.md"))) (list key-a)))

;; Codex rejects `permissionDecision: allow` unless a rewrite accompanies it, so
;; advice alone must not carry one -- it would turn a note into a permission grant.
(define advice-only (advice-only-decision "hello"))
(define advice-fields (field-ref advice-only "hookSpecificOutput"))
(define combined
  (decision-with-advice
    (list (list "hookSpecificOutput"
                (list (list "hookEventName" "PreToolUse")
                      (list "permissionDecision" "allow")
                      (list "updatedInput" (list (list "command" "x"))))))
    "note"))
(define combined-fields (field-ref combined "hookSpecificOutput"))

(define checks
  (list (list 'shell-sleep-ms long-shell)
        (list 'codex-sleep-ms long-codex)
        (list 'short-sleep-ms short)
        (list 'sleep-in-a-pipeline-ms piped)
        (list 'advises-on-long-sleep (and sleep-advice #t))
        (list 'silent-when-already-said (not sleep-again))
        (list 'silent-on-short-sleep (not short-advice))
        (list 'advises-on-repeat (and repeat-advice #t))
        (list 'silent-on-different-argument (not no-repeat))
        (list 'advice-carries-no-permission
              (absent? (field-ref advice-fields "permissionDecision" #f)))
        (list 'rewrite-keeps-its-permission
              (equal? (field-ref combined-fields "permissionDecision" "") "allow"))
        (list 'disabled-by-default (not (steer-enabled?)))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (= long-shell 55000)
                 (= long-codex 55000)
                 (= short 2000)
                 (= piped 30000)
                 (and sleep-advice #t)
                 (not sleep-again)
                 (not short-advice)
                 (and repeat-advice #t)
                 (not no-repeat)
                 (not (string=? key-a key-b))
                 (absent? (field-ref advice-fields "permissionDecision" #f))
                 (equal? (field-ref combined-fields "permissionDecision" "") "allow")
                 (string-contains? (field-ref combined-fields "additionalContext" "") "note")
                 (not (steer-enabled?)))))
