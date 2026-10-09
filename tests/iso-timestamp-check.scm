;;; iso-timestamp-check.scm -- reading an instant back out of a timestamp.
;;;
;;; `date` prints an ISO instant, and until now could not read one. Every caller
;;; that needed to parse a timestamp wrote its own, and the first one read the
;;; digits at fixed offsets and ignored the zone -- so a `+01:00` would have been
;;; an hour wrong with no error, which is worse than failing. `(date '((iso ...)))`
;;; exists so there is one parser rather than one per caller.
;;;
;;; What is pinned here is the arithmetic against known epochs, that a zone moves
;;; the instant rather than being ignored, that the forms it accepts all name the
;;; same instant, and that the forms it does not accept are refused rather than
;;; guessed at.

(define (instant text)
  (let ((d (catch-errors (lambda () (date (list (list 'iso text)))))))
    (if (error? d) #f (field-ref d 'epoch-seconds))))

(define (zoned? text)
  (let ((d (catch-errors (lambda () (date (list (list 'iso text)))))))
    (if (error? d) #f (field-ref d 'zoned #f))))

;; 2026-10-09T21:23:55Z, written every way this accepts.
(define same
  '("2026-10-09T21:23:55Z"
    "2026-10-09T21:23:55.234Z"
    "2026-10-09T21:23:55,234Z"
    "2026-10-09t21:23:55z"
    "2026-10-09 21:23:55Z"
    "20261009T212355Z"
    "2026-10-09T22:23:55+01:00"
    "2026-10-09T14:23:55-07:00"
    "2026-10-09T22:23:55+0100"
    "2026-10-09T22:23:55+01"))

(define reference (instant "2026-10-09T21:23:55Z"))

;; Forms this does not handle. Refusing them is the point: a parser that accepts
;; a week date by reading "41" as a month is worse than one that says no.
(define refused
  '("2026-W41-5" "2026-282" "not a timestamp" "" "2026-13-09T00:00:00Z"
    "2026-10-09T25:00:00Z" "2026-10-09T21:23:55+99:00" "2026-10-09T21:23:55Zrubbish"))

(define round-trip
  ;; What it prints, it must read back to the same instant.
  (let* ((now (date))
         (printed (field-ref now 'iso ""))
         (back (instant printed)))
    (and (number? back) (= back (field-ref now 'epoch-seconds)))))

(list
  (list 'checks
        (list (list 'epoch (instant "1970-01-01T00:00:00Z"))
              (list 'y2k (instant "2000-01-01T00:00:00Z"))
              (list 'reference reference)
              (list 'forms-agreeing (count-if (lambda (t) (equal? (instant t) reference)) same))
              (list 'forms-tried (length same))
              (list 'refused (count-if (lambda (t) (not (instant t))) refused))
              (list 'refusals-tried (length refused))
              (list 'date-only (instant "2026-10-09"))
              (list 'zone-reported (zoned? "2026-10-09T21:23:55Z"))
              (list 'missing-zone-reported (zoned? "2026-10-09T21:23:55"))
              (list 'round-trips round-trip)))
  (list 'checks-hold
        (and (= (instant "1970-01-01T00:00:00Z") 0)
             (= (instant "2000-01-01T00:00:00Z") 946684800)
             ;; Every accepted spelling is the same instant.
             (= (count-if (lambda (t) (equal? (instant t) reference)) same) (length same))
             ;; Every unsupported or invalid form is refused.
             (= (count-if (lambda (t) (not (instant t))) refused) (length refused))
             ;; A bare date is midnight UTC.
             (= (instant "2026-10-09") (- reference (+ (* 21 3600) (* 23 60) 55)))
             ;; A missing zone is read as UTC, and the caller is told so.
             (zoned? "2026-10-09T21:23:55Z")
             (not (zoned? "2026-10-09T21:23:55"))
             (number? (instant "2026-10-09T21:23:55"))
             round-trip)))
