;;; redirect.scm -- rewrite a shell command into the tool proven to replace it.
;;;
;;; A PreToolUse hook may return `updatedInput`, which replaces the tool input
;;; before it runs, so the call the agent asked for simply becomes a different one.
;;; Claude Code and Codex both accept it; nothing has to be explained to the model.
;;;
;;; It is also the most dangerous thing in this project, because a rewrite the
;;; agent cannot see is a silent change to what it is told about the world. So one
;;; rule governs it:
;;;
;;;   A command shape is rewritten only if a published tool claims that shape, and
;;;   carries evidence of reproducing it exactly -- same bytes, same exit status --
;;;   and of being faster.
;;;
;;; The `grep_numbered` run is why the first two clauses exist: 4.5x smaller, 7x
;;; faster, perfectly stable, and it returned nothing at all, because it did not
;;; implement GNU basic-regex alternation.
;;;
;;; The third clause exists because of what measuring this cost. A process launch
;;; is 1.89 ms against 4.6 us for an in-process glob over 200 files, so replacing
;;; `grep -rn X | head -20` looked like an easy win. End to end it is a loss: 27 ms
;;; against grep's 11 ms, because a fresh interpreter start costs more than the
;;; extra process the fusion saves, and GNU grep out-scans the interpreter about
;;; ten to one. Reproducing a command byte for byte leaves speed as the only axis
;;; left to win on -- identical output cannot be cheaper output -- and against a
;;; mature C tool on its own ground there is usually nothing there.
;;;
;;; So this rewrites nothing today, and that is the mechanism working. Where the
;;; win lives is where the shape of the interaction changes: fusing calls that
;;; currently cost separate round trips. A fused tool is not byte-identical to any
;;; single command, so it cannot be a transparent rewrite of one.

;; Off unless asked for. Installing the observer must never start changing
;; behaviour on its own.
(define (redirect-enabled?) (setting-on? "TOOLSCHEME_REDIRECT"))

