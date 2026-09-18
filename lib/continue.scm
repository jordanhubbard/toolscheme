;;; continue.scm -- keep going when the agent already said what it would do next.
;;;
;;; Measured across 30 Codex sessions: 34.5 hours idle waiting for a human, 10.1 of
;;; them in gaps short enough to be worth recovering, against 2.7 hours asleep on a
;;; timer. Idle is twelve times the problem `wait-for` was built for.
;;;
;;; The obvious reading of that is wrong, and the data says so. Of 141 such stalls,
;;; 2 ended with a question or an offer to proceed. The other 139 ended with a
;;; completion summary that named the next step -- "Full borrow task718 remains
;;; open. Next required slice: nested resource projections" -- and stopped anyway.
;;; So this does not auto-approve decisions. It declines to stop when the agent has
;;; already said what it would do next.
;;;
;;; This is the most dangerous thing in this project, because an agent that never
;;; stops has no natural place left to check its own work. Every guard below exists
;;; so that it stops for a reason rather than running until something breaks, and
;;; the whole thing is off unless switched on.
;;;
;;; Claude Code only: Codex has no Stop hook to decline.

(define continue-default-cap 3)

(define (continue-enabled?) (setting-on? "TOOLSCHEME_CONTINUE"))

(define (continue-cap)
  (let ((configured (setting "TOOLSCHEME_CONTINUE_MAX")))
    (if (string? configured)
        (let ((n (string->number configured)))
          (if (number? n) n continue-default-cap))
        continue-default-cap)))

(define (message-tail text limit)
  (if (> (string-length text) limit)
      (substring text (- (string-length text) limit) (string-length text))
      text))

;; A question is the one case that genuinely wants a person. Answering it on the
;; user's behalf is exactly the failure this must not have.
(define (asks-a-question? text)
  (let ((tail (string-downcase (message-tail text 400))))
    (or (string-suffix? "?" (string-trim tail))
        (any? (lambda (phrase) (string-contains? tail phrase))
              '("shall i" "should i" "would you like" "do you want"
                "let me know" "which would you prefer" "confirm whether")))))

;; Forward-looking language, stated by the agent about its own work. Deliberately
;; narrow: a vague sign-off is not a plan, and continuing on one produces an agent
;; that wanders rather than works.
(define (names-a-next-step? text)
  (let ((lower (string-downcase text)))
    (any? (lambda (phrase) (string-contains? lower phrase))
          '("next step" "next required" "next slice" "remains open" "still open"
            "still needs" "not yet implemented" "not yet done" "todo:" "to do:"
            "remaining work" "follow-up" "follow up:" "outstanding"))))

(define (continue-marker session) (string-append "CONTINUED " session))

(define (continuations-so-far keys session)
  (count-if (lambda (line) (string-contains? line (continue-marker session))) keys))

;; #f means "stop, as the agent intended". Anything else is a decision to keep
;; going, and every path to it is guarded.
(define (continue-decision request)
  (let* ((session (field-ref request "session_id" ""))
         (message (field-ref request "last_assistant_message" ""))
         (keys (session-keys session))
         (used (continuations-so-far keys session)))
    (cond
      ((not (continue-enabled?)) #f)
      ((not (string? message)) #f)
      ((string-null? (string-trim message)) #f)
      ;; The agent is asking, not reporting.
      ((asks-a-question? message) #f)
      ;; No stated next step is no mandate to invent one.
      ((not (names-a-next-step? message)) #f)
      ;; Bounded, so a loop ends by arithmetic rather than by someone noticing.
      ((>= used (continue-cap)) #f)
      (else
        (begin
          (remember-key session (continue-marker session) "stop")
          (list (list "hookSpecificOutput"
                      (list (list "hookEventName" "Stop")
                            (list "continueConversation" #t)
                            (list "systemMessage"
                                  (string-append
                                    "Continuing without waiting: you named a next step and "
                                    "nothing here needs a decision. Do that next step now. "
                                    "This is continuation " (number->string (+ used 1))
                                    " of " (number->string (continue-cap))
                                    ". If the remaining work genuinely needs a person, say "
                                    "so plainly and stop."))))))))))
