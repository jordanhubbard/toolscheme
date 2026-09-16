;;; agentlog.scm -- normalize coding-agent transcripts into tool-call events.
;;;
;;; Two on-disk schemas exist in the wild and they differ in shape and casing:
;;;
;;;   nested  ~/.claude/projects/*/*.jsonl
;;;           {"message": {"content": [{"type":"tool_use","name":…,"input":…}],
;;;                        "usage": {...}}, "timestamp": …}
;;;           with a matching later record carrying "toolUseResult"
;;;
;;;   flat    ~/.claude/transcripts/*.jsonl
;;;           {"type":"tool_use","tool_name":…,"tool_input":…,"timestamp":…}
;;;
;;; Both reduce to the same event record, so everything downstream sees one shape.
;;; Adapters are looked up by name, so a third agent is configuration, not code.

;; Tool names are normalized to lower case because the two schemas spell them
;; differently -- "Bash" nested, "bash" flat -- and a corpus that mixes them would
;; otherwise tally the same tool twice and rank both halves too low.
(define (nth-or rest n fallback)
  (cond ((null? rest) fallback)
        ((= n 0) (car rest))
        (else (nth-or (cdr rest) (- n 1) fallback))))

;; Optional trailing values are (directory call at): where the call ran, the id
;; that joins a call to its result, and when it happened. Only the hook adapter
;; supplies the last two -- transcripts record neither -- which is why live
;; observation can report latency and transcript analysis cannot.
(define (event kind tool input bytes cache-read cache-created . rest)
  (list (list 'kind kind)
        (list 'tool (string-downcase tool))
        (list 'input input)
        (list 'directory (nth-or rest 0 ""))
        (list 'call (nth-or rest 1 ""))
        (list 'at (nth-or rest 2 0))
        (list 'result-bytes bytes)
        (list 'cache-read-tokens cache-read)
        (list 'cache-created-tokens cache-created)))

(define (json-lines text)
  (filter (lambda (line) (not (string-null? (string-trim line))))
          (field-ref (text-lines text) 'lines)))

;; Result payloads vary by tool; their size is what the analysis needs, and the
;; canonical written form is a faithful proxy for it.
(define (payload-bytes value)
  (if (absent? value) 0 (string-length (write-to-string value))))

(define (flat-event record)
  (let ((kind (field-ref record "type" "")))
    (if (string=? kind "tool_use")
        (list (event 'tool-call
                     (field-ref record "tool_name" "")
                     (field-ref record "tool_input" '())
                     0 0 0))
        (if (string=? kind "tool_result")
            (list (event 'tool-result
                         (field-ref record "tool_name" "")
                         '()
                         (payload-bytes (field-ref record "tool_output" #f))
                         0 0))
            '()))))

(define (nested-usage record)
  (field-ref (field-ref record "message" '()) "usage" '()))

(define (nested-event record)
  (let* ((message (field-ref record "message" '()))
         (content (field-ref message "content" '()))
         (usage (nested-usage record))
         (cache-read (field-ref usage "cache_read_input_tokens" 0))
         (cache-created (field-ref usage "cache_creation_input_tokens" 0))
         (result (field-ref record "toolUseResult" #f))
         (directory (let ((cwd (field-ref record "cwd" #f)))
                      (if (string? cwd) cwd ""))))
    (append
      ;; A turn's cache accounting belongs to the calls it made, so it rides on
      ;; the events rather than being reported separately.
      (if (list? content)
          (flatten
            (map (lambda (block)
                   (if (and (list? block) (equal? (field-ref block "type" "") "tool_use"))
                       (list (event 'tool-call
                                    (field-ref block "name" "")
                                    (field-ref block "input" '())
                                    0 cache-read cache-created directory))
                       '()))
                 content))
          '())
      (if (absent? result)
          '()
          (list (event 'tool-result "" '() (payload-bytes result) 0 0))))))

;; Toolscheme's own hook writes the third schema. It is the only one that records
;; a working directory, a call id and a timestamp, because it watches the calls
;; happen rather than reading what was written down afterwards.
(define (hook-event record)
  (let* ((tool (field-ref record "tool" ""))
         (command (field-ref record "command" ""))
         (post (string=? (field-ref record "event" "") "post"))
         ;; Rebuilt so the shell analysis and repeat detection both work unchanged:
         ;; a command when there is one, otherwise the clipped argument summary.
         (input (if (string-null? command)
                    (list (list "summary" (field-ref record "input" "")))
                    (list (list "command" command)))))
    (list (event (if post 'tool-result 'tool-call)
                 tool
                 input
                 (field-ref record "bytes" 0)
                 0 0
                 (field-ref record "cwd" "")
                 (field-ref record "call" "")
                 (field-ref record "at" 0)))))

;; The two schemas are told apart by a field only the flat one has. Getting this
;; test wrong is invisible: every nested record falls through the flat parser,
;; which finds no tool calls in it and reports an empty corpus rather than an error.
(define (events-of-record record)
  (cond ((equal? (field-ref record "source" #f) "toolscheme-hook") (hook-event record))
        ((absent? (field-ref record "tool_name" #f)) (nested-event record))
        (else (flat-event record))))

;; Accepts a path or the transcript text itself, so pasting a session into the
;; analyzer needs no file plumbing.
(define (agent-log-text source)
  (if (string-contains? source "\n")
      source
      (let ((file (read-file source (list (list 'limit 67108864)))))
        (if (error? file) "" (field-ref file 'text)))))

(define (agent-log-events source)
  (let* ((text (agent-log-text source))
         (records (map (lambda (line)
                         (let ((parsed (json-parse line)))
                           (if (error? parsed) '() (field-ref parsed 'value))))
                       (json-lines text)))
         (events (flatten (map (lambda (record)
                                 (if (null? record) '() (events-of-record record)))
                               records))))
    (list (list 'events events)
          (list 'count (length events))
          (list 'calls (count-if (lambda (e) (eq? (field-ref e 'kind) 'tool-call)) events)))))

;; Every .jsonl under the given directories, newest first is not needed -- the
;; analysis is order-independent.
(define (agent-log-files directories)
  ;; glob reports paths relative to the capability root, not to the directory it
  ;; searched, so they are already usable as-is.
  (flatten
    (map (lambda (directory)
           (let ((listing (glob "**/*.jsonl" (list (list 'directory directory)))))
             (if (error? listing)
                 '()
                 (map (lambda (entry) (field-ref entry 'path))
                      (field-ref listing 'entries)))))
         directories)))
