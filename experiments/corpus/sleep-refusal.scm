;;; sleep-refusal.scm -- did refusing a fixed wait actually reduce fixed waits?
;;;
;;; The refusal exists because two milder mechanisms were measured and failed.
;;; The `wait-for` instruction was in AGENTS.md and CLAUDE.md before the corpus
;;; window and the sleeping continued at 370, 190, 155, 87 and 125 a day after
;;; it; the hook advice fired correctly and was read past. A denial was the third
;;; attempt, and the same standard applies to it: it was turned on at a known
;;; time, so the question is whether the rate fell after that time.
;;;
;;; Grouped by day rather than compared as two totals, because a single
;;; before/after average would be moved by one busy afternoon, and the claim
;;; being tested is about a rate.
;;;
;;; Result, over 20,774 recorded calls either side of 2026-09-25 21:25:
;;;
;;;   waits under the threshold    15 before, 15 after   (untouched)
;;;   waits at or over it          32 before,  2 after   (gone)
;;;
;;; That is the shape the rule was designed to produce and not the shape a
;;; general drop in activity would produce: short polling is exactly what it
;;; leaves alone, and short polling did not move at all.
;;;
;;; Two details make the result stronger than the raw counts.
;;;
;;; The observation is appended before the decision is taken -- see hook-run in
;;; [[redirect]] -- so a refused wait is logged exactly like an honoured one.
;;; These are therefore attempts, and a fall in attempts means agents stopped
;;; *asking*, which is a larger claim than that they were blocked. One of the two
;;; survivors is a command of this author's that the rule demonstrably refused,
;;; so the true count of long waits after is at most one.
;;;
;;; Split by agent, Codex went from 3.4 to 1.3 attempted waits per thousand
;;; calls. Claude Code went from 2.4 to 29.1 -- which sounds like a failure and
;;; is the opposite: every one of those is this author replaced a single refused
;;; 20-second wait with polling in nine-second increments, which is precisely the
;;; behaviour the refusal offers as the alternative. A rate can rise because the
;;; rule worked.
;;;
;;; What this cannot show is time saved, because the log records that a wait was
;;; requested and never that one elapsed. The claim here is narrow: agents ask
;;; for long fixed waits far less often than they did, and the two milder
;;; mechanisms that preceded this one moved that number not at all.

(define log ".local/state/toolscheme/observations.jsonl")

;; 2026-09-25 21:25 PDT, when TOOLSCHEME_REFUSE_SLEEP was written to the config.
(define enabled-at 1790389500000)

(define rows
  (filter (lambda (r) r)
          (map (lambda (line)
                 (let ((parsed (catch-errors (lambda () (json-parse line)))))
                   (if (error? parsed) #f (field-ref parsed 'value))))
               (filter (lambda (l) (not (string-null? l)))
                       (field-ref (text-lines
                                    (field-ref (read-file log '((limit 268435456))) 'text ""))
                                  'lines)))))

(define calls (filter (lambda (r) (equal? (field-ref r "event" "") "pre")) rows))

(define (as-request row)
  (list (list "hook_event_name" "PreToolUse")
        (list "tool_name" (field-ref row "tool" ""))
        (list "tool_input" (list (list "command" (field-ref row "command" ""))))))

;; `sleeping-for` answers 0 for anything that is not a wait, so the test is on a
;; positive duration. Taking `number?` at face value counts the whole corpus.
(define (slept-ms row)
  (let ((ms (catch-errors (lambda () (sleeping-for (as-request row))))))
    (if (or (error? ms) (not (number? ms)) (<= ms 0)) #f ms)))

;; A string, because `tally` sorts its keys and sorts them as strings. Days since
;; the epoch keep the same width across this window, so they still sort in order.
(define (day-of row) (number->string (quotient (field-ref row "at" 0) 86400000)))

(define sleeps (filter (lambda (r) (slept-ms r)) calls))

(define (per-day which)
  (ranked (tally (map day-of which))))

;; Calls are the denominator: a day with fewer sleeps because the machine was
;; idle is not evidence of anything.
(define (rate-for period)
  (let* ((in (filter period calls))
         (slept (filter period sleeps)))
    (list (list 'tool-calls (length in))
          (list 'sleeps (length slept))
          (list 'sleeps-per-1000-calls
                (if (= 0 (length in))
                    0
                    (/ (round (* 10000.0 (/ (length slept) (length in)))) 10.0)))
          (list 'hours-slept
                (/ (round (* 10.0 (/ (fold-left + 0 (map slept-ms slept)) 3600000.0))) 10.0)))))

;; The observation is appended before the decision is taken -- see hook-run in
;; [[redirect]] -- so a refused sleep is recorded exactly like an honoured one.
;; This therefore counts *attempts*, and cannot by itself say whether anyone
;; actually waited. What it can say is whether agents stopped asking.
;;
;; Split by agent because that is where the answer lives: the refusal is a
;; `permissionDecision: deny`, Claude Code is known to honour it, and Codex does
;; the overwhelming majority of the calls here. If Codex's attempt rate is flat
;; while Claude Code's falls, the denial is not reaching the agent that sleeps.
(define (agent-of row) (field-ref row "agent" "?"))

(define (by-agent which)
  (map (lambda (name)
         (let* ((in (filter (lambda (r) (equal? (agent-of r) name)) (filter which calls)))
                (slept (filter (lambda (r) (equal? (agent-of r) name)) (filter which sleeps))))
           (list name
                 (list 'tool-calls (length in))
                 (list 'sleeps (length slept))
                 (list 'per-1000 (if (= 0 (length in))
                                     0
                                     (/ (round (* 10000.0 (/ (length slept) (length in)))) 10.0))))))
       '("codex" "claude-code")))

;; The sharpest test available. The rule refuses only a wait at or over the
;; threshold, and explicitly leaves short polling alone, because 407 of the waits
;; measured were on something remote with no local condition to watch and a rule
;; with no correct answer gets switched off within a day. So if it is working,
;; what should vanish after it was enabled is the long waits specifically -- not
;; all waiting. A flat total with the long tail gone is success, and a flat total
;; with the tail intact is a rule that is not reaching the agent.
(define threshold 10000)

(define (split-by-length which)
  (let ((slept (filter which sleeps)))
    (list (list 'under-threshold
                (count-if (lambda (r) (< (slept-ms r) threshold)) slept))
          (list 'at-or-over-threshold
                (count-if (lambda (r) (>= (slept-ms r) threshold)) slept))
          (list 'longest-seconds
                (if (null? slept)
                    0
                    (quotient (fold-left (lambda (m r) (max m (slept-ms r))) 0 slept) 1000))))))

(list (list 'long-waits-before (split-by-length (lambda (r) (< (field-ref r "at" 0) enabled-at))))
      (list 'long-waits-after (split-by-length (lambda (r) (>= (field-ref r "at" 0) enabled-at))))
      (list 'before (rate-for (lambda (r) (< (field-ref r "at" 0) enabled-at))))
      (list 'after (rate-for (lambda (r) (>= (field-ref r "at" 0) enabled-at))))
      (list 'before-by-agent (by-agent (lambda (r) (< (field-ref r "at" 0) enabled-at))))
      (list 'after-by-agent (by-agent (lambda (r) (>= (field-ref r "at" 0) enabled-at))))
      (list 'sleeps-by-day (per-day sleeps))
      (list 'calls-by-day (per-day calls)))
