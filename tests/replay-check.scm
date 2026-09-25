;;; replay-check.scm -- proves the publication gate actually gates.
;;;
;;; Two candidates answer the same question: the real fused tool, and one that
;;; silently drops half its matches. The gate must publish the first and refuse
;;; the second. A gate that cannot reject is not a gate.

(define cases
  (list
    ;; The legacy pattern as the corpus recorded it: locate with grep, then read
    ;; the files that matched. Both halves are charged to the legacy side.
    (list (list 'command "grep -n define-tool lib/*.scm | head -20")
          (list 'then-reads '("lib/prelude.scm" "lib/synthesis.scm"))
          (list 'arguments '((pattern "define-tool") (source (glob "lib/*.scm"))
                             (limit 20) (context 2))))
    (list (list 'command "grep -n tally lib/*.scm | head -20")
          (list 'then-reads '("lib/prelude.scm" "lib/analysis.scm"))
          (list 'arguments '((pattern "tally") (source (glob "lib/*.scm"))
                             (limit 20) (context 2))))))

;; A candidate that looks right, returns a well-formed structured result, is
;; stable, and is cheaper -- and is wrong.
(define (search-read-lossy request)
  (let* ((full (search-read request))
         (halved (map (lambda (file)
                        (list (list 'path (field-ref file 'path))
                              (list 'matches
                                    (let loop ((rest (field-ref file 'matches)) (keep #t) (out '()))
                                      (cond ((null? rest) (reverse out))
                                            (keep (loop (cdr rest) #f (cons (car rest) out)))
                                            (else (loop (cdr rest) #t out)))))
                              (list 'regions (field-ref file 'regions))))
                      (field-ref full 'files))))
    (list (list 'files halved)
          (list 'count (field-ref full 'count))
          (list 'truncated (field-ref full 'truncated #f))
          (list 'files-scanned (field-ref full 'files-scanned 0)))))

(define-tool (list (list 'name "search-read-lossy")
                   (list 'description "deliberately broken candidate")
                   (list 'procedure search-read-lossy)))

;; Replaying means running. What the gate is allowed to execute is decided before
;; any of this, by which samples the analyzer is willing to offer, so that rule is
;; checked here rather than trusted: the shape being safe is not enough when the
;; recorded line around it is not.
(define safety
  (let* ((log (string-append
                "{\"type\":\"tool_use\",\"tool_name\":\"bash\",\"tool_input\":"
                ;; A plain command, because a pipeline is no longer offered as
                ;; evidence: a single-purpose tool cannot reproduce what a shell
                ;; program prints, and offering one only wastes a synthesis round
                ;; on a candidate the gate is certain to refuse.
                "{\"command\":\"grep -n x f.c\"}}\n"
                "{\"type\":\"tool_use\",\"tool_name\":\"bash\",\"tool_input\":"
                "{\"command\":\"rm -rf /tmp/scratch\"}}\n"
                "{\"type\":\"tool_use\",\"tool_name\":\"bash\",\"tool_input\":"
                "{\"command\":\"cd /repo && ./deploy.sh | head -c 100\"}}\n"))
         (found (opportunities (analyze-events (field-ref (agent-log-events log) 'events))))
         (offered (filter (lambda (o) (field-ref o 'replayable #f)) found))
         (patterns (map (lambda (o) (field-ref o 'pattern)) offered)))
    (list (list 'offered patterns)
          (list 'safe-shape-offered (and (member "grep -n" patterns) #t))
          (list 'destructive-refused (not (member "rm -rf" patterns)))
          ;; `head -c` is a safe program, but the line it appeared in ran a deploy
          ;; script. The shape must not launder the command around it.
          (list 'unsafe-line-refused (not (member "head -c" patterns)))
          )))


;; A compound line is not evidence even when every program in it is safe. This is
;; what four consecutive synthesis runs died on: `f=path; grep ... $f` was offered
;; as a sample of the "grep" shape, and no single-purpose tool can stand in for a
;; shell program, so the gate refused every case.
(define compound-log
  (string-append
    "{\"type\":\"tool_use\",\"tool_name\":\"bash\",\"tool_input\":"
    "{\"command\":\"f=/tmp/o; cat $f\"}}\n"))

(define compound-offered
  (map (lambda (o) (field-ref o 'pattern))
       (filter (lambda (o) (field-ref o 'replayable #f))
               (opportunities
                 (analyze-events (field-ref (agent-log-events compound-log) 'events))))))


;; A tool that cannot fail the way the command fails.
;;
;; This is the flaw that got past the gate and into a live rewrite: `cat` on a
;; missing file exits 1 and prints nothing; the published tool returned a record
;; with empty text and exited 0. Byte-for-byte identical output, opposite
;; meanings, and the agent had no way to tell it had been told a file was empty
;; rather than absent. Equivalence now includes agreeing about failure.
(define (swallows-errors arguments)
  (let ((r (catch-errors (lambda () (read-file (field-ref arguments "path" ""))))))
    ;; The mistake in miniature: a default standing in for an error.
    (list (list "text" (if (error? r) "" (field-ref r 'text ""))))))

(define-tool
  (list (list 'name "swallowing-reader")
        (list 'description "reports success whatever happened")
        (list 'shapes '("cat"))
        (list 'proven '(((shape "cat") (cases 1) (agreed 1) (worthwhile 1))))
        (list 'translate "swallowing-translate")
        (list 'legacy-form "swallowing-render")
        (list 'procedure swallows-errors)))

(define (swallowing-translate command)
  (list (list "path" (car (cdr (string-split command " "))))))
(define (swallowing-render result) (field-ref result "text" ""))

;; Replayed against a file that does not exist: cat fails, the tool does not.
(define missing-case
  (list (list (list 'command "cat definitely-absent-file.txt")
              (list 'directory "")
              (list 'arguments (list (list "path" "definitely-absent-file.txt"))))))

(define failure-verdict (replay "swallowing-reader" missing-case swallowing-render))


;; `sed` cannot be allowlisted wholesale -- -i rewrites files in place -- but the
;; bounded line read is the largest substitutable shape in the corpus, at twice
;; the volume of cat. It is recognised narrowly, and the narrowness is the point.
(define sed-safety
  (list (list 'range-read (read-only-sed? "sed -n '120,180p' f.c"))
        (list 'single-line (read-only-sed? "sed -n '42p' f.c"))
        (list 'unquoted (read-only-sed? "sed -n 1,90p f.c"))
        (list 'in-place-refused (not (read-only-sed? "sed -i 's/a/b/' f.c")))
        (list 'substitution-refused (not (read-only-sed? "sed -n 's/a/b/p' f.c")))
        (list 'write-command-refused (not (read-only-sed? "sed -n '1,5w /tmp/o' f.c")))
        (list 'offered-to-replay (replayable-command? "sed -n '1,90p' f.c"))
        (list 'sed-i-still-refused (not (replayable-command? "sed -i 's/a/b/' f.c")))))

(define good (replay "search-read" cases search-read->grep))
(define bad (replay "search-read-lossy" cases search-read->grep))

(define (report label verdict)
  (list (list 'candidate label)
        (list 'publish (field-ref verdict 'publish))
        (list 'equivalent (field-ref verdict 'equivalent))
        (list 'stable (field-ref verdict 'stable))
        (list 'agreed (field-ref verdict 'agreed))
        (list 'considered (field-ref verdict 'considered))
        (list 'legacy-bytes (field-ref verdict 'legacy-bytes))
        (list 'candidate-bytes (field-ref verdict 'candidate-bytes))))

;; The gate is proven by both outcomes, not one: the real tool must publish and
;; the broken one must not.
(list (list 'good (report "search-read" good))
      (list 'bad (report "search-read-lossy" bad))
      (list 'safety safety)
      (list 'sed-safety sed-safety)
      (list 'failure-disagreement-caught (not (field-ref failure-verdict 'publish)))
      (list 'compound-line-not-offered (null? compound-offered))
      (list 'gate-holds (and (field-ref good 'publish)
                             (not (field-ref bad 'publish))
                             (field-ref safety 'safe-shape-offered)
                             (field-ref safety 'destructive-refused)
                             (field-ref safety 'unsafe-line-refused)
                             (null? compound-offered)
                             ;; A tool that cannot fail correctly is refused.
                             (not (field-ref failure-verdict 'publish))
                             (field-ref sed-safety 'range-read)
                             (field-ref sed-safety 'in-place-refused)
                             (field-ref sed-safety 'substitution-refused)
                             (field-ref sed-safety 'write-command-refused)
                             (field-ref sed-safety 'sed-i-still-refused)))
      (list 'rejection-evidence (field-ref bad 'disagreement))
      (list 'good-disagreement (field-ref good 'disagreement)))
