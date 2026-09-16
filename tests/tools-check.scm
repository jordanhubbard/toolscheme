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

(list (list 'tools (length manifest))
      (list 'audit audit)
      (list 'checks-hold complete))
