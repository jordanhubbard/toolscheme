;;; replay.scm -- the gate a synthesized tool must pass before an agent sees it.
;;;
;;; A model wrote this code. Nothing published it on the strength of that. Each
;;; candidate is replayed against invocations taken from real transcripts, beside
;;; the legacy path it claims to replace, and it is published only if it agrees on
;;; every case *and* measurably wins on at least one axis.
;;;
;;; The comparison normalizes whitespace and ordering, because agreeing on content
;;; is the claim -- not agreeing on formatting.

(define (normalize-output text)
  (string-join (filter (lambda (line) (not (string-null? line)))
                       (map string-trim (field-ref (text-lines text) 'lines)))
               "\n"))

;; The legacy side runs the command the agent actually used. A recorded pattern is
;; often more than one call -- `grep -> read` was the top cross-tool pair in the
;; corpus -- and the follow-up reads are part of what answering cost, so their
;; bytes are charged to the legacy path too. Otherwise a fused tool is compared
;; against only the cheapest half of what it replaces.
(define (legacy-run command follow-up-reads directory)
  (let ((started (field-ref (time) 'epoch-milliseconds))
        (result (sh (if (string-null? directory)
                        (list (list 'command command))
                        (list (list 'command command) (list 'directory directory))))))
    (if (error? result)
        result
        (let* ((finished (process-wait (field-ref result 'job)))
               (stdout (field-ref finished 'stdout ""))
               ;; Whether the legacy output is itself stable has to be measured, not
               ;; assumed. It decides whether the candidate's stability is a win or
               ;; merely table stakes, and `cat` is stable while `ls -l` is not.
               (again (sh (if (string-null? directory)
                              (list (list 'command command))
                              (list (list 'command command) (list 'directory directory)))))
               (second (if (error? again)
                           #f
                           (field-ref (process-wait (field-ref again 'job)) 'stdout "")))
               (follow-up (fold-left
                            (lambda (n path)
                              (let ((read (catch-errors (lambda () (read-file path)))))
                                (if (error? read)
                                    n
                                    (+ n (string-length (field-ref read 'text ""))))))
                            0 follow-up-reads)))
          (list (list 'output (normalize-output stdout))
                (list 'stable (and (string? second) (string=? second stdout)))
                (list 'status (field-ref finished 'exit-status -1))
                (list 'bytes (+ (string-length stdout) follow-up))
                (list 'command-bytes (string-length stdout))
                (list 'follow-up-bytes follow-up)
                (list 'elapsed-ms (- (field-ref (time) 'epoch-milliseconds) started)))))))

;; What a rewrite actually costs the agent.
;;
;; The candidate is timed in process; a redirect spawns a fresh interpreter for
;; every call. Measured on this machine: sed 1ms, sed through sh -c 2ms, and the
;; same work through the rewrite 18ms, of which ~16ms is starting the
;; interpreter. Crediting a candidate with in-process speed it will never have is
;; how two tools were published as faster than the commands they replace while
;; being nine times slower in the only way they are ever invoked.
;;
;; Measured rather than assumed, because it is a property of the host and not of
;; this project, and a constant here would be wrong on someone else's machine.
(define (interpreter-start-ms)
  (let* ((binary (or (env-value "TOOLSCHEME_BINARY") "toolscheme"))
         (started (field-ref (time) 'epoch-milliseconds))
         (job (catch-errors
                (lambda () (process-start (list (list 'program binary)
                                                (list 'arguments '("-e" "1"))
                                                (list 'timeout-ms 10000))))))
         (done (if (error? job) #f (catch-errors
                                     (lambda () (process-wait (field-ref job 'job)))))))
    (if (or (error? job) (not done) (error? done))
        0
        (- (field-ref (time) 'epoch-milliseconds) started))))

(define (candidate-run name arguments)
  (let* ((started (field-ref (time) 'epoch-milliseconds))
         (result (catch-errors (lambda () (tool-invoke name arguments)))))
    (if (error? result)
        result
        (list (list 'output (normalize-output (write-to-string result)))
              (list 'result result)
              ;; A tool that returns a record saying nothing went wrong has
              ;; succeeded, whatever is in it. The distinction matters because a
              ;; tool can report success while having read nothing -- which is
              ;; how `cat missing.txt` became "the file is empty" with exit 0.
              (list 'failed (and (list? result)
                                 (not (absent? (field-ref result "error" #f)))))
              (list 'bytes (string-length (write-to-string result)))
              (list 'elapsed-ms (- (field-ref (time) 'epoch-milliseconds) started))))))

;; Stability is checked directly rather than inferred: call it twice and compare
;; bytes. This is the property the whole tool design rests on, so it is the one
;; axis a candidate cannot fake.
(define (stable? name arguments)
  (let ((first (catch-errors (lambda () (tool-invoke name arguments))))
        (second (catch-errors (lambda () (tool-invoke name arguments)))))
    (and (not (error? first))
         (not (error? second))
         (string=? (write-to-string first) (write-to-string second)))))

;; Comparing a structured result against a text tool's stdout needs a rendering,
;; not a substring test: a record and a line of grep output never share bytes even
;; when they carry exactly the same answer. `render` maps the candidate's result
;; into the legacy tool's shape, and equivalence is judged there. A candidate with
;; no rendering is compared on its raw written form, which is strictly weaker.
(define (default-render result) (write-to-string result))

;; Both sides have to run where the command was recorded: the legacy side because
;; its paths are relative to that directory, the candidate because its paths came
;; out of the same command text. The working directory is restored afterwards so
;; one case cannot move the ground under the next.
(define (replay-case name case render)
  (let* ((directory (field-ref case 'directory ""))
         (origin (field-ref (pwd) 'path ""))
         (moved (if (string-null? directory) #f (cd directory)))
         (legacy (legacy-run (field-ref case 'command "")
                             (field-ref case 'then-reads '())
                             directory))
         (candidate (candidate-run name (field-ref case 'arguments '())))
         (restored (if (string-null? directory) #f (cd origin))))
    (cond
      ((error? legacy) (list (list 'verdict 'skipped) (list 'reason "legacy path unavailable")))
      ((error? candidate) (list (list 'verdict 'failed)
                                (list 'reason (field-ref candidate 'error))))
      (else
        (let* (;; `render` is model-written and called on every result, including
               ;; ones describing a failure. A tool that refuses to render a
               ;; failed read is behaving well -- but it refuses by raising, and
               ;; an unguarded call took the whole run down with it. Refusing to
               ;; render is read as the tool reporting failure, which is what it
               ;; means.
               (raw (catch-errors (lambda () (render (field-ref candidate 'result)))))
               (render-refused (error? raw))
               (rendered (if render-refused "" (normalize-output raw)))
               (expected (field-ref legacy 'output ""))
               ;; Same bytes is not the same answer. `cat missing.txt` prints
               ;; nothing and exits 1; a tool that prints nothing and exits 0
               ;; matches it byte for byte and means something entirely
               ;; different, and the agent has no way to tell.
               (legacy-failed (not (= (field-ref legacy 'status 0) 0)))
               (candidate-failed (or (field-ref candidate 'failed #f) render-refused))
               (same-status (eq? legacy-failed candidate-failed))
               (agrees (and (string=? rendered expected) same-status)))
          (list (list 'verdict (if agrees 'agrees 'differs))
                (list 'same-status same-status)
                (list 'legacy-failed legacy-failed)
                (list 'candidate-failed candidate-failed)
                (list 'command (field-ref case 'command ""))
                (list 'expected expected)
                (list 'rendered rendered)
                (list 'legacy-bytes (field-ref legacy 'bytes 0))
                (list 'legacy-stable (field-ref legacy 'stable #f))
                (list 'legacy-follow-up-bytes (field-ref legacy 'follow-up-bytes 0))
                (list 'candidate-bytes (field-ref candidate 'bytes 0))
                (list 'legacy-ms (field-ref legacy 'elapsed-ms 0))
                (list 'candidate-ms (field-ref candidate 'elapsed-ms 0))
                (list 'stable (stable? name (field-ref case 'arguments '())))))))))

;; The verdict. Equivalence is not optional and no single win substitutes for it:
;; a faster tool that answers differently is not a replacement, it is a bug with
;; better latency. Winning on any one axis is enough once it agrees on all of them.
(define (replay name cases . rest)
  (let* ((render (if (null? rest) default-render (car rest)))
         (results (map (lambda (c) (replay-case name c render)) cases))
         (considered (filter (lambda (r) (not (eq? (field-ref r 'verdict) 'skipped))) results))
         (agreed (count-if (lambda (r) (eq? (field-ref r 'verdict) 'agrees)) considered))
         (legacy-bytes (fold-left (lambda (n r) (+ n (field-ref r 'legacy-bytes 0))) 0 considered))
         (candidate-bytes
           (fold-left (lambda (n r) (+ n (field-ref r 'candidate-bytes 0))) 0 considered))
         (legacy-ms (fold-left (lambda (n r) (+ n (field-ref r 'legacy-ms 0))) 0 considered))
         ;; Charged one interpreter start per case, because that is what a
         ;; rewrite pays and what the agent waits for. An MCP caller does not
         ;; pay it, so `candidate-in-process-ms` is kept alongside: the same
         ;; tool can be worth substituting through one path and not the other.
         (start-ms (interpreter-start-ms))
         (candidate-in-process-ms
           (fold-left (lambda (n r) (+ n (field-ref r 'candidate-ms 0))) 0 considered))
         (candidate-ms (+ candidate-in-process-ms (* start-ms (length considered))))
         (all-stable (and (not (null? considered))
                          (= (count-if (lambda (r) (field-ref r 'stable #f)) considered)
                             (length considered))))
         (equivalent (and (not (null? considered)) (= agreed (length considered))))
         ;; Evidence for a rejection: the first case where the two disagreed.
         (disagreement (let loop ((rest considered))
                         (cond ((null? rest) '())
                               ((eq? (field-ref (car rest) 'verdict) 'agrees) (loop (cdr rest)))
                               (else (list (list 'command (field-ref (car rest) 'command ""))
                                           (list 'expected (field-ref (car rest) 'expected ""))
                                           (list 'rendered (field-ref (car rest) 'rendered "")))))))
         ;; A tool whose output churns invalidates the agent's prompt cache, so
         ;; replacing an unstable command with a stable one is a real win. Being
         ;; stable where the old command was already stable is not a win, it is the
         ;; baseline -- and counting it as one published a tool that was bigger and
         ;; no faster than the `cat` it replaced.
         (legacy-stable (and (not (null? considered))
                             (= (count-if (lambda (r) (field-ref r 'legacy-stable #f)) considered)
                                (length considered))))
         (stability-win (and all-stable (not legacy-stable)))
         ;; Two different questions, and conflating them published tools as
         ;; faster than the commands they replace while being slower in the only
         ;; way they were ever invoked.
         ;;
         ;; An MCP caller holds a running interpreter and pays only the work:
         ;; `cat` replaced costs 0ms against 61ms, a real win. A redirect spawns
         ;; a fresh interpreter for every call and pays ~20ms before reading a
         ;; byte, which no file read can earn back. So a tool can be worth
         ;; serving and not worth substituting, and most will be.
         (wins-in-process (and all-stable
                               (or stability-win
                                   (< candidate-bytes legacy-bytes)
                                   (< candidate-in-process-ms legacy-ms))))
         (wins-as-rewrite (and all-stable
                               (or stability-win
                                   (< candidate-bytes legacy-bytes)
                                   (< candidate-ms legacy-ms))))
         (wins wins-in-process))
    (list (list 'tool name)
          (list 'cases (length results))
          (list 'considered (length considered))
          (list 'agreed agreed)
          (list 'equivalent equivalent)
          (list 'stable all-stable)
          (list 'legacy-stable legacy-stable)
          (list 'stability-win stability-win)
          (list 'wins-in-process wins-in-process)
          ;; Written into the published tool as its claim: only a tool that wins
          ;; as a rewrite may claim shapes for redirection.
          (list 'redirect-worthy wins-as-rewrite)
          (list 'legacy-bytes legacy-bytes)
          (list 'candidate-bytes candidate-bytes)
          (list 'legacy-ms legacy-ms)
          (list 'candidate-ms candidate-ms)
          (list 'candidate-in-process-ms candidate-in-process-ms)
          (list 'interpreter-start-ms start-ms)
          (list 'publish (and equivalent wins))
          (list 'disagreement disagreement)
          (list 'results results))))

;; The evidence the gate just computed, in the shape redirection reads: one row
;; per command shape, with how many cases carried it and how many agreed.
;;
;; Without this a published tool can never be used. `redirect.scm` will only
;; rewrite a command when a tool claims its shapes and carries agreement on all
;; of them, and the loop was computing exactly that and then throwing it away --
;; so every tool it published was correct, loadable, and unreachable.
(define (proven-rows verdict)
  (let* ((results (filter (lambda (r) (not (eq? (field-ref r 'verdict) 'skipped)))
                          (field-ref verdict 'results '())))
         (shapes (fold-left (lambda (seen shape) (if (member shape seen) seen (cons shape seen)))
                            '()
                            (flatten (map (lambda (r) (command-shapes (field-ref r 'command "")))
                                          results)))))
    (map (lambda (shape)
           (let* ((carrying (filter (lambda (r) (member shape (command-shapes
                                                                (field-ref r 'command ""))))
                                    results))
                  (agreed (count-if (lambda (r) (eq? (field-ref r 'verdict) 'agrees)) carrying)))
             (list (list 'shape shape)
                   (list 'cases (length carrying))
                   (list 'agreed agreed)
                   (list 'worthwhile agreed))))
         shapes)))

;; The model returns a complete define-tool form, and the fields redirection needs
;; are not its to know: they come from the replay that has just happened. They are
;; inserted rather than asked for, so the model cannot claim evidence for itself.
(define (with-proven-fields source name verdict)
  (let ((opening (string-index source "(list ")))
    ;; A tool that does not win as a rewrite is still published -- it is served
    ;; over MCP, where it does win -- but it claims no shapes, so redirection
    ;; will never substitute it.
    (if (or (not opening) (not (field-ref verdict 'redirect-worthy #f)))
        source
        (string-append
          (substring source 1 (+ opening 5))
          "\n                   (list 'shapes '"
          (write-to-string (map (lambda (row) (field-ref row 'shape "")) (proven-rows verdict)))
          ")\n                   (list 'proven '"
          (write-to-string (proven-rows verdict))
          ")\n                   (list 'translate \"" name "-translate\")"
          "\n                   (list 'legacy-form \"" name "-legacy-form\")"
          (substring source (+ opening 6) (string-length source))))))

;; Where a proven tool is written. The state directory when there is one, because
;; that is what Git shares and what the interpreter loads; the library directory
;; otherwise, which is a checkout being worked in.
;;
;; This is what makes an evolved tool durable beyond the machine that evolved it.
;; Written here, it is live locally at once, offered to every other consumer on
;; the next sync, and inert on each of them until someone adopts it.
(define (publishing-directory)
  (if (and (string? adopted-tool-directory) (not (string-null? adopted-tool-directory)))
      adopted-tool-directory
      tool-directory))

;; Publication is writing a reviewable file. The provenance header is part of the
;; artifact: which pattern motivated the tool, what the replay measured, and when.
(define (publish-tool-source directory tool verdict)
  (let* ((name (field-ref tool "name"))
         (path (string-append directory "/" name ".scm"))
         (header (string-append
                   ";;; " name " -- synthesized replacement tool.\n"
                   ";;;\n"
                   ";;; Replaces: " (field-ref tool "replaces_pattern" "") "\n"
                   ";;; Why:      " (field-ref tool "rationale" "") "\n"
                   ";;; Stability: " (field-ref tool "stability_contract" "") "\n"
                   ";;; Replay:   " (write-to-string
                                      (list (list 'cases (field-ref verdict 'considered 0))
                                            (list 'agreed (field-ref verdict 'agreed 0))
                                            (list 'stable (field-ref verdict 'stable #f))
                                            (list 'legacy-bytes (field-ref verdict 'legacy-bytes 0))
                                            (list 'candidate-bytes
                                                  (field-ref verdict 'candidate-bytes 0))))
                   "\n;;;\n"
                   ";;; Written by a model and gated by replay; edit freely.\n\n")))
    ;; The destination may not exist yet: a host that has never published has no
    ;; tools directory, and that is the common case rather than the odd one.
    (catch-errors (lambda () (mkdir directory '((parents #t)))))
    ;; Checked, because the destination is often outside the sandbox the loop
    ;; runs in: publishing aims at the state directory while synthesis is rooted
    ;; at the corpus, and write-file reports that refusal by returning an error
    ;; rather than raising it. A gate that approves a tool and then loses it is
    ;; worse than one that refuses, because it reports success either way.
    ;; The two harness procedures are written beside the tool and named, because
    ;; redirection calls them by name: without them on disk a rewrite has no way
    ;; to turn a command into arguments, or the answer back into what the shell
    ;; would have printed.
    (let* ((body (string-append
                   "(define " name "-translate\n"
                   (field-ref tool "translate_source" "") ")\n\n"
                   "(define " name "-legacy-form\n"
                   (field-ref tool "legacy_form_source" "") ")\n\n"
                   (with-proven-fields (field-ref tool "scheme_source" "") name verdict)
                   "\n"))
           (written (write-file path (string-append header body))))
      (if (error? written)
          written
          (list (list 'published path))))))
