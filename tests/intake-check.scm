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
    "\"usage\":{\"cache_read_input_tokens\":1000,\"cache_creation_input_tokens\":250}},"
    "\"cwd\":\"/home/someone/project\"}\n"
    "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[]},"
    "\"toolUseResult\":{\"stdout\":\"a\\nb\\nc\"}}\n"))

;; Codex writes a fourth schema: rollout records whose tool calls wrap the shell
;; in JavaScript, with unquoted object keys that no JSON parser will accept, and
;; sometimes several commands in one call.
(define codex-text
  (string-append
    "{\"type\":\"response_item\",\"payload\":{\"type\":\"custom_tool_call\","
    "\"name\":\"exec\",\"call_id\":\"c9\",\"input\":"
    "\"text(await tools.exec_command({cmd:\\\"grep -n x f.c | head -20\\\","
    "workdir:\\\"/repo\\\"}));\","
    "\"internal_chat_message_metadata_passthrough\":{\"create_time\":1789515871.5}}}\n"
    "{\"type\":\"response_item\",\"payload\":{\"type\":\"custom_tool_call_output\","
    "\"call_id\":\"c9\",\"output\":[{\"type\":\"input_text\",\"text\":\"f.c:1:x\"}],"
    "\"internal_chat_message_metadata_passthrough\":{\"create_time\":1789515871.9}}}\n"))

