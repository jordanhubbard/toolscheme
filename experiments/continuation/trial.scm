;;; trial.scm -- what Codex continuation has done since the interface changed.
;;;
;;; Run against $XDG_STATE_HOME/toolscheme, any time. It reads the baseline
;;; written when 0.6.0 went in and diffs the log against it, so the trial's
;;; result does not depend on anyone remembering to look on a particular day.
;;;
;;; The question is narrow and the honest answers include zero. Continuation has
;;; delivered two messages in its existence, both in controlled tests I ran
;;; myself. Everything before 0.6.0 is excluded because it measured a different
;;; mechanism: sessions had to be launched through a shim against an app server
;;; toolscheme ran, and 449 of the pre-0.6.0 "continued" records are from a
;;; period when a misread exit status recorded every failed delivery as a
;;; success. Codex now runs its own daemon and `codex queue` reaches any session,
;;; so this is the first measurement of the feature as it actually works.
;;;
;;; What would make it worth keeping: continuations that arrive and are followed
;;; by tool calls. What would retire it: a fortnight of real use in which it
;;; either never fires, or fires and produces nothing.

(define baseline
  (let ((r (catch-errors
             (lambda () (read-file "reports/trial-0.6.0.json" '((limit 65536)))))))
    (if (error? r)
        #f
        (field-ref (json-parse (field-ref r 'text "")) 'value '()))))

(define rows
  (filter (lambda (r) r)
          (map (lambda (line)
                 (let ((parsed (catch-errors (lambda () (json-parse line)))))
                   (if (error? parsed) #f (field-ref parsed 'value))))
               (filter (lambda (l) (not (string-null? l)))
                       (field-ref (text-lines
                                    (field-ref (read-file "observations.jsonl"
                                                          '((limit 268435456)))
                                               'text ""))
                                  'lines)))))

(define started (if baseline (field-ref baseline "at" 0) 0))
(define now (field-ref (time) 'epoch-milliseconds))
(define since (filter (lambda (r) (> (field-ref r "at" 0) started)) rows))

(define delivered
  (filter (lambda (r) (equal? (field-ref r "event" "") "continued")) since))
(define failed
  (filter (lambda (r) (equal? (field-ref r "event" "") "continue-failed")) since))

;; Work is a tool call in the same thread, after the continuation, inside a
;; window long enough for a turn to do something. A continuation that produced
;; none talked an agent into carrying on and got nothing for it.
(define window-ms 300000)

(define (calls-after record)
  (let ((thread (field-ref record "session" ""))
        (at (field-ref record "at" 0)))
    (count-if (lambda (r)
                (and (equal? (field-ref r "session" "") thread)
                     (equal? (field-ref r "event" "") "pre")
                     (not (string-null? (field-ref r "tool" "")))
                     (> (field-ref r "at" 0) at)
                     (< (field-ref r "at" 0) (+ at window-ms))))
              since)))

(define outcomes (map calls-after delivered))

(list
  (list 'trial-days (/ (round (* 10.0 (/ (- now started) 86400000.0))) 10.0))
  (list 'codex-calls-observed
        (count-if (lambda (r) (and (equal? (field-ref r "agent" "") "codex")
                                   (equal? (field-ref r "event" "") "pre")))
                  since))
  (list 'delivered (length delivered))
  (list 'failed-to-arrive (length failed))
  (list 'produced-work (count-if (lambda (n) (> n 0)) outcomes))
  (list 'produced-nothing (count-if (lambda (n) (= n 0)) outcomes))
  (list 'calls-after-total (fold-left + 0 outcomes))
  ;; Named so a reader can tell "it never fired" from "the baseline is missing".
  (list 'baseline (if baseline 'read 'MISSING)))
