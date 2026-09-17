;;; steer.scm -- tell the agent something useful, without changing what it asked for.
;;;
;;; `additionalContext` on a PreToolUse hook adds text to the model's context and
;;; changes nothing else: the call it asked for still runs, and still returns what
;;; it always returned.
;;;
;;; That difference is the whole reason this is worth doing. A rewrite has to be
;;; proven byte-identical before it is allowed, because the agent cannot see that it
;;; happened -- and `search-read` showed how hard that bar is, reproducing grep
;;; exactly and still losing on speed. Advice needs no such proof. It cannot be
;;; wrong about the answer, only about whether it was worth saying.
;;;
;;; So the bar here is different in kind: not "is this equivalent" but "is this
;;; specific, true, and actionable". Anything else is noise in a context window,
;;; which is the one thing this project exists to stop wasting.

(define steer-sleep-threshold-ms 10000)

(define (steer-enabled?)
  (let ((flag (env-value "TOOLSCHEME_STEER")))
    (and (string? flag) (not (string=? flag "0")) (not (string=? flag "")))))

;; Recent history for one session only. Scanning the shared log would cost more on
;; every call as the corpus grows and would read other sessions' work for nothing;
;; a session's own keys are bounded by that session.
(define (session-key-path session)
  (string-append "sessions/" (if (string-null? session) "unknown" session) ".keys"))

(define (session-keys session)
  (let ((file (catch-errors
                (lambda () (read-file (session-key-path session) '((limit 262144)))))))
    (if (error? file)
        '()
        (filter (lambda (l) (not (string-null? l)))
                (field-ref (text-lines (field-ref file 'text "")) 'lines)))))

(define (remember-key session key)
  (let ((path (session-key-path session)))
    (if (error? (catch-errors
                  (lambda () (write-file path (string-append key "\n") '((append #t))))))
        (begin (catch-errors (lambda () (mkdir "sessions" '((parents #t)))))
               (catch-errors
                 (lambda () (write-file path (string-append key "\n") '((append #t))))))
        #t)))

(define (count-key keys key)
  (count-if (lambda (k) (string=? k key)) keys))

;; The command's identity for repeat detection. Deliberately the whole command:
;; `grep x a` and `grep x b` are not the same work, and normalizing them together
;; would report a repeat that never happened.
(define (steer-key request)
  (let ((command (hook-command-of (field-ref request "tool_input" '()))))
    (string-append (field-ref request "tool_name" "") "\t"
                   (if (string-null? command)
                       (clip (write-to-string (field-ref request "tool_input" '())) 400)
                       command))))

;; A fixed sleep is a guess about how long something takes. The corpus spent 9.3
;; hours of 52.8 on exactly this, in identical 45-to-60 second increments.
(define (sleeping-for request)
  (let* ((input (field-ref request "tool_input" '()))
         (declared (field-ref input "duration_ms" #f))
         (command (hook-command-of input))
         (parsed (field-ref (shell-parse command) 'commands '())))
    (cond
      ((number? declared) declared)
      ((null? parsed) 0)
      (else
        (let loop ((rest parsed))
          (cond ((null? rest) 0)
                ((string=? (field-ref (car rest) 'name "") "sleep")
                 (let ((seconds (string->number
                                  (let ((args (field-ref (car rest) 'arguments '())))
                                    (if (null? args) "0" (car args))))))
                   (if (number? seconds) (* 1000 seconds) 0)))
                (else (loop (cdr rest)))))))))

(define (advice-for request keys)
  (let ((slept (sleeping-for request))
        (key (steer-key request)))
    (cond
      ;; Said once per session: the point is made, and repeating it every call
      ;; would cost more context than the advice saves.
      ((and (>= slept steer-sleep-threshold-ms)
            (= (count-key keys "\tADVISED-SLEEP") 0))
       (list (list 'kind "\tADVISED-SLEEP")
             (list 'text
                   (string-append
                     "This waits a fixed " (number->string (quotient slept 1000))
                     "s whether or not the thing you are waiting for has happened. "
                     "toolscheme has `(wait-for '(exists PATH))`, `(wait-for '(matches PATH TEXT))` "
                     "and `(process-expect JOB TEXT)`, which return the moment it does "
                     "and report whether it happened or the deadline expired."))))
      ;; Said every time, because it names a specific call and stays true.
      ((> (count-key keys key) 0)
       (list (list 'kind #f)
             (list 'text
                   (string-append
                     "You have already run this exact invocation "
                     (number->string (count-key keys key))
                     (if (= (count-key keys key) 1) " time" " times")
                     " in this session. If nothing has changed since, you already have "
                     "the answer."))))
      (else #f))))

;; Returns the advice string, or #f. Recording the key is a side effect of asking,
;; because a call that is never remembered can never be recognized as a repeat.
(define (steer-advice request)
  (if (or (not (steer-enabled?))
          (not (equal? (field-ref request "hook_event_name" "") "PreToolUse")))
      #f
      (let* ((session (field-ref request "session_id" ""))
             (keys (session-keys session))
             (found (advice-for request keys))
             (key (steer-key request)))
        (remember-key session key)
        (if (not found)
            #f
            (begin
              (if (field-ref found 'kind) (remember-key session (field-ref found 'kind)) #f)
              (field-ref found 'text))))))