(define codex (agent-log-events codex-text))
(define codex-call (car (field-ref codex 'events)))

;; An agent may call a hook more than once for the same event. Claude Code calls
;; this one twice per tool call, milliseconds apart with an identical call id, so a
;; reader that takes the log at face value doubles every count it reports.
;; Both agents spell a shell call "Bash", so the tool name cannot tell them apart
;; and a merged corpus would average two different sets of habits together.
(define mixed
  (analyze-events
    (field-ref
      (agent-log-events
        (string-append
          "{\"source\":\"toolscheme-hook\",\"agent\":\"codex\",\"event\":\"pre\","
          "\"session\":\"s1\",\"call\":\"c1\",\"tool\":\"bash\",\"cwd\":\"/w\","
          "\"command\":\"sed -n 1,40p f\",\"input\":\"()\",\"at\":1,\"bytes\":0}\n"
          "{\"source\":\"toolscheme-hook\",\"agent\":\"claude-code\",\"event\":\"pre\","
          "\"session\":\"s2\",\"call\":\"c2\",\"tool\":\"bash\",\"cwd\":\"/w\","
          "\"command\":\"head -20 f\",\"input\":\"()\",\"at\":2,\"bytes\":0}\n"))
      'events)))

(define (agent-calls report name)
  (let loop ((rest (field-ref report 'agents '())))
    (cond ((null? rest) 0)
          ((equal? (field-ref (car rest) 'agent "") name) (field-ref (car rest) 'calls 0))
          (else (loop (cdr rest))))))

(define (hook-line call event command)
  (string-append
    "{\"source\":\"toolscheme-hook\",\"event\":\"" event "\",\"session\":\"s\","
    "\"call\":\"" call "\",\"tool\":\"bash\",\"cwd\":\"/w\",\"command\":\"" command "\","
    "\"input\":\"()\",\"at\":1,\"bytes\":1}\n"))

(define duplicated
  (agent-log-events
    (string-append (hook-line "a" "pre" "grep -n x f")
                   (hook-line "a" "pre" "grep -n x f")
                   (hook-line "b" "pre" "wc -l f")
                   (hook-line "b" "pre" "wc -l f")
                   (hook-line "a" "post" "grep -n x f")
                   (hook-line "a" "post" "grep -n x f"))))

;; Order has to survive deduplication, because consecutive-call analysis reads it.
(define deduped-commands
  (map (lambda (e) (field-ref (field-ref e 'input '()) "command" ""))
       (filter (lambda (e) (eq? (field-ref e 'kind) 'tool-call))
               (field-ref duplicated 'events))))

(define flat (agent-log-events flat-text))
(define nested (agent-log-events nested-text))
(define both (analyze-events (append (field-ref flat 'events) (field-ref nested 'events))))
(define codex-report (analyze-events (field-ref codex 'events)))

;; Same tool, spelled differently by the two schemas, must tally as one.
(define tools (field-ref both 'tools))
(define bash-row
  (let loop ((rest tools))
    (cond ((null? rest) '())
          ((string=? (field-ref (car rest) 'tool "") "bash") (car rest))
          (else (loop (cdr rest))))))

;; Only the nested schema records where a command ran, and the replay gate cannot
;; re-run one without it: the same command means different things in different
;; directories.
(define nested-call (car (field-ref nested 'events)))

;; A corpus of 13,000 calls used to kill the process outright: `adjacent-pairs`
;; recursed inside a `cons`, so the call was not in tail position and it held one
;; stack frame per call. The interpreter guarantees tail calls, which is exactly
;; what makes the one call that is not a tail call easy to write by accident, so
;; the analysis stages that walk a whole corpus are exercised at a size no stack
;; would survive if it were done wrong again.
(define deep-calls
  (let loop ((i 20000) (out '()))
    (if (= i 0) out (loop (- i 1) (cons (list (list 'tool "bash")) out)))))

(define deep-survives
  (and (= (length (adjacent-pairs deep-calls)) 19999)
       (= (length (take deep-calls 19999)) 19999)))

(define checks
  (list (list 'flat-calls (field-ref flat 'calls))
        (list 'working-directory (field-ref nested-call 'directory ""))
        (list 'nested-calls (field-ref nested 'calls))
        (list 'tool-names-merged (= (field-ref bash-row 'calls 0) 2))
        (list 'shell-parsed (= (field-ref both 'shell-calls) 2))
        (list 'cache-visible (field-ref (field-ref both 'cache) 'available))
        (list 'cache-created (field-ref (field-ref both 'cache) 'cache-created-tokens 0))
        (list 'result-bytes-counted (> (field-ref both 'result-bytes) 0))
        (list 'large-corpus-survives deep-survives)
        (list 'agents-separated (field-ref mixed 'agents))
        (list 'duplicate-events-collapsed (field-ref duplicated 'count))
        (list 'order-preserved deduped-commands)
        (list 'codex-command (field-ref (field-ref codex-call 'input '()) "command" ""))
        (list 'codex-workdir (field-ref codex-call 'directory ""))
        (list 'codex-shell-calls (field-ref codex-report 'shell-calls))
        (list 'codex-latency-ms (field-ref (field-ref codex-report 'latency) 'total-ms 0))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (= (field-ref flat 'calls) 1)
                 (string=? (field-ref nested-call 'directory "") "/home/someone/project")
                 (= (field-ref nested 'calls) 1)
                 (= (field-ref bash-row 'calls 0) 2)
                 (= (field-ref both 'shell-calls) 2)
                 (eq? (field-ref (field-ref both 'cache) 'available) #t)
                 (= (field-ref (field-ref both 'cache) 'cache-created-tokens 0) 250)
                 (> (field-ref both 'result-bytes) 0)
                 ;; Codex: JavaScript unwrapped, workdir kept, timing paired.
                 (string=? (field-ref (field-ref codex-call 'input '()) "command" "")
                           "grep -n x f.c | head -20")
                 (string=? (field-ref codex-call 'directory "") "/repo")
                 (= (field-ref codex-report 'shell-calls) 1)
                 (> (field-ref (field-ref codex-report 'latency) 'total-ms 0) 0)
                 ;; Six lines, three distinct events, in the order they happened.
                 (= (field-ref duplicated 'count) 3)
                 (equal? deduped-commands '("grep -n x f" "wc -l f"))
                 (= (agent-calls mixed "codex") 1)
                 (= (agent-calls mixed "claude-code") 1)
                 deep-survives)))
