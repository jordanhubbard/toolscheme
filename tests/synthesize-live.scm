;;; synthesize-live.scm -- the whole loop, once, against a real corpus.
;;;
;;;   transcripts -> opportunity -> model -> replay gate -> published .scm file
;;;
;;; Needs a credential (NVIDIA_INFERENCE_API_KEY or ANTHROPIC_API_KEY). Nothing
;;; here decides a tool is good; the gate does, and it refuses on disagreement no
;;; matter how much cheaper the candidate is.
;;;
;;; Only opportunities the analyzer marked replayable are offered, because replaying
;;; a recorded command means running it, and a corpus contains commands that must
;;; never be re-run on anyone's machine.
;;;
;;; Usage: toolscheme tests/synthesize-live.scm <transcript-dir> \
;;;          --lib lib --allow-process --allow-program curl ...

(define transcript-directory
  (if (null? command-arguments) ".claude/transcripts" (car command-arguments)))

;; An optional second argument names the pattern to work on. Without it the highest
;; ranked replayable opportunity is taken, which is the unattended behaviour; with
;; it a human can point the loop at a specific target they want tried.
(define wanted-pattern
  (if (or (null? command-arguments) (null? (cdr command-arguments)))
      #f
      (cadr command-arguments)))

(define report (analyze-sessions (list transcript-directory)))

(define chosen
  (let loop ((rest (field-ref report 'opportunities '())))
    (cond ((null? rest) #f)
          ((and (field-ref (car rest) 'replayable #f)
                (not (null? (field-ref (car rest) 'samples '())))
                (or (not wanted-pattern)
                    (string=? (field-ref (car rest) 'pattern "") wanted-pattern)))
           (car rest))
          (else (loop (cdr rest))))))

;; A model wrote this source. Evaluating it is the first thing that can go wrong,
;; and it is a result rather than an exception: report it and publish nothing.
(define (evaluated source)
  (catch-errors (lambda () (eval (read-from-string source)))))

;; A sample the model's translate procedure cannot turn into arguments is dropped
;; rather than guessed at. If that leaves nothing, the gate has no evidence and the
;; tool is refused -- which is the right answer, not an inconvenience.
(define (build-cases translate samples)
  (flatten
    (map (lambda (sample)
           (let* ((command (field-ref sample 'command ""))
                  (arguments (catch-errors (lambda () (translate command)))))
             (if (error? arguments)
                 '()
                 (list (list (list 'command command)
                             (list 'directory (field-ref sample 'directory ""))
                             (list 'arguments arguments))))))
         samples)))

(define (attempt opportunity)
  (let ((written (synthesize opportunity)))
    (if (error? written)
        (list (list 'stage 'synthesis) (list 'outcome written))
        (let* ((tool (field-ref written 'tool))
               (name (field-ref tool "name" ""))
               (defined (evaluated (field-ref tool "scheme_source" "")))
               (translate (evaluated (field-ref tool "translate_source" "")))
               (render (evaluated (field-ref tool "legacy_form_source" ""))))
          (cond
            ((error? defined)
             (list (list 'stage 'evaluation) (list 'tool name) (list 'outcome defined)
                   (list 'source (field-ref tool "scheme_source" ""))))
            ((or (error? translate) (error? render))
             (list (list 'stage 'harness)
                   (list 'tool name)
                   (list 'outcome (if (error? translate) translate render))))
            (else
              (let* ((cases (build-cases translate (field-ref opportunity 'samples '())))
                     (verdict (if (null? cases)
                                  (list (list 'publish #f)
                                        (list 'reason "no case survived translation"))
                                  (replay name cases render))))
                (if (field-ref verdict 'publish)
                    (begin (publish-tool-source tool-directory tool verdict)
                           (list (list 'stage 'published)
                                 (list 'tool name)
                                 (list 'cache-read-tokens (field-ref written 'cache-read-tokens 0))
                                 (list 'verdict verdict)))
                    (list (list 'stage 'refused)
                          (list 'tool name)
                          (list 'cache-read-tokens (field-ref written 'cache-read-tokens 0))
                          (list 'verdict verdict)
                          (list 'source (field-ref tool "scheme_source" "")))))))))))

(if (not chosen)
    (list (list 'error "no replayable opportunity in this corpus")
          (list 'code 'not-found)
          (list 'operation 'synthesize-live)
          (list 'note "fusion patterns are reported but have no shell command to replay against"))
    (list (list 'opportunity (list (list 'pattern (field-ref chosen 'pattern))
                                   (list 'occurrences (field-ref chosen 'occurrences))
                                   (list 'samples (field-ref chosen 'samples))))
          (list 'result (attempt chosen))))
