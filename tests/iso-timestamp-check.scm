;;; Validate the ISO parser used by opportunity.scm against known epoch values
;;; and against the forms it must refuse rather than guess at.

(define (days-from-civil year month day)
  (let* ((y (if (<= month 2) (- year 1) year))
         (era (quotient (if (>= y 0) y (- y 399)) 400))
         (yoe (- y (* era 400)))
         (doy (+ (quotient (+ (* 153 (+ month (if (> month 2) -3 9))) 2) 5) (- day 1)))
         (doe (+ (* yoe 365) (quotient yoe 4) (- (quotient yoe 100)) doy)))
    (+ (* era 146097) doe -719468)))

(define (two-at text index) (string->number (substring text index (+ index 1))))

(define (iso-millis stamp)
  (let ((n (string-length stamp)))
    (if (or (< n 20)
            (not (equal? (substring stamp 5 5) "-"))
            (not (equal? (substring stamp 8 8) "-"))
            (not (equal? (substring stamp 11 11) "T"))
            (not (equal? (substring stamp 14 14) ":"))
            (not (equal? (substring stamp 17 17) ":")))
        #f
        (let ((year (string->number (substring stamp 1 4)))
              (month (two-at stamp 6))
              (day (two-at stamp 9))
              (hour (two-at stamp 12))
              (minute (two-at stamp 15))
              (second (two-at stamp 18))
              (zone (substring stamp n n)))
          (if (not (and (number? year) (number? month) (number? day)
                        (number? hour) (number? minute) (number? second)))
              #f
              (let* ((utc (+ (* (days-from-civil year month day) 86400)
                             (* hour 3600) (* minute 60) second))
                     (sign-at (- n 5))
                     (sign (if (> n 6) (substring stamp sign-at sign-at) ""))
                     (offset
                       (cond ((equal? zone "Z") 0)
                             ((or (equal? sign "+") (equal? sign "-"))
                              (let ((oh (two-at stamp (- n 4)))
                                    (om (two-at stamp (- n 1))))
                                (if (and (number? oh) (number? om))
                                    (* (if (equal? sign "-") -1 1)
                                       (+ (* oh 3600) (* om 60)))
                                    #f)))
                             (else #f))))
                (if (number? offset) (* 1000 (- utc offset)) #f)))))))

(define (seconds stamp)
  (let ((ms (iso-millis stamp))) (if (number? ms) (quotient ms 1000) ms)))

(list
  (list 'checks
        (list
          ;; Known epochs.
          (list 'epoch (seconds "1970-01-01T00:00:00Z"))
          (list 'y2k (seconds "2000-01-01T00:00:00Z"))
          (list 'with-fraction (seconds "2026-10-09T21:23:55.234Z"))
          (list 'without-fraction (seconds "2026-10-09T21:23:55Z"))
          ;; An offset must move the instant, not be ignored.
          (list 'plus-one-hour (seconds "2026-10-09T22:23:55+01:00"))
          (list 'minus-seven (seconds "2026-10-09T14:23:55-07:00"))
          ;; Forms this does not claim to handle must be refused, not guessed.
          (list 'basic-form (seconds "20261009T142355Z"))
          (list 'no-zone (seconds "2026-10-09T14:23:55"))
          (list 'rubbish (seconds "not a timestamp at all"))
          (list 'empty (seconds ""))))
  (list 'checks-hold
        (and (= (seconds "1970-01-01T00:00:00Z") 0)
             (= (seconds "2000-01-01T00:00:00Z") 946684800)
             ;; Fraction ignored: both forms are the same instant.
             (= (seconds "2026-10-09T21:23:55.234Z") (seconds "2026-10-09T21:23:55Z"))
             ;; 22:23:55+01:00 is 21:23:55Z.
             (= (seconds "2026-10-09T22:23:55+01:00") (seconds "2026-10-09T21:23:55Z"))
             ;; 14:23:55-07:00 is 21:23:55Z.
             (= (seconds "2026-10-09T14:23:55-07:00") (seconds "2026-10-09T21:23:55Z"))
             ;; And the refusals.
             (not (seconds "20261009T142355Z"))
             (not (seconds "2026-10-09T14:23:55"))
             (not (seconds "not a timestamp at all"))
             (not (seconds "")))))
