;;; sql-check.scm -- the optional analytical store, and its sandbox.
;;;
;;; DuckDB is optional: this project has no external dependencies by design, and
;;; `toolscheme` has to build, run and keep collecting without it. So the check
;;; reports that it is absent rather than failing, and the build stays green on a
;;; machine that never fetched it.
;;;
;;; What is worth testing is not that SELECT works. It is that a query cannot read
;;; outside the capability root -- a query reads files, so `sql-query` is the one
;;; primitive that could quietly become a hole in a boundary the rest of the API
;;; enforces. It was exactly that when first written: `duckdb_set_config` refuses
;;; `allowed_directories`, the return value went unchecked, and the sandbox was
;;; simply absent while looking configured.

(define available (and (member 'sql-query (primitive-names)) #t))

(define (run sql) (sql-query sql))

(define checks
  (if (not available)
      (list (list 'duckdb "not built; nothing to check"))
      (let* ((simple (run "SELECT 42 AS answer"))
             (typed (run "SELECT 1 AS i, 'x' AS s, NULL AS n"))
             (outside (run (string-append "SELECT * FROM read_csv('/etc/passwd')")))
             (widen (run "SET allowed_directories=['/etc']"))
             (bad (run "SELECT * FROM nonexistent_table_xyz")))
        (list (list 'answer (field-ref (car (field-ref simple 'rows)) 'answer))
              ;; Every value arrives as text: one representation for every column
              ;; type, rather than a partial copy of DuckDB's type system here.
              (list 'null-is-empty (field-ref (car (field-ref typed 'rows)) 'n))
              (list 'outside-root-refused (error? outside))
              (list 'cannot-widen-the-sandbox (error? widen))
              (list 'bad-query-is-an-error (error? bad))))))

(list (list 'available available)
      (list 'checks checks)
      (list 'checks-hold
            (or (not available)
                (and (equal? (field-ref (car (field-ref (run "SELECT 42 AS answer") 'rows))
                                        'answer) "42")
                     (error? (run "SELECT * FROM read_csv('/etc/passwd')"))
                     (error? (run "SET allowed_directories=['/etc']"))
                     (error? (run "SELECT * FROM nonexistent_table_xyz"))))))
