;;; run-tool.scm -- stand in for a shell command with the tool proven to replace it.
;;;
;;; Invoked only by a PreToolUse rewrite, with the tool name and the original
;;; command. Emits exactly what the command emitted -- that equivalence is what the
;;; replay gate established, and it is the only reason substituting is allowed.
;;;
;;; Exit status is part of the answer. `grep -q X && ...` reads it and nothing
;;; else, so a tool declares what an empty result means for the command it
;;; replaces; returning a record rather than a string is how that reaches the
;;; process exit code without printing anything.
(let* ((name (car command-arguments))
       (text (tool-render name (cadr command-arguments)))
       (row (manifest-row name))
       ;; The status an empty result should carry is a function of the command,
       ;; not a constant: see search-read-empty-status.
       (status-form (if row (field-ref row 'empty-status "") ""))
       (empty-status (if (string-null? status-form)
                         0
                         ((eval (string->symbol status-form)) (cadr command-arguments)))))
  (if (and (string-null? text) (not (= empty-status 0)))
      (list (list 'error "no match") (list 'code 'not-found) (list 'operation 'run-tool))
      text))
