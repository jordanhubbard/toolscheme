;;; classify.scm -- ask a model the question the phrase lists get wrong.
;;;
;;; The lists in [[continue]] decide whether a stopped agent should be told to
;;; carry on. Measured over 162 labelled stalls from this machine's own
;;; transcripts they score precision 0.15 and recall 0.11, and catch 1 of the 28
;;; messages that ask the user something. A cheap classifier over the same
;;; corpus scores 0.76 / 0.70 and catches 20. This file, measured the same way
;;; and called exactly as a hook calls it, scores 0.83 / 0.68 and catches 24 --
;;; the difference being the three-question guard below. The harness is in
;;; experiments/stall-classifier; these are its output, not an estimate.
;;;
;;; Why the lists fail is the useful part. "The client remains open. No source
;;; changes made yet." matched "remains open" -- an application window, not work
;;; remaining. A message ending "which OS?" was missed because Codex appends
;;; citation XML after the prose and the question window only reaches back 400
;;; characters. Surface strings do not carry the sense, and no amount of adding
;;; phrases fixes that.
;;;
;;; This asks every question in one round trip, because the cost of a request is
;;; the round trip and not the questions in it. That shape is also why a System
;;; One model -- constrained outputs, flat latency in the number of questions --
;;; drops in by setting TOOLSCHEME_CLASSIFY_MODEL and nothing else.

(define classify-endpoint
  (or (env-value "TOOLSCHEME_CLASSIFY_ENDPOINT")
      "https://inference-api.nvidia.com/v1/messages"))

;; Cheap on purpose. This runs on a hook path, and the measurement that
;; justified it was taken with exactly this model.
(define classify-model
  (or (env-value "TOOLSCHEME_CLASSIFY_MODEL") "azure/anthropic/claude-haiku-4-5"))

;; A hook that hangs breaks the session it is meant to help, so the budget is
;; well inside the handler timeout and a miss falls back rather than failing.
(define classify-default-timeout-ms 3000)

(define (classify-timeout-ms)
  (let ((configured (setting "TOOLSCHEME_CLASSIFY_TIMEOUT_MS")))
    (if (string? configured)
        (let ((n (string->number configured)))
          (if (number? n) n classify-default-timeout-ms))
        classify-default-timeout-ms)))

;; Explicit rather than "on when a credential happens to exist", so that whether
;; a hook talks to the network is a decision someone made and can find again.
;; A missing credential deliberately does not turn this back off: asking for
;; classification and silently getting the phrase lists instead is the failure
;; that hides itself. With this on and no key, nothing continues.
(define (classify-enabled?) (setting-on? "TOOLSCHEME_CLASSIFY"))

;; One question for the decision and three for the guard, deliberately worded
;; differently. The failures worth catching were not all questions in the
;; grammatical sense: "Awaiting diff review before running..." and "Then tell me
;; to retry." both wait on a person without a question mark between them.
(define classify-prompt
  (string-append
    "You are labelling the final message of a coding-agent turn. The agent "
    "stopped after this message and waited for a human.\n\n"
    "Answer these four questions about it:\n"
    "1. names_next_step: does the message state a specific next action ON THE "
    "TASK that the agent itself could carry out with no decision from a human? "
    "A vague sign-off, a completion report with nothing left, or a suggestion "
    "that the HUMAN do something are all false.\n"
    "2. asks_question: does it ask the human anything, or request a decision, "
    "approval, preference, or clarification?\n"
    "3. awaits_human: does it say it is waiting on a person, a review, an "
    "approval, or an external party before it can proceed?\n"
    "4. needs_choice: would carrying on require choosing between alternatives "
    "the message leaves open?\n\n"
    "Reply with ONLY a JSON object, no prose:\n"
    "{\"names_next_step\":true,\"asks_question\":false,"
    "\"awaits_human\":false,\"needs_choice\":false}"))

