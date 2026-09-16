;;; search-read -- one call for the `grep -> read` pattern.
;;;
;;; Replaces: grep -rn PATTERN DIR | head -N, followed by reading each hit's file.
;;; Motivated by: `grep -> read` was the largest cross-tool pair in the measured
;;; corpus (192 occurrences), and `read -> read` the largest repeat (1203). The
;;; agent greps to locate, then pays for whole files to see context.
;;;
;;; Why this is cheaper: it returns only the regions around matches, merged where
;;; they overlap, bounded by `limit`. Nothing volatile is included, so repeating
;;; the call is byte-identical and the prompt-cache prefix survives.
;;;
;;; Hand-authored from the analyzer's report. The synthesis path writes the same
;;; shape; this one exists so the gate could be proven before the loop ran.

;; Merge overlapping windows so a cluster of matches costs one region, not one
;; region per match.
(define (merge-windows windows)
  (if (null? windows)
      '()
      (let loop ((rest (cdr windows))
                 (from (car (car windows)))
                 (to (cadr (car windows)))
                 (out '()))
        (cond ((null? rest) (reverse (cons (list from to) out)))
              ((<= (car (car rest)) (+ to 1))
               (loop (cdr rest) from (max to (cadr (car rest))) out))
              (else (loop (cdr rest) (car (car rest)) (cadr (car rest))
                          (cons (list from to) out)))))))

(define (region-text lines from to)
  (string-join (take (list-tail lines (- from 1)) (+ (- to from) 1)) "\n"))

(define (file-regions path windows context)
  (let* ((text (field-ref (read-file path) 'text ""))
         (lines (field-ref (text-lines text) 'lines))
         (total (length lines))
         (padded (map (lambda (w)
                        (list (max 1 (- (car w) context))
                              (min total (+ (cadr w) context))))
                      windows)))
    (map (lambda (w)
           (list (list 'from (car w))
                 (list 'to (cadr w))
                 (list 'text (region-text lines (car w) (cadr w)))))
         (merge-windows padded))))

;; A source arrives tagged from Scheme -- (glob "...") -- but over MCP it is JSON,
;; where there are no symbols: the same value shows up as ("glob" "..."), a bare
;; pattern string, or a plain array of paths. Accepting all of them here keeps one
;; tool usable from both callers instead of needing an MCP-shaped duplicate.
(define (normalize-source source)
  (cond ((string? source) (list 'glob source))
        ((null? source) '())
        ((symbol? (car source)) source)
        ((and (string? (car source)) (string=? (car source) "glob"))
         (cons 'glob (cdr source)))
        ((and (string? (car source)) (string=? (car source) "files"))
         (cons 'files (cdr source)))
        (else (cons 'files source))))

(define (search-read request)
  (let* ((pattern (field-ref request 'pattern ""))
         (source (normalize-source (field-ref request 'source '())))
         (limit (field-ref request 'limit 20))
         (context (field-ref request 'context 2))
         (found (grep pattern source (list (list 'line-numbers #t) (list 'limit limit))))
         (matches (field-ref found 'matches '()))
         ;; Group by path, preserving the order the files were scanned in, so the
         ;; result is a function of the inputs alone.
         (paths (let loop ((rest matches) (seen '()))
                  (cond ((null? rest) (reverse seen))
                        ((member (field-ref (car rest) 'path) seen) (loop (cdr rest) seen))
                        (else (loop (cdr rest) (cons (field-ref (car rest) 'path) seen))))))
         (files (map (lambda (path)
                       (let* ((hits (filter (lambda (m) (string=? (field-ref m 'path) path))
                                            matches))
                              (windows (map (lambda (m)
                                              (let ((n (field-ref m 'line)))
                                                (list n n)))
                                            hits)))
                         (list (list 'path path)
                               (list 'matches (map (lambda (m)
                                                     (list (list 'line (field-ref m 'line))
                                                           (list 'text (field-ref m 'text ""))))
                                                   hits))
                               (list 'regions (file-regions path windows context)))))
                     paths)))
    (list (list 'files files)
          (list 'count (length matches))
          (list 'truncated (field-ref found 'truncated #f))
          (list 'files-scanned (field-ref found 'files-scanned 0)))))

;; The grep-shaped projection the replay gate compares against. Rendering is how a
;; structured result proves it answers the same question as the text tool.
(define (search-read->grep result)
  (string-join
    (flatten (map (lambda (file)
                    (map (lambda (m)
                           (string-append (field-ref file 'path) ":"
                                          (number->string (field-ref m 'line)) ":"
                                          (field-ref m 'text "")))
                         (field-ref file 'matches)))
                  (field-ref result 'files)))
    "\n"))

(define-tool
  (list (list 'name "search-read")
        (list 'description
              "Find a pattern and return the surrounding regions of each matching file in one call.")
        (list 'parameters
              '((pattern string "the pattern to find" #t)
                (source string "a glob pattern, a list of paths, or a tagged source" #t)
                (limit integer "maximum matches to return")
                (context integer "lines of context around each match")))
        (list 'stability
              "No volatile fields; regions depend only on file content, so repeats are byte-identical.")
        (list 'provenance
              '((pattern "grep -> read") (observed-pairs 192) (corpus "158 transcripts")))
        (list 'procedure search-read)))
