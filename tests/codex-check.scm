;;; codex-check.scm -- when an idle Codex thread may be continued, and when not.
;;;
;;; This is more dangerous than the Claude Code path in [[continue]], because
;;; nothing is waiting on the answer: declining to stop leaves a session where
;;; it already was, while queueing a message starts work on a thread whose
;;; terminal nobody may be watching. So most of what is checked here is refusal.

(define summary
  "Landed the borrow checker slice and all tests pass. Full borrow task718 remains
   open. Next required slice: nested resource projections with place identity.")
(define question "I could take approach A or approach B here. Which would you prefer?")
(define both "Next step is the parser rewrite. Shall I proceed with that?")

;; What the first live session actually said when it stopped.
(define terse "Uppercased a.txt. Next: uppercase b.txt.")

(define idle 600)
(define cap 3)

;; A real rollout name. The thread id is read from it rather than from the file.
(define real-name
  "2026/09/18/rollout-2026-09-18T12-49-55-01a0b611-81a6-7183-ad6b-b81ba12f99f6.jsonl")

(define agent-line
  "{\"payload\":{\"type\":\"agent_message\",\"message\":\"Next step: ship it.\"}}")
(define user-line
  "{\"payload\":{\"type\":\"user_message\",\"message\":\"go on\"}}")
;; The other shape Codex writes a turn in. Missing this one would report every
;; session as mid-turn and continue nothing at all.
(define response-line
  "{\"payload\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"text\":\"done\"}]}}")
(define noise-line "not json at all")

;; Both records Codex writes for a single queued message. Counting them both
;; halves the cap silently, which a live run did before this was caught.
(define both-records-of-one-continuation
  (list (list (list 'text "{\"type\":\"response_item\",\"x\":\"toolscheme-continuation\"}"))
        (list (list 'text "{\"type\":\"event_msg\",\"x\":\"toolscheme-continuation\"}"))))

(define (speaker lines) (let ((s (codex-last-exchange lines))) (if s (car s) "none")))
(define (said lines) (let ((s (codex-last-exchange lines))) (if s (car (cdr s)) "")))

(define checks
  (list
    (list 'thread-read-from-the-name (codex-thread-of real-name))
    (list 'name-without-an-id-yields-nothing (codex-thread-of "rollout-short.jsonl"))
    (list 'last-speaker-wins (speaker (list agent-line user-line)))
    (list 'agent-last-is-seen (speaker (list user-line agent-line)))
    (list 'other-turn-shape-is-read (speaker (list response-line)))
    (list 'text-of-that-shape (said (list response-line)))
    (list 'unparsable-lines-are-skipped (speaker (list agent-line noise-line)))
    (list 'continues-the-terse-shape
          (if (codex-continue-decision "assistant" terse idle 0 cap) #t #f))
    (list 'continues-a-named-next-step
          (if (codex-continue-decision "assistant" summary idle 0 cap) #t #f))
    (list 'refuses-when-the-user-spoke-last
          (if (codex-continue-decision "user" summary idle 0 cap) #t #f))
    (list 'refuses-while-still-warm
          (if (codex-continue-decision "assistant" summary 10 0 cap) #t #f))
    (list 'refuses-a-thread-long-dead
          (if (codex-continue-decision "assistant" summary 999999 0 cap) #t #f))
    (list 'refuses-a-question
          (if (codex-continue-decision "assistant" question idle 0 cap) #t #f))
    (list 'refuses-a-question-that-also-names-a-step
          (if (codex-continue-decision "assistant" both idle 0 cap) #t #f))
    (list 'refuses-at-the-cap
          (if (codex-continue-decision "assistant" summary idle 3 cap) #t #f))
    (list 'one-continuation-counts-once
          (codex-count-marks both-records-of-one-continuation))
    (list 'the-injected-text-carries-its-own-mark
          (string-contains? (codex-continuation-text 0 3) codex-continuation-mark))
    (list 'off-unless-switched-on (not (codex-enabled?)))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (equal? (codex-thread-of real-name) "01a0b611-81a6-7183-ad6b-b81ba12f99f6")
                 (string-null? (codex-thread-of "rollout-short.jsonl"))
                 (equal? (speaker (list agent-line user-line)) "user")
                 (equal? (speaker (list user-line agent-line)) "assistant")
                 (equal? (speaker (list response-line)) "assistant")
                 (equal? (said (list response-line)) "done")
                 (equal? (speaker (list agent-line noise-line)) "assistant")
                 ;; The one case that should fire.
                 (string? (codex-continue-decision "assistant" summary idle 0 cap))
                 (string? (codex-continue-decision "assistant" terse idle 0 cap))
                 ;; Every case that must not.
                 (not (codex-continue-decision "user" summary idle 0 cap))
                 (not (codex-continue-decision "assistant" summary 10 0 cap))
                 (not (codex-continue-decision "assistant" summary 999999 0 cap))
                 (not (codex-continue-decision "assistant" question idle 0 cap))
                 (not (codex-continue-decision "assistant" both idle 0 cap))
                 (not (codex-continue-decision "assistant" summary idle 3 cap))
                 (= (codex-count-marks both-records-of-one-continuation) 1)
                 (string-contains? (codex-continuation-text 0 3) codex-continuation-mark)
                 (not (codex-enabled?)))))
