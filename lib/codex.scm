;;; codex.scm -- continuing a Codex session, which has no stop to decline.
;;;
;;; Claude Code fires a Stop hook and will honour a decision not to stop.
;;; Codex has no such event. Its hook list is PreToolUse, PermissionRequest,
;;; PostToolUse, PreCompact, PostCompact, SessionStart, SessionEnd,
;;; SubagentStart, SubagentStop and Interrupt -- none of which run when a turn
;;; ends. So the approach in [[continue]] has nothing to attach to here, and
;;; this is the larger half of the problem: of the 34.5 idle hours measured
;;; across 30 sessions, all of them were Codex.
;;;
;;; What Codex has instead is an app server. Start one on a socket, launch
;;; sessions against it with `--remote`, and `codex queue` hands a message to a
;;; session that has already finished its turn and is sitting idle. That was
;;; verified end to end before any of this was written:
;;;
;;;   [19:50:01] assistant: READY           <- turn over, session idle
;;;   [19:50:29] user: <queued message>     <- codex queue --thread ...
;;;   [19:50:31] assistant: BANANA7431      <- the idle session resumed
;;;
;;; Two costs come with that route, and neither is hidden. It reaches only
;;; sessions launched against the socket, because a plain `codex` keeps its
;;; core in-process where nothing outside can reach it -- so adoption is
;;; per-launch, not global. And with no hook to carry the decision, this polls
;;; the rollout files Codex already writes rather than being told.
;;;
;;; Everything in [[continue]] about why this is dangerous applies here and
;;; more so, because nothing is waiting on our answer: a mistake here starts
;;; work rather than declining to stop. It is off unless switched on.

(define codex-default-idle-seconds 90)
(define codex-default-stale-seconds 21600)

;; The mark rides along in the injected message, so a thread's own transcript
;; records how many times this has fired on it. Counting from the transcript
;; rather than from a side file means the bound cannot drift out of step with
;; reality, survives a restart, and is visible to anyone reading the session.
(define codex-continuation-mark "toolscheme-continuation")

(define (codex-enabled?) (setting-on? "TOOLSCHEME_CODEX_CONTINUE"))

(define (codex-setting-number name fallback)
  (let ((configured (setting name)))
    (if (string? configured)
        (let ((n (string->number configured)))
          (if (number? n) n fallback))
        fallback)))

(define (codex-idle-seconds) (codex-setting-number "TOOLSCHEME_CODEX_IDLE"
                                                   codex-default-idle-seconds))

;; A session idle since April is not stalled, it is over. Without this the scan
;; would queue work onto threads whose terminals closed months ago.
(define (codex-stale-seconds) (codex-setting-number "TOOLSCHEME_CODEX_STALE"
                                                    codex-default-stale-seconds))

(define (codex-socket)
  (let ((configured (setting "TOOLSCHEME_CODEX_SOCKET")))
    (if (and (string? configured) (not (string-null? configured))) configured "")))

;; The rollout file name ends with the thread id, which is exactly what `codex
;; queue --thread` wants. Taking it from the name rather than opening the file
;; keeps the scan cheap: rollouts here reach 4.8 MB and the scan sees every
;; session on the machine.
(define (codex-thread-of path)
  (let ((n (string-length path)))
    (if (and (> n 42) (string-suffix? ".jsonl" path))
        (let ((id (substring path (- n 41) (- n 6))))
          (if (= (string-length id) 36) id ""))
        "")))