;; The message is data, not instruction, and it arrives from a transcript this
;; process does not control. It is fenced and labelled as the thing being judged
;; so that a transcript telling the model what to answer reads as part of the
;; specimen.
(define (classify-request message)
  (list (list "model" classify-model)
        (list "max_tokens" 200)
        (list "messages"
              (list (list (list "role" "user")
                          (list "content"
                                (string-append classify-prompt
                                               "\n\n--- message to label ---\n"
                                               message
                                               "\n--- end of message ---")))))))

;; The reply is an object, possibly with a stray sentence around it. Scanning
;; back for the closing brace is what makes a chatty model harmless here.
(define (last-brace text)
  (let loop ((i (string-length text)))
    (cond ((< i 1) #f)
          ((char=? (string-ref text i) #\}) i)
          (else (loop (- i 1))))))

(define (classify-answer response)
  (let ((parsed (json-parse (field-ref response 'body ""))))
    (if (error? parsed)
        parsed
        (let* ((message (field-ref parsed 'value))
               (content (field-ref message "content" #f)))
          (if (not (list? content))
              (list (list 'error "unexpected response shape") (list 'code 'malformed))
              (let* ((text (fold-left (lambda (acc block)
                                        (string-append acc (field-ref block "text" "")))
                                      "" content))
                     (start (string-index text "{"))
                     (stop (last-brace text)))
                (if (not (and start stop))
                    (list (list 'error "no object in the reply") (list 'code 'malformed))
                    (let ((object (json-parse (substring text start stop))))
                      (if (error? object) object (field-ref object 'value))))))))))

(define (classify-stall message)
  (let ((credential (synthesis-credential)))
    (if (not credential)
        (list (list 'error "no credential") (list 'code 'capability-missing))
        (let* ((body (field-ref (json-write (classify-request message)) 'text))
               (response (catch-errors
                           (lambda ()
                             (http-request
                               (list (list 'url classify-endpoint)
                                     (list 'method "POST")
                                     (list 'timeout-ms (classify-timeout-ms))
                                     (list 'headers
                                           (list (list (field-ref credential 'header)
                                                       (field-ref credential 'value))
                                                 (list "anthropic-version" "2023-06-01")
                                                 (list "content-type" "application/json")))
                                     (list 'body body)))))))
          (if (error? response) response (classify-answer response))))))

;; Any of the three guard answers stops it. They are separate questions rather
;; than one because 8 of 28 asks still slipped past a single question in the
;; measurement, and asking more of them costs one round trip either way.
(define (classify-blocked? answer)
  (or (eq? (field-ref answer "asks_question" #f) #t)
      (eq? (field-ref answer "awaits_human" #f) #t)
      (eq? (field-ref answer "needs_choice" #f) #t)))

;; What the decision actually consults. Returns the two signals and where they
;; came from, so a caller can record which path decided and a test can tell a
;; fallback from a classification.
(define (stall-signals message)
  (if (not (classify-enabled?))
      ;; Nobody asked for the network. The lists are what is left, and they are
      ;; poor; the cap is the real safeguard on this path.
      (list (list 'names-next-step (names-a-next-step? message))
            (list 'asks-question (asks-a-question? message))
            (list 'source "phrases"))
      (let ((answer (classify-stall message)))
        (if (or (error? answer) (not (list? answer)))
            ;; Unreachable, slow, or malformed. This does NOT fall back to the
            ;; phrase lists: continuing is the action with consequences, and
            ;; falling back would take it on the strength of a signal measured
            ;; at 0.15 precision, silently, exactly when something is already
            ;; wrong. Declining to continue costs an idle session; continuing
            ;; wrongly costs whatever the agent does next. Measured round trips
            ;; ran 1.0-2.5s against a 3s budget, so this path is reached in
            ;; normal operation and not only in a crisis.
            (list (list 'names-next-step #f)
                  (list 'asks-question #f)
                  (list 'source "unavailable"))
            (list (list 'names-next-step
                        (eq? (field-ref answer "names_next_step" #f) #t))
                  ;; The phrase list is unioned in rather than replaced. It fired
                  ;; on exactly one question in the corpus and was right about
                  ;; it, so it costs nothing to keep and can only add.
                  (list 'asks-question
                        (or (classify-blocked? answer) (asks-a-question? message)))
                  (list 'source "model"))))))