;; A tool's claim is only as good as its evidence. `proven` is written by the
;; replay gate and lists the shapes it actually agreed on; a shape named in
;; `shapes` but absent from `proven` is an unbacked claim and is ignored.
(define (proven-shapes tool)
  (map (lambda (row) (field-ref row 'shape ""))
       (filter (lambda (row) (and (field-ref row 'agreed 0)
                                  (> (field-ref row 'agreed 0) 0)
                                  (= (field-ref row 'agreed 0)
                                     (field-ref row 'cases 0))))
               (field-ref tool 'proven '()))))

(define (covers-all? proven shapes)
  (null? (filter (lambda (s) (not (member s proven))) shapes)))

;; The whole command must be covered, not just the part that matched. A pipeline
;; is one unit: replacing `grep -n X f` inside `grep -n X f | wc -l` with a tool
;; that emits a different shape would change what `wc` counts.
(define (manifest-row name)
  (let loop ((rest (field-ref (tool-manifest) 'tools)))
    (cond ((null? rest) #f)
          ((equal? (field-ref (car rest) 'name "") name) (car rest))
          (else (loop (cdr rest))))))

(define (tool-for-command text)
  (let ((shapes (command-shapes text)))
    (if (null? shapes)
        #f
        (let loop ((rest (field-ref (tool-manifest) 'tools)))
          (cond ((null? rest) #f)
                ((and (covers-all? (proven-shapes (car rest)) shapes)
                      (not (string-null? (field-ref (car rest) 'translate "")))
                      (not (string-null? (field-ref (car rest) 'legacy-form ""))))
                 (field-ref (car rest) 'name ""))
                (else (loop (cdr rest))))))))

;; Running a proven tool in place of the command it replaces. The rendering is the
;; point: the agent asked a shell question and must get the shell's answer back,
;; byte for byte, or the substitution is visible and therefore wrong.
(define (tool-render name command)
  (let ((row (manifest-row name)))
    (if (not row)
        ""
        (let* ((translate (eval (string->symbol (field-ref row 'translate ""))))
               (render (eval (string->symbol (field-ref row 'legacy-form ""))))
               (arguments (translate command))
               (result (tool-invoke name arguments)))
          ;; An error is returned, not flattened to "". Swallowing it made a
          ;; failed tool indistinguishable from a command that printed nothing
          ;; and succeeded -- so a rewritten `cat missing.txt` told the agent the
          ;; file was empty rather than absent, with exit 0. That is the precise
          ;; failure a silent substitution must never have.
          (if (error? result) result (render result))))))

;; Single-quote for /bin/sh: everything is literal inside single quotes, and the
;; only character needing care is the quote itself.
(define (shell-quote text)
  (string-append "'" (string-replace text "'" "'\\''") "'"))

(define (redirect-command tool text)
  (string-join
    (list (shell-quote (or (env-value "TOOLSCHEME_BINARY") "toolscheme"))
          (shell-quote (string-append (or (env-value "TOOLSCHEME_HOOKS") "hooks")
                                      "/run-tool.scm"))
          (shell-quote tool)
          (shell-quote text)
          "--lib" (shell-quote (or (env-value "TOOLSCHEME_LIB") "lib"))
          ;; --text prints a string verbatim; --quiet keeps the empty-result record
          ;; from being printed while still setting the exit status.
          "--text" "--quiet")
    " "))

;; #f means "leave it alone", which is the answer for everything except a command
;; entirely covered by proven evidence.
;; Split from the env check so the policy can be exercised directly: whether
;; redirection is switched on is a deployment question, whether a given command
;; may be rewritten is the part worth testing.
;; The substitute runs rooted at the directory the agent is working in, so a
;; command naming an absolute path outside it reads fine as a shell command and
;; fails as a tool. Measured: `cat /tmp/outside.txt` gives cat 21 bytes and exit
;; 0, and the tool exit 1 -- visible rather than silent, and still not what the
;; agent asked for. A command the substitute cannot reach is left alone.
(define (rewrite-reachable? text cwd)
  (or (string-null? cwd)
      (not (any? (lambda (word)
                   (and (string-prefix? "/" word)
                        (not (string-prefix? cwd word))))
                 (string-split text " ")))))

(define (redirect-rewrite request)
  (let* ((input (field-ref request "tool_input" '()))
         (text (hook-command-of input))
         (cwd (field-ref request "cwd" "")))
    (if (string-null? text)
        #f
        (let ((tool (tool-for-command text)))
          ;; Proven *and* worth it. The evidence clauses above say a rewrite
          ;; would be faithful; this one says it would be worth taking, because a
          ;; silent substitution on a call that prints nothing buys nothing and
          ;; risks the same as one that prints a screenful.
          (if (or (not tool)
                  (not (worth-redirecting? text))
                  (not (rewrite-reachable? text cwd)))
              #f
              (list (list "hookSpecificOutput"
                          (list (list "hookEventName" "PreToolUse")
                                ;; Codex rejects updatedInput without an explicit
                                ;; allow, and Claude Code accepts the same shape.
                                (list "permissionDecision" "allow")
                                (list "updatedInput"
                                      (hook-input-with-command input
                                                               (redirect-command tool text))))))))))
)

(define (redirect-decision request)
  (if (redirect-enabled?) (redirect-rewrite request) #f))


;; The hook's whole job in one call: record what happened, offer advice when there
;; is any worth giving, and -- only for PreToolUse, only when asked, and only for a
;; command entirely covered by proven evidence -- hand back a rewrite.
;;
;; The two are combined carefully because the agents disagree about what is legal.
;; Codex rejects `permissionDecision: allow` on its own and rejects `updatedInput`
;; without it, so a rewrite carries the decision and advice alone carries none.
;; Claude Code accepts both shapes.
;; Advice on its own carries no permissionDecision: Codex rejects `allow` unless a
;; rewrite accompanies it, and adding one here would turn a note into a permission
;; grant the hook never meant to make.
(define (advice-only-decision text)
  (list (list "hookSpecificOutput"
              (list (list "hookEventName" "PreToolUse")
                    (list "additionalContext" text)))))

(define (decision-with-advice rewrite text)
  (list (list "hookSpecificOutput"
              (append (field-ref rewrite "hookSpecificOutput")
                      (list (list "additionalContext" text))))))

(define (session-decision request)
  (let ((note (catch-errors (lambda () (session-start-advice request)))))
    (if (or (error? note) (not (string? note)))
        #f
        (list (list "hookSpecificOutput"
                    (list (list "hookEventName" "SessionStart")
                          (list "additionalContext" note)))))))

;; Bounding a read the agent asked to make whole.
;;
;; This is substitution rather than advice, which is the only mechanism measured to
;; remove a step: told to "prefer" a tool, an agent adds it and carries on. The cost
;; is that a bare read carries no statement of what is wanted -- by the time the
;; agent issues Read(path), the pattern it searched for is gone -- so nothing
;; downstream can bound it except by guessing that the answer comes early. That
;; guess is the whole experiment; it is not a safe default and is off unless asked
;; for.
(define (read-bound) (let ((n (setting "TOOLSCHEME_READ_LIMIT")))
                       (if (string? n) (string->number n) #f)))

(define (bound-read-decision request)
  (let* ((input (field-ref request "tool_input" '()))
         (path (field-ref input "file_path" #f))
         (limit (read-bound)))
    (if (or (not limit)
            (not (string=? (field-ref request "tool_name" "") "Read"))
            (not (string? path))
            ;; Only an unbounded read: one the agent already bounded is its own.
            (not (absent? (field-ref input "limit" #f)))
            (not (absent? (field-ref input "offset" #f))))
        #f
        (list (list "hookSpecificOutput"
                    (list (list "hookEventName" "PreToolUse")
                          (list "permissionDecision" "allow")
                          (list "updatedInput"
                                (append input (list (list "limit" limit))))))))))

;;; ---------------------------------------------------------------------------
;;; What a command is about to print.
;;;
;;; A rewrite is a silent change to what the agent is told about the world, so it
;;; is not worth the risk on a call that was going to print two hundred bytes.
;;; This answers how much a command is likely to return, and it is the cheapest
;;; useful predictor measured by a distance.
;;;
;;; Over 3,560 distinct commands from this machine's own transcripts, labelled
;;; with the output size actually recorded rather than with a model's opinion, a
;;; lookup keyed on the program name scores F1 0.57 against 2KB. The fastest
;;; local classifier measured scored 0.37 on the same corpus and guessing scores
;;; 0.35; handing the classifier only the programs the table had never seen
;;; changed nothing to two decimal places. See experiments/output-size.
;;;
;;; The aggregate is kept in its own small file rather than derived from the
;;; observation log. That log grows by around 20MB a day and this is consulted
;;; before every call: a predictor that has to read the corpus to answer is not a
;;; cheap predictor. One row per program keeps both the read and the write
;;; proportional to the number of distinct programs, which is dozens.

(define memo-path "memo/output-bytes")

;; Two hooks finishing at once can lose one update here. That is acceptable for a
;; running average over thousands of calls and would not be for anything that had
;; to be exact.
(define (memo-rows)
  (let ((read (catch-errors (lambda () (read-file memo-path '((limit 65536)))))))
    (if (error? read)
        '()
        (fold-left
          (lambda (rows line)
            (let ((parts (string-split line "\t")))
              (if (< (length parts) 3)
                  rows
                  (let ((count (string->number (list-ref parts 2)))
                        (total (string->number (list-ref parts 3))))
                    (if (and (number? count) (number? total))
                        (cons (list (list-ref parts 1) count total) rows)
                        rows)))))
          '()
          (field-ref (text-lines (field-ref read 'text "")) 'lines)))))

(define (memo-write rows)
  (let* ((text (string-join
                 (map (lambda (row)
                        (string-append (car row) "\t"
                                       (number->string (car (cdr row))) "\t"
                                       (number->string (car (cdr (cdr row))))))
                      rows)
                 "\n"))
         (write-once (lambda () (write-file memo-path (string-append text "\n")))))
    (if (error? (catch-errors write-once))
        (begin (catch-errors (lambda () (mkdir "memo" '((parents #t)))))
               (catch-errors write-once))
        #t)))

(define (command-program text)
  (let ((parsed (catch-errors (lambda () (shell-parse text)))))
    (if (error? parsed)
        ""
        (let ((programs (field-ref parsed 'programs '())))
          (if (pair? programs) (car programs) "")))))

(define (remember-output! text bytes)
  (let ((program (command-program text)))
    (if (or (string-null? program) (not (number? bytes)))
        #f
        (let* ((rows (memo-rows))
               (seen (any? (lambda (row) (equal? (car row) program)) rows))
               (updated
                 (if seen
                     (map (lambda (row)
                            (if (equal? (car row) program)
                                (list program (+ (car (cdr row)) 1)
                                      (+ (car (cdr (cdr row))) bytes))
                                row))
                          rows)
                     (cons (list program 1 bytes) rows))))
          (memo-write updated)))))

;; #f means "never seen this program", which is different from "seen, and small".
;; Treating the two alike would let an unknown command inherit whatever the
;; default happened to be.
(define (predicted-output-bytes text)
  (let* ((program (command-program text))
         (row (if (string-null? program)
                  #f
                  (fold-left (lambda (found r) (if (equal? (car r) program) r found))
                             #f (memo-rows)))))
    (if (not row) #f (quotient (car (cdr (cdr row))) (max 1 (car (cdr row)))))))

(define redirect-default-min-bytes 2000)

(define (redirect-min-bytes)
  (let ((configured (setting "TOOLSCHEME_REDIRECT_MIN_BYTES")))
    (if (string? configured)
        (let ((n (string->number configured)))
          (if (number? n) n redirect-default-min-bytes))
        redirect-default-min-bytes)))

;; An unseen program is not rewritten. The evidence a rewrite rests on is about
;; the shape being reproducible; this is about it being worth doing at all, and
;; there is no evidence either way the first time something runs.
(define (worth-redirecting? text)
  (let ((predicted (predicted-output-bytes text)))
    (and (number? predicted) (>= predicted (redirect-min-bytes)))))

(define (hook-decision request)
  (let ((event (field-ref request "hook_event_name" "")))
    (cond ((equal? event "SessionStart") (session-decision request))
          ((equal? event "Stop")
           (let ((decision (catch-errors (lambda () (continue-decision request)))))
             (if (or (error? decision) (not decision)) #f decision)))
          (else (tool-decision request)))))

(define (tool-decision request)
  (let* ((refused (catch-errors (lambda () (dogfood-decision request))))
         (bounded (catch-errors (lambda () (bound-read-decision request))))
         (rewrite (catch-errors
                    (lambda ()
                      (if (equal? (field-ref request "hook_event_name" "") "PreToolUse")
                          (redirect-decision request)
                          #f))))
         (advice (catch-errors (lambda () (steer-advice request))))
         (rewriting (and (not (error? rewrite)) rewrite))
         (advising (and (not (error? advice)) (string? advice) advice)))
    (cond
      ;; A refusal ends it: there is nothing to advise about a call that will
      ;; not run, and nothing to rewrite.
      ((and (not (error? refused)) refused) refused)
      ;; A bounded read is a complete decision on its own.
      ((and (not (error? bounded)) bounded) bounded)
      ((and (not rewriting) (not advising)) #f)
      ((not rewriting) (advice-only-decision advising))
      ((not advising) rewriting)
      (else (decision-with-advice rewriting advising)))))

(define (hook-run)
  (let ((request (catch-errors (lambda () (hook-request)))))
    (if (error? request)
        ""
        (begin
          (catch-errors
            (lambda () (if (null? request) #f (hook-append (hook-observation request)))))
          ;; A finished call is the only place the output size is known, so the
          ;; table is fed here and read on the way in to the next one.
          (catch-errors
            (lambda ()
              (if (equal? (field-ref request "hook_event_name" "") "PostToolUse")
                  (let ((input (field-ref request "tool_input" '()))
                        (response (field-ref request "tool_response" #f)))
                    (remember-output! (hook-command-of input)
                                      (if (absent? response)
                                          0
                                          (string-length (write-to-string response)))))
                  #f)))
          (let ((decision (catch-errors (lambda () (hook-decision request)))))
            (if (or (error? decision) (not decision))
                ""
                (field-ref (json-write decision) 'text)))))))
