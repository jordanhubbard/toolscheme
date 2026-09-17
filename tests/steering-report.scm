;;; steering-report.scm -- what the experiment measured.
;;;
;;; Reads the observation logs the trials wrote and compares the two arms on the
;;; thing the corpus complained about: time spent waiting on a fixed schedule.

(define (arm-logs directory prefix)
  (filter (lambda (p) (string-contains? p prefix))
          (agent-log-files (list directory))))

(define (events-of paths)
  (flatten (map (lambda (p)
                  (let ((r (catch-errors (lambda () (agent-log-events p)))))
                    (if (error? r) '() (field-ref r 'events))))
                paths)))

(define (fixed-sleep-ms call)
  (let* ((command (bash-command (field-ref call 'input '())))
         (parsed (field-ref (shell-parse command) 'commands '())))
    (let loop ((rest parsed) (total 0))
      (cond ((null? rest) total)
            ((string=? (field-ref (car rest) 'name "") "sleep")
             (let* ((args (field-ref (car rest) 'arguments '()))
                    (n (if (null? args) 0 (string->number (car args)))))
               (loop (cdr rest) (+ total (if (number? n) (* 1000 n) 0)))))
            (else (loop (cdr rest) total))))))

(define (summarize label events)
  (let* ((calls (tool-calls events))
         (sleeps (map fixed-sleep-ms calls))
         (slept (fold-left + 0 sleeps))
         (long (count-if (lambda (ms) (>= ms 10000)) sleeps))
         (used (count-if (lambda (c)
                           (string-contains? (bash-command (field-ref c 'input '()))
                                             "wait-for"))
                         calls)))
    (list (list 'arm label)
          (list 'sessions (length (field-ref (latency-report events) 'by-tool '())))
          (list 'tool-calls (length calls))
          (list 'fixed-sleep-seconds (quotient slept 1000))
          (list 'sleeps-over-10s long)
          (list 'wait-for-invocations used)
          (list 'total-tool-ms (field-ref (latency-report events) 'total-ms 0)))))

(define directory (if (null? command-arguments) "steer-experiment" (car command-arguments)))
(list (summarize "steering off" (events-of (arm-logs directory "state-off")))
      (summarize "steering on" (events-of (arm-logs directory "state-on"))))
