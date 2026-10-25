;;; poll-check.scm -- only the measured, simple local-file loop is refused.

(define (poll-request command event)
  (list (list "hook_event_name" event)
        (list "tool_name" "Bash")
        (list "tool_input" (list (list "command" command)))))

(define exact (poll-request "while [ ! -e READY ]; do sleep 1; done" "PreToolUse"))
(define rejected (poll-refusal exact))
(define reason (field-ref (field-ref rejected "hookSpecificOutput" '())
                          "permissionDecisionReason" ""))

(list (list 'checks
            (list (list 'off-by-default (not (setting-on? "TOOLSCHEME_REFUSE_POLL")))
                  (list 'exact-path (simple-poll-path "while [ ! -e READY ]; do sleep 1; done"))
                  (list 'denied (field-ref (field-ref rejected "hookSpecificOutput" '())
                                           "permissionDecision" ""))
                  (list 'replacement (string-contains? reason "wait-for"))
                  (list 'other-work (poll-refusal (poll-request "while [ ! -e READY ]; do sleep 1; done; echo done" "PreToolUse")))
                  (list 'remote-poll (poll-refusal (poll-request "while ! gh run view 123; do sleep 1; done" "PreToolUse")))
                  (list 'not-a-shell (poll-refusal
                                      (list (list "hook_event_name" "PreToolUse")
                                            (list "tool_name" "apply_patch")
                                            (list "tool_input" (list (list "command" "while [ ! -e READY ]; do sleep 1; done"))))))
                  (list 'post-event (poll-refusal (poll-request "while [ ! -e READY ]; do sleep 1; done" "PostToolUse")))))
      (list 'checks-hold
            (and (not (setting-on? "TOOLSCHEME_REFUSE_POLL"))
                 (equal? (simple-poll-path "while [ ! -e READY ]; do sleep 1; done") "READY")
                 (equal? (field-ref (field-ref rejected "hookSpecificOutput" '())
                                    "permissionDecision" "") "deny")
                 (string-contains? reason "wait-for")
                 (not (poll-refusal (poll-request "while [ ! -e READY ]; do sleep 1; done; echo done" "PreToolUse")))
                 (not (poll-refusal (poll-request "while ! gh run view 123; do sleep 1; done" "PreToolUse")))
                 (not (poll-refusal
                        (list (list "hook_event_name" "PreToolUse")
                              (list "tool_name" "apply_patch")
                              (list "tool_input" (list (list "command" "while [ ! -e READY ]; do sleep 1; done"))))))
                 (not (poll-refusal (poll-request "while [ ! -e READY ]; do sleep 1; done" "PostToolUse"))))))
