;;; adoption-report.scm -- did the instruction land, and is it doing anything?
;;;
;;; Claude Code does not record its loaded instructions in the transcript, so
;;; whether CLAUDE.md was read cannot be checked directly. It can be checked by
;;; behaviour, which is what the 2x2 measured and what this reads back out of the
;;; observation log:
;;;
;;;   polled but never advised   the instruction did not reach this session
;;;   polled and then advised    the hook is doing the work; the note did not load
;;;   used the tool, no advice   the note loaded and worked
;;;
;;; Sessions with no waiting in them say nothing either way, and are reported as
;;; such rather than counted as success.

(define (sessions-of events)
  (dedup-keyed (map (lambda (e) (list (field-ref e 'session "") (field-ref e 'session "")))
                    events)))

(define (session-report events session)
  (let* ((all (filter (lambda (e) (string=? (field-ref e 'session "") session)) events))
         (mine (if (null? all) (list (list (list 'agent ""))) all))
         (commands (map (lambda (e) (bash-command (field-ref e 'input '()))) all))
         (polled (any? (lambda (c) (string-contains? c "do sleep")) commands))
         (used (any? (lambda (c) (string-contains? c "wait-for")) commands))
         (waited (any? (lambda (c) (string-contains? c "sleep")) commands)))
    (list (list 'session session)
          (list 'agent (field-ref (car mine) 'agent ""))
          (list 'calls (length all))
          (list 'verdict
                (cond ((and used (not polled)) 'instruction-working)
                      ((and used polled) 'corrected-after-polling)
                      (polled 'polled-uncorrected)
                      (waited 'waited-without-polling)
                      (else 'nothing-to-judge))))))

;; The hook records its own session id, so events carry one even though the
;; analyzer's event shape does not name it; read it back from the raw log.
(define (raw-events path)
  (let* ((text (agent-log-text path))
         (records (map (lambda (l) (let ((p (json-parse l)))
                                     (if (error? p) '() (field-ref p 'value))))
                       (json-lines text))))
    (filter (lambda (r) (and (not (null? r))
                             (equal? (field-ref r "event" "") "pre")))
            records)))

(define (report path)
  (let* ((rows (raw-events path))
         (events (map (lambda (r)
                        (list (list 'session (field-ref r "session" ""))
                              (list 'agent (field-ref r "agent" ""))
                              (list 'input (list (list "command" (field-ref r "command" ""))))))
                      rows))
         (ids (sessions-of events)))
    (map (lambda (s) (session-report events s)) ids)))

(report (if (null? command-arguments) "observations.jsonl" (car command-arguments)))
