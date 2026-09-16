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
                "{\"command\":\"grep -n x f.c | head -20\"}}\n"
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
          (list 'unsafe-line-refused (not (member "head -c" patterns))))))

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
      (list 'gate-holds (and (field-ref good 'publish)
                             (not (field-ref bad 'publish))
                             (field-ref safety 'safe-shape-offered)
                             (field-ref safety 'destructive-refused)
                             (field-ref safety 'unsafe-line-refused)))
      (list 'rejection-evidence (field-ref bad 'disagreement))
      (list 'good-disagreement (field-ref good 'disagreement)))
