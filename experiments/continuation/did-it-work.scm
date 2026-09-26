;;; did-it-work.scm -- what each continuation produced.
;;;
;;; Continuation is the riskiest thing this project turns on: it declines to stop
;;; an agent that said it had more to do. Whether that is worth having is a
;;; question about what happened next, and until the continuation recorded itself
;;; in the log there was no way to ask it -- the ledger said only that a session
;;; had continued at some point, so a stop the hook continued could not be told
;;; from one the human answered.
;;;
;;; Run against $XDG_STATE_HOME/toolscheme.

(define window-ms 300000)   ; five minutes is long enough for a turn to do work

(define (row-of line)
  (let ((parsed (catch-errors (lambda () (json-parse line)))))
    (if (error? parsed) #f (field-ref parsed 'value))))

(define rows
  (filter (lambda (r) r)
          (map row-of
               (filter (lambda (l) (not (string-null? l)))
                       (field-ref (text-lines
                                    (field-ref (read-file "observations.jsonl"
                                                          '((limit 268435456)))
                                               'text ""))
                                  'lines)))))

(define continuations
  (filter (lambda (r) (equal? (field-ref r "event" "") "continued")) rows))

;; Work is a tool call in the same session, after the continuation, inside the
;; window. A continuation that produced none is one that talked an agent into
;; carrying on and got nothing for it.
(define (work-after record)
  (let ((session (field-ref record "session" ""))
        (at (field-ref record "at" 0)))
    (filter (lambda (r)
              (and (equal? (field-ref r "session" "") session)
                   (equal? (field-ref r "event" "") "pre")
                   (not (string-null? (field-ref r "tool" "")))
                   (> (field-ref r "at" 0) at)
                   (< (field-ref r "at" 0) (+ at window-ms))))
            rows)))

(define outcomes
  (map (lambda (c)
         (let ((did (work-after c)))
           (list (list 'session (field-ref c "session" ""))
                 (list 'continuation (field-ref c "continuation" 0))
                 (list 'calls-after (length did))
                 ;; Which tools, because "it kept going" and "it kept going
                 ;; usefully" are different claims.
                 (list 'tools (take (ranked (tally (map (lambda (r) (field-ref r "tool" "?")) did)))
                                    (min 4 (length (tally (map (lambda (r) (field-ref r "tool" "?")) did))))))
                 (list 'triggered-by (field-ref c "message" "")))))
       continuations))

(list (list 'continuations (length continuations))
      (list 'produced-work (count-if (lambda (o) (> (field-ref o 'calls-after 0) 0)) outcomes))
      (list 'produced-nothing (count-if (lambda (o) (= (field-ref o 'calls-after 0) 0)) outcomes))
      (list 'total-calls-after
            (fold-left (lambda (n o) (+ n (field-ref o 'calls-after 0))) 0 outcomes))
      (list 'detail outcomes))
