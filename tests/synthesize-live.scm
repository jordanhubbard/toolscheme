;;; synthesize-live.scm -- the whole loop, once, against the real corpus.
;;;
;;;   transcripts -> opportunity -> model -> replay gate -> published .scm file
;;;
;;; Needs ANTHROPIC_API_KEY. Nothing here decides a tool is good; the gate does,
;;; and it refuses on disagreement no matter how much cheaper the candidate is.
;;;
;;; Usage: toolscheme tests/synthesize-live.scm <transcript-dir> \
;;;          --lib lib --allow-process --allow-program curl

(define transcript-directory
  (if (null? command-arguments) ".claude/transcripts" (car command-arguments)))

(define report (analyze-sessions (list transcript-directory)))
(define chosen
  (let ((found (opportunities report)))
    (if (null? found) #f (car found))))

(define (attempt opportunity)
  (let ((written (synthesize opportunity)))
    (if (error? written)
        (list (list 'stage 'synthesis) (list 'outcome written))
        (let* ((tool (field-ref written 'tool))
               (source (field-ref tool "scheme_source" ""))
               (defined (catch-errors
                          (lambda () (eval (read-from-string source))))))
          (if (error? defined)
              ;; A model wrote code that does not even load. That is a result, not
              ;; an exception: report it and publish nothing.
              (list (list 'stage 'evaluation)
                    (list 'outcome defined)
                    (list 'source source))
              (let* ((name (field-ref tool "name" ""))
                     ;; Replay needs recorded invocations. Until the analyzer
                     ;; carries them per opportunity, the gate has nothing to
                     ;; compare against and must refuse rather than assume.
                     (cases (field-ref opportunity 'cases '()))
                     (verdict (if (null? cases)
                                  (list (list 'publish #f)
                                        (list 'reason "no recorded invocations to replay"))
                                  (replay name cases))))
                (if (field-ref verdict 'publish)
                    (begin (publish-tool-source tool-directory tool verdict)
                           (list (list 'stage 'published)
                                 (list 'tool name)
                                 (list 'verdict verdict)))
                    (list (list 'stage 'refused)
                          (list 'tool name)
                          (list 'verdict verdict)
                          (list 'source source)))))))))

(if (not chosen)
    (list (list 'error "no opportunity found in the corpus")
          (list 'code 'not-found)
          (list 'operation 'synthesize-live))
    (list (list 'opportunity chosen)
          (list 'cache-note "a second run should report cache-read-tokens above zero")
          (list 'result (attempt chosen))))
