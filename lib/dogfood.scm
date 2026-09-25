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

;; Only the first program of the line is judged. A pipeline whose head is denied
;; is denied; one that merely contains `grep` deep inside a build command is not
;; the case this is aimed at, and refusing it would make the rule hated.
(define (dogfood-offender text)
  (let ((parsed (catch-errors (lambda () (shell-parse text)))))
    (if (error? parsed)
        #f
        (let ((programs (field-ref parsed 'programs '())))
          (if (null? programs)
              #f
              (let ((first (car programs)))
                (if (dogfood-equivalent first) first #f)))))))

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
        (let ((offender (dogfood-offender text)))
          (if (not offender)
              #f
              (list (list "hookSpecificOutput"
                          (list (list "hookEventName" "PreToolUse")
                                (list "permissionDecision" "deny")
                                (list "permissionDecisionReason"
                                      (string-append
                                        "This tree is toolscheme's own, and `" offender
                                        "` is one of the tools it exists to replace. Use:\n  "
                                        (dogfood-equivalent offender)
                                        "\n\nIf toolscheme cannot express what you need, that is "
                                        "the finding: add the capability and commit it, rather "
                                        "than working around it here. Unset TOOLSCHEME_DOGFOOD "
                                        "to lift this."))))))))))

(define (dogfood-decision request) (dogfood-refusal request (dogfood-roots)))
