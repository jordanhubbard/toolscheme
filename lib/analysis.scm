;;; analysis.scm -- rank what an agent's tool use actually costs.
;;;
;;; The report answers four questions, in the order they matter:
;;;   which tools dominate; which shell commands hide inside Bash calls; which
;;;   calls repeat verbatim; and which calls correlate with prompt-cache churn.
;;;
;;; Commands are normalized by *shape*, not by literal text, because
;;; `grep -n needle src/a.c` and `grep -n other src/b.c` are the same opportunity.

(define (bash-command input)
  (let ((command (field-ref input "command")))
    (if (string? command) command "")))

;; A command's shape is its program plus its sorted flags: the invariant part
;; that identifies the pattern, with paths and patterns stripped out.
(define (command-shapes text)
  (let ((parsed (shell-parse text)))
    (map (lambda (command)
           (let ((flags (field-ref command 'flags '())))
             (string-append (field-ref command 'name "")
                            (if (null? flags) "" " ")
                            ;; `sort` is the text tool and returns a record; the
                            ;; list sort is `list-sort`.
                            (string-join (list-sort flags) " "))))
         (field-ref parsed 'commands))))

(define (programs-in text) (field-ref (shell-parse text) 'programs))

(define (tool-calls events)
  (filter (lambda (e) (eq? (field-ref e 'kind) 'tool-call)) events))

;; An invocation's identity for repeat detection: the tool plus its exact
;; arguments. A verbatim repeat is work the agent already paid for.
(define (invocation-key call)
  (string-append (field-ref call 'tool "") " " (write-to-string (field-ref call 'input '()))))

(define (repeat-report calls)
  (let* ((counts (tally (map invocation-key calls)))
         (repeated (filter (lambda (row) (> (cadr row) 1)) counts))
         (wasted (fold-left (lambda (n row) (+ n (- (cadr row) 1))) 0 repeated)))
    (list (list 'repeated-invocations (length repeated))
          (list 'redundant-calls wasted)
          (list 'worst (rows->records (top repeated 10) 'invocation 'calls)))))

;; Cache churn: a tool whose output changes between calls forces the model to
;; re-read the suffix of its prompt. Turns that created cache rather than reading
;; it are the expensive ones, and they are attributed to the tools they ran.
(define (cache-report calls)
  (let* ((created (tally-by (map (lambda (c)
                                   (list (field-ref c 'tool "")
                                         (field-ref c 'cache-created-tokens 0)))
                                 calls)
                            cadr))
         (read (tally-by (map (lambda (c)
                                (list (field-ref c 'tool "")
                                      (field-ref c 'cache-read-tokens 0)))
                              calls)
                         cadr))
         (total-created (fold-left (lambda (n row) (+ n (cadr row))) 0 created))
         (total-read (fold-left (lambda (n row) (+ n (cadr row))) 0 read)))
    ;; Only the nested transcript schema records per-turn token usage. Saying so is
    ;; better than reporting zeros that look like a measurement.
    (if (and (= total-created 0) (= total-read 0))
        (list (list 'available #f)
              (list 'note "this transcript format records no per-turn token usage"))
        (list (list 'available #t)
              (list 'cache-created-tokens total-created)
              (list 'cache-read-tokens total-read)
              (list 'churn-by-tool (rows->records (top created 10) 'tool 'created-tokens))))))

;; Consecutive pairs are fusion candidates: two calls that always follow one
;; another are one call the toolbox does not yet offer.
(define (adjacent-pairs calls)
  (if (or (null? calls) (null? (cdr calls)))
      '()
      (cons (string-append (field-ref (car calls) 'tool "") " -> "
                           (field-ref (cadr calls) 'tool ""))
            (adjacent-pairs (cdr calls)))))

(define (analyze-events events)
  (let* ((calls (tool-calls events))
         (bash (filter (lambda (c) (string-contains? (string-downcase (field-ref c 'tool "")) "bash"))
                       calls))
         (commands (flatten (map (lambda (c) (command-shapes (bash-command (field-ref c 'input '()))))
                                 bash)))
         (programs (flatten (map (lambda (c) (programs-in (bash-command (field-ref c 'input '()))))
                                 bash)))
         (bytes (fold-left (lambda (n e) (+ n (field-ref e 'result-bytes 0))) 0 events)))
    (list (list 'events (length events))
          (list 'tool-calls (length calls))
          (list 'shell-calls (length bash))
          (list 'result-bytes bytes)
          (list 'tools (rows->records (top (tally (map (lambda (c) (field-ref c 'tool "")) calls)) 15)
                                      'tool 'calls))
          (list 'programs (rows->records (top (tally programs) 25) 'program 'calls))
          (list 'command-shapes (rows->records (top (tally commands) 25) 'shape 'calls))
          (list 'fusion-candidates (rows->records (top (tally (adjacent-pairs calls)) 10)
                                                  'sequence 'occurrences))
          (list 'repeats (repeat-report calls))
          (list 'cache (cache-report calls)))))

;; Turns the report into a ranked list of concrete opportunities, which is what
;; the synthesis step consumes. Each names the pattern and why it is worth
;; replacing, so the model is briefed rather than left to infer intent.
;; Ranking by raw frequency picks `echo`, which is the busiest command in the
;; corpus and the least worth replacing: its output is progress text nobody parses.
;; An opportunity is only real if a structured, bounded, stable tool would return
;; something the agent actually consumes.
(define no-consumable-output
  '("echo" "export" "sleep" "true" "false" "set" "unset" "alias" "source" "."
    "printf" "exit" "return" "shift" "trap" "wait"))

(define (shape-program shape)
  (let ((words (string-split shape " ")))
    (if (null? words) "" (car words))))

(define (worth-replacing? shape)
  (not (member (shape-program shape) no-consumable-output)))

;; `bash` is the generic escape hatch, not a tool: a bash -> bash pair says only
;; that the agent ran two shell commands, which the command shapes already cover.
(define (fusion-worth-replacing? sequence)
  (not (string-contains? sequence "bash")))

(define (opportunities report)
  (let* ((shapes (filter (lambda (row) (worth-replacing? (field-ref row 'shape "")))
                         (field-ref report 'command-shapes '())))
         (fusions (filter (lambda (row) (fusion-worth-replacing? (field-ref row 'sequence "")))
                          (field-ref report 'fusion-candidates '())))
         (scored
           (append
             (map (lambda (row)
                    (list (list 'kind 'command)
                          (list 'pattern (field-ref row 'shape))
                          (list 'occurrences (field-ref row 'calls))
                          ;; One call replaced saves one call.
                          (list 'score (field-ref row 'calls))
                          (list 'rationale
                                "A shell command run often enough that a typed, bounded, stable-output replacement would pay for itself.")))
                  (take shapes (min 8 (length shapes))))
             (map (lambda (row)
                    (list (list 'kind 'fusion)
                          (list 'pattern (field-ref row 'sequence))
                          (list 'occurrences (field-ref row 'occurrences))
                          ;; Fusing a pair removes a whole round trip, so each
                          ;; occurrence is worth two calls, not one.
                          (list 'score (* 2 (field-ref row 'occurrences)))
                          (list 'rationale
                                "Two calls that repeatedly follow one another; one fused tool would halve the round trips.")))
                  (take fusions (min 5 (length fusions)))))))
    (list-sort scored (lambda (a b) (> (field-ref a 'score 0) (field-ref b 'score 0))))))

;; The entry point the CLI calls: read every transcript under the given
;; directories, analyze them as one corpus, and rank what is worth replacing.
;; A source that fails to parse contributes nothing rather than aborting the run --
;; one malformed transcript should not cost the other hundred and fifty.
(define (analyze-sessions directories)
  (let* ((paths (agent-log-files directories))
         (events (flatten
                   (map (lambda (path)
                          (let ((read (catch-errors
                                        (lambda () (agent-log-events path)))))
                            (if (error? read) '() (field-ref read 'events))))
                        paths)))
         (report (analyze-events events)))
    (append (list (list 'sources (length paths))) report
            (list (list 'opportunities (opportunities report))))))
