; and.asm
; Behavior:
;   R1 = 7
;   R2 = 3
;   R3 = R1 AND R2 (expected 3)
;   Print a message and halt

.ORIG x3000                     ; program starts at memory address x3000

; initialize registers
AND R1, R1, #0                  ; clear R1 -> R1 = 0
ADD R1, R1, #7                  ; R1 = 0 + 7 -> R1 = 7

AND R2, R2, #0                  ; clear R2 -> R2 = 0
ADD R2, R2, #3                  ; R2 = 0 + 3 -> R2 = 3

; perform AND operation
AND R3, R1, R2                  ; R3 = R1 AND R2 -> 7 & 3 = 3

; print message
LEA R0, MSG                     ; load address of MSG into R0
TRAP x22                        ; PUTS -> print string stored at R0

; stop program
HALT                            ; stop execution

; data
MSG     .STRINGZ "AND executed."

.END