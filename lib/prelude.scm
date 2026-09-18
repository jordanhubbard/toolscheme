;;; prelude.scm -- shared helpers for the toolscheme library.
;;;
;;; Everything here is ordinary Scheme over the primitive surface. It is loaded
;;; first, so the rest of the library can assume it.

(define (any? predicate items)
  (cond ((null? items) #f)
        ((predicate (car items)) #t)
        (else (any? predicate (cdr items)))))

(define (count-if predicate items)
  (fold-left (lambda (n item) (if (predicate item) (+ n 1) n)) 0 items))

;; Accumulating rather than building on the way out: `(cons x (take ...))` holds a
;; frame per element, which is fine for the small n this is usually called with and
;; not fine for any other. The same shape crashed the analyzer once already.
(define (take items n)
  (let loop ((rest items) (left n) (out '()))
    (if (or (= left 0) (null? rest))
        (reverse out)
        (loop (cdr rest) (- left 1) (cons (car rest) out)))))

(define (flatten lists)
  (fold-right append '() lists))

;; Counting by scanning an association list per item is quadratic, and the corpus
;; has thousands of distinct keys -- that alone took minutes. Sorting once and
;; walking the runs is O(n log n) and keeps counting a pure fold.
(define (group-runs sorted)
  (if (null? sorted)
      '()
      (let loop ((rest (cdr sorted)) (key (car (car sorted))) (sum (cadr (car sorted))) (out '()))
        (cond ((null? rest) (reverse (cons (list key sum) out)))
              ((equal? (car (car rest)) key)
               (loop (cdr rest) key (+ sum (cadr (car rest))) out))
              (else (loop (cdr rest) (car (car rest)) (cadr (car rest))
                          (cons (list key sum) out)))))))

(define (tally-pairs pairs)
  (group-runs (list-sort pairs (lambda (a b) (string<? (car a) (car b))))))

(define (tally items)
  (tally-pairs (map (lambda (key) (list key 1)) items)))

(define (tally-by items amount-of)
  (tally-pairs (map (lambda (item) (list (car item) (amount-of item))) items)))

;; Ranked descending by count, which is how every report here reads.
(define (ranked table)
  (list-sort table (lambda (a b) (> (cadr a) (cadr b)))))

(define (top table n)
  (take (ranked table) n))

(define (rows->records table key-name count-name)
  (map (lambda (row) (list (list key-name (car row)) (list count-name (cadr row))))
       table))

;; Reads every published tool in a directory. A tool is an ordinary Scheme file
;; that ends in a `define-tool` call, so publishing is writing a file.
(define (load-published-tools directory)
  (let ((listing (glob "*.scm" (list (list 'directory directory)))))
    (if (error? listing)
        (list (list 'loaded 0))
        (let ((paths (map (lambda (entry) (field-ref entry 'path))
                          (field-ref listing 'entries))))
          (for-each
            (lambda (path)
              ;; `glob` reports paths relative to the capability root, not to the
              ;; directory searched, so the entry path is already usable.
              (let ((source (read-file path)))
                (if (not (error? source))
                    (catch-errors (lambda () (eval (read-from-string
                                                     (string-append "(begin "
                                                                    (field-ref source 'text)
                                                                    ")")))))
                    #f)))
            paths)
          (list (list 'loaded (length paths)))))))

;; `field-ref` reports a missing field as #f, not as unspecified -- testing for
;; unspecified is a silent no-op that routes every record down the wrong branch.
;; JSON `false` and a missing field are indistinguishable here, which is fine for
;; the fields these callers ask about and worth knowing before adding more.
(define (absent? value) (or (unspecified? value) (eq? value #f)))

;; One environment variable, or #f. Configuration reaches a script this way rather
;; than through ambient state the language can read on its own.
(define (env-value name)
  (let ((found (env (list (list 'name name)))))
    (if (or (error? found) (null? (field-ref found 'variables '())))
        #f
        (cadr (car (field-ref found 'variables))))))

;; Keeps the first item of each key and preserves order. Sorting by (key, position)
;; finds the duplicates; sorting the survivors back by position restores the
;; sequence, which matters because consecutive-call analysis reads order.
(define (dedup-keyed pairs)
  (let* ((indexed (let loop ((i 1) (rest pairs) (out '()))
                    (if (null? rest)
                        (reverse out)
                        (loop (+ i 1) (cdr rest)
                              (cons (list (car (car rest)) i (cadr (car rest))) out)))))
         (sorted (list-sort indexed
                            (lambda (a b)
                              (if (string=? (car a) (car b))
                                  (< (cadr a) (cadr b))
                                  (string<? (car a) (car b))))))
         (kept (let loop ((rest sorted) (previous #f) (out '()))
                 (cond ((null? rest) out)
                       ((and previous (string=? (car (car rest)) previous))
                        (loop (cdr rest) previous out))
                       (else (loop (cdr rest) (car (car rest)) (cons (car rest) out))))))
         (ordered (list-sort kept (lambda (a b) (< (cadr a) (cadr b))))))
    (map caddr ordered)))

;; Configuration that has to hold however the agent was started. An environment
;; variable only reaches a hook if the agent inherited it, and whether it did
;; depends on whether the session began from a shell, a desktop launcher, or
;; another agent -- so the variable wins when it is set, and a file beside the
;; observations answers when it is not. Lines are NAME=value; # begins a comment.
;; Split out so the parsing can be exercised without a file: whether a comment, a
;; stray space, or a name that merely starts the same is handled correctly is the
;; part that can be wrong.
(define (setting-in-lines lines name)
  (cond
    ((null? lines) #f)
    (else
      (let* ((line (string-trim (car lines)))
             (cut (string-index line "=")))
        (if (or (string-null? line)
                (string-prefix? "#" line)
                (not cut)
                (not (string=? (string-trim (substring line 1 (- cut 1))) name)))
            (setting-in-lines (cdr lines) name)
            (string-trim (substring line (+ cut 1) (string-length line))))))))

(define (setting-from-file name)
  (let ((file (catch-errors (lambda () (read-file "config" '((limit 16384)))))))
    (if (error? file)
        #f
        (setting-in-lines (field-ref (text-lines (field-ref file 'text "")) 'lines) name))))

(define (setting name)
  (let ((from-environment (env-value name)))
    (if (string? from-environment) from-environment (setting-from-file name))))

(define (setting-on? name)
  (let ((value (setting name)))
    (and (string? value)
         (not (string=? value ""))
         (not (string=? value "0"))
         (not (string=? value "false")))))

;; An absolute path inside the capability root. Needed for SQL, where DuckDB
;; refuses a relative name rather than resolving it, and harmless everywhere else.
(define (rooted path)
  (if (and (> (string-length path) 0) (char=? (string-ref path 1) #\/))
      path
      (string-append capability-root "/" path)))
