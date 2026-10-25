;;; A reproducible one-question comparison, not a productivity claim.
;;; The legacy interaction searches for a symbol and then reads around its hit.
;;; The inspector answers the same location question in one call. Compare total
;;; returned bytes and require the same path, line, and source text.

(define query "define (simple-poll-path")
(define roots '("."))
(define include '("lib/dogfood.scm"))
(define found (search query roots '((literal #t) (include ("lib/dogfood.scm")) (limit 8))))
(define hit (car (field-ref found 'matches '())))
(define path (field-ref hit 'path ""))
(define line (field-ref hit 'line 0))
(define nearby (read-file path
                          (list (list 'first-line (max 1 (- line 4)))
                                (list 'last-line (+ line 4)))))
(define inspected (inspect query roots '((include ("lib/dogfood.scm"))
                                        (context 4) (limit 8))))
(define seen (car (field-ref inspected 'matches '())))

(list (list 'same-location (and (equal? path (field-ref seen 'path ""))
                                (= line (field-ref seen 'line 0))
                                (equal? (field-ref hit 'text "")
                                        (field-ref seen 'text ""))
                                (string-contains? (field-ref nearby 'text "")
                                                  (field-ref seen 'text ""))))
      (list 'baseline-calls 2)
      (list 'inspect-calls 1)
      (list 'baseline-result-bytes
            (+ (string-length (write-to-string found))
               (string-length (write-to-string nearby))))
      (list 'inspect-result-bytes (string-length (write-to-string inspected)))
      (list 'inspect-truncated (field-ref inspected 'truncated #f)))
