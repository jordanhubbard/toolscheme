;;; sleeps.scm -- the fixed-wait figure, recomputed.
;;;
;;; 1,168 sleeps and 11.9 hours is the number the refusal in [[dogfood]] rests
;;; on, and it was computed with a tokenizer that dropped everything after a
;;; quoted word. A `sleep 60` sitting after one was invisible, so the figure
;;; could only ever have been low. Run under both binaries to find out by how
;;; much, rather than assuming it is small.

(define log ".local/state/toolscheme/observations.jsonl")

(define rows
  (filter (lambda (r) r)
          (map (lambda (line)
                 (let ((parsed (catch-errors (lambda () (json-parse line)))))
                   (if (error? parsed) #f (field-ref parsed 'value))))
               (filter (lambda (l) (not (string-null? l)))
                       (field-ref (text-lines
                                    (field-ref (read-file log '((limit 268435456))) 'text ""))
                                  'lines)))))

;; `sleeping-for` takes a hook request, and already knows both spellings -- a
;; shell `sleep N` and Codex's duration_ms tool -- and already declines to count
;; a sleep that was backgrounded, which is work being simulated rather than an
;; agent waiting on it.
(define (as-request row)
  (list (list "hook_event_name" "PreToolUse")
        (list "tool_name" (field-ref row "tool" ""))
        (list "tool_input" (list (list "command" (field-ref row "command" ""))))))

;; `sleeping-for` answers 0 for a command that is not a wait at all, rather than
;; refusing, so the filter is on a positive duration. Taking `number?` at face
;; value counted every command in the corpus as a sleep.
(define durations
  (filter (lambda (n) (and (number? n) (> n 0)))
          (map (lambda (row)
                 (let ((ms (catch-errors (lambda () (sleeping-for (as-request row))))))
                   (if (error? ms) #f ms)))
               (filter (lambda (r) (equal? (field-ref r "event" "") "pre")) rows))))

(define total-ms (fold-left + 0 durations))

(list (list 'sleeps (length durations))
      (list 'total-hours (/ (round (* 10.0 (/ total-ms 3600000.0))) 10.0))
      (list 'median-seconds
            (if (null? durations)
                0
                (quotient (list-ref (list-sort durations) (quotient (length durations) 2)) 1000))))
