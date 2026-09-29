;;; tool-layer.scm -- is the tool-replacement thesis supported by the corpus?
;;;
;;; This project's charter is that agents spend their budget on programs designed
;;; for a different era, and that better-shaped tools would cost less. That is a
;;; claim about a corpus, and the corpus is now large enough to test it: 17,526
;;; shell calls and 31MB of tool output from live sessions on this machine.
;;;
;;; Run against $HOME.
;;;
;;; The answer, in short: for the workload measured here, the tool layer is not
;;; supported and the instrument is. Every number below is produced by this
;;; script; the argument is in docs/relevance.md.

(define log ".local/state/toolscheme/observations.jsonl")

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
(define finished
  (filter (lambda (r) (and (equal? (field-ref r "event" "") "post")
                           (not (string-null? (field-ref r "command" "")))))
          rows))

;; --- where the bytes actually go -------------------------------------------
;;
;; The premise says search and read dominate. They do -- but through `bash`,
;; not through the agent's own Read and Grep tools, which is what the published
;; MCP tools compete with.

(define (bytes-of tool)
  (fold-left (lambda (n r) (+ n (field-ref r "bytes" 0)))
             0
             (filter (lambda (r) (equal? (field-ref r "tool" "") tool))
                     (filter (lambda (r) (equal? (field-ref r "event" "") "post")) rows))))

(define (first-substantive text)
  (let ((parsed (catch-errors (lambda () (shell-parse text)))))
    (if (error? parsed)
        "?"
        (let loop ((rest (field-ref parsed 'programs '())))
          (cond ((null? rest) "?")
                ((member (car rest) dogfood-transparent) (loop (cdr rest)))
                (else (car rest)))))))

(define by-program
  (let ((totals '()))
    (for-each
      (lambda (r)
        (let* ((key (first-substantive (field-ref r "command" "")))
               (prior (assoc key totals)))
          (set! totals (cons (cons key (+ (if prior (cdr prior) 0) (field-ref r "bytes" 0)))
                             (filter (lambda (x) (not (equal? (car x) key))) totals)))))
      finished)
    (take (list-sort totals (lambda (a b) (> (cdr a) (cdr b)))) 8)))

;; --- can a per-call tool beat a shell that composes? ------------------------
;;
;; This is the finding that explains the other four. A Bash call is a program in
;; a language: `a && b | c` is one call. An MCP tool is one operation. To match
;; the average composed call takes 1.8 tool calls, and the worst takes 12.

(define (substantive-count text)
  (let ((parsed (catch-errors (lambda () (shell-parse text)))))
    (if (error? parsed)
        0
        (length (filter (lambda (p) (not (member p dogfood-transparent)))
                        (field-ref parsed 'programs '()))))))

(define composition (map (lambda (r) (substantive-count (field-ref r "command" "")))
                         (filter (lambda (r) (not (string-null? (field-ref r "command" ""))))
                                 calls)))

;; --- the specific tool that exists, against the command it replaces ---------
;;
;; `read_line_range` does exactly what `sed -n '10,40p'` does, and that is the
;; single largest source of output bytes in the corpus. It still loses: the
;; reads arrive batched, 1.7 to a call and up to 8, so one range per call is
;; more calls, not fewer -- and the bytes are the same lines either way.

(define (occurrences text needle)
  (let ((n (string-length needle)))
    (let loop ((i 1) (total 0))
      (cond ((> i (- (string-length text) (- n 1))) total)
            ((equal? (substring text i (+ i (- n 1))) needle) (loop (+ i n) (+ total 1)))
            (else (loop (+ i 1) total))))))

(define range-read-calls
  (filter (lambda (c) (string-contains? c "sed -n"))
          (map (lambda (r) (field-ref r "command" "")) calls)))

(define ranges-per-call (map (lambda (c) (occurrences c "sed -n")) range-read-calls))

;; --- was there a flood of unbounded output to tame? -------------------------
;;
;; The charter says `| head -N` is a workaround for tools that cannot bound
;; themselves. If so, the unbounded calls should be the expensive ones. They are
;; the cheap ones.

(define (bounded? c)
  (or (string-contains? c "head ") (string-contains? c "head -")
      (string-contains? c "tail ") (string-contains? c "sed -n")
      (string-contains? c "-m ") (string-contains? c "--max")
      (string-contains? c "| wc") (string-contains? c "-c ")))

(define (spend which)
  (let ((rs (filter (lambda (r) (which (field-ref r "command" ""))) finished)))
    (list (list 'calls (length rs))
          (list 'bytes (fold-left (lambda (n r) (+ n (field-ref r "bytes" 0))) 0 rs))
          (list 'mean (if (null? rs)
                          0
                          (quotient (fold-left (lambda (n r) (+ n (field-ref r "bytes" 0))) 0 rs)
                                    (length rs)))))))

(list
  (list 'shell-calls (length calls))
  (list 'bytes-by-agent-tool
        (map (lambda (t) (list t (bytes-of t))) '("Bash" "Edit" "Read" "Grep")))
  (list 'bytes-by-program by-program)
  (list 'composition
        (list (list 'calls (length composition))
              (list 'composed (count-if (lambda (n) (> n 1)) composition))
              (list 'percent (/ (round (* 1000.0 (/ (count-if (lambda (n) (> n 1)) composition)
                                                    (length composition))))
                                10.0))
              (list 'mean (/ (round (* 10.0 (/ (fold-left + 0 composition)
                                               (length composition))))
                             10.0))
              (list 'most (fold-left max 0 composition))))
  (list 'range-reads
        (list (list 'calls (length range-read-calls))
              (list 'reads (fold-left + 0 ranges-per-call))
              (list 'mean-per-call (/ (round (* 10.0 (/ (fold-left + 0 ranges-per-call)
                                                        (max 1 (length range-read-calls)))))
                                      10.0))
              (list 'most-in-one (fold-left max 0 ranges-per-call))))
  (list 'bounded (spend bounded?))
  (list 'unbounded (spend (lambda (c) (not (bounded? c))))))
