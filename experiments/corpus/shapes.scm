;;; corpus2.scm -- the shape distribution, in toolscheme.

(define log ".local/state/toolscheme/observations.jsonl")

(define lines
  (filter (lambda (l) (not (string-null? l)))
          (field-ref (text-lines (field-ref (read-file log '((limit 268435456))) 'text ""))
                     'lines)))

(define commands
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
    '() lines))

;; Counting by key uses the library's `tally`, which sorts and groups runs.
;; This script originally hand-rolled an accumulator that rebuilt an association
;; list per key, because `tally` could not be found -- it is Scheme, and
;; `primitive-names` lists only what C++ installed. The hand-rolled version was
;; six times slower for the same answer. `(apropos "tally")` now says so.

(define (classify command)
  (let ((parsed (catch-errors (lambda () (shell-parse command)))))
    (if (error? parsed)
        'unparsed
        (let ((count (field-ref parsed 'count 0))
              (heredocs (field-ref parsed 'heredocs '())))
          (cond ((not (null? heredocs)) 'heredoc)
                ((string-contains? command "$") 'variable)
                ((> count 1) 'compound)
                (else 'plain))))))

;; `tally` wants strings, and the classifier answers in symbols.
(define shapes (tally (map (lambda (c) (symbol->string (classify c))) commands)))

;; The first program of each line, which is what a replacement would have to be.
(define (first-program command)
  (let ((parsed (catch-errors (lambda () (shell-parse command)))))
    (if (error? parsed)
        "?"
        (let ((names (field-ref parsed 'programs '())))
          (if (null? names) "?" (car names))))))

(define programs (tally (map first-program commands)))

(list (list 'commands (length commands))
      (list 'shapes (ranked shapes))
      (list 'top-programs (top (ranked programs) 12)))
