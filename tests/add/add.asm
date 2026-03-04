; add.asm
; Behavior:
;   R1 = 5
;   R2 = 10
;   R3 = R1 + R2 (expected 15)
;   Print a message and halt

.ORIG x3000                      ; program starts at memory address x3000

; initialize registers
AND R1, R1, #0                   ; clear R1 -> R1 = 0
ADD R1, R1, #5                   ; R1 = 0 + 5 -> R1 = 5

AND R2, R2, #0                   ; clear R2 -> R2 = 0
ADD R2, R2, #10                  ; R2 = 0 + 10 -> R2 = 10

; perform addition
ADD R3, R1, R2                   ; R3 = R1 + R2 -> 5 + 10 = 15

; print message
LEA R0, MSG                      ; load address of MSG into R0
TRAP x22                         ; PUTS -> print string stored at R0

; stop program
HALT                             ; stop execution

; data
MSG     .STRINGZ "ADD executed."

.END