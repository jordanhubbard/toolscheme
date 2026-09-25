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

;; Counting by key. There is no tally primitive, so this is the accumulator that
;; every one of the Python versions wrote as `collections.Counter`.
(define (tally-add table key)
  (let ((seen (field-ref table key 0)))
    (cons (list key (+ seen 1))
          (filter (lambda (row) (not (equal? (car row) key))) table))))

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

(define shapes
  (fold-left (lambda (table command) (tally-add table (classify command)))
             '() commands))

;; The first program of each line, which is what a replacement would have to be.
(define programs
  (fold-left
    (lambda (table command)
      (let ((parsed (catch-errors (lambda () (shell-parse command)))))
        (if (error? parsed)
            table
            (let ((names (field-ref parsed 'programs '())))
              (if (null? names) table (tally-add table (car names)))))))
    '() commands))

(define (top table n)
  (take (list-sort table (lambda (a b) (> (car (cdr a)) (car (cdr b))))) n))

(list (list 'commands (length commands))
      (list 'shapes (list-sort shapes (lambda (a b) (> (car (cdr a)) (car (cdr b))))))
      (list 'top-programs (top programs 12)))
