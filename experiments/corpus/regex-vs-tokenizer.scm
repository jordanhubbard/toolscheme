;;; compare.scm -- the regex classification against the tokenizer, same input.
;;;
;;; The Python analyses in this session called a line compound if it contained
;;; any of | && ; $ ` > < anywhere. That counts a `>` inside a quoted string, a
;;; `$` inside a sed script, and every character of a heredoc body. The tokenizer
;;; knows what a heredoc is -- it was written because a regex sweep reported `e`,
;;; `if` and `out.field` as the top commands in this very corpus.

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

;; What the Python said.
(define (regex-compound? command)
  (any? (lambda (mark) (string-contains? command mark))
        '("|" "&&" ";" "$" "`" ">" "<")))

;; What the tokenizer says.
(define (really-compound? command)
  (let ((parsed (catch-errors (lambda () (shell-parse command)))))
    (and (not (error? parsed)) (> (field-ref parsed 'count 0) 1))))

(define counts
  (fold-left
    (lambda (acc command)
      (let ((r (regex-compound? command))
            (t (really-compound? command)))
        (list (list 'regex-says (+ (field-ref acc 'regex-says 0) (if r 1 0)))
              (list 'tokenizer-says (+ (field-ref acc 'tokenizer-says 0) (if t 1 0)))
              (list 'regex-wrong (+ (field-ref acc 'regex-wrong 0) (if (and r (not t)) 1 0)))
              (list 'tokenizer-only (+ (field-ref acc 'tokenizer-only 0) (if (and t (not r)) 1 0))))))
    '((regex-says 0) (tokenizer-says 0) (regex-wrong 0) (tokenizer-only 0))
    commands))

;; A few the regex called compound and the tokenizer did not.
(define examples
  (take (filter (lambda (c) (and (regex-compound? c) (not (really-compound? c)))) commands) 4))

(list (list 'commands (length commands))
      (list 'counts counts)
      (list 'over-counted-examples (map (lambda (c) (substring c 1 (min 64 (string-length c))))
                                        examples)))
