;;; pipeline-check.scm -- does pipeline-run reproduce the shell, byte for byte?
;;;
;;; Checked against the real programs rather than against an expectation written
;;; here, because the claim is equivalence with what the agent would otherwise
;;; have run. An expectation would only record what was believed at the time.
;;;
;;; A stage the runner does not implement must decline the whole pipeline rather
;;; than approximate it. `declined` is therefore a passing verdict and `DIFFERS`
;;; is not: guessing at a tool's exact output is how a substitution starts lying.

(define cases
  (list
    "sed -n '1,5p' lib/prelude.scm"
    "sed -n '40,45p' lib/prelude.scm"
    "rg -n define lib/dogfood.scm | head -5"
    "rg tally lib/prelude.scm"
    "rg -n tally lib/prelude.scm | head -3"
    "cat VERSION.txt"
    "tail -3 lib/prelude.scm"
    "head -4 Makefile"
    "sed -n '1,20p' lib/steer.scm | rg -n define"
    "nl lib/dogfood.scm | head -3"))

(define (shell-output command)
  (let ((r (sh (list (list 'command command)))))
    (if (error? r)
        #f
        (let ((done (process-wait (field-ref r 'job))))
          (field-ref done 'stdout "")))))

;; The shell keeps a trailing newline; joining lines does not.
(define (normalize text)
  (if (and (string? text) (> (string-length text) 0)
           (char=? (string-ref text (string-length text)) #\newline))
      (substring text 1 (- (string-length text) 1))
      text))

(define results
  (map (lambda (command)
         (let* ((theirs (normalize (shell-output command)))
                (ours (pipeline-run command)))
           (list (list 'command command)
                 (list 'verdict
                       (cond ((not ours) 'declined)
                             ((equal? ours theirs) 'identical)
                             (else 'DIFFERS)))
                 (list 'their-bytes (if (string? theirs) (string-length theirs) -1))
                 (list 'our-bytes (if (string? ours) (string-length ours) -1)))))
       cases))

(list (list 'identical (count-if (lambda (r) (eq? (field-ref r 'verdict) 'identical)) results))
      (list 'declined (count-if (lambda (r) (eq? (field-ref r 'verdict) 'declined)) results))
      (list 'differs (count-if (lambda (r) (eq? (field-ref r 'verdict) 'DIFFERS)) results))
      (list 'detail results)
      ;; The only unacceptable outcome is disagreeing with the shell while
      ;; claiming to have answered.
      (list 'checks-hold
            (and (= 0 (count-if (lambda (r) (eq? (field-ref r 'verdict) 'DIFFERS)) results))
                 (> (count-if (lambda (r) (eq? (field-ref r 'verdict) 'identical)) results) 0))))
