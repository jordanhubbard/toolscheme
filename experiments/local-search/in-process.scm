;;; Measure a persistent interpreter. Run once with TOOLSCHEME_CACHE=1 and once
;;; with TOOLSCHEME_CACHE=0 to separate the read-file cache from search and glob.
;;; The result is operation time only: no CLI launch, MCP, or result transport.

(define (elapsed thunk n)
  (let ((start (field-ref (time) 'monotonic-nanoseconds 0)))
    (let loop ((i 0))
      (if (< i n)
          (begin (thunk) (loop (+ i 1)))
          (- (field-ref (time) 'monotonic-nanoseconds 0) start)))))

(define (search-once)
  (search "cache_lookup" '(".") '((literal #t) (include ("*.cpp")) (limit 1000))))
(define (glob-once) (glob "**/*.cpp"))
(define (read-once) (read-file "toolscheme.cpp"))

(search-once)
(glob-once)
(read-once)
(read-once)
(list (list 'search-repetitions 100)
      (list 'search-ns (elapsed search-once 100))
      (list 'glob-repetitions 100)
      (list 'glob-ns (elapsed glob-once 100))
      (list 'read-repetitions 3000)
      (list 'read-ns (elapsed read-once 3000)))
