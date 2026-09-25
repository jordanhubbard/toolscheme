;;; dogfood-check.scm -- refusing the tools this project replaces, inside its own tree.
;;;
;;; The mechanism is a denial rather than advice because advice was measured and
;;; does not work: it is added to the context and ignored. A refusal cannot be
;;; ignored -- either the equivalent gets used, or the missing capability gets
;;; written. Both are the point.

(define (request command cwd)
  (list (list "hook_event_name" "PreToolUse")
        (list "tool_name" "Bash")
        (list "cwd" cwd)
        (list "tool_input" (list (list "command" command)))))

(define roots (list "/home/jkh/Src/toolscheme"))

(define (denied? command cwd)
  (let ((d (dogfood-refusal (request command cwd) roots)))
    (and d (equal? (field-ref (field-ref d "hookSpecificOutput") "permissionDecision") "deny"))))

(define (reason command cwd)
  (let ((d (dogfood-refusal (request command cwd) roots)))
    (if d (field-ref (field-ref d "hookSpecificOutput") "permissionDecisionReason" "") "")))

(define inside "/home/jkh/Src/toolscheme")
(define outside "/home/jkh/Src/other")

(define checks
  (list
    ;; Off unless a tree is named: this is a rule about working on toolscheme,
    ;; not a claim that the shell should be unavailable.
    (list 'off-by-default (null? (dogfood-roots)))
    (list 'scoped-to-named-trees (not (denied? "python3 -c 1" outside)))
    (list 'refuses-python-in-tree (denied? "python3 - <<EOF" inside))
    (list 'refuses-sed (denied? "sed -i s/a/b/ f.scm" inside))
    (list 'refuses-grep (denied? "grep -n x lib/prelude.scm" inside))
    ;; The tools it does not replace are left alone, or the rule becomes hated
    ;; and gets switched off, which helps nobody.
    (list 'allows-make (not (denied? "make test" inside)))
    (list 'allows-git (not (denied? "git commit -m x" inside)))
    (list 'allows-toolscheme (not (denied? "./toolscheme -e 1" inside)))
    ;; A refusal that leaves the caller stuck teaches nothing.
    (list 'names-the-equivalent
          (string-contains? (reason "grep -n x f" inside) "(grep \"pattern\""))
    (list 'says-what-to-do-when-there-is-none
          (string-contains? (reason "python3 -c 1" inside) "add the capability"))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (null? (dogfood-roots))
                 (not (denied? "python3 -c 1" outside))
                 (denied? "python3 - <<EOF" inside)
                 (denied? "sed -i s/a/b/ f.scm" inside)
                 (denied? "grep -n x lib/prelude.scm" inside)
                 (not (denied? "make test" inside))
                 (not (denied? "git commit -m x" inside))
                 (not (denied? "./toolscheme -e 1" inside))
                 (string-contains? (reason "grep -n x f" inside) "(grep \"pattern\"")
                 (string-contains? (reason "python3 -c 1" inside) "add the capability"))))
