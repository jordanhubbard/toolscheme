;;; tools-check.scm -- registry completeness for the published tool library.
;;;
;;; The primitive registry is enforced this way already: a primitive with no
;;; worked example fails the build. A published tool carries more obligations, not
;;; fewer -- an agent chooses it from the manifest alone, and a model wrote it --
;;; so every field it needs to be chosen correctly is required here.

(define manifest (field-ref (tool-manifest) 'tools))

(define (missing tool)
  (filter (lambda (name)
            (let ((value (field-ref tool name #f)))
              (or (absent? value)
                  (and (string? value) (string-null? value))
                  (and (list? value) (null? value)))))
          '(name description parameters stability provenance)))

;; A parameter an agent cannot read a type or a purpose from is not documented.
(define (bad-parameters tool)
  (filter (lambda (parameter)
            (or (not (list? parameter))
                (< (length parameter) 3)
                (not (string? (caddr parameter)))
                (string-null? (caddr parameter))))
          (field-ref tool 'parameters '())))

(define audit
  (map (lambda (tool)
         (list (list 'tool (field-ref tool 'name ""))
               (list 'missing-fields (missing tool))
               (list 'undocumented-parameters (bad-parameters tool))
               ;; Provenance is the whole point of publishing a file: why this
               ;; tool exists, and what the replay measured.
               (list 'has-provenance (not (absent? (field-ref tool 'provenance #f))))))
       manifest))

(define complete
  (and (not (null? manifest))
       (null? (filter (lambda (row)
                        (or (not (null? (field-ref row 'missing-fields)))
                            (not (null? (field-ref row 'undocumented-parameters)))))
                      audit))))

;; A source written the way the tool's own description implies -- tagged, as it
;; would be in Scheme -- reaching the tool as a JSON string. Read as a literal
;; glob pattern it matches nothing, and the tool answers `(count 0)` with no
;; error, which the caller reads as "not in this codebase". A confident wrong
;; answer to a question nobody asked is the worst failure a tool can have, and
;; this one found it on its first real use over MCP, on its own author.
(define source-forms
  (list (list 'plain (normalize-source "*.cpp"))
        (list 'tagged-string (normalize-source "(glob \"*.cpp\")"))
        (list 'json-array (normalize-source (list "glob" "*.cpp")))
        (list 'scheme-tagged (normalize-source (list 'glob "*.cpp")))))

(define sources-agree
  (and (equal? (field-ref source-forms 'plain) (list 'glob "*.cpp"))
       (equal? (field-ref source-forms 'tagged-string) (list 'glob "*.cpp"))
       (equal? (field-ref source-forms 'json-array) (list 'glob "*.cpp"))
       (equal? (field-ref source-forms 'scheme-tagged) (list 'glob "*.cpp"))))

;; This audit sees whatever `tool-manifest` returns in the environment it runs
;; in, and `make` runs it with TOOLSCHEME_STATE= cleared -- so it sees the
;; repository's own lib/tools and not the state directory, which is where
;; synthesized tools are published and where the tools agents are actually
;; served come from. Two of those are live over MCP right now and neither
;; carries the structured `provenance` field asserted above; their evidence is
;; in a prose header, so the audit would have failed them had it looked.
;;
;; Named here rather than fixed here. Making this check read the live state
;; would make a unit test depend on a machine's history, which is worse; the
;; fix belongs in whatever publishes a tool, so that the field is written when
;; the tool is written. Recorded so that the gap is a known one.
(define audits-the-repository-only #t)

(list (list 'tools (length manifest))
      (list 'audit audit)
      (list 'source-forms source-forms)
      (list 'scope (if audits-the-repository-only 'repository 'live))
      (list 'checks-hold (and complete sources-agree)))
