;;; Run the wired decision path -- stall-signals, classification on -- over the
;;; corpus. This is the shipping code, one request per stall, exactly as a hook
;;; would call it.
(let* ((raw (field-ref (read-file "corpus.jsonl" '((limit 8388608))) 'text ""))
       (lines (filter (lambda (l) (not (string-null? l)))
                      (field-ref (text-lines raw) 'lines))))
  (string-join
    (map (lambda (line)
           (let* ((row (field-ref (json-parse line) 'value))
                  (message (field-ref row "message" ""))
                  (signals (stall-signals message))
                  (names (field-ref signals 'names-next-step #f))
                  (asks (field-ref signals 'asks-question #f)))
             (string-append (if names "1" "0") "\t" (if asks "1" "0") "\t"
                            (if (and names (not asks)) "1" "0") "\t"
                            (field-ref signals 'source ""))))
         lines)
    "\n"))
