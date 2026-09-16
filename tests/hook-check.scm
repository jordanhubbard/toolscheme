;;; hook-check.scm -- the live observation path, without a coding agent attached.
;;;
;;; The hook is the one piece of this project that runs inside someone else's
;;; session, on every single tool call. What it must never do is more important
;;; than what it does: never raise, never exceed the size that keeps an append
;;; atomic, never lose the field that makes a record joinable.

(define (observation-of json)
  (hook-observation (field-ref (json-parse json) 'value)))

(define (record-of observation)
  (field-ref (json-parse (field-ref (json-write observation) 'text)) 'value))

(define pre
  (observation-of
    (string-append
      "{\"hook_event_name\":\"PreToolUse\",\"session_id\":\"s\",\"tool_use_id\":\"c1\","
      "\"tool_name\":\"Bash\",\"cwd\":\"/w\",\"tool_input\":{\"command\":\"grep -n x f.c\"}}")))

(define post
  (observation-of
    (string-append
      "{\"hook_event_name\":\"PostToolUse\",\"session_id\":\"s\",\"tool_use_id\":\"c1\","
      "\"tool_name\":\"Bash\",\"cwd\":\"/w\",\"tool_input\":{\"command\":\"grep -n x f.c\"},"
      "\"tool_response\":{\"type\":\"text\",\"text\":\"f.c:1:x\"}}")))

;; A tool with no command at all still has to be told apart from another call to
;; the same tool, or repeat detection collapses every Read into one.
;; A failed call is still a cost, and it arrives under a different event name.
(define failure
  (observation-of
    (string-append
      "{\"hook_event_name\":\"PostToolUseFailure\",\"session_id\":\"s\",\"tool_use_id\":\"c1\","
      "\"tool_name\":\"Bash\",\"cwd\":\"/w\",\"tool_input\":{\"command\":\"false\"},"
      "\"tool_response\":{\"type\":\"text\",\"text\":\"exit 1\"}}")))

;; Once redirection is on, a PostToolUse event carries the command toolscheme
;; substituted, not the one the agent asked for. Counting that as demand would let
;; the analyzer measure its own output and rank it as a pattern worth replacing.
(define rewritten
  (observation-of
    (string-append
      "{\"hook_event_name\":\"PostToolUse\",\"session_id\":\"s\",\"tool_use_id\":\"c4\","
      "\"tool_name\":\"Bash\",\"cwd\":\"/w\",\"tool_input\":{\"command\":"
      "\"/p/toolscheme /p/hooks/run-tool.scm search-read 'grep -n x f' --text\"},"
      "\"tool_response\":{\"type\":\"text\",\"text\":\"f:1:x\"}}")))

(define rewritten-events (events-of-record (record-of rewritten)))

(define read-call
  (observation-of
    (string-append
      "{\"hook_event_name\":\"PreToolUse\",\"session_id\":\"s\",\"tool_use_id\":\"c2\","
      "\"tool_name\":\"Read\",\"cwd\":\"/w\",\"tool_input\":{\"filePath\":\"/a/b.c\"}}")))

;; An agent pasting a large heredoc must not produce a record that exceeds the
;; size an O_APPEND write stays atomic at; two interleaved appends corrupt both.
(define huge
  (observation-of
    (string-append
      "{\"hook_event_name\":\"PreToolUse\",\"session_id\":\"s\",\"tool_use_id\":\"c3\","
      "\"tool_name\":\"Bash\",\"cwd\":\"/w\",\"tool_input\":{\"command\":\""
      (let loop ((n 500) (out "")) (if (= n 0) out (loop (- n 1) (string-append out "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"))))
      "\"}}")))

(define huge-bytes (string-length (field-ref (json-write huge) 'text)))

;; The adapter has to recognize these as a third schema and keep what only live
;; observation knows: the directory, the call id and the timestamp.
(define events (flatten (map (lambda (o) (events-of-record (record-of o))) (list pre post))))
(define first-event (car events))
(define latency (latency-report events))

;; Nothing an agent can send may make the hook raise.
(define malformed (catch-errors (lambda () (events-of-record '()))))

(define checks
  (list (list 'pre-is-call (eq? (field-ref first-event 'kind) 'tool-call))
        (list 'directory-kept (field-ref first-event 'directory))
        (list 'call-id-kept (field-ref first-event 'call))
        (list 'result-bytes (field-ref (cadr events) 'result-bytes))
        (list 'read-distinguished
              (string-contains? (field-ref read-call "input" "") "/a/b.c"))
        (list 'huge-record-bytes huge-bytes)
        (list 'latency-available (field-ref latency 'available))
        (list 'failure-is-post (field-ref failure "event" ""))
        (list 'failure-marked-not-ok (field-ref failure "ok" #t))
        (list 'rewrite-marked (field-ref rewritten "rewritten" #f))
        (list 'rewrite-not-counted-as-demand
              (null? (field-ref (car rewritten-events) 'input '())))
        (list 'malformed-survived (not (error? malformed)))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (eq? (field-ref first-event 'kind) 'tool-call)
                 (eq? (field-ref (cadr events) 'kind) 'tool-result)
                 (string=? (field-ref first-event 'directory "") "/w")
                 (string=? (field-ref first-event 'call "") "c1")
                 (> (field-ref first-event 'at 0) 0)
                 (> (field-ref (cadr events) 'result-bytes 0) 0)
                 (string-contains? (field-ref read-call "input" "") "/a/b.c")
                 (< huge-bytes 4096)
                 (eq? (field-ref latency 'available) #t)
                 (string=? (field-ref failure "event" "") "post")
                 (eq? (field-ref failure "ok" #t) #f)
                 (eq? (field-ref rewritten "rewritten" #f) #t)
                 (null? (field-ref (car rewritten-events) 'input '()))
                 (not (error? malformed)))))
