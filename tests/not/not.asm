; not.asm
; Behavior:
;   R1 = 5
;   R2 = NOT R1
;   Print a message and halt

.ORIG x3000                         ; program starts at memory address x3000

; initialize register
AND R1, R1, #0                      ; clear R1 → R1 = 0
ADD R1, R1, #5                      ; R1 = 0 + 5 → R1 = 5

; perform NOT operation
NOT R2, R1                          ; R2 = bitwise NOT of R1

; print message
LEA R0, MSG                         ; load address of MSG into R0
TRAP x22                            ; PUTS → print string stored at R0

; stop program
HALT                                ; stop execution

; data
MSG     .STRINGZ "NOT executed."

.END