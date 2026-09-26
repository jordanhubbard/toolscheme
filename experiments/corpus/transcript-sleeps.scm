;;; transcript-sleeps.scm -- where the fixed-wait figure actually comes from.
;;;
;;; The refusal in [[dogfood]] is justified by "1,168 sleeps, 11.9 hours". When
;;; shell-parse turned out to drop everything after a quoted word, the obvious
;;; worry was that a `sleep 60` sitting behind one had never been counted and
;;; the figure was low. It is not, and the reason is worth recording: in this
;;; corpus a fixed wait is almost never a shell command. `sleep` does not appear
;;; among the top 25 programs at all. It is Codex's own sleep and wait tools,
;;; read from JSON, which the tokenizer never touches.
;;;
;;; So the number behind the live refusal is uncontaminated. That is a finding
;;; about which measurements a parser bug can reach, and it was cheaper to check
;;; than to assume in either direction.

(define report (analyze-sessions (list ".codex/sessions")))

(define (tool-calls name)
  (let loop ((rest (field-ref report 'tools '())))
    (cond ((null? rest) 0)
          ((equal? (field-ref (car rest) 'tool "") name) (field-ref (car rest) 'calls 0))
          (else (loop (cdr rest))))))

;; Whether `sleep` reached the program table at all is the whole question; if it
;; did not, no amount of tokenizer fixing moves the figure.
(define shell-sleeps
  (let loop ((rest (field-ref report 'programs '())))
    (cond ((null? rest) 0)
          ((equal? (field-ref (car rest) 'program "") "sleep") (field-ref (car rest) 'calls 0))
          (else (loop (cdr rest))))))

(list (list 'shell-calls (field-ref report 'shell-calls 0))
      (list 'programs-listed (length (field-ref report 'programs '())))
      (list 'sleep-as-a-shell-program shell-sleeps)
      (list 'sleep-tool (tool-calls "sleep"))
      (list 'wait-tool (tool-calls "wait"))
      (list 'fixed-waits-total (+ (tool-calls "sleep") (tool-calls "wait") shell-sleeps)))
