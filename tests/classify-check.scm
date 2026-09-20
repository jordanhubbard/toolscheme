;;; classify-check.scm -- the classifier's guards, without a network.
;;;
;;; What matters here is not that the model is right -- that is measured in
;;; experiments/stall-classifier, over a real corpus -- but that every way the
;;; call can fail leaves the session alone rather than continuing it on a guess.

;; Any one guard answer blocks. The three exist because a single question let 8
;; of 28 asks through in the measurement.
(define clear '(("names_next_step" #t) ("asks_question" #f)
                ("awaits_human" #f) ("needs_choice" #f)))
(define asked '(("names_next_step" #t) ("asks_question" #t)
                ("awaits_human" #f) ("needs_choice" #f)))
(define waiting '(("names_next_step" #t) ("asks_question" #f)
                  ("awaits_human" #t) ("needs_choice" #f)))
(define choosing '(("names_next_step" #t) ("asks_question" #f)
                   ("awaits_human" #f) ("needs_choice" #t)))
;; A reply missing a field must not read as "false, carry on".
(define partial '(("names_next_step" #t)))

(define (source-of message) (field-ref (stall-signals message) 'source))
(define (continues? message) (field-ref (stall-signals message) 'names-next-step))

(define chatty "Sure! Here you go: {\"names_next_step\":true} -- hope that helps.")

(define checks
  (list
    (list 'clear-answer-does-not-block (not (classify-blocked? clear)))
    (list 'a-question-blocks (classify-blocked? asked))
    (list 'waiting-on-a-person-blocks (classify-blocked? waiting))
    (list 'an-open-choice-blocks (classify-blocked? choosing))
    (list 'a-missing-field-is-not-consent (not (classify-blocked? partial)))
    (list 'object-found-in-chatty-text (last-brace chatty))
    (list 'no-object-is-detected (last-brace "no object here"))
    ;; Off: the lists decide, and they are weak but auditable.
    (list 'disabled-uses-the-lists (source-of "Next: ship it."))
    (list 'disabled-still-decides (continues? "Next: ship it."))
    (list 'classification-off-by-default (not (classify-enabled?)))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (not (classify-blocked? clear))
                 (classify-blocked? asked)
                 (classify-blocked? waiting)
                 (classify-blocked? choosing)
                 (not (classify-blocked? partial))
                 (= (last-brace chatty) 43)
                 (not (last-brace "no object here"))
                 (equal? (source-of "Next: ship it.") "phrases")
                 (continues? "Next: ship it.")
                 (not (classify-enabled?)))))
