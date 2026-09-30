;;; mcp.scm -- the Model Context Protocol server, in Scheme.
;;;
;;; The binary only moves lines of JSON on stdio; the protocol lives here, so it
;;; can be changed without a rebuild. Every published tool becomes an MCP tool,
;;; and `toolscheme_eval` exposes the interpreter itself.

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

;; Only `toolscheme_eval` is served. The published tools that used to appear here
;; were removed with the substitution stack: measured against the corpus they had
;; no axis left to win on, because a shell call is a program in a language while
;; a tool call is one operation, and 44% of real calls compose more than one
;; program. `toolscheme_eval` is the exception -- it takes composed work in a
;; single call, which is the only shape that could compete. Whether an agent ever
;; reaches for it is a separate question, and an open one.
(define (mcp-tools) (mcp-builtin-tools))

(define (mcp-call name arguments)
  (if (string=? name "toolscheme_eval")
      (let ((expression (field-ref arguments "expression")))
        (if (not (string? expression))
            (mcp-failure-content "toolscheme_eval needs an `expression` string")
            (let ((outcome (catch-errors
                             (lambda () (list (list 'value (eval (read-from-string expression))))))))
              (if (error? outcome)
                  (mcp-failure-content (field-ref outcome 'error))
                  (mcp-content (write-to-string (field-ref outcome 'value)))))))
      (mcp-failure-content (string-append "unknown tool: " name))))

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
                     (list "instructions" learning-notes))))
            ((string=? method "tools/list")
             (mcp-envelope id (list (list "tools" (mcp-tools)))))
            ((string=? method "tools/call")
             (mcp-envelope id (mcp-call (field-ref params "name" "")
                                        (field-ref params "arguments" '()))))
            ((string=? method "ping") (mcp-envelope id '()))
            (else (mcp-error id -32601 (string-append "unknown method: " method))))))))
