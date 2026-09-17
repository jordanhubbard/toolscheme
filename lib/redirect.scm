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
(define (redirect-enabled?)
  (let ((flag (env-value "TOOLSCHEME_REDIRECT")))
    (and (string? flag) (not (string=? flag "0")) (not (string=? flag "")))))

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
          (if (error? result) "" (render result))))))

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
(define (redirect-rewrite request)
  (let* ((input (field-ref request "tool_input" '()))
         (text (hook-command-of input)))
    (if (string-null? text)
        #f
        (let ((tool (tool-for-command text)))
          (if (not tool)
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

(define (hook-decision request)
  (let* ((rewrite (catch-errors
                    (lambda ()
                      (if (equal? (field-ref request "hook_event_name" "") "PreToolUse")
                          (redirect-decision request)
                          #f))))
         (advice (catch-errors (lambda () (steer-advice request))))
         (rewriting (and (not (error? rewrite)) rewrite))
         (advising (and (not (error? advice)) (string? advice) advice)))
    (cond
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
          (let ((decision (catch-errors (lambda () (hook-decision request)))))
            (if (or (error? decision) (not decision))
                ""
                (field-ref (json-write decision) 'text)))))))
