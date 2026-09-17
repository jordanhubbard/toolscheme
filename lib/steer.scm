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

(define (steer-enabled?) (setting-on? "TOOLSCHEME_STEER"))

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

;; Each line is the invocation key and the call it belonged to. The call id is not
;; decoration: Claude Code invokes the hook twice for every tool call, so a key
;; recorded by the first invocation is already there when the second one looks, and
;; counting lines would report every single call as a repeat of itself. This was
;; caught by the advice firing on a call that had not been repeated.
(define (remember-key session key call)
  (let* ((path (session-key-path session))
         ;; The call comes first because it cannot contain a tab and the key can:
         ;; splitting on the first separator then always finds the right boundary.
         (line (string-append call "\t" key "\n"))
         (write-once (lambda () (write-file path line '((append #t))))))
    (if (error? (catch-errors write-once))
        (begin (catch-errors (lambda () (mkdir "sessions" '((parents #t)))))
               (catch-errors write-once))
        #t)))

(define (line-call line)
  (let ((cut (string-index line "\t")))
    (if cut (substring line 1 (- cut 1)) line)))

(define (line-key line)
  (let ((cut (string-index line "\t")))
    (if cut (substring line (+ cut 1) (string-length line)) "")))

;; Counts the *calls* that used this key, not the lines recording them, and never
;; counts the call now being made.
(define (count-key keys key current-call)
  (let loop ((rest keys) (seen '()) (n 0))
    (cond
      ((null? rest) n)
      ((or (not (string=? (line-key (car rest)) key))
           (string=? (line-call (car rest)) current-call)
           (member (line-call (car rest)) seen))
       (loop (cdr rest) seen n))
      (else (loop (cdr rest) (cons (line-call (car rest)) seen) (+ n 1))))))

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
        ;; A sleep the agent backgrounded is the work being simulated, not the agent
        ;; waiting for it: `(sleep 25; touch READY) &` returns at once. Counting it
        ;; turns every such setup into a false accusation -- which is what teaching
        ;; the tokenizer about subshells immediately caused.
        (let loop ((rest parsed))
          (cond ((null? rest) 0)
                ((field-ref (car rest) 'background #f) (loop (cdr rest)))
                ((string=? (field-ref (car rest) 'name "") "sleep")
                 (let ((seconds (string->number
                                  (let ((args (field-ref (car rest) 'arguments '())))
                                    (if (null? args) "0" (car args))))))
                   (if (number? seconds) (* 1000 seconds) 0)))
                (else (loop (cdr rest)))))))))

;; Seen this exact call already? Then this is the agent's second invocation of the
;; hook for one tool call, and whatever was worth saying was said on the first.
;; Without this the model receives the same sentence twice.
(define (already-seen? keys call)
  (and (not (string-null? call))
       (> (count-if (lambda (line) (string=? (line-call line) call)) keys) 0)))

;; The corpus showed 45-to-60 second sleeps because Codex's `sleep` tool takes a
;; duration. In a shell an agent polls instead -- `while [ ! -f x ]; do sleep 1;
;; done` -- and a threshold tuned to the corpus misses that completely. An
;; experiment run against this very advice caught it: thirty-six one-second sleeps
;; went by without a word, because none of them was ten seconds long.
(define loop-keywords '("while" "until" "for"))

(define (polling-loop? request)
  (let* ((command (hook-command-of (field-ref request "tool_input" '())))
         (parsed (filter (lambda (c) (not (field-ref c 'background #f)))
                         (field-ref (shell-parse command) 'commands '())))
         (names (map (lambda (c) (field-ref c 'name "")) parsed)))
    (and (member "sleep" names)
         ;; A loop keyword is grammar, so shell-parse drops it from the command
         ;; list; its presence has to be read from the text.
         (any? (lambda (word) (string-contains? command word)) loop-keywords)
         #t)))

(define (advice-for request keys call)
  (let ((slept (sleeping-for request))
        (key (steer-key request)))
    (cond
      ((already-seen? keys call) #f)
      ;; Said once per session: the point is made, and repeating it every call
      ;; would cost more context than the advice saves.
      ((and (>= slept steer-sleep-threshold-ms)
            (= (count-key keys "ADVISED-SLEEP" "") 0))
       (list (list 'kind "ADVISED-SLEEP")
             (list 'text
                   ;; Naming the tool is not enough: advice the agent cannot act on
                   ;; is noise in a context window, which is the thing this project
                   ;; exists to stop wasting. So it gives the command to run.
                   (string-append
                     "This waits a fixed " (number->string (quotient slept 1000))
                     "s whether or not the thing you are waiting for has happened. "
                     "`toolscheme` is on PATH and returns the moment it does:\n"
                     "  toolscheme -e '(wait-for (quote (exists \"some/path\")))'\n"
                     "  toolscheme -e '(wait-for (quote (matches \"some.log\" \"ready\")))'\n"
                     "It reports whether the condition was met or the deadline expired, "
                     "and takes (timeout-ms N)."))))
      ;; A polling loop is the same mistake at a finer grain, and worth saying once
      ;; per session for the same reason.
      ((and (polling-loop? request)
            (= (count-key keys "ADVISED-SLEEP" "") 0))
       (list (list 'kind "ADVISED-SLEEP")
             (list 'text
                   (string-append
                     "This polls in a loop. `toolscheme` is on PATH and blocks until the "
                     "condition holds, waking on the event rather than on a timer:
"
                     "  toolscheme -e '(wait-for (quote (exists \"some/path\")))'
"
                     "  toolscheme -e '(wait-for (quote (matches \"some.log\" \"ready\")))'
"
                     "It takes (timeout-ms N) and reports whether the condition held or "
                     "the deadline expired."))))
      ;; Said every time, because it names a specific call and stays true.
      ((> (count-key keys key call) 0)
       (list (list 'kind #f)
             (list 'text
                   (string-append
                     "You have already run this exact invocation "
                     (number->string (count-key keys key call))
                     (if (= (count-key keys key call) 1) " time" " times")
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
             (call (field-ref request "tool_use_id" ""))
             (keys (session-keys session))
             (found (advice-for request keys call))
             (key (steer-key request)))
        (remember-key session key call)
        (if (not found)
            #f
            (begin
              (if (field-ref found 'kind)
                  (remember-key session (field-ref found 'kind) call)
                  #f)
              (field-ref found 'text))))))


;; ---------------------------------------------------------------------------
;; Saying it once, before it matters
;; ---------------------------------------------------------------------------
;;
;; PreToolUse advice is reactive: it fires when the agent has already decided to
;; sleep for a minute, and since the call is not blocked, that minute is still
;; spent. Only the next one can be better. SessionStart is the same information
;; delivered before the decision, which costs one injection per session instead of
;; one per matching call.
;;
;; Kept deliberately short. This is charged to every session's context whether or
;; not it turns out to be relevant, so it earns its place by being four lines that
;; can be acted on directly, not a catalogue.
(define session-start-note
  (string-append
    "`toolscheme` is on PATH. When waiting for something to happen, it returns the "
    "moment it does rather than after a fixed delay:\n"
    "  toolscheme -e '(wait-for (quote (exists \"path/to/file\")))'\n"
    "  toolscheme -e '(wait-for (quote (matches \"some.log\" \"ready\")))'\n"
    "Both take (timeout-ms N) and report whether the condition held or the deadline "
    "expired. Prefer them to a fixed `sleep` when the wait has an observable end."))

(define (session-start-advice request)
  (let* ((session (field-ref request "session_id" ""))
         (keys (session-keys session)))
    (cond
      ((not (steer-enabled?)) #f)
      ;; An agent may start a session hook more than once, as it does for tools.
      ((> (count-key keys "SESSION-ADVICE" "") 0) #f)
      (else
        (remember-key session "SESSION-ADVICE" "session")
        session-start-note))))
