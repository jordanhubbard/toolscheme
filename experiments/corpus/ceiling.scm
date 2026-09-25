;;; ceiling.scm -- the substitutable fraction, measured with the tokenizer.

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

;; Text tools toolscheme already has, and which read rather than write.
(define replaceable
  '("cat" "head" "tail" "grep" "egrep" "fgrep" "rg" "wc" "sort" "uniq" "cut" "nl"
    "tr" "basename" "dirname" "stat" "ls" "find" "sed" "column" "awk"))

(define (verdict command)
  (let ((parsed (catch-errors (lambda () (shell-parse command)))))
    (if (error? parsed)
        'unparsed
        (let* ((count (field-ref parsed 'count 0))
               (heredocs (field-ref parsed 'heredocs '()))
               (programs (field-ref parsed 'programs '()))
               (all-replaceable (and (not (null? programs))
                                     (not (any? (lambda (p) (not (member p replaceable)))
                                                programs)))))
          (cond ((not (null? heredocs)) 'embeds-a-program)
                ((and (= count 1) all-replaceable) 'single-substitutable)
                ((and (> count 1) all-replaceable) 'pipeline-substitutable)
                ((= count 1) 'single-other)
                (else 'compound-other))))))

(define (tally-add table key)
  (let ((seen (field-ref table key 0)))
    (cons (list key (+ seen 1))
          (filter (lambda (row) (not (equal? (car row) key))) table))))

(define verdicts
  (fold-left (lambda (table c) (tally-add table (verdict c))) '() commands))

(define total (length commands))
(define (share key) (quotient (* 100 (field-ref verdicts key 0)) total))

(list (list 'commands total)
      (list 'verdicts (list-sort verdicts (lambda (a b) (> (car (cdr a)) (car (cdr b))))))
      (list 'percent
            (list (list 'single-substitutable (share 'single-substitutable))
                  (list 'pipeline-substitutable (share 'pipeline-substitutable))
                  (list 'ceiling-together (+ (share 'single-substitutable)
                                             (share 'pipeline-substitutable))))))
