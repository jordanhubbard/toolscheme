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
      (list 'gate-holds (and (field-ref good 'publish) (not (field-ref bad 'publish))))
      (list 'rejection-evidence (field-ref bad 'disagreement))
      (list 'good-disagreement (field-ref good 'disagreement)))
