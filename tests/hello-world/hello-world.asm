.ORIG x3000                         ; program starts at memory address x3000

LEA R0, HELLO_STR                   ; load the address of HELLO_STR into R0
PUTS                                ; print the string stored at the address in R0
HALT                                ; stop program execution

HELLO_STR .STRINGZ "Hello World!"   ; store null-terminated string in memory

.END                                ; end of assembly program