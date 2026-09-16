;;; mcp-check.scm -- the protocol surface, driven through `mcp-handle` directly.
;;;
;;; The binary only moves lines, so everything worth testing is here. The failure
;;; that motivated this file was silent: parameter names arrived as symbols and
;;; json-write rendered `properties` as an array of pairs -- a syntactically valid
;;; document and a meaningless schema, which a client ignores without complaint.

(define (handle text) (field-ref (json-parse (mcp-handle text)) 'value))
(define (result-of text) (field-ref (handle text) "result" '()))

(define initialize
  (result-of "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}"))
(define listing (result-of "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}"))
(define tools (field-ref listing "tools" '()))

(define (tool-named name)
  (let loop ((rest tools))
    (cond ((null? rest) '())
          ((equal? (field-ref (car rest) "name" "") name) (car rest))
          (else (loop (cdr rest))))))

(define published (tool-named "search-read"))
(define schema (field-ref published "inputSchema" '()))
(define properties (field-ref schema "properties" '()))

;; An object's fields are (key value) pairs with string keys; an array of pairs
;; has neither. Telling them apart is exactly what went wrong.
(define (json-object? value)
  (and (list? value)
       (not (null? value))
       (string? (car (car value)))))

(define unknown-method
  (handle "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"nonsense\"}"))
(define malformed (handle "not json at all"))
(define notification
  (mcp-handle "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"))
(define failing-call
  (result-of (string-append "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
                            "\"params\":{\"name\":\"no-such-tool\",\"arguments\":{}}}")))
(define eval-call
  (result-of (string-append "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
                            "\"params\":{\"name\":\"toolscheme_eval\","
                            "\"arguments\":{\"expression\":\"(+ 20 22)\"}}}")))

(define checks
  (list (list 'protocol-version (field-ref initialize "protocolVersion" ""))
        (list 'lists-builtin (not (null? (tool-named "toolscheme_eval"))))
        (list 'lists-published (not (null? published)))
        (list 'schema-properties-is-object (json-object? properties))
        (list 'schema-required (field-ref schema "required" '()))
        (list 'eval-works (string-contains? (field-ref (car (field-ref eval-call "content" '()))
                                                       "text" "")
                                            "42"))
        (list 'missing-tool-is-error (field-ref failing-call "isError" #f))
        (list 'unknown-method-is-rpc-error
              (= (field-ref (field-ref unknown-method "error" '()) "code" 0) -32601))
        (list 'malformed-is-parse-error
              (= (field-ref (field-ref malformed "error" '()) "code" 0) -32700))
        (list 'notification-has-no-reply (eq? notification #f))))

(list (list 'checks checks)
      (list 'checks-hold
            (and (equal? (field-ref initialize "protocolVersion" "") mcp-protocol-version)
                 (not (null? (tool-named "toolscheme_eval")))
                 (not (null? published))
                 (json-object? properties)
                 (member "pattern" (field-ref schema "required" '()))
                 (field-ref failing-call "isError" #f)
                 (= (field-ref (field-ref unknown-method "error" '()) "code" 0) -32601)
                 (= (field-ref (field-ref malformed "error" '()) "code" 0) -32700)
                 (eq? notification #f)
                 #t)))
