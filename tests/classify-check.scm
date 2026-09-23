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

;;; The typed backend. These bodies are the published /v1/systemone response
;;; shape, which Jev, Von and OpenJev all answer in; parsing them is checked
;;; here because the live call cannot be.

;; The parser is exercised directly rather than through the backend switch, so
;; these checks do not depend on environment set half way through a test file.
(define (answered body) (systemone-answer (list (list 'body body))))

(define confident
  "{\"model\":\"von-1.0.0\",\"answers\":{\"names_next_step\":{\"type\":\"noul\",\"noul\":0.95},\"asks_question\":{\"type\":\"noul\",\"noul\":0.02},\"awaits_human\":{\"type\":\"noul\",\"noul\":0.04},\"needs_choice\":{\"type\":\"noul\",\"noul\":0.01}},\"usage\":{\"input_tokens\":304}}")

;; 0.35 is nowhere near sure that a question was asked, and it still stops.
;; That is the asymmetry the thresholds exist for.
(define faint-suspicion
  "{\"answers\":{\"names_next_step\":{\"type\":\"noul\",\"noul\":0.91},\"asks_question\":{\"type\":\"noul\",\"noul\":0.35},\"awaits_human\":{\"type\":\"noul\",\"noul\":0.02},\"needs_choice\":{\"type\":\"noul\",\"noul\":0.01}}}")

;; And the same number in the other direction is not enough to act on.
(define lukewarm
  "{\"answers\":{\"names_next_step\":{\"type\":\"noul\",\"noul\":0.62},\"asks_question\":{\"type\":\"noul\",\"noul\":0.01},\"awaits_human\":{\"type\":\"noul\",\"noul\":0.01},\"needs_choice\":{\"type\":\"noul\",\"noul\":0.01}}}")

(define truncated "{\"answers\":{\"names_next_step\":{\"type\":\"noul\",\"noul\":0.99}}}")

(define (says body key) (field-ref (answered body) key #f))

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
    (list 'classification-off-by-default (not (classify-enabled?)))
    ;; The chat backend stays the default; the typed one is asked for.
    (list 'chat-backend-by-default (not (classify-systemone?)))
    ;; Host, header and model name follow the credential together. Letting the
    ;; header follow it alone sent an Anthropic key to the gateway under a model
    ;; name only the gateway knows, which fails as a 401 that reads like a bad
    ;; key. The checks run with no gateway key, so this is the direct host.
    (list 'host-follows-the-credential
          (string-contains? (classify-endpoint) "api.anthropic.com"))
    (list 'model-name-follows-it-too (classify-model))
    (list 'confident-answer-continues (says confident "names_next_step"))
    (list 'confident-answer-is-unblocked (not (classify-blocked? (answered confident))))
    (list 'faint-suspicion-still-blocks (classify-blocked? (answered faint-suspicion)))
    (list 'lukewarm-go-ahead-refused (not (says lukewarm "names_next_step")))
    (list 'lukewarm-is-otherwise-clear (not (classify-blocked? (answered lukewarm))))
    ;; Missing guards in a truncated reply must read as blocked, not as consent.
    (list 'truncated-reply-blocks (classify-blocked? (answered truncated)))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (not (classify-blocked? clear))
                 (classify-blocked? asked)
                 (classify-blocked? waiting)
                 (classify-blocked? choosing)
                 (not (classify-blocked? partial))
                 (= (last-brace chatty) 43)
                 (not (last-brace "no object here"))
                 (not (classify-systemone?))
                 (string-contains? (classify-endpoint) "api.anthropic.com")
                 (equal? (classify-model) "claude-haiku-4-5")
                 (says confident "names_next_step")
                 (not (classify-blocked? (answered confident)))
                 ;; The two that encode the asymmetry.
                 (classify-blocked? (answered faint-suspicion))
                 (not (says lukewarm "names_next_step"))
                 (not (classify-blocked? (answered lukewarm)))
                 ;; The one that must never invert.
                 (classify-blocked? (answered truncated)))))
