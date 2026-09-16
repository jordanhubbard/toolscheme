;;; redirect-check.scm -- the rewrite policy, exercised with a fixture.
;;;
;;; No shipped tool currently claims a proven shape, because none has earned one:
;;; reproducing a command's bytes exactly leaves time as the only axis to win on,
;;; and a fresh interpreter start costs more than the extra process a fusion saves.
;;; The policy still has to be right for when one does, so it is tested here
;;; against a fixture that declares a claim.

(define (request-for command)
  (list (list "hook_event_name" "PreToolUse")
        (list "tool_name" "Bash")
        (list "cwd" "/w")
        (list "tool_input" (list (list "command" command)))))

;; Codex in code mode wraps the shell in JavaScript. A rewrite has to land inside
;; that wrapper, not replace it.
(define codex-request
  (list (list "hook_event_name" "PreToolUse")
        (list "tool_name" "exec")
        (list "cwd" "/w")
        (list "tool_input"
              (string-append "const r = await tools.exec_command({\"cmd\":"
                             "\"grep -n define-tool lib/prelude.scm | head -20\","
                             "\"workdir\":\"/w\"});\nreturn r;"))))

(define (rewritten request)
  (let ((decision (redirect-rewrite request)))
    (if (not decision)
        #f
        (field-ref (field-ref decision "hookSpecificOutput") "updatedInput"))))

;; Before the claim exists, nothing is rewritten.
(define before (rewritten (request-for "grep -n define-tool lib/prelude.scm | head -20")))

(define-tool
  (list (list 'name "redirect-fixture")
        (list 'description "fixture that claims a proven shape")
        (list 'shapes '("grep -n" "head -20"))
        (list 'proven '(((shape "grep -n") (cases 2) (agreed 2) (worthwhile 2))
                        ((shape "head -20") (cases 2) (agreed 2) (worthwhile 2))))
        (list 'translate "search-read-translate")
        (list 'legacy-form "search-read->grep")
        (list 'empty-status "search-read-empty-status")
        (list 'procedure search-read)))

(define after (rewritten (request-for "grep -n define-tool lib/prelude.scm | head -20")))
(define codex-after (rewritten codex-request))

;; A claim covering only part of a pipeline must not rewrite the pipeline: a tool
;; emitting a different shape inside `... | wc -l` changes what wc counts.
(define partial (rewritten (request-for "grep -n x f.c | wc -l")))
(define uncovered (rewritten (request-for "wc -l lib/prelude.scm")))
(define destructive (rewritten (request-for "rm -rf /tmp/scratch")))

;; An unbacked claim must never be honoured, however loudly it is declared.
(define-tool
  (list (list 'name "redirect-liar")
        (list 'description "declares a shape with no agreement behind it")
        (list 'shapes '("wc -l"))
        (list 'proven '(((shape "wc -l") (cases 2) (agreed 0) (worthwhile 0))))
        (list 'translate "search-read-translate")
        (list 'legacy-form "search-read->grep")
        (list 'procedure search-read)))

(define liar (rewritten (request-for "wc -l lib/prelude.scm")))

(define checks
  (list (list 'unclaimed-left-alone (eq? before #f))
        (list 'claimed-rewritten (not (eq? after #f)))
        (list 'codex-wrapper-preserved
              (and codex-after (string-contains? codex-after "tools.exec_command(")))
        (list 'codex-command-replaced
              (and codex-after (string-contains? codex-after "run-tool.scm")))
        (list 'partial-pipeline-left-alone (eq? partial #f))
        (list 'uncovered-left-alone (eq? uncovered #f))
        (list 'destructive-left-alone (eq? destructive #f))
        (list 'unbacked-claim-refused (eq? liar #f))
        (list 'disabled-by-default (not (redirect-enabled?)))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (eq? before #f)
                 (not (eq? after #f))
                 (string-contains? (field-ref after "command" "") "run-tool.scm")
                 codex-after
                 (string-contains? codex-after "tools.exec_command(")
                 (string-contains? codex-after "run-tool.scm")
                 (eq? partial #f)
                 (eq? uncovered #f)
                 (eq? destructive #f)
                 (eq? liar #f)
                 (not (redirect-enabled?)))))