;; One transcript line, reduced to who spoke and what they said. Codex writes
;; an agent turn two ways depending on the event stream, so both are read here;
;; missing one of them would silently report every session as mid-turn.
(define (codex-message-of record)
  (let* ((payload (field-ref record "payload" '()))
         (kind (field-ref payload "type" "")))
    (cond
      ((equal? kind "agent_message") (list "assistant" (field-ref payload "message" "")))
      ((equal? kind "user_message") (list "user" (field-ref payload "message" "")))
      ((equal? kind "message")
       (let* ((role (field-ref payload "role" ""))
              (content (field-ref payload "content" '()))
              (head (if (pair? content) (car content) '())))
         (list role (field-ref head "text" ""))))
      (else #f))))

;; The last thing said in the thread. A finished turn leaves the agent speaking
;; last; a thread that is mid-tool or streaming does not, and continuing either
;; would talk over work already in progress.
(define (codex-last-exchange lines)
  (fold-left
    (lambda (found line)
      (let ((parsed (catch-errors (lambda () (json-parse line)))))
        (if (or (error? parsed) (not parsed))
            found
            (let ((spoken (codex-message-of (field-ref parsed 'value))))
              (if spoken spoken found)))))
    #f lines))

(define (codex-continuation-text used cap)
  (string-append
    "Continuing without waiting: your last message named a next step and nothing "
    "in it needed a decision from anyone. Do that next step now. This is "
    "continuation " (number->string (+ used 1)) " of " (number->string cap)
    ". If the remaining work genuinely needs a person, say so plainly and stop. "
    "(" codex-continuation-mark ")"))

;; The whole decision, with nothing in it that touches the world, so the guards
;; can be tested without a live session. #f means leave the thread alone.
(define (codex-continue-decision role message idle used cap)
  (cond
    ((not (equal? role "assistant")) #f)
    ((not (string? message)) #f)
    ((string-null? (string-trim message)) #f)
    ;; Still warm: the turn may simply be slow, and interrupting it is worse
    ;; than waiting.
    ((< idle (codex-idle-seconds)) #f)
    ((> idle (codex-stale-seconds)) #f)
    ;; A question is the one case that genuinely wants a person.
    ((asks-a-question? message) #f)
    ((not (names-a-next-step? message)) #f)
    ((>= used cap) #f)
    (else (codex-continuation-text used cap))))

;;; ---------------------------------------------------------------------------
;;; The scan. Everything below touches the world.

;; Rooted at the Codex sessions directory, so the only thing this can read is
;; the transcripts it is there to read.
(define (codex-rollout-paths)
  (let ((found (catch-errors (lambda () (glob "**/rollout-*.jsonl" '((limit 500)))))))
    (if (error? found)
        '()
        (map (lambda (entry) (field-ref entry 'path ""))
             (field-ref found 'entries '())))))

(define (codex-now-seconds) (quotient (field-ref (time) 'epoch-milliseconds) 1000))

;; How long the thread has been quiet. Modification time is volatile and so is
;; withheld by default; here it is the whole signal, so it is asked for.
(define (codex-idle-of path now)
  (let ((info (catch-errors (lambda () (stat path '((volatile #t)))))))
    (if (error? info) -1 (- now (field-ref info 'modified now)))))

;; Counted over the whole transcript rather than the tail window, because the
;; bound has to hold across a long thread whose earlier continuations have
;; scrolled well out of sight.
;;
;; Codex records one queued message twice -- once as a `response_item` holding
;; the user turn, once as an `event_msg` announcing it -- so a plain count of
;; lines carrying the mark reads two for every one continuation, and a cap of
;; three would bind after one and a half. Measured, not guessed: the first live
;; run reported 2 marks after one continuation and 4 after two. Counting one
;; shape is what makes the bound mean what it says.
(define codex-user-turn-shape "\"type\":\"response_item\"")

(define (codex-count-marks matches)
  (count-if (lambda (match)
              (string-contains? (field-ref match 'text "") codex-user-turn-shape))
            matches))

(define (codex-marks path)
  (let ((hits (catch-errors (lambda () (grep codex-continuation-mark (list 'files path))))))
    (if (error? hits) 0 (codex-count-marks (field-ref hits 'matches '())))))

(define (codex-tail path)
  (let ((read (catch-errors (lambda () (tail (list 'files path) '((count 40)))))))
    (if (error? read) '() (field-ref read 'lines '()))))

(define (codex-queue! thread message)
  (let* ((socket (codex-socket))
         (started (catch-errors
                    (lambda ()
                      (process-start
                        (list (list 'program "codex")
                              (list 'arguments
                                    (list "queue"
                                          "--remote" (string-append "unix://" socket)
                                          "--thread" thread
                                          "--message" message))
                              (list 'timeout-ms 30000)))))))
    (if (error? started)
        started
        (let ((finished (catch-errors (lambda () (process-wait (field-ref started 'job))))))
          (if (error? finished)
              finished
              (list (list "thread" thread)
                    (list "status" (field-ref finished 'status -1))
                    (list "output" (string-trim (field-ref finished 'stdout "")))))))))

;; One pass over every thread on the machine. Returns what it looked at and what
;; it did, so a dry run reads the same as a live one minus the queueing.
(define (codex-scan act?)
  (let ((now (codex-now-seconds))
        (cap (continue-cap)))
    (fold-left
      (lambda (report path)
        (let ((thread (codex-thread-of path))
              (idle (codex-idle-of path now)))
          (if (or (string-null? thread) (< idle (codex-idle-seconds))
                  (> idle (codex-stale-seconds)))
              report
              (let* ((spoken (codex-last-exchange (codex-tail path)))
                     (role (if spoken (car spoken) ""))
                     (message (if spoken (car (cdr spoken)) ""))
                     (used (codex-marks path))
                     (decision (codex-continue-decision role message idle used cap)))
                (if (not decision)
                    report
                    (cons (list (list "thread" thread)
                                (list "idle-seconds" idle)
                                (list "continuation" (+ used 1))
                                (list "of" cap)
                                (list "queued"
                                      (if act?
                                          (let ((sent (codex-queue! thread decision)))
                                            (if (error? sent) "failed" "sent"))
                                          "dry-run")))
                          report)))))
        )
      '() (codex-rollout-paths))))
