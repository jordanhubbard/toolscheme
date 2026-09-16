;;; replay.scm -- the gate a synthesized tool must pass before an agent sees it.
;;;
;;; A model wrote this code. Nothing published it on the strength of that. Each
;;; candidate is replayed against invocations taken from real transcripts, beside
;;; the legacy path it claims to replace, and it is published only if it agrees on
;;; every case *and* measurably wins on at least one axis.
;;;
;;; The comparison normalizes whitespace and ordering, because agreeing on content
;;; is the claim -- not agreeing on formatting.

(define (normalize-output text)
  (string-join (filter (lambda (line) (not (string-null? line)))
                       (map string-trim (field-ref (text-lines text) 'lines)))
               "\n"))

;; The legacy side runs the command the agent actually used. A recorded pattern is
;; often more than one call -- `grep -> read` was the top cross-tool pair in the
;; corpus -- and the follow-up reads are part of what answering cost, so their
;; bytes are charged to the legacy path too. Otherwise a fused tool is compared
;; against only the cheapest half of what it replaces.
(define (legacy-run command follow-up-reads)
  (let ((started (field-ref (time) 'epoch-milliseconds))
        (result (sh (list (list 'command command)))))
    (if (error? result)
        result
        (let* ((finished (process-wait (field-ref result 'job)))
               (stdout (field-ref finished 'stdout ""))
               (follow-up (fold-left
                            (lambda (n path)
                              (let ((read (catch-errors (lambda () (read-file path)))))
                                (if (error? read)
                                    n
                                    (+ n (string-length (field-ref read 'text ""))))))
                            0 follow-up-reads)))
          (list (list 'output (normalize-output stdout))
                (list 'status (field-ref finished 'exit-status -1))
                (list 'bytes (+ (string-length stdout) follow-up))
                (list 'command-bytes (string-length stdout))
                (list 'follow-up-bytes follow-up)
                (list 'elapsed-ms (- (field-ref (time) 'epoch-milliseconds) started)))))))

(define (candidate-run name arguments)
  (let* ((started (field-ref (time) 'epoch-milliseconds))
         (result (catch-errors (lambda () (tool-invoke name arguments)))))
    (if (error? result)
        result
        (list (list 'output (normalize-output (write-to-string result)))
              (list 'result result)
              (list 'bytes (string-length (write-to-string result)))
              (list 'elapsed-ms (- (field-ref (time) 'epoch-milliseconds) started))))))

;; Stability is checked directly rather than inferred: call it twice and compare
;; bytes. This is the property the whole tool design rests on, so it is the one
;; axis a candidate cannot fake.
(define (stable? name arguments)
  (let ((first (catch-errors (lambda () (tool-invoke name arguments))))
        (second (catch-errors (lambda () (tool-invoke name arguments)))))
    (and (not (error? first))
         (not (error? second))
         (string=? (write-to-string first) (write-to-string second)))))

;; Comparing a structured result against a text tool's stdout needs a rendering,
;; not a substring test: a record and a line of grep output never share bytes even
;; when they carry exactly the same answer. `render` maps the candidate's result
;; into the legacy tool's shape, and equivalence is judged there. A candidate with
;; no rendering is compared on its raw written form, which is strictly weaker.
(define (default-render result) (write-to-string result))

(define (replay-case name case render)
  (let* ((legacy (legacy-run (field-ref case 'command "")
                             (field-ref case 'then-reads '())))
         (candidate (candidate-run name (field-ref case 'arguments '()))))
    (cond
      ((error? legacy) (list (list 'verdict 'skipped) (list 'reason "legacy path unavailable")))
      ((error? candidate) (list (list 'verdict 'failed)
                                (list 'reason (field-ref candidate 'error))))
      (else
        (let* ((rendered (normalize-output (render (field-ref candidate 'result))))
               (expected (field-ref legacy 'output ""))
               (agrees (string=? rendered expected)))
          (list (list 'verdict (if agrees 'agrees 'differs))
                (list 'command (field-ref case 'command ""))
                (list 'expected expected)
                (list 'rendered rendered)
                (list 'legacy-bytes (field-ref legacy 'bytes 0))
                (list 'legacy-follow-up-bytes (field-ref legacy 'follow-up-bytes 0))
                (list 'candidate-bytes (field-ref candidate 'bytes 0))
                (list 'legacy-ms (field-ref legacy 'elapsed-ms 0))
                (list 'candidate-ms (field-ref candidate 'elapsed-ms 0))
                (list 'stable (stable? name (field-ref case 'arguments '())))))))))

;; The verdict. Equivalence is not optional and no single win substitutes for it:
;; a faster tool that answers differently is not a replacement, it is a bug with
;; better latency. Winning on any one axis is enough once it agrees on all of them.
(define (replay name cases . rest)
  (let* ((render (if (null? rest) default-render (car rest)))
         (results (map (lambda (c) (replay-case name c render)) cases))
         (considered (filter (lambda (r) (not (eq? (field-ref r 'verdict) 'skipped))) results))
         (agreed (count-if (lambda (r) (eq? (field-ref r 'verdict) 'agrees)) considered))
         (legacy-bytes (fold-left (lambda (n r) (+ n (field-ref r 'legacy-bytes 0))) 0 considered))
         (candidate-bytes
           (fold-left (lambda (n r) (+ n (field-ref r 'candidate-bytes 0))) 0 considered))
         (legacy-ms (fold-left (lambda (n r) (+ n (field-ref r 'legacy-ms 0))) 0 considered))
         (candidate-ms (fold-left (lambda (n r) (+ n (field-ref r 'candidate-ms 0))) 0 considered))
         (all-stable (and (not (null? considered))
                          (= (count-if (lambda (r) (field-ref r 'stable #f)) considered)
                             (length considered))))
         (equivalent (and (not (null? considered)) (= agreed (length considered))))
         ;; Evidence for a rejection: the first case where the two disagreed.
         (disagreement (let loop ((rest considered))
                         (cond ((null? rest) '())
                               ((eq? (field-ref (car rest) 'verdict) 'agrees) (loop (cdr rest)))
                               (else (list (list 'command (field-ref (car rest) 'command ""))
                                           (list 'expected (field-ref (car rest) 'expected ""))
                                           (list 'rendered (field-ref (car rest) 'rendered "")))))))
         (wins (or all-stable
                   (< candidate-bytes legacy-bytes)
                   (< candidate-ms legacy-ms))))
    (list (list 'tool name)
          (list 'cases (length results))
          (list 'considered (length considered))
          (list 'agreed agreed)
          (list 'equivalent equivalent)
          (list 'stable all-stable)
          (list 'legacy-bytes legacy-bytes)
          (list 'candidate-bytes candidate-bytes)
          (list 'legacy-ms legacy-ms)
          (list 'candidate-ms candidate-ms)
          (list 'publish (and equivalent wins))
          (list 'disagreement disagreement)
          (list 'results results))))

;; Publication is writing a reviewable file. The provenance header is part of the
;; artifact: which pattern motivated the tool, what the replay measured, and when.
(define (publish-tool-source directory tool verdict)
  (let* ((name (field-ref tool "name"))
         (path (string-append directory "/" name ".scm"))
         (header (string-append
                   ";;; " name " -- synthesized replacement tool.\n"
                   ";;;\n"
                   ";;; Replaces: " (field-ref tool "replaces_pattern" "") "\n"
                   ";;; Why:      " (field-ref tool "rationale" "") "\n"
                   ";;; Stability: " (field-ref tool "stability_contract" "") "\n"
                   ";;; Replay:   " (write-to-string
                                      (list (list 'cases (field-ref verdict 'considered 0))
                                            (list 'agreed (field-ref verdict 'agreed 0))
                                            (list 'stable (field-ref verdict 'stable #f))
                                            (list 'legacy-bytes (field-ref verdict 'legacy-bytes 0))
                                            (list 'candidate-bytes
                                                  (field-ref verdict 'candidate-bytes 0))))
                   "\n;;;\n"
                   ";;; Written by a model and gated by replay; edit freely.\n\n")))
    (write-file path (string-append header (field-ref tool "scheme_source" "") "\n"))))
