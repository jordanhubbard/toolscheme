;;; pipeline.scm -- run a shell pipeline of text tools in one process.
;;;
;;; The single-command substitution work found almost nothing worth doing: a
;;; rewrite of `cat` pays ~20ms of interpreter start to save a 2ms process, and
;;; the gate refused every candidate on that arithmetic. The unit was wrong. A
;;; pipeline is several processes, and replacing the whole line pays the start
;;; once.
;;;
;;; Measured over this machine's corpus, with the tokenizer rather than a regex:
;;; 1,677 pipelines composed entirely of text tools, 17% of every command run,
;;; against 7% for single commands. And the vocabulary is tiny -- rg, sed, head,
;;; tail and nl account for nearly all of them:
;;;
;;;   sed 408 | rg|head 280 | sed|rg 138 | sed|rg|head 120 | rg|sed 101
;;;
;;; So this implements those, and nothing else. A stage it does not know makes
;;; the whole pipeline unrunnable here, which is the honest answer: the shell is
;;; still there, and guessing at a tool's exact output is how a substitution
;;; starts lying.
;;;
;;; Every stage takes a list of lines and returns a list of lines, which is what
;;; the shell is doing with its pipes. The first stage may instead read a file
;;; named on its own command line.

(define (flag-value flags prefix fallback)
  (let loop ((rest flags))
    (cond ((null? rest) fallback)
          ((and (string-prefix? prefix (car rest))
                (> (string-length (car rest)) (string-length prefix)))
           (let ((n (string->number (substring (car rest)
                                               (+ (string-length prefix) 1)
                                               (string-length (car rest))))))
             (if (number? n) n fallback)))
          (else (loop (cdr rest))))))

;; `-n 20` and `-20` are the same request, and the corpus uses both.
(define (count-flag flags arguments fallback)
  (let ((short (flag-value flags "-" fallback)))
    (if (not (= short fallback))
        short
        (let loop ((rest flags))
          (cond ((null? rest) fallback)
                ((equal? (car rest) "-n")
                 (let ((n (if (null? arguments) #f (string->number (car arguments)))))
                   (if (number? n) n fallback)))
                (else (loop (cdr rest))))))))

(define (non-flag arguments) (filter (lambda (a) (not (string-prefix? "-" a))) arguments))

(define (lines-of-file path)
  (let ((read (catch-errors (lambda () (read-file path)))))
    (if (error? read) #f (field-ref (text-lines (field-ref read 'text "")) 'lines))))

;; `sed -n 'A,Bp'` and `sed -n 'Np'`: a bounded read and nothing else. Any other
;; sed script is refused rather than approximated.
(define (sed-range script)
  (let* ((text (unquoted script))
         (n (string-length text)))
    (if (or (= n 0) (not (char=? (string-ref text n) #\p)))
        #f
        (let ((body (substring text 1 (- n 1))))
          (let ((comma (string-index body ",")))
            (if comma
                (let ((a (string->number (substring body 1 (- comma 1))))
                      (b (string->number (substring body (+ comma 1) (string-length body)))))
                  (if (and (number? a) (number? b)) (list a b) #f))
                (let ((a (string->number body)))
                  (if (number? a) (list a a) #f))))))))

(define (take-range lines from to)
  (let loop ((rest lines) (i 1) (out '()))
    (cond ((null? rest) (reverse out))
          ((> i to) (reverse out))
          ((>= i from) (loop (cdr rest) (+ i 1) (cons (car rest) out)))
          (else (loop (cdr rest) (+ i 1) out)))))

;; rg and grep with -n print `line:text`; without it, just the matching line.
;; Only a single named file is handled, because with several the prefix becomes
;; `path:line:text` and getting that subtly wrong is worse than declining.
(define (matching-lines lines pattern numbered)
  (let loop ((rest lines) (i 1) (out '()))
    (cond ((null? rest) (reverse out))
          ((string-contains? (car rest) pattern)
           (loop (cdr rest) (+ i 1)
                 (cons (if numbered
                           (string-append (number->string i) ":" (car rest))
                           (car rest))
                       out)))
          (else (loop (cdr rest) (+ i 1) out)))))

(define (numbered-lines lines)
  (let loop ((rest lines) (i 1) (out '()))
    (if (null? rest)
        (reverse out)
        (loop (cdr rest) (+ i 1)
              ;; nl right-aligns in six columns and separates with a tab.
              (cons (string-append (pad-left (number->string i) 6) "\t" (car rest)) out)))))

(define (pad-left text width)
  (if (>= (string-length text) width)
      text
      (pad-left (string-append " " text) width)))

;; One stage. #f means "this pipeline is not ours", and the caller must fall back
;; to the shell rather than guess.
(define (stage-run program flags arguments input)
  (let ((files (non-flag arguments)))
    (cond
      ((member program '("cat"))
       (if input input (if (null? files) #f (lines-of-file (car files)))))
      ((member program '("sed"))
       (if (not (member "-n" flags))
           #f
           (let* ((script (if (null? files) #f (car files)))
                  (range (if script (sed-range script) #f))
                  (source (if (and script (> (length files) 1))
                              (lines-of-file (list-ref files 2))
                              input)))
             (if (or (not range) (not source))
                 #f
                 (take-range source (car range) (car (cdr range)))))))
      ((member program '("head"))
       (let ((source (if input input (if (null? files) #f (lines-of-file (car files))))))
         (if (not source) #f (take source (min (count-flag flags files 10) (length source))))))
      ((member program '("tail"))
       (let* ((source (if input input (if (null? files) #f (lines-of-file (car files)))))
              (n (if source (min (count-flag flags files 10) (length source)) 0)))
         (if (not source) #f (take-range source (+ (- (length source) n) 1) (length source)))))
      ((member program '("nl"))
       (let ((source (if input input (if (null? files) #f (lines-of-file (car files))))))
         (if (not source) #f (numbered-lines source))))
      ((member program '("grep" "egrep" "rg"))
       (let* ((patterns (non-flag arguments))
              (pattern (if (null? patterns) #f (unquoted (car patterns))))
              (source (if input
                          input
                          (if (< (length patterns) 2) #f (lines-of-file (list-ref patterns 2))))))
         (if (or (not pattern) (not source))
             #f
             (matching-lines source pattern (and (member "-n" flags) #t)))))
      (else #f))))

;; The whole line. Returns the text the shell would have printed, or #f when any
;; stage is outside what is implemented -- there is no partial credit, because a
;; pipeline that is nearly right is a pipeline that lies.
(define (pipeline-run command)
  (let ((parsed (catch-errors (lambda () (shell-parse command)))))
    (if (or (error? parsed) (not (null? (field-ref parsed 'heredocs '()))))
        #f
        (let loop ((stages (field-ref parsed 'commands '())) (input #f))
          (cond
            ((null? stages)
             (if input (string-join input "\n") #f))
            (else
              (let* ((stage (car stages))
                     ;; Only pipes thread data. `;` and `&&` are sequencing, and
                     ;; treating them as a pipe would silently change the meaning.
                     (connector (field-ref stage 'connector ""))
                     (out (stage-run (field-ref stage 'name "")
                                     (field-ref stage 'flags '())
                                     (field-ref stage 'arguments '())
                                     input)))
                (cond ((not out) #f)
                      ((null? (cdr stages)) (string-join out "\n"))
                      ((not (equal? connector "|")) #f)
                      (else (loop (cdr stages) out))))))))))
