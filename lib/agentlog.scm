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
        (list 'agent (nth-or rest 3 ""))
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
                 ;; A rewritten command is toolscheme's own doing, not a choice the
                 ;; agent made; the call still counts, but its shape is not demand.
                 (if (field-ref record "rewritten" #f) '() input)
                 (field-ref record "bytes" 0)
                 0 0
                 (field-ref record "cwd" "")
                 (field-ref record "call" "")
                 (field-ref record "at" 0)
                 (field-ref record "agent" "")))))

;; Codex writes a fourth schema: one JSON object per rollout record, with tool
;; calls as `response_item` payloads. Unlike either Claude format it carries an
;; epoch timestamp and a call id on every record, so durations come out of a
;; Codex transcript without needing a live hook at all.
(define (codex-call-time payload)
  (let ((meta (field-ref payload "internal_chat_message_metadata_passthrough" '())))
    (let ((created (field-ref meta "create_time" 0)))
      ;; Codex records seconds as a float; milliseconds keep the resolution that
      ;; matters without pretending to more of it.
      (if (number? created) (floor (* 1000 created)) 0))))

(define (codex-output-bytes payload)
  (let ((output (field-ref payload "output" '())))
    (if (list? output)
        (fold-left (lambda (n block) (+ n (string-length (field-ref block "text" "")))) 0 output)
        (string-length (write-to-string output)))))

(define (codex-event record)
  (let* ((payload (field-ref record "payload" '()))
         (kind (field-ref payload "type" ""))
         (name (field-ref payload "name" ""))
         (call (field-ref payload "call_id" ""))
         (at (codex-call-time payload)))
    (cond
      ;; Code mode: the shell arrives wrapped in JavaScript.
      ((string=? kind "custom_tool_call")
       (let ((input (field-ref payload "input" "")))
         (list (event 'tool-call name (list (list "command" (hook-command-of input)))
                      0 0 0 (hook-workdir-of input "") call at))))
      ((string=? kind "function_call")
       (let* ((raw (field-ref payload "arguments" ""))
              (parsed (if (string? raw) (json-parse raw) #f))
              (arguments (if (or (not parsed) (error? parsed)) '() (field-ref parsed 'value))))
         (list (event 'tool-call name arguments 0 0 0 "" call at))))
      ((or (string=? kind "custom_tool_call_output") (string=? kind "function_call_output"))
       (list (event 'tool-result "" '() (codex-output-bytes payload) 0 0 "" call at)))
      (else '()))))

;; The two schemas are told apart by a field only the flat one has. Getting this
;; test wrong is invisible: every nested record falls through the flat parser,
;; which finds no tool calls in it and reports an empty corpus rather than an error.
(define (events-of-record record)
  (cond ((equal? (field-ref record "source" #f) "toolscheme-hook") (hook-event record))
        ((equal? (field-ref record "type" #f) "response_item") (codex-event record))
        ((absent? (field-ref record "tool_name" #f)) (nested-event record))
        (else (flat-event record))))

;; Accepts a path or the transcript text itself, so pasting a session into the
;; analyzer needs no file plumbing.
(define (agent-log-text source)
  (if (string-contains? source "\n")
      source
      (let ((file (read-file source (list (list 'limit 67108864)))))
        (if (error? file) "" (field-ref file 'text)))))

;; An agent may invoke a hook more than once for the same event -- this one is
;; called twice per tool call, a few milliseconds apart with an identical call id --
;; and a log that takes both at face value doubles every count in every report.
;; The event is identified by the session, the call and which end of it this is;
;; anything else keeps its position as its identity and is never dropped.
(define (record-identity record index)
  (if (equal? (field-ref record "source" #f) "toolscheme-hook")
      (string-append (field-ref record "session" "") "|"
                     (field-ref record "call" "") "|"
                     (field-ref record "event" ""))
      (string-append "#" (number->string index))))

(define (dedup-records records)
  (dedup-keyed
    (let loop ((i 1) (rest records) (out '()))
      (if (null? rest)
          (reverse out)
          (loop (+ i 1) (cdr rest)
                (cons (list (record-identity (car rest) i) (car rest)) out))))))

(define (agent-log-events source)
  (let* ((text (agent-log-text source))
         (records (dedup-records
                    (map (lambda (line)
                           (let ((parsed (json-parse line)))
                             (if (error? parsed) '() (field-ref parsed 'value))))
                         (json-lines text))))
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

;; ---------------------------------------------------------------------------
;; Reading a command out of an agent's tool input
;; ---------------------------------------------------------------------------
;;
;; Claude Code hands over {"command": "..."}. Codex in code mode hands over
;; JavaScript that calls tools.exec_command({cmd:"...", workdir:"..."}) -- and
;; those object keys are unquoted, so it is JavaScript and not JSON, which a JSON
;; parser rejects outright. One snippet may also contain several such calls.
;; Both agents are invoking a shell; only the wrapping differs, so both are
;; unwrapped here rather than in two separate adapters.

;; Reads the JavaScript string literal starting at `from` (which must be the
;; opening quote), returning (text next-index) or #f. Escapes are honoured because
;; a command containing \" would otherwise terminate the value early and truncate
;; whatever followed.
(define (js-string-at text from)
  (if (or (> from (string-length text)) (not (char=? (string-ref text from) #\")))
      #f
      (let loop ((i (+ from 1)) (out ""))
        (if (> i (string-length text))
            #f
            (let ((c (string-ref text i)))
              (cond ((char=? c #\") (list out (+ i 1)))
                    ((char=? c #\\)
                     (if (> (+ i 1) (string-length text))
                         #f
                         (let ((e (string-ref text (+ i 1))))
                           (loop (+ i 2)
                                 (string-append out
                                                (cond ((char=? e #\n) "\n")
                                                      ((char=? e #\t) "\t")
                                                      ((char=? e #\r) "\r")
                                                      (else (list->string (list e)))))))))
                    (else (loop (+ i 1) (string-append out (list->string (list c)))))))))))

;; Every value of `key` in the snippet, in order. Codex writes JavaScript object
;; literals with bare keys (`cmd:`) while JSON quotes them (`"cmd":`), and the two
;; appear in the same corpus, so the closing quote is skipped when present rather
;; than being searched for as part of the key.
(define (key-value-start text from key)
  (let ((hit (string-index (substring text from (string-length text)) key)))
    (if (not hit)
        #f
        (let* ((at (+ from hit -1))
               (after (+ at (string-length key)))
               (skipped (if (and (<= after (string-length text))
                                 (char=? (string-ref text after) #\"))
                            (+ after 1)
                            after)))
          (if (and (<= skipped (string-length text))
                   (char=? (string-ref text skipped) #\:))
              (list (+ skipped 1) (+ at 1))
              (list #f (+ at 1)))))))

(define (js-values key text)
  (let loop ((from 1) (out '()))
    (let ((found (key-value-start text from key)))
      (cond ((not found) (reverse out))
            ((not (car found)) (loop (cadr found) out))
            (else
              (let ((quoted (js-string-at text (car found))))
                (if quoted
                    (loop (cadr quoted) (cons (car quoted) out))
                    (loop (cadr found) out))))))))

(define (exec-commands text)
  (if (string? text) (js-values "cmd" text) '()))

;; Several commands in one snippet are one tool call that ran a small script, so
;; they are joined: the shape analysis reads the union, and a rewrite has to cover
;; all of it or cover none.
(define (hook-command-of input)
  (let ((direct (field-ref input "command" #f)))
    (cond ((string? direct) direct)
          ((string? input) (string-join (exec-commands input) "\n"))
          (else
            (let ((nested (field-ref input "input" #f)))
              (if (string? nested) (string-join (exec-commands nested) "\n") ""))))))

(define (hook-workdir-of input fallback)
  (let* ((text (cond ((string? input) input)
                     ((string? (field-ref input "input" #f)) (field-ref input "input" ""))
                     (else #f)))
         (found (if text (js-values "workdir" text) '())))
    (if (null? found) fallback (car found))))

;; Rebuilding an input with a different command. Only a single-command snippet is
;; understood; anything else returns #f so the caller leaves the call alone rather
;; than guessing at a structure it does not recognize.
(define (hook-input-with-command input replacement)
  (cond ((string? (field-ref input "command" #f))
         (map (lambda (pair)
                (if (equal? (car pair) "command") (list "command" replacement) pair))
              input))
        (else
          (let* ((text (cond ((string? input) input)
                             ((string? (field-ref input "input" #f)) (field-ref input "input" ""))
                             (else #f)))
                 (commands (if text (exec-commands text) '())))
            (if (or (not text) (not (= (length commands) 1)))
                #f
                (let ((rebuilt (string-replace text (car commands) replacement)))
                  (if (string? input)
                      rebuilt
                      (map (lambda (pair)
                             (if (equal? (car pair) "input") (list "input" rebuilt) pair))
                           input))))))))
