;;; credential.scm -- which key is present, and how it must be presented.
;;;
;;; Lifted out of the synthesis library when that was removed. The classifier is
;;; the remaining caller: it needs a key and a header, and the rule for choosing
;;; between them is the same one synthesis used.

;; A variable set to the empty string is not a credential. Only #f is false here,
;; so an unset-but-exported key -- exactly what a shell wrapper produces when its
;; lookup finds nothing -- would otherwise read as present and route the request
;; to a host that has no key for it. One of three fallbacks had been written that
;; way and had therefore never worked.
(define (credential-value name)
  (let ((found (setting name)))
    (if (and (string? found) (not (string-null? (string-trim found)))) (string-trim found) #f)))

(define (llm-via-gateway?) (if (credential-value "NVIDIA_INFERENCE_API_KEY") #t #f))

;; Credentials live in the environment or the config file, never in the
;; repository. Which variable supplies the key also decides how it is presented:
;; the gateway takes a bearer token, Anthropic directly takes x-api-key, and
;; sending the wrong one is a 401 that reads as a bad key rather than a bad
;; header.
;;
;; `setting` rather than `env-value`, so a key can live in the config file beside
;; every other toolscheme setting. A hook is not started from a login shell and
;; inherits whatever the agent was launched with, so requiring the environment
;; would mean the credential is present when a human runs the tool and absent
;; when the hook does -- working in every test and never in practice.
(define (llm-credential)
  (let ((gateway (credential-value "NVIDIA_INFERENCE_API_KEY"))
        (anthropic (credential-value "ANTHROPIC_API_KEY")))
    (cond (gateway (list (list 'key gateway) (list 'header "authorization")
                         (list 'value (string-append "Bearer " gateway))))
          (anthropic (list (list 'key anthropic) (list 'header "x-api-key")
                           (list 'value anthropic)))
          (else #f))))
