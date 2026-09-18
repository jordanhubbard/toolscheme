;;; continue-check.scm -- when the agent is allowed to keep going, and when not.
;;;
;;; This is the most dangerous decision in the project: an agent that does not stop
;;; has no natural place left to check its own work. So the checks here are mostly
;;; about refusing, and the one that matters most is the last -- a message that both
;;; names a next step and asks a question must stop, because the question wins.

(define (named text) (names-a-next-step? text))
(define (asked text) (asks-a-question? text))

;; The shape the corpus actually produced: a completion summary that names what is
;; left. 139 of 141 stalls looked like this.
(define summary
  "Landed the borrow checker slice and all tests pass. Full borrow task718 remains
   open. Next required slice: nested resource projections with place identity.")

;; The other 2 of 141: a real decision, which is the one case a person is wanted.
(define question "I could take approach A or approach B here. Which would you prefer?")

;; Both signals at once. The question has to win, or this answers on the user's
;; behalf -- the single failure mode that would make the feature indefensible.
(define both "Next step is the parser rewrite. Shall I proceed with that?")

(define plain "All tests pass. Done.")

;; Counting is what bounds the loop, so it is checked directly rather than by
;; driving the hook until it stops.
(define marker (continue-marker "S"))
(define none '())
(define twice (list (string-append "a\t" marker) (string-append "b\t" marker)))

(define checks
  (list (list 'summary-names-a-step (named summary))
        (list 'summary-is-not-a-question (not (asked summary)))
        (list 'question-detected (asked question))
        (list 'question-names-no-step (not (named question)))
        (list 'both-signals-counts-as-a-question (asked both))
        (list 'plain-sign-off-names-nothing (not (named plain)))
        (list 'count-starts-at-zero (continuations-so-far none "S"))
        (list 'count-follows-the-markers (continuations-so-far twice "S"))
        (list 'default-cap (continue-cap))
        (list 'disabled-unless-asked-for (not (continue-enabled?)))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (named summary)
                 (not (asked summary))
                 (asked question)
                 (not (named question))
                 ;; The one that must never regress.
                 (asked both)
                 (not (named plain))
                 (= (continuations-so-far none "S") 0)
                 (= (continuations-so-far twice "S") 2)
                 (= (continue-cap) 3)
                 ;; Off unless switched on, whatever else is true.
                 (not (continue-enabled?)))))
