;;; inspect.scm -- answer a repository-location question in one bounded call.
;;;
;;; `search` already returns line context. This names the complete task and gives
;;; it small defaults so an agent can ask once instead of searching, then making
;;; separate reads for each hit. `truncated` is preserved: a bounded answer must
;;; say when it may be incomplete.

(define (inspect-option options name fallback)
  (let ((value (field-ref options name #f)))
    (if (absent? value) fallback value)))

(define (inspect pattern roots options)
  (let* ((context (inspect-option options 'context 4))
         (limit (inspect-option options 'limit 8))
         (include (inspect-option options 'include '()))
         (literal (inspect-option options 'literal #t)))
    (if (or (not (string? pattern)) (string-null? pattern)
            (not (list? roots))
            (not (number? context)) (< context 0) (> context 20)
            (not (number? limit)) (< limit 1) (> limit 50)
            (not (list? include)))
        '((error "inspect needs a pattern, roots, context 0..20, and limit 1..50")
          (code invalid-argument) (operation inspect))
        (let ((found (search pattern roots
                             (list (list 'literal literal)
                                   (list 'include include)
                                   (list 'before context)
                                   (list 'after context)
                                   (list 'limit limit)))))
          (if (error? found) found
              (list (list 'query pattern)
                    (list 'matches (field-ref found 'matches '()))
                    (list 'count (field-ref found 'count 0))
                    (list 'files-scanned (field-ref found 'files-scanned 0))
                    (list 'truncated (field-ref found 'truncated #f))))))))
