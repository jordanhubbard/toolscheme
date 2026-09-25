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

;; The NVIDIA inference gateway fronts many providers behind one token. It is
;; OpenAI-shaped on /v1/chat/completions, but it also speaks the Anthropic Messages
;; API natively on /v1/messages -- structured output, cache_control breakpoints and
;; all -- which is the shape this file already builds and parses. So only the host,
;; the auth header and the model name differ from talking to Anthropic directly,
;; and both are supported rather than one replacing the other.
;;
;; Which one is in use follows the credential, and all three have to move with it.
;; An Anthropic key sent to the gateway, under a model name only the gateway
;; understands, fails as a 401 that reads like a bad key. Before this, setting
;; only ANTHROPIC_API_KEY produced exactly that: the header followed the
;; credential and the other two did not, so the fallback path had never worked.
;; A variable set to the empty string is not a credential. Only #f is false here,
;; so an unset-but-exported key -- which is exactly what a shell wrapper produces
;; when its lookup finds nothing -- would otherwise read as present and route the
;; request to a host that has no key for it.
(define (credential-value name)
  (let ((found (setting name)))
    (if (and (string? found) (not (string-null? (string-trim found)))) (string-trim found) #f)))

(define (llm-via-gateway?) (if (credential-value "NVIDIA_INFERENCE_API_KEY") #t #f))

(define synthesis-endpoint
  (or (env-value "TOOLSCHEME_SYNTHESIS_ENDPOINT")
      (if (llm-via-gateway?)
          "https://inference-api.nvidia.com/v1/messages"
          "https://api.anthropic.com/v1/messages")))

;; Same model either way; the gateway prefixes its routes.
(define synthesis-model
  (or (env-value "TOOLSCHEME_SYNTHESIS_MODEL")
      (if (llm-via-gateway?) "azure/anthropic/claude-opus-5" "claude-opus-5")))

;; Credentials live in the environment, never in the repository. Which variable
;; supplies the key also decides how it is presented: the gateway takes a bearer
;; token, Anthropic directly takes x-api-key, and sending the wrong one is a 401
;; that looks like a bad key rather than a bad header.
;; `setting` rather than `env-value`, so a key can live in the config file beside
;; every other toolscheme setting. A hook is not started from a login shell and
;; inherits whatever the agent happened to be launched with, so requiring the
;; environment would mean the credential is present when a human runs the tool
;; and absent when the hook does -- working in every test and never in practice.
(define (synthesis-credential)
  (let ((gateway (credential-value "NVIDIA_INFERENCE_API_KEY"))
        (anthropic (credential-value "ANTHROPIC_API_KEY")))
    (cond (gateway (list (list 'key gateway) (list 'header "authorization")
                         (list 'value (string-append "Bearer " gateway))))
          (anthropic (list (list 'key anthropic) (list 'header "x-api-key")
                           (list 'value anthropic)))
          (else #f))))

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
                                                     (list "description" "Which fields are deterministic across identical calls.")))
                    ;; Without these two the gate has nothing to do: it cannot build
                    ;; a call to the new tool from a recorded command, and it cannot
                    ;; tell whether the structured answer matches the text one.
                    (list "translate_source"
                          (list (list "type" "string")
                                (list "description"
                                      "Scheme source for (lambda (command) arguments): given one recorded shell command string, return the argument record to call this tool with.")))
                    (list "legacy_form_source"
                          (list (list "type" "string")
                                (list "description"
                                      "Scheme source for (lambda (result) text): render this tool's result exactly as the command it replaces prints it, so the two can be compared.")))))
        (list "required" (list "name" "description" "scheme_source" "replaces_pattern"
                               "rationale" "stability_contract" "translate_source"
                               "legacy_form_source"))
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
    "\n\nUse only the primitives listed above. Anything not on that list does not\n"
    "exist, and a tool that calls it is rejected before it is ever replayed.\n"
    "Note that list-ref and string-ref are 1-based, only #f is false, and every\n"
    "tool takes exactly one argument: a record of (name value) fields.\n"
    ;; Written from what a real run got wrong. A weaker model follows the
    ;; contract and then reaches for Scheme it knows from elsewhere -- assoc,
    ;; a two-argument catch-errors, a read-file that returns a string -- and
    ;; every one of those fails the gate for a reason the model never sees.
    ;; Stating the idioms costs a few cached tokens and is the difference
    ;; between a candidate that can be judged and one that cannot run.
    "\nRead arguments and results with field-ref, never assoc or cadr:\n"
    "  (field-ref arguments \"path\" \"\")        ; missing reads as the default\n"
    "Primitives return records, not strings. read-file gives a record whose\n"
    "text is in the 'text field:\n"
    "  (field-ref (read-file path) 'text \"\")\n"
    "catch-errors takes one thunk and returns either the value or an error\n"
    "record; test it with error?:\n"
    "  (let ((r (catch-errors (lambda () (read-file path)))))\n"
    "    (if (error? r) (list (list \"error\" #t)) (list (list \"text\" ...))))\n"
    "\n\nReturn a complete define-tool form, for example:\n"
    "(define-tool (list (list 'name \"search_read\")\n"
    "                   (list 'description \"...\")\n"
    "                   (list 'parameters (list (list \"pattern\" \"string\" \"...\" #t)))\n"
    "                   (list 'procedure (lambda (arguments) ...))))\n"
    "\ntranslate_source is where candidates most often die. It receives one\n"
    "recorded command string and returns the argument record, or #f when that\n"
    "command is not one this tool can take -- a compound command, a pipeline,\n"
    "anything with a shape you did not plan for. Returning #f skips that case;\n"
    "raising kills the candidate. Write it to recognise the simple form and\n"
    "decline everything else:\n"
    "  (lambda (command)\n"
    "    (let ((parts (string-split (string-trim command) \" \")))\n"
    "      (if (and (= (length parts) 2) (string=? (list-ref parts 1) \"cat\"))\n"
    "          (list (list \"path\" (list-ref parts 2)))\n"
    "          #f)))\n"
    "\nThe tool will be proved against the command it replaces by replaying real\n"
    "recorded invocations, so it is not enough to write it: supply also\n"
    "translate_source, which turns one of those recorded command strings into the\n"
    "argument record for your tool, and legacy_form_source, which renders your\n"
    "result in exactly the text the old command printed. Equivalence is judged on\n"
    "that rendering. If they disagree on any recorded case the tool is refused,\n"
    "however much cheaper it is.\n"))

(define (synthesis-request opportunity . rest)
  ;; An optional previous failure. The candidate is thrown away and rewritten
  ;; rather than patched, but the model is told what went wrong -- otherwise the
  ;; same mistake is as likely the second time, and the commonest one by far is
  ;; a define-tool form that is one closing parenthesis short.
  (let ((failure (if (null? rest) "" (car rest))))
  (list
    (list "model" synthesis-model)
    ;; Three Scheme procedures in one structured object is a lot of output, and a
    ;; truncated response is not partial data -- it is invalid JSON, which surfaces
    ;; as a parse error that says nothing about the real cause.
    (list "max_tokens" 32000)
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
                              (write-to-string opportunity)
                              "\n\nThe `samples` are real recorded invocations; your\n"
                              "translate_source must handle them.\n"
                              (if (string-null? failure)
                                  ""
                                  (string-append
                                    "\nA previous attempt failed with:\n  " failure
                                    "\nWrite it again and avoid that. Count the closing\n"
                                    "parentheses of every form before returning.\n"))))))))))

(define (synthesize opportunity . rest)
  (let ((credential (synthesis-credential))
        (failure (if (null? rest) "" (car rest))))
    (if (not credential)
        (list (list 'error
                    "no synthesis credential: set NVIDIA_INFERENCE_API_KEY or ANTHROPIC_API_KEY")
              (list 'code 'capability-missing)
              (list 'operation 'synthesize))
        (let* ((body (field-ref (json-write (synthesis-request opportunity failure)) 'text))
               (response (http-request
                           (list (list 'url synthesis-endpoint)
                                 (list 'method "POST")
                                 (list 'timeout-ms 600000)
                                 (list 'headers
                                       (list (list (field-ref credential 'header)
                                                   (field-ref credential 'value))
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
            ;; Running out of output budget produces a valid HTTP response holding
            ;; half an object. Saying so beats "unexpected end of JSON input".
            ((string=? stop "max_tokens")
             (list (list 'error "the model ran out of output budget before finishing")
                   (list 'code 'truncated)
                   (list 'operation 'synthesize)
                   (list 'output-tokens (field-ref usage "output_tokens" 0))))
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
