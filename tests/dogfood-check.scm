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
    ;; The shape every real call actually has. Judging only the very first
    ;; program let all of these through: the commands being aimed at all begin
    ;; `cd somewhere && ...`, so the program judged was `cd` and the python3
    ;; behind it was never seen. The rule reported itself working and would have
    ;; refused nothing anyone types.
    (list 'sees-past-cd (denied? "cd /home/jkh/Src/toolscheme; python3 - <<EOF" inside))
    (list 'sees-past-cd-and (denied? "cd /x && sed -i s/a/b/ f" inside))
    (list 'sees-past-echo (denied? "echo hi; grep -n x f" inside))
    ;; But judgement stops at the first program that does something, so a build
    ;; that happens to contain grep further along is untouched.
    (list 'stops-at-the-first-real-program (not (denied? "make test; grep -n x log" inside)))
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
                 ;; The ones that made the difference between a rule and a
                 ;; gesture.
                 (denied? "cd /home/jkh/Src/toolscheme; python3 - <<EOF" inside)
                 (denied? "cd /x && sed -i s/a/b/ f" inside)
                 (denied? "echo hi; grep -n x f" inside)
                 (not (denied? "make test; grep -n x log" inside))
                 (not (denied? "make test" inside))
                 (not (denied? "git commit -m x" inside))
                 (not (denied? "./toolscheme -e 1" inside))
                 (string-contains? (reason "grep -n x f" inside) "(grep \"pattern\"")
                 (string-contains? (reason "python3 -c 1" inside) "add the capability")
                 ;; `cat > f <<EOF` is how a file gets created, not how one gets
                 ;; read, and the rule used to answer it with `(read-file
                 ;; "path")` -- advice so plainly wrong for a write that it
                 ;; diagnosed the misclassification rather than the offence.
                 ;; Still refused, since a shell is still not the way to write a
                 ;; file here; the equivalent named is now a write.
                 (denied? "cat > f.sh <<EOF" inside)
                 (string-contains? (reason "cat > f.sh <<EOF" inside) "(write-file")
                 (string-contains? (reason "cat >> f.sh <<EOF" inside) "(write-file")
                 ;; A genuine read whose output happens to be redirected is
                 ;; still a read, and must keep pointing at read-file.
                 (string-contains? (reason "cat f.sh > out.txt" inside) "(read-file")
                 (string-contains? (reason "cat f.sh" inside) "(read-file"))))
