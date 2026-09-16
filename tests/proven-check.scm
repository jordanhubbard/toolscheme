;;; proven-check.scm -- a tool may only claim what it can be shown to do.
;;;
;;; A published tool carries `shapes` (what it offers to replace), `cases` (the
;;; recorded invocations behind the offer) and `proven` (the verdict). The hook
;;; reads `proven` at rewrite time, because it cannot run a gate on every tool
;;; call -- so `proven` is a cached claim, and a cached claim nobody rechecks is
;;; just an assertion. This recomputes it from the cases every time.
;;;
;;; The bar here is stricter than the publication gate's. Publication asks whether
;;; two answers agree after normalizing whitespace. A redirect hands its bytes
;;; straight to the agent and its exit status straight to the shell, so both must
;;; match exactly. Everything found while building this was in that gap: a missing
;;; trailing newline, grep's path prefix appearing on single-file searches, and a
;;; pipeline's exit status being its last command's rather than grep's.

;; Timed over several runs because the interesting differences here are a few
;; milliseconds and the clock has millisecond resolution.
(define timing-runs 5)

(define (timed thunk)
  (let ((started (field-ref (time) 'epoch-milliseconds)))
    (let loop ((n timing-runs) (last #f))
      (if (= n 0)
          (list (list 'value last)
                (list 'ms (- (field-ref (time) 'epoch-milliseconds) started)))
          (loop (- n 1) (thunk))))))

(define (run-legacy command directory)
  (let ((started (sh (if (string-null? directory)
                         (list (list 'command command))
                         (list (list 'command command) (list 'directory directory))))))
    (if (error? started)
        #f
        (let ((finished (process-wait (field-ref started 'job))))
          (list (list 'out (field-ref finished 'stdout ""))
                (list 'status (field-ref finished 'exit-status -1)))))))

;; The candidate is run exactly as a redirect would run it -- as the rewritten
;; shell command, in a fresh process. Calling tool-render in this already-warm
;; interpreter instead would compare an in-process function call against a
;; subprocess spawn and report a speedup that does not exist: measured that way
;; this tool looked 5x faster than grep, and measured honestly it is 5x slower.
(define (run-candidate tool command directory)
  (run-legacy (redirect-command tool command) directory))

(define (compare tool case)
  (let* ((command (field-ref case 'command ""))
         (directory (field-ref case 'directory ""))
         (legacy-timed (timed (lambda () (run-legacy command directory))))
         (legacy (field-ref legacy-timed 'value))
         (ours-timed (timed (lambda () (run-candidate tool command directory))))
         (ours (field-ref ours-timed 'value)))
    (if (not legacy)
        (list (list 'command command) (list 'verdict 'skipped))
        (let* ((same-bytes (string=? (field-ref legacy 'out) (field-ref ours 'out)))
               (same-status (= (field-ref legacy 'status) (field-ref ours 'status)))
               (legacy-ms (field-ref legacy-timed 'ms))
               (our-ms (field-ref ours-timed 'ms))
               ;; Reproducing a command's bytes exactly means the only axis left to
               ;; win on is time. That is not a detail: it is the whole economics of
               ;; a transparent substitution, and it is why this check exists.
               (worthwhile (and same-bytes same-status (< our-ms legacy-ms))))
          (list (list 'command command)
                (list 'verdict (if (and same-bytes same-status) 'identical 'differs))
                (list 'shapes (command-shapes command))
                (list 'same-bytes same-bytes)
                (list 'same-status same-status)
                (list 'legacy-ms legacy-ms)
                (list 'our-ms our-ms)
                (list 'worthwhile worthwhile))))))

;; A shape is proven when every case exercising it matched exactly, and at least
;; one case exercised it. Silence is not evidence.
(define (shape-verdicts results shapes)
  (map (lambda (shape)
         (let* ((touching (filter (lambda (r) (member shape (field-ref r 'shapes '()))) results))
                (agreed (count-if (lambda (r) (eq? (field-ref r 'verdict) 'identical)) touching))
                (better (count-if (lambda (r) (field-ref r 'worthwhile #f)) touching)))
           (list (list 'shape shape)
                 (list 'cases (length touching))
                 (list 'agreed agreed)
                 (list 'worthwhile better))))
       shapes))

(define (audit tool)
  (let* ((name (field-ref tool 'name ""))
         (results (map (lambda (c) (compare name c)) (field-ref tool 'cases '())))
         (computed (shape-verdicts results (field-ref tool 'shapes '())))
         (declared (field-ref tool 'proven '()))
         ;; Overclaiming is the failure that matters: a shape declared proven that
         ;; the cases do not actually establish would be silently rewritten.
         (overclaimed
           (filter (lambda (d)
                     (let ((match (filter (lambda (c) (equal? (field-ref c 'shape)
                                                              (field-ref d 'shape)))
                                          computed)))
                       (or (null? match)
                           (= (field-ref (car match) 'cases 0) 0)
                           (not (= (field-ref (car match) 'agreed 0)
                                   (field-ref (car match) 'cases 0)))
                           ;; Correct but not better is still not grounds to
                           ;; silently substitute: the agent would pay for the
                           ;; swap and get nothing for it.
                           (not (= (field-ref (car match) 'worthwhile 0)
                                   (field-ref (car match) 'cases 0))))))
                   declared)))
    (list (list 'tool name)
          (list 'results results)
          (list 'computed computed)
          (list 'declared declared)
          (list 'overclaimed overclaimed))))

;; A gate that cannot catch an overclaim is not checking anything, so one is made
;; here on purpose: a tool whose case does not establish the shape it declares.
(define (overclaiming-audit)
  (audit (list (list 'name "search-read")
               (list 'shapes '("grep -n"))
               (list 'cases (list (list (list 'command "grep -n define-tool lib/prelude.scm")
                                        (list 'directory ""))))
               ;; `wc -l` is never exercised by that case.
               (list 'proven '(((shape "wc -l") (cases 1) (agreed 1)))))))

(define overclaim (overclaiming-audit))

(define audits
  (map audit (filter (lambda (t) (not (null? (field-ref t 'cases '()))))
                     (field-ref (tool-manifest) 'tools))))

;; Tools carry their cases in the manifest only if `cases` is exported; when it is
;; not, there is nothing to recheck and that itself is the finding.
(list (list 'audited (length audits))
      (list 'audits audits)
      (list 'overclaim-detected (not (null? (field-ref overclaim 'overclaimed))))
      (list 'checks-hold
            (and (> (length audits) 0)
                 ;; Every real tool backs what it declares...
                 (null? (flatten (map (lambda (a) (field-ref a 'overclaimed)) audits)))
                 ;; ...and a tool that does not is caught.
                 (not (null? (field-ref overclaim 'overclaimed))))))
