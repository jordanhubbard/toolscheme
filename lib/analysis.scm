;;; analysis.scm -- rank what an agent's tool use actually costs.
;;;
;;; The report answers four questions, in the order they matter:
;;;   which tools dominate; which shell commands hide inside Bash calls; which
;;;   calls repeat verbatim; and which calls correlate with prompt-cache churn.
;;;
;;; Commands are normalized by *shape*, not by literal text, because
;;; `grep -n needle src/a.c` and `grep -n other src/b.c` are the same opportunity.

;; Every agent runs shells through a differently named tool -- Bash, exec,
;; local_shell -- and Codex wraps the command in JavaScript besides. What makes a
;; call a shell call is that a command can be read out of it, not what it is named.
;;
;; The exception is a payload that merely *has* a field called `command`. Codex's
;; apply_patch keeps the patch there, and shell-parsing a diff invents commands
;; called `+` and `***`; in the live corpus those two together claimed nearly ten
;; thousand calls that never happened and headed the ranking. New records are
;; filtered where the tool name is known, but records already written are not, so
;; the analysis refuses them here as well.
(define (patch-payload? text)
  (or (string-prefix? "*** Begin Patch" text)
      (string-prefix? "*** Update File" text)
      (string-prefix? "--- " text)
      (string-prefix? "+++ " text)))

