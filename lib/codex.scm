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
;; The cheap guards are checked before the message is read at all, so a scan
;; over every thread on the machine costs one request per genuine candidate
;; rather than one per file.
(define (codex-continue-decision role message idle used cap)
  (cond
    ((not (equal? role "assistant")) #f)
    ((not (string? message)) #f)
    ((string-null? (string-trim message)) #f)
    ;; Still warm: the turn may simply be slow, and interrupting it is worse
    ;; than waiting.
    ((< idle (codex-idle-seconds)) #f)
    ((> idle (codex-stale-seconds)) #f)
    ((>= used cap) #f)
    (else (codex-message-decision message used cap))))

(define (codex-message-decision message used cap)
  (let ((signals (stall-signals message)))
    (cond
      ;; A question is the one case that genuinely wants a person.
      ((field-ref signals 'asks-question #f) #f)
      ((not (field-ref signals 'names-next-step #f)) #f)
      (else (codex-continuation-text used cap)))))

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

;; Counting continuations by grepping the rollout is what this used to do, and it
;; cannot work. A transcript is not a ledger: it records what was *delivered*, so
;; a failed queue leaves no trace and the count stays at zero forever; and Codex
;; compacts long threads, so even a delivered continuation is eventually rewritten
;; out of existence and the count falls back to zero. Both were observed -- 431
;; continuations on one thread against a cap of 2, and the only two surviving
;; marks on it sitting inside `"type":"compacted"` records.
;;
;; The observation log is the ledger: append-only, never rewritten, and already
;; the place both agents record a continuation. Kept for the tests and for
;; reading a transcript by hand; not for deciding anything.
(define (codex-marks path)
  (let ((hits (catch-errors (lambda () (grep codex-continuation-mark (list 'files path))))))
    (if (error? hits) 0 (codex-count-marks (field-ref hits 'matches '())))))

;; What the ledger says about one thread: how many continuations were delivered,
;; and how many were attempted. The second number is the backstop. A cap on
;; deliveries alone still permits an unbounded loop whenever delivery fails,
;; which is exactly the failure that happened -- so attempts are bounded too, and
;; a thread that cannot be reached is abandoned rather than retried forever.
(define codex-attempt-multiple 3)

(define (codex-attempt-cap cap) (* cap codex-attempt-multiple))

(define (codex-used-of counts thread)
  (let ((row (assoc thread counts)))
    (if row (cdr row) (list (list "sent" 0) (list "attempts" 0)))))

;; Reads the observation log, which means this runs rooted at the state
;; directory -- a different root from the scan, which is why the watcher is
;; staged rather than a single pass.
;;
;; The previous generation is read as well, because the log is append-only only
;; until it rotates. At 64MB it becomes observations.jsonl.1 and the current file
;; starts empty, so for a while after that every thread reads as never having
;; been continued -- and a live thread would be continued up to the cap again,
;; and an unreachable one retried up to the attempt bound again, every time the
;; log turned over. Choosing this file as the ledger and then not noticing it is
;; designed to be truncated would have put a smaller version of the same runaway
;; on a six-day timer.
;;
;; One generation back is enough and two would be waste: a thread idle longer
;; than `codex-stale-seconds` -- six hours by default -- is skipped by the scan
;; regardless, and a generation holds days.
;;
;; And it is only read when the current log does not already reach back that far,
;; which is exactly the window after a rotation. Reading it unconditionally cost
;; 1.7 seconds of every minute here, because the previous generation is 155MB --
;; a 3% duty cycle, permanently, to cover a few hours once every several days.
(define (codex-log-text path)
  (let ((r (catch-errors (lambda () (read-file path '((limit 268435456)))))))
    (if (error? r) "" (field-ref r 'text ""))))

;; The timestamp of the first record, or #f when there is nothing to read. Used
;; only to decide whether the file reaches back far enough.
(define (codex-log-starts-at lines)
  (let loop ((rest lines))
    (cond ((null? rest) #f)
          ((string-null? (car rest)) (loop (cdr rest)))
          (else
            (let ((parsed (catch-errors (lambda () (json-parse (car rest))))))
              (if (error? parsed)
                  (loop (cdr rest))
                  (let ((at (field-ref (field-ref parsed 'value) "at" #f)))
                    (if (number? at) at (loop (cdr rest))))))))))

(define (codex-used-counts)
  (let* ((current (hook-log-path))
         (own (field-ref (text-lines (codex-log-text current)) 'lines '()))
         (starts (codex-log-starts-at own))
         (reaches-back
           (and starts
                (<= starts (- (field-ref (time) 'epoch-milliseconds)
                              (* 1000 (codex-stale-seconds))))))
         (lines (if reaches-back
                    own
                    (append (field-ref (text-lines
                                         (codex-log-text (hook-generation-path current 1)))
                                       'lines '())
                            own))))
    (fold-left
      (lambda (counts line)
        (if (or (string-null? line)
                (not (string-contains? line "\"agent\":\"codex\""))
                (not (string-contains? line "continu")))
            counts
            (let ((parsed (catch-errors (lambda () (json-parse line)))))
              (if (error? parsed)
                  counts
                  (let* ((row (field-ref parsed 'value))
                         (event (field-ref row "event" ""))
                         (thread (field-ref row "session" "")))
                    (if (or (string-null? thread)
                            (not (or (equal? event "continued")
                                     (equal? event "continue-failed"))))
                        counts
                        (let* ((prior (codex-used-of counts thread))
                               (sent (+ (field-ref prior "sent" 0)
                                        (if (equal? event "continued") 1 0)))
                               (attempts (+ (field-ref prior "attempts" 0) 1)))
                          (cons (cons thread (list (list "sent" sent)
                                                   (list "attempts" attempts)))
                                (filter (lambda (r) (not (equal? (car r) thread)))
                                        counts)))))))))
      '() lines)))

(define (codex-tail path)
  (let ((read (catch-errors (lambda () (tail (list 'files path) '((count 40)))))))
    (if (error? read) '() (field-ref read 'lines '()))))

;; Delivered, not merely attempted. The exit status is the whole point of this
;; procedure and it used to be read from the wrong field -- `process-wait`
;; answers `exit-status`, this asked for `status` and always got the -1 default
;; -- while the caller tested only whether the *process* had errored. So a queue
;; that exited 1 saying "no rollout found for thread id" was reported as sent.
;;
;; That single misread field is what made the loop unbounded: nothing was
;; delivered, so no evidence of a continuation ever appeared, so the count of
;; continuations stayed at zero and the next tick tried again. 431 times against
;; a cap of 2, on one thread, over twelve hours.
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
        (list (list "thread" thread) (list "delivered" #f)
              (list "exit-status" -1) (list "detail" "could not start codex"))
        (let ((finished (catch-errors (lambda () (process-wait (field-ref started 'job))))))
          (if (error? finished)
              (list (list "thread" thread) (list "delivered" #f)
                    (list "exit-status" -1) (list "detail" "codex queue did not finish"))
              (let ((status (field-ref finished 'exit-status -1)))
                (list (list "thread" thread)
                      (list "delivered" (equal? status 0))
                      (list "exit-status" status)
                      (list "detail"
                            (clip (string-trim
                                    (if (equal? status 0)
                                        (field-ref finished 'stdout "")
                                        (field-ref finished 'stderr "")))
                                  400)))))))))

(define (codex-delivered? result)
  (and (not (error? result)) (field-ref result "delivered" #f) #t))

;; The same record the Claude Code path writes in [[continue]], so that one
;; question -- what did each continuation produce -- can be asked of both without
;; caring which agent it was. Without it a Codex continuation existed only in the
;; systemd journal, and experiments/continuation/did-it-work.scm, which reads the
;; observation log, could not see a single one: the feature ran and was
;; unmeasurable.
;;
;; The session field is the rollout thread id, which is exactly what the hook
;; already records as the session for a Codex tool call -- checked against a live
;; thread rather than assumed -- so a continuation joins to the work after it.
;;
;; This runs in a *second* pass, rooted at the state directory, because there is
;; one filesystem root per run and the scan needs it pointed at the rollouts.
;; Called from the scan instead, `hook-append` reported `(appended #t)` while
;; writing into ~/.codex/sessions/observations.jsonl -- the log path is relative,
;; so it silently followed whatever root it was given. Widening the scan's root
;; to $HOME would have fixed it by handing a timer-driven job with process
;; privileges the run of the home directory, which is the wrong trade.
;;
;; Wrapped, because a watcher on a timer must not fail over its own bookkeeping.
(define (codex-record! thread event continuation cap message detail)
  (catch-errors
    (lambda ()
      (hook-append
        (list (list "source" "toolscheme-hook")
              (list "event" event)
              (list "agent" "codex")
              (list "session" thread)
              (list "tool" "")
              (list "command" "")
              (list "continuation" continuation)
              (list "of" cap)
              (list "message" (clip message hook-command-limit))
              (list "detail" (clip detail 400))
              (list "at" (field-ref (time) 'epoch-milliseconds))
              (list "bytes" 0))))))

;; Records every attempt, delivered or not. Recording only successes is what
;; allowed the loop to run unbounded: a failure left nothing behind, so the next
;; tick saw a thread that had never been continued and tried again, forever.
;; Failures are the entries that stop it.
(define (codex-record-all! threads)
  (fold-left
    (lambda (n thread)
      (let ((queued (field-ref thread "queued" "")))
        (if (or (equal? queued "sent") (equal? queued "failed"))
            (begin (codex-record! (field-ref thread "thread" "")
                                  (if (equal? queued "sent") "continued" "continue-failed")
                                  (field-ref thread "continuation" 0)
                                  (field-ref thread "of" 0)
                                  (field-ref thread "message" "")
                                  (field-ref thread "detail" ""))
                   (+ n 1))
            n)))
    0 threads))

;; One pass over every thread on the machine. Returns what it looked at and what
;; it did, so a dry run reads the same as a live one minus the queueing.
(define (codex-scan act? counts)
  (let ((now (codex-now-seconds))
        (cap (continue-cap)))
    (fold-left
      (lambda (report path)
        (let ((thread (codex-thread-of path))
              (idle (codex-idle-of path now)))
          (if (or (string-null? thread) (< idle (codex-idle-seconds))
                  (> idle (codex-stale-seconds)))
              report
              (let* ((prior (codex-used-of counts thread))
                     (used (field-ref prior "sent" 0))
                     (attempts (field-ref prior "attempts" 0))
                     (spoken (codex-last-exchange (codex-tail path)))
                     (role (if spoken (car spoken) ""))
                     (message (if spoken (car (cdr spoken)) ""))
                     ;; The attempt bound is checked first and separately, so a
                     ;; thread that cannot be reached is abandoned rather than
                     ;; retried every minute until it ages out.
                     (decision (if (>= attempts (codex-attempt-cap cap))
                                   #f
                                   (codex-continue-decision role message idle used cap))))
                (if (not decision)
                    report
                    (cons (let ((sent (if act? (codex-queue! thread decision) #f)))
                            (list (list "thread" thread)
                                  (list "idle-seconds" idle)
                                  (list "continuation" (+ used 1))
                                  (list "of" cap)
                                  (list "attempts" (+ attempts 1))
                                  ;; Carried so the recording stage can write this
                                  ;; without re-reading the rollout it cannot see.
                                  (list "message" (clip decision hook-command-limit))
                                  (list "detail"
                                        (if (and act? (not (error? sent)))
                                            (field-ref sent "detail" "")
                                            ""))
                                  (list "queued"
                                        (cond ((not act?) "dry-run")
                                              ((codex-delivered? sent) "sent")
                                              (else "failed")))))
                          report)))))
        )
      '() (codex-rollout-paths))))
