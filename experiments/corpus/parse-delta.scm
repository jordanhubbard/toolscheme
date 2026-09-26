;;; parse-delta.scm -- one line per recorded command, naming what shell-parse
;;; found in it.
;;;
;;; Run under two binaries and diff the output: that difference is exactly what a
;;; tokenizer change did to the corpus, with no estimate in the middle. Written
;;; because the first figure quoted for the quoted-word bug -- 9.2% -- was an
;;; upper bound from a string search for a quote followed by an operator, which
;;; also matches quotes inside heredoc bodies and operators inside string
;;; literals. An upper bound is not a measurement, and it was reported as one.

(define log ".local/state/toolscheme/observations.jsonl")

(define lines
  (filter (lambda (l) (not (string-null? l)))
          (field-ref (text-lines (field-ref (read-file log '((limit 268435456))) 'text ""))
                     'lines)))

(define commands
  (reverse
    (fold-left
      (lambda (acc line)
        (let ((parsed (catch-errors (lambda () (json-parse line)))))
          (if (error? parsed)
              acc
              (let* ((row (field-ref parsed 'value))
                     (command (field-ref row "command" "")))
                (if (and (equal? (field-ref row "event" "") "pre")
                         (string? command) (not (string-null? command)))
                    (cons command acc)
                    acc)))))
      '() lines)))

;; Written to a file rather than printed: there is no `display` here, and the
;; result of a script is rendered as a value, which is the wrong shape for diff.
(define out ".local/state/toolscheme/parse-dump.txt")

(write-file out
  (string-join
    (map (lambda (command)
           (let ((parsed (catch-errors (lambda () (shell-parse command)))))
             (if (error? parsed)
                 "<error>"
                 (string-join (field-ref parsed 'programs '()) ","))))
         commands)
    "\n"))

(list (list 'commands (length commands)) (list 'written out))
