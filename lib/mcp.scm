;;; mcp.scm -- the Model Context Protocol server, in Scheme.
;;;
;;; The binary only moves lines of JSON on stdio; the protocol lives here, so it
;;; can be changed without a rebuild. The task-shaped inspector and evaluator
;;; are the supported MCP tools.

(define mcp-protocol-version "2025-06-18")

(define (json-of value) (field-ref (json-write value) 'text))

(define (mcp-envelope id payload)
  (json-of (list (list "jsonrpc" "2.0") (list "id" id) (list "result" payload))))

(define (mcp-error id code message)
  (json-of (list (list "jsonrpc" "2.0")
                 (list "id" id)
                 (list "error" (list (list "code" code) (list "message" message))))))

(define (mcp-internal-error message)
  (mcp-error 'null -32603 (string-append "internal error: " message)))

;; MCP tool results are content blocks. Toolscheme results are association lists,
;; so they are handed over as canonical Scheme source: it is what the agent will
;; read back, and it round-trips exactly.
(define (mcp-content text)
  (list (list "content" (list (list (list "type" "text") (list "text" text))))))

(define (mcp-failure-content text)
  (list (list "content" (list (list (list "type" "text") (list "text" text))))
        (list "isError" #t)))

(define (mcp-builtin-tools)
  (list
    (list (list "name" "toolscheme_inspect")
          (list "description"
                "Find a pattern in repository files and return matching lines with nearby context, paths, and line numbers in one bounded call. The result reports truncation. Use for locating code before opening whole files.")
          (list "inputSchema"
                (list (list "type" "object")
                      (list "properties"
                            (list (list "pattern" (list (list "type" "string")))
                                  (list "roots" (list (list "type" "array")
                                                      (list "items" (list (list "type" "string")))))
                                  (list "include" (list (list "type" "array")
                                                        (list "items" (list (list "type" "string")))))
                                  (list "context" (list (list "type" "integer")
                                                        (list "minimum" 0) (list "maximum" 20)))
                                  (list "limit" (list (list "type" "integer")
                                                      (list "minimum" 1) (list "maximum" 50)))))
                      (list "required" (list "pattern")))))
    (list (list "name" "toolscheme_eval")
          (list "description"
                (string-append
                  "Evaluate a toolscheme expression and return its canonical result. "
                  "Results are association lists, stable across identical calls. "
                  "Text tools accept inline data or a tagged source: "
                  "(grep \"needle\" '(glob \"src/**/*.c\") '((limit 20)))."))
          (list "inputSchema"
                (list (list "type" "object")
                      (list "properties"
                            (list (list "expression"
                                        (list (list "type" "string")
                                              (list "description" "The expression to evaluate.")))))
                      (list "required" (list "expression")))))))

;; The published one-command replacements were removed with the substitution
;; stack. These two tools take a whole inspection task or composed work in one
;; call, which is the shape the corpus identified as potentially useful.
(define (mcp-tools) (mcp-builtin-tools))

(define (mcp-call name arguments)
  (if (string=? name "toolscheme_inspect")
      (let ((outcome (catch-errors
                       (lambda () (inspect (field-ref arguments "pattern" #f)
                                           (field-ref arguments "roots" '("."))
                                           (list (list 'include (field-ref arguments "include" '()))
                                                 (list 'context (field-ref arguments "context" 4))
                                                 (list 'limit (field-ref arguments "limit" 8))))))))
        (if (error? outcome)
            (mcp-failure-content (write-to-string outcome))
            (mcp-content (write-to-string outcome))))
      (if (string=? name "toolscheme_eval")
          (let ((expression (field-ref arguments "expression")))
            (if (not (string? expression))
                (mcp-failure-content "toolscheme_eval needs an `expression` string")
                (let ((outcome (catch-errors
                                 (lambda () (list (list 'value (eval (read-from-string expression))))))))
                  (if (error? outcome)
                      (mcp-failure-content (field-ref outcome 'error))
                      (mcp-content (write-to-string (field-ref outcome 'value)))))))
          (mcp-failure-content (string-append "unknown tool: " name)))))

;; What a client is told on connect.
;;
;; `initialize` carries an `instructions` field that every MCP client reads, and
;; this server returned the empty string in it -- so an agent that connected was
;; told nothing about why the server existed or when to reach for it. That is the
;; one introduction the protocol gives you for free, and it was going unused
;; while the tools sat unused beside it.
;;
;; Deliberately short, and about *when* rather than *what*: the tool list already
;; says what. Shared learnings, when a snapshot has been published locally, follow
;; it rather than replacing it -- an empty snapshot used to mean an empty
;; introduction.
(define mcp-introduction
  (string-append
    "Use toolscheme_inspect to locate code and get nearby lines in one bounded call: "
    "pattern is required; roots, include, context, and limit are optional. "
    "Check truncated before treating results as complete.\n\n"
    "toolscheme_eval evaluates toolscheme expressions and returns association lists rather than "
    "text to be scraped. Identical calls return identical bytes, so repeating one "
    "costs nothing in a cached prompt prefix.\n\n"
    "Worth reaching for when a shell command would need parsing afterwards: "
    "(grep \"needle\" '(glob \"src/**/*.c\") '((limit 20))) answers with paths and "
    "line numbers already separated. Every text tool takes (limit N), so no "
    "pipeline terminator is needed.\n\n"
    "Also (wait-for '(exists \"path\")) and (wait-for '(matches \"log\" \"ready\")), "
    "which return the moment a condition holds rather than after a fixed delay."))

(define (mcp-instructions)
  (if (string-null? learning-notes)
      mcp-introduction
      (string-append mcp-introduction "\n\nShared learnings (advisory):\n" learning-notes)))

(define (mcp-handle line)
  (let ((parsed (json-parse line)))
    (if (error? parsed)
        (mcp-error 'null -32700 (field-ref parsed 'error))
        (let* ((request (field-ref parsed 'value))
               (id (field-ref request "id" 'null))
               (method (field-ref request "method" ""))
               (params (field-ref request "params" '())))
          (cond
            ;; Notifications carry no id and expect no reply.
            ((and (equal? id 'null) (string-prefix? "notifications/" method)) #f)
            ((string=? method "initialize")
             (mcp-envelope id
               (list (list "protocolVersion" mcp-protocol-version)
                     (list "capabilities" (list (list "tools" (list (list "listChanged" #f)))))
                     (list "serverInfo" (list (list "name" "toolscheme")
                                              (list "version" "0.2.0")))
                     (list "instructions" (mcp-instructions)))))
            ((string=? method "tools/list")
             (mcp-envelope id (list (list "tools" (mcp-tools)))))
            ((string=? method "tools/call")
             (mcp-envelope id (mcp-call (field-ref params "name" "")
                                        (field-ref params "arguments" '()))))
            ((string=? method "ping") (mcp-envelope id '()))
            (else (mcp-error id -32601 (string-append "unknown method: " method))))))))
