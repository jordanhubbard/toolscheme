;;; dogfood.scm -- refuse, inside chosen trees, the tools this project replaces.
;;;
;;; The author of this file used Python heredocs for nearly every edit and every
;;; analysis while building it, and toolscheme almost only to test itself. That
;;; is not laziness so much as gravity: reaching for the familiar tool is one
;;; call, and adding the missing capability is a detour. So every limitation was
;;; routed around and none was recorded, which is exactly how a tool intended to
;;; replace the shell ends up never being used by the person replacing it.
;;;
;;; A denial is the forcing function. PreToolUse may refuse a call outright, and
;;; a refusal cannot be routed around silently: either the equivalent exists and
;;; gets used, or it does not exist and has to be written. Both outcomes are the
;;; point. Advice would not do it -- advice was measured, and it gets added to
;;; the context and ignored.
;;;
;;; Scoped to trees named explicitly, because this is a rule about working *on*
;;; toolscheme, not a claim that the shell should be unavailable generally.

(define (dogfood-roots)
  (let ((configured (setting "TOOLSCHEME_DOGFOOD")))
    (if (and (string? configured) (not (string-null? configured)))
        (filter (lambda (s) (not (string-null? s))) (string-split configured ":"))
        '())))

(define (within-dogfood? cwd roots)
  (and (string? cwd)
       (any? (lambda (root) (string-prefix? root cwd)) roots)))

;; What toolscheme already does, and the spelling to use instead. Naming the
;; replacement is what separates a forcing function from an obstruction: a
;; refusal that leaves the caller stuck teaches nothing and will be switched off.
(define dogfood-equivalents
  '(("python3" . "toolscheme -e '(...)' -- or a script; see docs/api.md")
    ("python" . "toolscheme -e '(...)'")
    ("sed" . "(text-replace ...) for edits, (read-file p '((first-line a) (last-line b))) for ranges")
    ("awk" . "(text-lines ...) with (map ...), or (cut ...)")
    ("grep" . "(grep \"pattern\" '(files \"a\") '((line-numbers #t) (limit N)))")
    ("rg" . "(search ...) or (grep ... '(glob \"**/*.c\"))")
    ("head" . "(head DATA '((count N))) -- every tool takes (limit N) already")
    ("tail" . "(tail DATA '((count N)))")
    ("cat" . "(read-file \"path\")")
    ("wc" . "(wc DATA)")
    ("cut" . "(cut DATA '((fields (1 3))))")
    ("sort" . "(list-sort ...) or (sort DATA)")
    ("uniq" . "(uniq DATA)")
    ("find" . "(find \"*.scm\" '((directory \"lib\")))")
    ("ls" . "(glob \"*\") or (find ...)")))

(define (dogfood-equivalent program)
  (let ((row (assoc program dogfood-equivalents)))
    (if row (cdr row) #f)))

;; Programs that carry no work of their own. A line beginning with one of these
;; has not said what it does yet, so judgement passes to what follows.
;;
;; This list is why the rule works at all. Judging only the very first program
;; let every real call through on the first try: the author's commands all begin
;; `cd /home/jkh/Src/toolscheme && ...`, so the program judged was `cd` and the
;; python3 behind it was never seen. The rule was switched on, reported itself
;; working, and would have refused nothing anyone actually types.
(define dogfood-transparent
  '("cd" "pushd" "popd" "echo" "printf" "true" "false" "set" "export" "unset"
    "source" "." "env" "time" "mkdir" "touch"))

;; `cat > file <<'EOF'` is not a read at all: it is the ordinary way to create a
;; file from literal text. The rule refused it and answered with `(read-file
;; "path")` -- advice so plainly wrong for a write that it proved the
;; misclassification rather than the offence. Judging the program name alone
;; cannot tell the two apart; the redirections can, and `shell-parse` has
;; reported them all along.
(define (dogfood-writes-a-file? command)
  (let ((operators (map (lambda (r) (field-ref r 'operator ""))
                        (field-ref command 'redirections '()))))
    (and (memq 'heredoc operators)
         (or (memq '> operators) (memq '>> operators))
         #t)))

(define dogfood-write-advice
  "(write-file \"path\" \"text\") -- or your agent's own file-writing tool, which
  needs no shell quoting for anything long")

;; The first command that claims to do something. A build command that happens
;; to contain `grep` further along is still left alone -- judgement stops at the
;; first substantive program, and `make` is substantive -- which is the line
;; between a forcing function and an obstruction.
(define (dogfood-offending-command text)
  (let ((parsed (catch-errors (lambda () (shell-parse text)))))
    (if (error? parsed)
        #f
        (let loop ((rest (field-ref parsed 'commands '())))
          (cond ((null? rest) #f)
                ((member (field-ref (car rest) 'program "") dogfood-transparent)
                 (loop (cdr rest)))
                ((dogfood-equivalent (field-ref (car rest) 'program "")) (car rest))
                (else #f))))))

(define (dogfood-offender text)
  (let ((command (dogfood-offending-command text)))
    (if command (field-ref command 'program "") #f)))

;; The roots are a parameter so the rule can be exercised without reaching into
;; the environment; `dogfood-decision` is the thin wrapper that reads the
;; setting. Needing to mutate the environment to test a decision is a smell, and
;; here it would have meant adding a primitive for the benefit of one check.
(define (dogfood-refusal request roots)
  (let* ((event (field-ref request "hook_event_name" ""))
         (cwd (field-ref request "cwd" ""))
         (input (field-ref request "tool_input" '()))
         (text (hook-command-of input)))
    (if (or (not (equal? event "PreToolUse"))
            (string-null? text)
            (not (within-dogfood? cwd roots)))
        #f
        (let* ((command (dogfood-offending-command text))
               (offender (if command (field-ref command 'program "") #f)))
          (if (not offender)
              #f
              (list (list "hookSpecificOutput"
                          (list (list "hookEventName" "PreToolUse")
                                (list "permissionDecision" "deny")
                                (list "permissionDecisionReason"
                                      (string-append
                                        "This tree is toolscheme's own, and `" offender
                                        "` is one of the tools it exists to replace. Use:\n  "
                                        (if (dogfood-writes-a-file? command)
                                            dogfood-write-advice
                                            (dogfood-equivalent offender))
                                        "\n\n(apropos \"name\") lists every top-level binding "
                                        "matching a substring, library included -- check there "
                                        "before concluding something is missing. The first "
                                        "analysis written under this rule hand-rolled a counter "
                                        "six times slower than the `tally` it could not find."
                                        "\n\nIf toolscheme genuinely cannot express what you "
                                        "need, that is the finding: add the capability and commit "
                                        "it, rather than working around it here. Unset "
                                        "TOOLSCHEME_DOGFOOD to lift this."))))))))))

(define (dogfood-decision request) (dogfood-refusal request (dogfood-roots)))


;;; ---------------------------------------------------------------------------
;;; Refusing a fixed wait.
;;;
;;; The largest measured waste in this corpus by three orders of magnitude.
;;; 1,168 fixed-timer sleeps across two sessions, 11.9 hours, a median of 40
;;; seconds each -- against 22 seconds for the entire tool-substitution loop
;;; those sessions were being built to serve.
;;;
;;; Two milder mechanisms were tried on it and both failed, which is why this is
;;; a refusal. The `wait-for` instruction was already in AGENTS.md and CLAUDE.md
;;; before the corpus window, and the sleeping continued at 370, 190, 155, 87 and
;;; 125 a day after it. The hook advice fired correctly and was ignored; advice
;;; is added to the context and read past. A denial cannot be read past.
;;;
;;; What it must not do is demand `wait-for` unconditionally. Of the waits
;;; measured, 402 were followed immediately by a local observable check -- tail,
;;; cat, ls -- which is exactly what wait-for replaces, but 407 were followed by
;;; ssh, gh or curl, waiting on something with no local condition to watch. A
;;; rule that refuses those has no correct answer to offer, and a rule with no
;;; correct answer gets switched off within a day. So short polling stays
;;; available: it is worse than an event, and much better than a blind minute.

(define refuse-sleep-default-ms 10000)

(define (refuse-sleep-enabled?) (setting-on? "TOOLSCHEME_REFUSE_SLEEP"))

(define (refuse-sleep-threshold-ms)
  (let ((configured (setting "TOOLSCHEME_REFUSE_SLEEP_MS")))
    (if (string? configured)
        (let ((n (string->number configured)))
          (if (number? n) n refuse-sleep-default-ms))
        refuse-sleep-default-ms)))

;; Takes the threshold rather than reading it, so the rule can be exercised
;; directly -- the same reason `dogfood-refusal` takes its roots.
(define (sleep-refusal request threshold)
  (let ((event (field-ref request "hook_event_name" "")))
    (if (not (equal? event "PreToolUse"))
        #f
        ;; `sleeping-for` already knows both spellings -- Codex's duration_ms
        ;; tool and a shell `sleep N` -- and already declines to count a sleep
        ;; the agent backgrounded, which is the work being simulated rather than
        ;; the agent waiting for it.
        (let ((slept (catch-errors (lambda () (sleeping-for request)))))
          (if (or (error? slept) (not (number? slept)) (< slept threshold))
              #f
              (list (list "hookSpecificOutput"
                          (list (list "hookEventName" "PreToolUse")
                                (list "permissionDecision" "deny")
                                (list "permissionDecisionReason"
                                      (string-append
                                        "This waits a fixed "
                                        (number->string (quotient slept 1000))
                                        "s whether or not the thing you are waiting for has "
                                        "happened. Fixed waits cost 11.9 hours across the "
                                        "sessions on this machine.\n\n"
                                        "If the thing you are waiting for is observable here:\n"
                                        "  toolscheme -e '(wait-for (quote (exists \"some/path\")))'\n"
                                        "  toolscheme -e '(wait-for (quote (matches \"some.log\" \"ready\")))'\n"
                                        "Both take (timeout-ms N) and return the moment the "
                                        "condition holds.\n\n"
                                        "If you are waiting on something remote with nothing "
                                        "local to watch -- a CI run, a queue -- then poll in "
                                        "short increments instead: sleep "
                                        (number->string (quotient threshold 2000))
                                        " and check, rather than sleeping "
                                        (number->string (quotient slept 1000))
                                        " and hoping. Unset TOOLSCHEME_REFUSE_SLEEP to lift "
                                        "this."))))))))))

(define (sleep-decision request)
  (if (refuse-sleep-enabled?)
      (sleep-refusal request (refuse-sleep-threshold-ms))
      #f))
