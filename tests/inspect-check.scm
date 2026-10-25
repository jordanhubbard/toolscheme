;;; inspect-check.scm -- one bounded search supplies the lines needed to locate code.

(define found (inspect "simple-poll-path" '(".")
                       '((include ("lib/dogfood.scm")) (context 2) (limit 1))))
(define hit (car (field-ref found 'matches '())))
(define empty (inspect "this-pattern-is-not-in-the-fixture" '(".")
                       '((include ("lib/dogfood.scm")) (limit 1))))
(define bad (inspect "x" '(".") '((context 21))))

(list (list 'checks
            (list (list 'path (field-ref hit 'path ""))
                  (list 'line (field-ref hit 'line 0))
                  (list 'context (field-ref hit 'after '()))
                  (list 'bounded (field-ref found 'truncated #f))
                  (list 'no-match (field-ref empty 'count -1))
                  (list 'invalid-options (error? bad))))
      (list 'checks-hold
            (and (equal? (field-ref hit 'path "") "lib/dogfood.scm")
                 (> (field-ref hit 'line 0) 0)
                 (not (null? (field-ref hit 'after '())))
                 (field-ref found 'truncated #f)
                 (= (field-ref empty 'count -1) 0)
                 (error? bad))))
