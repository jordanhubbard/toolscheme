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

;; A tool's declared parameters become a JSON Schema. Parameters are
;; ((name type description required?) ...), which is enough for an agent to call
;; correctly without inventing a schema language.
;;
;; Names and types arrive as symbols because that is how they read in Scheme, and
;; they must go out as strings: json-write renders a list with symbol keys as an
;; array of pairs, which is a syntactically valid document and a meaningless
;; schema. A client would quietly ignore it rather than report anything.
(define (json-key value)
  (cond ((string? value) value)
        ((symbol? value) (symbol->string value))
        (else (write-to-string value))))

(define (mcp-parameter-schema parameters)
  (list (list "type" "object")
        (list "properties"
              (map (lambda (parameter)
                     (list (json-key (car parameter))
                           (list (list "type" (json-key (cadr parameter)))
                                 (list "description" (caddr parameter)))))
                   parameters))
        (list "required"
              (map (lambda (parameter) (json-key (car parameter)))
                   (filter (lambda (parameter)
                             (and (> (length parameter) 3) (list-ref parameter 4)))
                           parameters)))))

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

(define (mcp-published-tools)
  (map (lambda (tool)
         (list (list "name" (field-ref tool 'name))
               (list "description" (field-ref tool 'description ""))
               (list "inputSchema" (mcp-parameter-schema (field-ref tool 'parameters '())))))
       (field-ref (tool-manifest) 'tools)))

(define (mcp-tools) (append (mcp-builtin-tools) (mcp-published-tools)))

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
      ;; A raised error and a returned error record are both failures, and the
      ;; client should see them as one: `isError`, not a success block whose text
      ;; happens to describe a failure.
      (let ((outcome (catch-errors (lambda () (tool-invoke name arguments)))))
        (if (error? outcome)
            (mcp-failure-content (write-to-string outcome))
            (mcp-content (write-to-string outcome))))))

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
                                              (list "version" "0.2.0"))))))
            ((string=? method "tools/list")
             (mcp-envelope id (list (list "tools" (mcp-tools)))))
            ((string=? method "tools/call")
             (mcp-envelope id (mcp-call (field-ref params "name" "")
                                        (field-ref params "arguments" '()))))
            ((string=? method "ping") (mcp-envelope id '()))
            (else (mcp-error id -32601 (string-append "unknown method: " method))))))))
