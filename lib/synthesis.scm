;;; synthesis.scm -- ask a model to write a replacement tool, in Scheme.
;;;
;;; The plan called for a dedicated LLM capability in C++. It is not needed: the
;;; HTTP capability is already the dumb transport that design wanted, so the whole
;;; synthesis loop is Scheme and can be edited without a rebuild.
;;;
;;; Two things here are deliberate. The request uses structured output, so the
;;; model returns a validated object rather than prose to be scraped for code. And
;;; the cache breakpoint sits after the stable prefix -- the primitive catalogue
;;; and the contract -- with the volatile opportunity report after it, so repeated
;;; synthesis reads the prefix from cache instead of re-paying for it.

(define synthesis-model "claude-opus-5")
(define synthesis-endpoint "https://api.anthropic.com/v1/messages")

(define (anthropic-key)
  (let ((found (env (list (list 'name "ANTHROPIC_API_KEY")))))
    (if (or (error? found) (null? (field-ref found 'variables '())))
        #f
        (cadr (car (field-ref found 'variables))))))

;; What a synthesized tool must look like. Constraining the shape is what makes
;; the result usable without a parser, and what lets the replay gate check it.
(define synthesis-schema
  (list (list "type" "object")
        (list "properties"
              (list (list "name" (list (list "type" "string")
                                       (list "description" "Tool name, lowercase with underscores.")))
                    (list "description" (list (list "type" "string")
                                              (list "description" "One sentence an agent reads to decide whether to call it.")))
                    (list "scheme_source" (list (list "type" "string")
                                                (list "description" "A complete define-tool form, ready to evaluate.")))
                    (list "replaces_pattern" (list (list "type" "string")
                                                   (list "description" "The shell command shape or call sequence this replaces.")))
                    (list "rationale" (list (list "type" "string")
                                            (list "description" "Why this is cheaper: fewer round trips, fewer bytes, or stabler output.")))
                    (list "stability_contract" (list (list "type" "string")
                                                     (list "description" "Which fields are deterministic across identical calls.")))))
        (list "required" (list "name" "description" "scheme_source" "replaces_pattern"
                               "rationale" "stability_contract"))
        (list "additionalProperties" #f)))

;; The stable half of the prompt: it does not change between opportunities, so it
;; is what the cache breakpoint protects.
(define (synthesis-brief)
  (string-append
    "You write tools for toolscheme, a Scheme toolbox embedded in coding agents.\n\n"
    "Contract:\n"
    "- Every tool returns a proper list of (name value) fields. Never text to be scraped.\n"
    "- Results must be stable: identical calls produce byte-identical output. Omit\n"
    "  volatile host metadata unless the caller asks for it.\n"
    "- Bound every result with a (limit N) option so no caller needs to pipe to head.\n"
    "- Text tools take inline data or a tagged source: '(files \"a\") or '(glob \"**/*.c\").\n"
    "- Reach the host only through existing primitives; never invent a capability.\n\n"
    "Available primitives:\n"
    (string-join (map symbol->string (primitive-names)) " ")
    "\n\nReturn a complete define-tool form, for example:\n"
    "(define-tool (list (list 'name \"search_read\")\n"
    "                   (list 'description \"...\")\n"
    "                   (list 'parameters (list (list \"pattern\" \"string\" \"...\" #t)))\n"
    "                   (list 'procedure (lambda (arguments) ...))))\n"))

(define (synthesis-request opportunity)
  (list
    (list "model" synthesis-model)
    (list "max_tokens" 16000)
    (list "system"
          (list (list (list "type" "text")
                      (list "text" (synthesis-brief))
                      ;; Everything before this point is identical between calls.
                      (list "cache_control" (list (list "type" "ephemeral"))))))
    (list "output_config"
          (list (list "format" (list (list "type" "json_schema")
                                     (list "schema" synthesis-schema)))))
    (list "messages"
          (list (list (list "role" "user")
                      (list "content"
                            (string-append
                              "Write one replacement tool for this measured pattern.\n\n"
                              (write-to-string opportunity))))))))

(define (synthesize opportunity)
  (let ((key (anthropic-key)))
    (if (not key)
        (list (list 'error "ANTHROPIC_API_KEY is not set")
              (list 'code 'capability-missing)
              (list 'operation 'synthesize))
        (let* ((body (field-ref (json-write (synthesis-request opportunity)) 'text))
               (response (http-request
                           (list (list 'url synthesis-endpoint)
                                 (list 'method "POST")
                                 (list 'timeout-ms 600000)
                                 (list 'headers (list (list "x-api-key" key)
                                                      (list "anthropic-version" "2023-06-01")
                                                      (list "content-type" "application/json")))
                                 (list 'body body)))))
          (if (error? response)
              response
              (synthesis-result response))))))

(define (synthesis-result response)
  (let ((parsed (json-parse (field-ref response 'body ""))))
    (if (error? parsed)
        parsed
        (let* ((message (field-ref parsed 'value))
               (stop (field-ref message "stop_reason" ""))
               (usage (field-ref message "usage" '())))
          (cond
            ;; A refusal is a successful HTTP response with empty content; reading
            ;; content[0] without checking this is how that becomes a crash.
            ((string=? stop "refusal")
             (list (list 'error "the model declined this request")
                   (list 'code 'refusal)
                   (list 'operation 'synthesize)
                   (list 'details (field-ref message "stop_details" '()))))
            ;; A missing field reads as #f, not as unspecified, so an unexpected
            ;; response shape has to be tested for what it is: not a list of blocks.
            ;; Folding over it instead raises, which would abort the loop rather
            ;; than report -- the one thing a structured surface must never do.
            ((not (list? (field-ref message "content" #f)))
             (list (list 'error (string-append "unexpected response: "
                                               (write-to-string message)))
                   (list 'code 'host-error)
                   (list 'operation 'synthesize)))
            (else
              (let* ((blocks (field-ref message "content"))
                     (text (fold-left (lambda (acc block)
                                        (if (equal? (field-ref block "type" "") "text")
                                            (string-append acc (field-ref block "text" ""))
                                            acc))
                                      "" blocks))
                     (tool (json-parse text)))
                (if (error? tool)
                    tool
                    (list (list 'tool (field-ref tool 'value))
                          (list 'cache-read-tokens (field-ref usage "cache_read_input_tokens" 0))
                          (list 'cache-created-tokens (field-ref usage "cache_creation_input_tokens" 0))
                          (list 'output-tokens (field-ref usage "output_tokens" 0)))))))))))
