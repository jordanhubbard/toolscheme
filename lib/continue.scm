;;; continue.scm -- keep going when the agent already said what it would do next.
;;;
;;; Measured across 30 Codex sessions: 34.5 hours idle waiting for a human, 10.1 of
;;; them in gaps short enough to be worth recovering, against 2.7 hours asleep on a
;;; timer. Idle is twelve times the problem `wait-for` was built for.
;;;
;;; This does not auto-approve decisions. It declines to stop when the agent has
;;; already said what it would do next -- "Full borrow task718 remains open. Next
;;; required slice: nested resource projections" -- which is what most stalls
;;; look like.
;;;
;;; An earlier version of this comment claimed 2 of 141 stalls ended in a
;;; question. That number came from `asks-a-question?` below, so it was a
;;; measurement of the detector rather than of the corpus, and it was wrong.
;;; Labelled independently over 162 stalls, 28 of them ask something, and these
;;; phrase lists find one: precision 0.15 and recall 0.11 on the decision, 1 of
;;; 28 on the guard. A cheap classifier over the same corpus scores 0.76 / 0.70
;;; and catches 20. See experiments/stall-classifier.
;;;
;;; The lists are kept because they need no network and no credential on a path
;;; that runs inside a hook, and because something auditable should remain when
;;; the classifier is unreachable. They are not good enough to justify raising
;;; the cap or turning this on by default.
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
;;
;; "next: " earns its place by observation rather than guesswork. The first live
;; Codex session this was tested against ended "Uppercased a.txt. Next: uppercase
;; b.txt." -- as clear a statement of intent as the corpus phrases, and missed by
;; every one of them. The trailing space keeps it from matching "nextfoo:".
(define (names-a-next-step? text)
  (let ((lower (string-downcase text)))
    (any? (lambda (phrase) (string-contains? lower phrase))
          '("next step" "next required" "next slice" "next: " "next up"
            "remains open" "still open" "still needs" "not yet implemented"
            "not yet done" "todo:" "to do:" "remaining work" "remaining:"
            "follow-up" "follow up:" "outstanding"))))

(define (continue-marker session) (string-append "CONTINUED " session))

(define (continuations-so-far keys session)
  (count-if (lambda (line) (string-contains? line (continue-marker session))) keys))

;; #f means "stop, as the agent intended". Anything else is a decision to keep
;; going, and every path to it is guarded.
(define (continue-decision request)
  (let* ((session (field-ref request "session_id" ""))
         (message (field-ref request "last_assistant_message" ""))
         (keys (session-keys session))
         (used (continuations-so-far keys session))
         ;; Classified when that is switched on, phrase lists otherwise. The
         ;; difference is large enough to matter: see [[classify]].
         (signals (if (string? message) (stall-signals message) '())))
    (cond
      ((not (continue-enabled?)) #f)
      ((not (string? message)) #f)
      ((string-null? (string-trim message)) #f)
      ;; The agent is asking, not reporting.
      ((field-ref signals 'asks-question #f) #f)
      ;; No stated next step is no mandate to invent one.
      ((not (field-ref signals 'names-next-step #f)) #f)
      ;; Bounded, so a loop ends by arithmetic rather than by someone noticing.
      ((>= used (continue-cap)) #f)
      (else
        (begin
          (remember-key session (continue-marker session) "stop")
          ;; Recorded in the log as well as the ledger, because the ledger says
          ;; only that a session continued at some point and the question worth
          ;; answering is what each continuation produced. Without a timestamped
          ;; record there is no way to tell a stop the hook continued from one
          ;; the human answered, and the first attempt to measure this could not
          ;; separate them.
          (catch-errors
            (lambda ()
              (hook-append
                (list (list "source" "toolscheme-hook")
                      (list "event" "continued")
                      (list "session" session)
                      (list "tool" "")
                      (list "command" "")
                      (list "continuation" (+ used 1))
                      (list "of" (continue-cap))
                      (list "message" (clip message hook-command-limit))
                      (list "at" (field-ref (time) 'epoch-milliseconds))
                      (list "bytes" 0)))))
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
