;;; intake-check.scm -- both transcript schemas, from pasted text.
;;;
;;; Two schemas exist in the wild and they are told apart by a field only one of
;;; them has. That test was wrong once, and the symptom was not an error: every
;;; nested record fell through the flat parser, which found no tool calls in it and
;;; reported an empty corpus. Silence is the failure mode worth a test.

(define flat-text
  (string-append
    "{\"type\":\"tool_use\",\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"grep -n x f\"}}\n"
    "{\"type\":\"tool_result\",\"tool_name\":\"Bash\",\"tool_output\":\"a\\nb\\nc\"}\n"))

(define nested-text
  (string-append
    "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":"
    "[{\"type\":\"text\",\"text\":\"ok\"},"
    "{\"type\":\"tool_use\",\"name\":\"bash\",\"input\":{\"command\":\"grep -n x f\"}}],"
    "\"usage\":{\"cache_read_input_tokens\":1000,\"cache_creation_input_tokens\":250}}}\n"
    "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[]},"
    "\"toolUseResult\":{\"stdout\":\"a\\nb\\nc\"}}\n"))

(define flat (agent-log-events flat-text))
(define nested (agent-log-events nested-text))
(define both (analyze-events (append (field-ref flat 'events) (field-ref nested 'events))))

;; Same tool, spelled differently by the two schemas, must tally as one.
(define tools (field-ref both 'tools))
(define bash-row
  (let loop ((rest tools))
    (cond ((null? rest) '())
          ((string=? (field-ref (car rest) 'tool "") "bash") (car rest))
          (else (loop (cdr rest))))))

(define checks
  (list (list 'flat-calls (field-ref flat 'calls))
        (list 'nested-calls (field-ref nested 'calls))
        (list 'tool-names-merged (= (field-ref bash-row 'calls 0) 2))
        (list 'shell-parsed (= (field-ref both 'shell-calls) 2))
        (list 'cache-visible (field-ref (field-ref both 'cache) 'available))
        (list 'cache-created (field-ref (field-ref both 'cache) 'cache-created-tokens 0))
        (list 'result-bytes-counted (> (field-ref both 'result-bytes) 0))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (= (field-ref flat 'calls) 1)
                 (= (field-ref nested 'calls) 1)
                 (= (field-ref bash-row 'calls 0) 2)
                 (= (field-ref both 'shell-calls) 2)
                 (eq? (field-ref (field-ref both 'cache) 'available) #t)
                 (= (field-ref (field-ref both 'cache) 'cache-created-tokens 0) 250)
                 (> (field-ref both 'result-bytes) 0))))