(define (bash-command input)
  (let ((command (hook-command-of input)))
    (if (patch-payload? (string-trim command)) "" command)))

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
;; Accumulating rather than building on the way out, because the recursive call in
;; `(cons x (adjacent-pairs ...))` is not in tail position: it holds a frame per
;; element, and a corpus of thirteen thousand calls exhausted the stack and killed
;; the process. The interpreter guarantees tail calls, which is exactly why the one
;; call that is not a tail call is easy to write by accident.
(define (adjacent-pairs calls)
  (let loop ((rest calls) (out '()))
    (if (or (null? rest) (null? (cdr rest)))
        (reverse out)
        (loop (cdr rest)
              (cons (string-append (field-ref (car rest) 'tool "") " -> "
                                   (field-ref (cadr rest) 'tool ""))
                    out)))))

;; Only live observation can report how long a tool took: a transcript records no
;; per-call timestamp and no way to pair a call with its result. Where the data is
;; there, this is the most direct statement of what a tool costs -- count times
;; duration, rather than count alone.
(define (call-endpoints events kind field)
  (list-sort
    (map (lambda (e) (list (field-ref e 'call "") (field-ref e 'at 0) (field-ref e field 0)))
         (filter (lambda (e) (and (eq? (field-ref e 'kind) kind)
                                  (not (string-null? (field-ref e 'call "")))))
                 events))
    (lambda (a b) (string<? (car a) (car b)))))

(define (paired-durations events)
  (let loop ((starts (call-endpoints events 'tool-call 'tool))
             (ends (call-endpoints events 'tool-result 'result-bytes))
             (out '()))
    (cond ((or (null? starts) (null? ends)) (reverse out))
          ((string<? (car (car starts)) (car (car ends))) (loop (cdr starts) ends out))
          ((string<? (car (car ends)) (car (car starts))) (loop starts (cdr ends) out))
          (else (loop (cdr starts) (cdr ends)
                      (cons (list (caddr (car starts))
                                  (max 0 (- (cadr (car ends)) (cadr (car starts)))))
                            out))))))

(define (latency-report events)
  (let ((paired (paired-durations events)))
    (if (null? paired)
        (list (list 'available #f)
              (list 'note "no paired call timings; this source records none"))
        (list (list 'available #t)
              (list 'measured-calls (length paired))
              (list 'total-ms (fold-left (lambda (n row) (+ n (cadr row))) 0 paired))
              (list 'by-tool (rows->records (top (tally-by paired cadr) 15) 'tool 'total-ms))))))

(define (analyze-events events)
  (let* ((calls (tool-calls events))
         (bash (filter (lambda (c) (not (string-null?
                                          (bash-command (field-ref c 'input '())))))
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
          (list 'command-samples (shape-samples bash))
          (list 'fusion-candidates (rows->records (top (tally (adjacent-pairs calls)) 10)
                                                  'sequence 'occurrences))
          ;; A merged corpus spans agents, and they do not behave alike; reporting
          ;; only the total hides exactly the comparison worth having.
          (list 'agents (rows->records
                          (top (tally (map (lambda (c) (field-ref c 'agent ""))
                                           (filter (lambda (c) (not (string-null?
                                                                      (field-ref c 'agent ""))))
                                                   calls)))
                               8)
                          'agent 'calls))
          (list 'latency (latency-report events))
          (list 'repeats (repeat-report calls))
          (list 'cache (cache-report calls)))))

;; Turns the report into a ranked list of concrete opportunities, which is what
;; the synthesis step consumes. Each names the pattern and why it is worth
;; replacing, so the model is briefed rather than left to infer intent.
;; Replaying a recorded command means *running* it. A corpus is full of commands
;; that must never be re-run on someone's machine -- `rm`, `docker`, `git push`,
;; a deploy script -- and full of commands that simply cannot run here, because
;; they referenced another project's files. So a shape is only ever offered to the
;; gate if its program is one that reads and reports and does nothing else. This
;; list is deliberately short; adding to it is a decision about what the gate is
;; allowed to execute, not a convenience.
(define replay-safe-programs
  '("grep" "egrep" "fgrep" "rg" "head" "tail" "cat" "wc" "ls" "find" "sort" "uniq"
    "cut" "nl" "basename" "dirname" "file" "stat" "du" "df" "which" "tr" "column"))

(define (replayable-shape? shape)
  (and (member (shape-program shape) replay-safe-programs) #t))

;; The shape being safe is not enough, and assuming otherwise is how a gate ends up
;; running something it should not: a recorded line matching the shape `head -c`
;; was `cd /home/jkh && time ./toolscheme analyze ... | head -c 3000`. Every command
;; in the line has to be safe, not just the one the shape came from.
(define (replayable-command? text)
  (let ((programs (programs-in text)))
    (and (not (null? programs))
         (null? (filter (lambda (p) (not (member p replay-safe-programs))) programs)))))

;; The concrete calls behind a shape, so the gate has something real to replay.
;; Shapes are the pattern; these are the evidence.
(define (shape-samples calls)
  (flatten
    (map (lambda (call)
           (let ((text (bash-command (field-ref call 'input '())))
                 (directory (field-ref call 'directory "")))
             (map (lambda (shape) (list shape text directory)) (command-shapes text))))
         calls)))

(define (samples-for shape samples wanted)
  (let loop ((rest samples) (seen '()) (commands '()) (n 0))
    (cond ((or (null? rest) (= n wanted)) (reverse seen))
          ((and (string=? (car (car rest)) shape)
                (replayable-command? (cadr (car rest)))
                (not (member (cadr (car rest)) commands)))
           (loop (cdr rest)
                 (cons (list (list 'command (cadr (car rest)))
                             (list 'directory (caddr (car rest))))
                       seen)
                 (cons (cadr (car rest)) commands)
                 (+ n 1)))
          (else (loop (cdr rest) seen commands n)))))

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
  (let* ((samples (field-ref report 'command-samples '()))
         (shapes (filter (lambda (row) (worth-replacing? (field-ref row 'shape "")))
                         (field-ref report 'command-shapes '())))
         (fusions (filter (lambda (row) (fusion-worth-replacing? (field-ref row 'sequence "")))
                          (field-ref report 'fusion-candidates '())))
         (scored
           (append
             (map (lambda (row)
                    (let ((shape (field-ref row 'shape "")))
                      (list (list 'kind 'command)
                            (list 'pattern shape)
                            (list 'occurrences (field-ref row 'calls))
                            ;; One call replaced saves one call.
                            (list 'score (field-ref row 'calls))
                            ;; Replayable means the gate has something it is allowed
                            ;; to run: a safe shape *and* at least one recorded line
                            ;; that is safe end to end.
                            (list 'replayable
                                  (and (replayable-shape? shape)
                                       (not (null? (samples-for shape samples 1)))))
                            (list 'samples (if (replayable-shape? shape)
                                               (samples-for shape samples 5)
                                               '()))
                            (list 'rationale
                                  "A shell command run often enough that a typed, bounded, stable-output replacement would pay for itself."))))
                  (take shapes (min 8 (length shapes))))
             (map (lambda (row)
                    (list (list 'kind 'fusion)
                          (list 'pattern (field-ref row 'sequence))
                          (list 'occurrences (field-ref row 'occurrences))
                          ;; Fusing a pair removes a whole round trip, so each
                          ;; occurrence is worth two calls, not one.
                          (list 'score (* 2 (field-ref row 'occurrences)))
                          ;; A fusion's legacy side is two agent-native tool calls,
                          ;; not a shell command, so there is nothing to replay
                          ;; against yet. It is still worth reporting.
                          (list 'replayable #f)
                          (list 'samples '())
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
