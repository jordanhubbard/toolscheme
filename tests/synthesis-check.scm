;;; synthesis-check.scm -- what can be proven about the synthesis leg without a key.
;;;
;;; The live call needs ANTHROPIC_API_KEY and is exercised by `make synthesize`.
;;; Everything else -- the request shape, the cache breakpoint, the structured
;;; output schema, and every response branch -- is checkable offline, and those are
;;; the parts that fail silently. A refusal returns HTTP 200 with no content block;
;;; reading content[0] without checking stop_reason is how that becomes a crash.

(define opportunity
  '((pattern "grep -> read") (occurrences 192) (result-bytes 1200000)))

(define request (synthesis-request opportunity))
(define body (field-ref (json-write request) 'text))
(define reparsed (field-ref (json-parse body) 'value))

;; The stable prefix carries the cache breakpoint, and the volatile report must sit
;; after it or every call re-pays for the catalogue.
(define system-block (car (field-ref reparsed "system")))
(define breakpoint-present
  (equal? (field-ref (field-ref system-block "cache_control" '()) "type" "") "ephemeral"))
(define brief-is-stable
  (not (string-contains? (field-ref system-block "text" "") "192")))
(define report-after-breakpoint
  (string-contains? (field-ref (car (field-ref reparsed "messages")) "content" "")
                    "grep -> read"))

;; Structured output, not prose to be scraped.
(define schema-requested
  (equal? (field-ref (field-ref (field-ref reparsed "output_config" '()) "format" '()) "type" "")
          "json_schema"))
(define model-is-current (equal? (field-ref reparsed "model" "") "claude-opus-5"))

;; Every response branch, driven by fabricated responses.
(define (result-of json) (synthesis-result (list (list 'body json))))

(define refusal
  (result-of "{\"stop_reason\":\"refusal\",\"content\":[],\"usage\":{}}"))
(define malformed
  (result-of "{\"stop_reason\":\"end_turn\",\"usage\":{}}"))
(define good
  (result-of
    (string-append
      "{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":"
      "\"{\\\"name\\\":\\\"demo\\\",\\\"scheme_source\\\":\\\"(define-tool 1)\\\"}\"}],"
      "\"usage\":{\"cache_read_input_tokens\":4096,\"cache_creation_input_tokens\":0,"
      "\"output_tokens\":120}}")))

;; No key configured must be a structured refusal, never a crash or a bare call.
(define keyless (synthesize opportunity))

(list
  (list 'request (list (list 'cache-breakpoint-present breakpoint-present)
                       (list 'brief-excludes-volatile-report brief-is-stable)
                       (list 'report-after-breakpoint report-after-breakpoint)
                       (list 'structured-output schema-requested)
                       (list 'model model-is-current)
                       (list 'body-is-valid-json (not (error? (json-parse body))))))
  (list 'responses (list (list 'refusal-detected (eq? (field-ref refusal 'code) 'refusal))
                         (list 'malformed-detected (error? malformed))
                         (list 'tool-extracted
                               (equal? (field-ref (field-ref good 'tool) "name" "") "demo"))
                         (list 'cache-read-reported
                               (= (field-ref good 'cache-read-tokens) 4096))))
  (list 'no-key-is-structured (eq? (field-ref keyless 'code) 'capability-missing))
  (list 'checks-hold
        (and breakpoint-present brief-is-stable report-after-breakpoint schema-requested
             model-is-current
             (eq? (field-ref refusal 'code) 'refusal)
             (error? malformed)
             (equal? (field-ref (field-ref good 'tool) "name" "") "demo")
             (= (field-ref good 'cache-read-tokens) 4096)
             (eq? (field-ref keyless 'code) 'capability-missing))))
