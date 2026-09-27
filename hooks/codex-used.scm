;;; codex-used.scm -- how many times each Codex thread has already been continued.
;;;
;;; The first of three stages, and the one that exists because the cap has to
;;; come from somewhere durable. It used to come from the rollout transcript,
;;; counted by grep, which cannot work: a transcript records what was delivered,
;;; so a failed queue leaves no trace and the count stays at zero; and Codex
;;; compacts long threads, so a delivered continuation is eventually rewritten
;;; out of the file and the count falls back to zero anyway. Both happened. One
;;; thread was continued 431 times against a cap of 2.
;;;
;;; The observation log is append-only and is already where both agents record a
;;; continuation, so it is the ledger. Reading it means running rooted at the
;;; state directory, which is a different root from the scan -- hence stages.

(field-ref (json-write (codex-used-counts)) 'text "")
