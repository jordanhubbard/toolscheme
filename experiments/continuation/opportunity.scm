;;; opportunity.scm -- how often is there anything for continuation to do?
;;;
;;; Run against $CODEX_HOME/sessions (normally ~/.codex/sessions).
;;;
;;; The trial has been quiet, and quiet has two explanations with opposite
;;; remedies. Either the delivery mechanism was broken and the opportunities went
;;; by unnoticed -- in which case fixing delivery is the whole job -- or agents
;;; do not actually stall the way the feature assumes, in which case no amount of
;;; working delivery would have helped and the premise is wrong.
;;;
;;; Rollouts settle it, because Codex writes them whether or not anything could
;;; reach the session. This walks every turn and counts the moments a
;;; continuation could have fired: an assistant message that names its own next
;;; step, followed by a gap. The gap is the measurement -- what happened next,
;;; and how long it took.
;;;
;;; Four outcomes, and they mean different things:
;;;
;;;   answered-at-once  a human replied within the idle threshold. Continuation
;;;                     would have raced them and lost; nothing to recover.
;;;   answered-later    a human replied, but only after the threshold. This is
;;;                     the recoverable case, and the gap is the prize.
;;;   never-answered    the turn is the last in the thread. Either the work was
;;;                     done, or the session was abandoned -- continuation would
;;;                     have carried on talking to nobody.
;;;   kept-going        the agent continued by itself. Nothing was stalled.

(define idle-threshold-ms 90000)

(define (json-of line)
  (let ((parsed (catch-errors (lambda () (json-parse line)))))
    (if (error? parsed) #f (field-ref parsed 'value))))

;; An assistant turn, a user turn, or neither -- read from the response_item
;; shape, which is the one that carries role and content.
(define (turn-of record)
  (if (not (equal? (field-ref record "type" "") "response_item"))
      #f
      (let ((payload (field-ref record "payload" '())))
        (if (not (equal? (field-ref payload "type" "") "message"))
            #f
            (let* ((role (field-ref payload "role" ""))
                   (content (field-ref payload "content" '()))
                   (text (fold-left
                           (lambda (acc part)
                             (string-append acc (field-ref part "text" "")))
                           "" content)))
              (list role text))))))

;; Days since 1970-01-01 from a civil date. The first version of this multiplied
;; the day-of-month by 86400, which is right inside one month and nonsense across
;; a boundary: a thread spanning 30 September to 1 October produced a gap of
;; minus 29 days, and the corpus spans three weeks. Counts that depend only on
;; ordering survived that; every gap did not, which is why the arithmetic is
;; exact here rather than approximate.
(define (days-from-civil year month day)
  (let* ((y (if (<= month 2) (- year 1) year))
         (era (quotient (if (>= y 0) y (- y 399)) 400))
         (yoe (- y (* era 400)))
         (doy (+ (quotient (+ (* 153 (+ month (if (> month 2) -3 9))) 2) 5) (- day 1)))
         (doe (+ (* yoe 365) (quotient yoe 4) (- (quotient yoe 100)) doy)))
    (+ (* era 146097) doe -719468)))

;; "2026-10-09T14:23:55.123Z" -- seconds are enough for a 90-second threshold,
;; so the fraction is ignored.
(define (millis record)
  (let ((stamp (field-ref record "timestamp" "")))
    (if (< (string-length stamp) 19)
        0
        (let ((year (string->number (substring stamp 1 4)))
              (month (string->number (substring stamp 6 7)))
              (day (string->number (substring stamp 9 10)))
              (hour (string->number (substring stamp 12 13)))
              (minute (string->number (substring stamp 15 16)))
              (second (string->number (substring stamp 18 19))))
          (if (and (number? year) (number? month) (number? day)
                   (number? hour) (number? minute) (number? second))
              (* 1000 (+ (* (days-from-civil year month day) 86400)
                         (* hour 3600) (* minute 60) second))
              0)))))

(define (classify path)
  (let* ((text (let ((r (catch-errors (lambda () (read-file path '((limit 33554432)))))))
                 (if (error? r) "" (field-ref r 'text ""))))
         (records (filter (lambda (r) r)
                          (map json-of
                               (filter (lambda (l) (not (string-null? l)))
                                       (field-ref (text-lines text) 'lines))))))
    ;; Walk the turns in order, and for each assistant turn that names a next
    ;; step, look at what came after it.
    (let loop ((rest records) (pending #f) (at 0) (out '()))
      (cond
        ((null? rest)
         (if pending (cons (list 'never-answered 0) out) out))
        (else
          (let* ((record (car rest))
                 (turn (turn-of record))
                 (when (millis record)))
            (cond
              ((not turn) (loop (cdr rest) pending at out))
              ;; A user turn closes any pending stall.
              ((equal? (car turn) "user")
               (if pending
                   (let ((gap (- when at)))
                     (loop (cdr rest) #f 0
                           (cons (list (if (< gap idle-threshold-ms)
                                           'answered-at-once
                                           'answered-later)
                                       gap)
                                 out)))
                   (loop (cdr rest) #f 0 out)))
              ;; An assistant turn either opens a stall or supersedes one, in
              ;; which case the agent kept going on its own.
              (else
                (let ((names-step (names-a-next-step? (car (cdr turn))))
                      (asks (asks-a-question? (car (cdr turn)))))
                  (loop (cdr rest)
                        (and names-step (not asks))
                        when
                        ;; The gap is carried because `kept-going` has a
                        ;; confound: Codex can emit more than one assistant
                        ;; message inside a single turn, and those arrive
                        ;; seconds apart. Counted naively they look like an
                        ;; agent carrying on by itself, which would inflate the
                        ;; one number this whole analysis turns on. A gap is how
                        ;; the two are told apart.
                        (if pending
                            (cons (list 'kept-going (- when at)) out)
                            out)))))))))))

(define findings
  (fold-left (lambda (acc path) (append (classify path) acc))
             '()
             (codex-rollout-paths)))

(define (of kind) (filter (lambda (f) (eq? (car f) kind)) findings))

(define recoverable (of 'answered-later))

(list
  (list 'threads-read (length (codex-rollout-paths)))
  (list 'moments-a-continuation-could-have-fired (length findings))
  (list 'answered-at-once (length (of 'answered-at-once)))
  (list 'answered-later (length recoverable))
  (list 'never-answered (length (of 'never-answered)))
  (list 'kept-going (length (of 'kept-going)))
  ;; Split, because a few seconds means the same turn still talking and a longer
  ;; gap means the agent genuinely went on working.
  (list 'kept-going-within-5s
        (count-if (lambda (f) (< (car (cdr f)) 5000)) (of 'kept-going)))
  (list 'kept-going-after-5s
        (count-if (lambda (f) (>= (car (cdr f)) 5000)) (of 'kept-going)))
  ;; The prize, if there is one: human time spent waiting after a stall that had
  ;; already named its own next step.
  (list 'recoverable-minutes
        (/ (round (/ (fold-left (lambda (n f) (+ n (car (cdr f)))) 0 recoverable) 6000.0)) 10.0))
  (list 'median-recoverable-seconds
        (if (null? recoverable)
            0
            (quotient (car (cdr (list-ref (list-sort (map (lambda (f) (list (car (cdr f)) (car (cdr f)))) recoverable))
                                          (quotient (length recoverable) 2))))
                      1000))))
