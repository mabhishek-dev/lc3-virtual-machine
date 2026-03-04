#include <stdio.h>
#include <stdint.h> //for uint16_t data type
#include <signal.h>

/* ---------- Unix only ---------- */
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h> /* older: #include <sys/termios.h> */
#include <sys/mman.h>


/*
    ---------- Windows only ----------
    Do NOT enable these on Unix / macOS.

    #include <Windows.h>
    #include <conio.h>
*/


/*
    The virtual machine has 10 registers, each 16 bits wide:
    
        8 general purpose registers (R0-R7) - Store's Data
        1 program counter (PC)              — address of next instruction
        1 conditional flag (COND)           — status flags (positive / zero / negative)
    
    Note: R_COUNT is not a register — it is just the number of registers.

    Enum usage:
        Enums are used to define named constants.
        They give readable names to fixed values
        and prevent those values from changing at runtime.
*/

enum{
    R_R0 = 0,
    R_R1,
    R_R2,
    R_R3,
    R_R4,
    R_R5,
    R_R6,
    R_R7,
    R_PC,
    R_COND,
    R_COUNT
};

/*
    The R_COND register stores the condition flags used for branching.
    After an instruction runs, the CPU marks the result as:
        - positive
        - zero
        - negative

    Each flag is stored as a single bit (a binary value),
    so each one has its own unique bit position.
*/

enum{
    FL_POS = 1 << 0,    /* Positive, 1 << 0 == 0001 == if 0001 then result is positive. */
    FL_ZRO = 1 << 1,    /* Zero,     1 << 1 == 0010 == if 0010 then result is zero. */
    FL_NEG = 1 << 2     /* Negative, 1 << 2 == 0100 == if 0100 then result is negative. */
};


/*
    In each LC-3 instruction there are two parts:

    - The opcode
      → describes the operation the CPU knows how to perform.

    - The parameters (operands)
      → values or registers provided as input to that operation.

    LC-3 instructions are 16 bits wide.
    The upper 4 bits are the opcode,
    and the remaining 12 bits are parameters.
    
    Note: the enum below must follow the same opcode order (0–15).
*/

enum{
    OP_BR = 0,  /* branch                 : branch to PC + offset if NZP condition matches current flags */
    OP_ADD,     /* add                    : compute (SR1 + SR2 / imm5) and store result in DR */
    OP_LD,      /* load                   : load value from memory at (PC + offset) into DR */
    OP_ST,      /* store                  : store the contents of SR into memory at (PC + offset9) */
    OP_JSR,     /* jump register          : jump to subroutine using a PC-relative offset */
    OP_AND,     /* bitwise and            : bitwise-AND (SR1 & SR2 / imm5) and store result in DR */
    OP_LDR,     /* load register          : load value from memory at (BaseR + offset6) into DR */
    OP_STR,     /* store register         : store contents of SR into memory at (BaseR + offset6) */
    OP_RTI,     /* unused                 : return from interrupt (privileged instruction; unsupported in LC-3 VM) */
    OP_NOT,     /* bitwise not            : bitwise invert SR and store result in DR */
    OP_LDI,     /* load indirect          : load a value from the memory address stored in memory into a register */
    OP_STI,     /* store indirect         : store contents of SR into memory at the address found via (PC + offset9) */
    OP_JMP,     /* jump                   : PC is replaced with the address stored in BaseR (SR1),and execution continues from that address */
    OP_RES,     /* reserved (unused)      : reserved / undefined instruction (ignored in this VM)*/ 
    OP_LEA,     /* load effective address : compute (PC + offset9) and store that address into DR (no memory read) */
    OP_TRAP     /* execute trap           : calls a system routine to perform I/O or control operations like input, output, or halt */
};

enum{
    /*
        Memory-mapped registers are special hardware registers accessed
        through fixed memory addresses instead of normal CPU registers.

        LC-3 uses:
            • KBSR — checks if a key was pressed
            • KBDR — reads which key was pressed

        Unlike GETC (blocking), these allow non-blocking keyboard input.
    */
   MR_KBSR = 0xFE00,    /* KBSR: keyboard status register */
   MR_KBDR = 0xFE02     /* KBDR: keyboard data register */
};

enum{ 
    /*
        LC-3 programs start at address 0x3000.

        The lower memory region 0x0000–0x2FFF is reserved for the system:
            • trap routines (GETC, PUTS, HALT, etc.)
            • interrupt vectors / OS support code

        We do not re-implement these routines in the VM.
        Instead, we call the built-in system trap handlers.
    */
    TRAP_GETC = 0x20,   /* get character from keyboard, not echoed onto the terminal */
    TRAP_OUT = 0x21,    /* output a character */
    TRAP_PUTS = 0x22,   /* output a word string */
    TRAP_IN = 0x23,     /* get a character from keyboard, echoed onto the terminal */
    TRAP_PUTSP = 0x24,  /* output a byte string */
    TRAP_HALT = 0x25    /* halt the program */
};

/* uint16_t: an unsigned 16-bit integer type (range 0 to 65535) */
#define MEMORY_MAX (1 << 16) // 1 << 16 == 2^16 == 65,536  (we do not write 2^16 because ^ is XOR in C)
uint16_t memory[MEMORY_MAX]; // 65,536 memory locations, each stores one 16-bit (2-byte) value -> total 128 KB

uint16_t reg[R_COUNT]; // array holding the registers

/* ---------- Unix only ---------- */
struct termios original_tio; /* variable used to save the original terminal settings */

/* ---------- Unix only ---------- */
/* Disable waiting for Enter and disable echoing typed characters */
void disable_input_buffering(){
    /* Get: read current terminal settings into original_tio */
    tcgetattr(STDIN_FILENO, &original_tio); 
    
    /* create a copy so we don’t lose the original settings */
    struct termios new_tio = original_tio;

    /* disable canonical mode (no Enter required) and echo */
    new_tio.c_lflag &= ~ICANON & ~ECHO; 

    /* Set: apply the modified settings to the terminal */
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio); /* TCSANOW: apply modified settings immediately */
}

/* ---------- Unix only ---------- */
void restore_input_buffering(){
    tcsetattr(STDIN_FILENO, TCSANOW, &original_tio); /* update the current terminal's setting immediately with original_tio */
}

/* ---------- Unix only ---------- */
uint16_t check_key(){
    /*
        FD means file descriptor.
        fd_set is a data structure used as a checklist of file descriptors.

        readfds is an fd_set used to watch input sources.
        FD_ZERO clears the set, meaning nothing is being watched initially.

        STDIN_FILENO is file descriptor 0, which represents the terminal input stream (input coming from the keyboard).

        FD_SET adds STDIN to the checklist, meaning we want to check if keyboard input is available.

        timeout is set to zero so select() does not wait (non-blocking).
        It only checks the current state instantly.

        select() asks the OS whether any input is available on STDIN.
        If a key was pressed, it returns non-zero; otherwise, it returns zero.
    */
   fd_set readfds;
   FD_ZERO(&readfds);
   FD_SET(STDIN_FILENO, &readfds);

   struct timeval timeout;
   timeout.tv_sec = 0;  //seconds
   timeout.tv_usec = 0; //microseconds

   /*
        select() explanation:

        - The first argument (1) tells select() how many file descriptors to consider.
          It means: only check descriptors from 0 up to (1 − 1), i.e., only STDIN (fd 0).

          Even if readfds could contain other descriptors,
          select() will ignore them and watch only the keyboard input.

        - &readfds tells select() to watch the input stream only.

        - The two NULLs mean:
          we are not checking writable streams or error streams.

        - timeout is set to 0 seconds and 0 microseconds,
          so select() does NOT wait — it checks instantly.

        - select() returns:
            non-zero → a key was pressed (input available)
            zero     → no key was pressed

        - The return expression converts this into true/false.
    */
   return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

/*
    ---------- Windows Only ----------
    For Windows compatibility:
    Comment out or remove the Unix-specific code above and use the Windows-specific implementation below instead.
*/
#if 0
/* ---------- Windows only ---------- */
HANDLE hStdin = INVALID_HANDLE_VALUE; /* handle to the console input stream (Windows equivalent of STDIN on Unix) */
DWORD fdwMode, fdwOldMode;            /* fdwOldMode = original console input settings, fdwMode = modified console input settings */

/* ---------- Windows only ---------- */
void disable_input_buffering(){
    hStdin = GetStdHandle(STD_INPUT_HANDLE); /* get handle to console input (keyboard) */

    GetConsoleMode(hStdin, &fdwOldMode);     /* save original console input settings */

    fdwMode = fdwOldMode                     
            ^ ENABLE_ECHO_INPUT              /* turn off echoing typed characters */
            ^ ENABLE_LINE_INPUT;             /* turn off line buffering (no Enter needed) */

    SetConsoleMode(hStdin, fdwMode);         /* apply modified input settings */

    FlushConsoleInputBuffer(hStdin);         /* clear any key presses made before mode change */
}

/* ---------- Windows only ---------- */
void restore_input_buffering(){
    SetConsoleMode(hStdin, fdwOldMode);      /* restore original console input settings */
}

/* ---------- Windows only ---------- */
uint16_t check_key(){
    /* 
       WaitForSingleObject waits up to 1000 ms (1 second) for keyboard input.
       - hStdin        : handle to the keyboard input stream
       - 1000          : timeout in milliseconds
       - WAIT_OBJECT_0 : returned when input becomes available

       _kbhit() then checks if a key is actually present in the input buffer.

       If both are true → a key was pressed.
    */
   return WaitForSingleObject(hStdin, 1000) == WAIT_OBJECT_0 && _kbhit();
}
#endif


/* handle SIGINT (interrupt signal); SIGINT = 2 */
void handle_interrupt(int signal){
    restore_input_buffering();
    printf("\n");
    exit(-2); /* program terminated */
}

uint16_t sign_extend(uint16_t x, int bit_count){
    /*
        Check the sign bit of the value.

        We shift the number so that the sign bit moves to the rightmost
        position, then mask it with 1 to keep only that bit.

        Result:
            0 → sign bit is 0 → value is positive (no extension needed)
            1 → sign bit is 1 → value is negative (extend with 1s)
    */
    if((x >> (bit_count - 1)) & 1){
        /*
            Perform sign-extension for a negative value.

            0xFFFF is a 16-bit value where all bits are 1.
            When we shift it left by bit_count, the upper bits remain 1
            and the lower <bit_count> bits become 0.

                Example (bit_count = 5):
                    0xFFFF << 5  →  1111 1111 1110 0000

            We then OR this mask with x:

                • the upper bits become 1 (this extends the sign)
                • the lower bits remain exactly the same value that x already had,
                because the mask contains 0s in those positions

            |= is shorthand for:
                x = x | (0xFFFF << bit_count);
        */
        x |= (0xFFFF << bit_count);
    }
    return x;
}

uint16_t swap16(uint16_t x){
    /*
        A 16-bit value has two bytes (8 bits each).

        • Left shift by 8  → moves the rightmost 8 bits to the left
        • Right shift by 8 → moves the leftmost 8 bits to the right

        OR-ing the two results combines them, effectively swapping the bytes.
    */
    return (x << 8) | (x >> 8);
}

void update_flags(uint16_t r){
    if(reg[r] == 0)
    {
        reg[R_COND] = FL_ZRO;
    } 
    else if(reg[r] >> 15) /* a 1 in the left-most bit (bit 15 == 1) indicates the value is negative */
    {
        reg[R_COND] = FL_NEG;
    } 
    else
    {
        reg[R_COND] = FL_POS;
    }
}

/*
    An LC-3 image file consists of 16-bit words.

    The first 16-bit word is special — it is the origin address.
    The origin specifies where in LC-3 memory the program should be loaded.

    After reading the origin, all remaining words in the file are loaded
    sequentially into memory starting at that address.

    This allows the CPU to know where the program begins and execute it correctly.
*/
void read_image_file(FILE *file){
    uint16_t origin;
    /* Reads 1 value of size sizeof(origin) (16 bits) from the file into origin */
    fread(&origin, sizeof(origin), 1, file);
    /*
        LC-3 image files store 16-bit values in big-endian format (high byte first).
        Most host machines (windows/mac/linux) use little-endian format (low byte first).

        Example value: 0x1234

        In LC-3 image file (big-endian):
            bytes:  0x12 0x34

        If read directly on a little-endian host (incorrect):
            interpreted value: 0x3412

        After swap16():
            bytes reordered to little-endian
            interpreted value: 0x1234 (correct)

        swap16() converts file byte order into host byte order.
    */
    origin = swap16(origin);

    /* 
        LC-3 memory has 65,536 locations (words).
        The program starts at `origin`, so the maximum number of 16-bit words we can load is MEMORY_MAX - origin.
    */
    uint16_t max_read = MEMORY_MAX - origin;

    /* Set pointer to the starting memory location where the program should be loaded (memory[origin]). */
    uint16_t *p = memory + origin;
    
    /* 
        Read sequential 16-bit words from the object file and copy them into LC-3 memory starting at `origin`, stopping at EOF (End Of File) or when memory space runs out.
    */
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

   /*
        `read` is the number of 16-bit words read from the file.
        The LC-3 object file stores words in big-endian format.
        Since the host machine is little-endian, each word must be byte-swapped.
    */
    while(read-- > 0){
        /* swap the 16-bit word in place */
        *p = swap16(*p);
        ++p;
    }
}

int read_image(const char* image_path){ 
    /* fopen returns:
       - a valid FILE pointer if the file exists
       - NULL if the file cannot be opened 
    */
    FILE *file = fopen(image_path, "rb");

    /* if file is NULL → image not found / cannot open */
    if(!file) { return 0; };

    /* read LC-3 image contents into memory */
    read_image_file(file);

    fclose(file);
    return 1; /* image read successfully */
}

/*
    Memory-mapped registers represent hardware (KBSR, KBDR), not normal RAM.
    So we do not access them directly.
    Instead, mem_read and mem_write act like getters and setters,
    handling keyboard input through these special memory addresses.
*/
void mem_write(uint16_t address, uint16_t val){
    memory[address] = val;
}

uint16_t mem_read(uint16_t address){
    /*
        mem_read behavior summary:

        1) Normal memory address:
        - Simply returns the value stored at memory[address].

        2) Address == MR_KBSR (Keyboard Status Register):
        - Checks whether a key has been pressed on the host system.
        - If a key is pressed:
                • Sets bit 15 of KBSR (ready bit = 1)
                • Reads the character from the host (getchar)
                • Stores that character into MR_KBDR
        - If no key is pressed:
                • Sets KBSR to 0
        - Returns the value of KBSR (0 or ready bit set).

        3) Address == MR_KBDR (Keyboard Data Register):
        - Returns the character previously stored when KBSR indicated a key press.

        KBSR tells whether a key is available.
        KBDR tells which key was pressed.
    */
   if(address == MR_KBSR){
    if(check_key()){
        memory[MR_KBSR] = (1 << 15); /* 1 is moved to the left most bit (ready bit) */
        memory[MR_KBDR] = getchar(); /* read character from host: os retrieves it */
    }
    else{
        memory[MR_KBSR] = 0;
    }
   }

   return memory[address];
}


int main(int argc, const char *argv[]){
    /*
        argc = number of command-line arguments

        argv[0] = program name
        argv[1] = first image file
        argv[2] = second image file (optional), etc.

        We require at least one image file.
        So argc must be >= 2.

        If argc < 2
            → no image file was provided
            → print usage message and exit

        Exit codes:
            exit(0) = success
            exit(1) = program/file error
            exit(2) = invalid usage / missing arguments
    */
   if(argc < 2){
        printf("Error: No image file provided.\n");
        printf("Usage: [program-file] [image-file] ...\n");
        exit(2);
   }

   // start from argv[1] because argv[0] is program name
   for(int j = 1; j<argc; j++){
        if(!read_image(argv[j])){
            printf("Failed to load image: %s\n", argv[j]);
            exit(1);
        }
   }

   
    /* When we press Ctrl+C , the OS sends a SIGINT (interrupt) signal to terminate the program */
    signal(SIGINT, handle_interrupt);

    /* Disable waiting for Enter and disable echoing typed characters */
    disable_input_buffering();

    /*
        Initialize condition flags — only one flag should be active at a time.
        Start in ZRO state so neither POS nor NEG is active.
    */
   reg[R_COND] = FL_ZRO;



    /*
        Set the PC to the program start address.

        0x3000 is the LC-3 default start location.
        The lower memory range (0x0000–0x2FFF) is reserved for system / OS use.

        PC_START is a named constant so the value is clear and cannot change at runtime.
    */
    enum{
        PC_START = 0X3000
    };
    reg[R_PC] = PC_START;

    //The CPU runs in a loop until a TRAP (HALT) instruction sets `running` to 0.
    int running = 1;
    while(running)
    {
        /* FETCH

        reg[R_PC] contains the address of the next instruction.

        We read the 16-bit instruction from memory at that address,
        then increment the PC so it points to the next instruction.

        LC-3 instructions are 16 bits:

            bits 15–12   bits 11–0
            [  OPCODE  |  PARAMETERS ]

        The upper 4 bits (15–12) are the opcode.
        The lower 12 bits (11–0) are the parameters.

        We extract the opcode by shifting the instruction right by 12 bits.

        Example:
            abcd efgh ijkl mnop   (original)
            >> 12
            0000 0000 0000 abcd   (opcode isolated)
        */
       uint16_t instr = mem_read(reg[R_PC]++);
       uint16_t op = instr >> 12;

       switch(op){
        /* Refer LC-3 ISA documentation for the binary layout of each opcode */

        case OP_ADD:
                    {
                        /*
                            ADD instruction format (16 bits) 

                                bits 15–12  → opcode  (0001 = ADD)

                                bits 11–9   → DR   (destination register)
                                bits 8–6    → SR1  (first source register)

                                bit 5       → mode flag
                                            0 = register mode
                                            1 = immediate mode

                                If bit 5 = 0   (register mode)
                                    bits 4–3 are unused (always 00)
                                    bits 2–0 → SR2 (second source register)

                                If bit 5 = 1   (immediate mode)
                                    bits 4–0 → imm5 (5-bit immediate value, needs sign-extension)

                            So ADD has two possible forms:

                                ADD DR, SR1, SR2       (register + register)
                                ADD DR, SR1, imm5      (register + immediate)
                        */
                        /*
                                Example (extracting DR):

                                Instruction:
                                    1001 010 011 100101

                                Shift right by 9:
                                    >> 9
                                    0000 0000 1001 010

                                Mask with 0x7:
                                        0000 0000 1001 010
                                        AND 
                                        0000 0000 0000 111
                                    =   0000 0000 0000 010

                                Result:
                                    010 (binary) = 2 (decimal) → DR = R2
                        */
                        // extract destination register (DR) — bits 11–9
                        uint16_t r0 = (instr >> 9) & 0x7;

                        //extract first operand (SR1) — bits 8–6
                        uint16_t r1 = (instr >> 6) & 0x7;

                        // find out which mode : immediate or register mode
                        uint16_t imm_flag = (instr >> 5) & 0x1;
                        
                        //if/else based on mode
                        if(imm_flag){
                            /*
                                In immediate mode, imm5 is a 5-bit value.

                                The CPU works with 16-bit values, so imm5 must be converted to 16 bits.
                                This is done using sign-extension:

                                    • If the left-most (top) bit of imm5 is 0 → value is positive → pad with 0s
                                    • If the left-most (top) bit of imm5 is 1 → value is negative → pad with 1s

                                The left-most bit of imm5 is called the sign bit.

                                Sign-extension preserves the numeric meaning when expanding a 5-bit value into a 16-bit value.

                                
                                Note: Bit masks used in decoding:

                                    0x1   → 0000 0000 0000 0001   (1 bit mask)
                                    0x7   → 0000 0000 0000 0111   (3 bit mask)
                                    0x1F  → 0000 0000 0001 1111   (5 bit mask)
                                    0x3F  → 0000 0000 0011 1111   (6 bit mask)
                                    0x1FF → 0000 0001 1111 1111   (9 bit mask)
                                    0x7FF → 0000 0111 1111 1111   (11 bit mask)
                                    0xFFFF→ 1111 1111 1111 1111   (all 16 bits set to 1)
                            */
                            uint16_t imm5 = sign_extend(instr & 0x1F, 5);
                            reg[r0] = reg[r1] + imm5; //using + (ADD)
                        } 
                        else{
                            /*
                                Register mode: extract SR2 (second source register).
                                SR2 is stored in bits 2–0 (the lowest 3 bits of the instruction).
                                We mask with 0x7 so that only these 3 bits are kept.
                            */
                            uint16_t r2 = instr & 0x7;
                            reg[r0] = reg[r1] + reg[r2]; //using + (ADD)
                        }

                        update_flags(r0);
                    }
                    break;
        case OP_AND:
                    {
                        uint16_t r0 = (instr >> 9) & 0x7;
                        uint16_t r1 = (instr >> 6) & 0x7;
                        uint16_t imm_flag = (instr >> 5) & 0x1;
        
                        if(imm_flag){
                            uint16_t imm5 = sign_extend(instr & 0x1F, 5);
                            reg[r0] = reg[r1] & imm5; //using & (AND)
                        } 
                        else{
                            uint16_t r2 = instr & 0x7;
                            reg[r0] = reg[r1] & reg[r2]; //using & (AND)
                        }

                        update_flags(r0);
                    }
                    break;
        case OP_NOT:
                    {
                        uint16_t r0 = (instr >> 9) & 0x7;
                        uint16_t r1 = (instr >> 6) & 0x7;

                        reg[r0] = ~reg[r1];
                        update_flags(r0);
                    }
                    break;
        case OP_BR:
                    {
                        /*
                            BR — Conditional Branch

                            After every operation, the CPU sets a condition flag
                            (NEGATIVE / ZERO / POSITIVE) in R_COND.

                            The BR instruction contains:
                                • NZP bits — which flags are allowed to branch
                                • PCoffset9 — where to jump if branch is taken

                            We check:
                                cond_flag  (NZP bits from instruction)
                                reg[R_COND] (current result flag)

                            If (cond_flag & reg[R_COND]) != 0
                                → the requested condition matches the current flag
                                → branch is taken: jump to PC + PCoffset9

                            If the condition does not match
                                → branch is NOT taken
                                → execution continues to the next instruction
                        */
                       uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                       uint16_t cond_flag = (instr >> 9 ) & 0x7;

                       if(cond_flag & reg[R_COND]){
                            reg[R_PC] +=pc_offset;
                       }
                    }
                    break;
        case OP_JMP:
                    {
                        /* Also handles RET (special case when r1 is 7) */
                        /* Extracting BaseR () — bits 8–6 */
                        uint16_t r1 = (instr >> 6) & 0x7;
                        reg[R_PC] = reg[r1];
                    }
                    break;
        case OP_JSR:
                    {
                        /*
                            JSR  → jump to subroutine using a PC-relative offset.
                            JSRR → jump to subroutine using an address in a register.
                        */
                        uint16_t long_flag = (instr >> 11) & 1;
                        // save current PC into R7 so RET can return to this instruction.
                        reg[R_R7] = reg[R_PC];

                        if(long_flag){
                            uint16_t long_pc_offset = sign_extend(instr & 0x7FF, 11);
                            reg[R_PC] += long_pc_offset; /* JSR: jump using PC-relative offset (like BR), but return address was saved in R7. */
                        }
                        else{
                            uint16_t r1 = (instr >> 6) & 0x7;
                            reg[R_PC] = reg[r1]; /* JSRR: jump to address in r1 (like JMP), return address was saved earlier in R7. */
                        }
                    }
                    break;
        case OP_LD:
                    {
                        /*
                            LD — Load (PC-relative)

                            1) Take PCoffset9 from the instruction
                            2) Sign-extend it and add it to the current PC → forms an address
                            3) Read the value stored at that memory address
                            4) Store that value into the destination register (DR)
                            5) Update condition flags based on the loaded value
                        */

                        /* Extracting DR () — bits 11–9 */
                        uint16_t r0 = (instr >> 9) & 0x7;

                        /* Masking and sign extending pc_offset*/
                        uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);

                        reg[r0] = mem_read(reg[R_PC]+ pc_offset);
                        update_flags(r0);
                    }
                    break;
        case OP_LDI:
                    {
                        // extract destination register (DR) — bits 11–9
                        uint16_t r0 = (instr >> 9) & 0x7; 

                        /*
                            LDI — Load Indirect

                            Opcode 1010 identifies the LDI instruction.
                            Bits 11–9 specify DR (the destination register).
                            Bits 8–0 form PCoffset9 — a 9-bit signed offset.

                            Execution steps:

                                1) Sign-extend PCoffset9 and add it to the current PC
                                → this produces address1

                                2) Read memory[address1]
                                → this value is an address (address2)

                                3) Read memory[address2]
                                → this is the final 16-bit value

                                4) Store that value into DR

                            It is called "indirect" because the instruction loads a value
                            by first reading an address from memory, then using that address
                            to fetch the actual data.
                        */
                       uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);

                       /*
                            1) compute address1 = PC + pc_offset
                            2) read memory[address1]  → gets address2
                            3) read memory[address2]  → load final value into DR
                        */
                       reg[r0] = mem_read(mem_read(reg[R_PC] + pc_offset));
                       update_flags(r0);
                    }
                    break;
        case OP_LDR:
                    {
                        /* LDR — Load Register (Base + Offset) */

                        /* Extracting DR () — bits 11–9 */
                        uint16_t r0 = (instr >> 9) & 0x7;

                        /* Extracting BaseR () — bits 8–6 */
                        uint16_t r1 = (instr >> 6) & 0x7;

                        /* Masking and sign extending offset6 (bits 5–0 (signed)) */
                        uint16_t offset = sign_extend(instr & 0x3F, 6);

                        /* load from memory[ BaseR + offset ] */
                        reg[r0] = mem_read(reg[r1] + offset);
                        update_flags(r0);
                    }
                     break;
        case OP_LEA:
                    {
                        /*
                            LEA — Load Effective Address

                            The instruction does NOT load data from memory.

                            Instead, it:
                                1) Sign-extends PCoffset9
                                2) Adds it to the current PC to compute an address
                                3) Stores that computed address into DR
                                4) Updates condition flags

                            DR now holds an address (a pointer), which can later be used with
                            LDR / STR or other memory-access instructions.
                        */

                        /* Extracting DR () — bits 11–9 */
                        uint16_t r0 = (instr >> 9) & 0x7;

                        /* Masking and sign extending offset9 (bits 8–0 (signed)) */
                        uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);

                        reg[r0] = reg[R_PC] + pc_offset;
                        update_flags(r0);
                    }
                    break;
        case OP_ST:
                    {
                        /*
                            ST — Store (PC-relative)

                            The instruction provides:
                                • SR         — source register (the register whose value will be stored)
                                • PCoffset9  — 9-bit signed offset

                            Execution:

                                1) Sign-extend PCoffset9
                                2) Add it to the current PC to compute an address
                                3) Take the value from SR
                                4) Store that value into memory[address]

                            Difference vs LD:
                                • LD  → loads data from memory into a register
                                • ST  → stores data from a register into memory
                        */

                        /* Extracting SR () — bits 11–9 */
                        uint16_t r0 = (instr >> 9) & 0x7;

                        /* Masking and sign extending offset9 (bits 8–0 (signed)) */
                        uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);

                        /* PC + offset → target address; write contents of SR to memory */
                        mem_write(reg[R_PC] + pc_offset, reg[r0]);
                    }
                    break;
        case OP_STI:
                    {
                        /* similar to ST, but indirect:
                            (PC + offset9) gives address1,
                            memory[address1] contains address2,
                            value from SR is stored into memory[address2] 
                        */
                        uint16_t r0 = (instr >> 9) & 0x7;
                        uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                        mem_write(mem_read(reg[R_PC] + pc_offset), reg[r0]);
                    }
                    break;
        case OP_STR:
                    {
                        /* 
                            similar to ST, but base-relative (not PC-relative):
                            (BaseR + offset6) gives the address,
                            and the contents of SR are stored in memory[address]
                        */
                        uint16_t r0 = (instr >> 9) & 0x7;
                        uint16_t r1 = (instr >> 6) & 0x7;
                        uint16_t offset = sign_extend(instr & 0x3F, 6);
                        mem_write(reg[r1] + offset, reg[r0]);
                    }
                    break;
        case OP_TRAP:
                    {
                        /* TRAP routines — execute an operating-system routine identified by trapvect8 (I/O or system service call). */

                        // save current PC into R7 so RET can return to this instruction.
                        reg[R_R7] = reg[R_PC];

                        switch(instr & 0xFF){
                            case TRAP_GETC:
                                            {
                                                /*
                                                    TRAP GETC:
                                                    Reads one character from keyboard, converts it to 16-bit,
                                                    and stores it in R0 (LC-3 convention: return value is always in R0).
                                                */
                                               reg[R_R0] = (uint16_t) getchar();
                                               update_flags(R_R0); /* GETC writes a value into R0, so condition flags must be updated */
                                            }
                                            break;
                            case TRAP_OUT:
                                            {
                                                /* TRAP_OUT outputs the single character stored in R0 to the console */
                                                putc((char) reg[R_R0], stdout);
                                                fflush(stdout);
                                            }
                                            break;
                            case TRAP_PUTS:
                                            {
                                                /*
                                                    TRAP_PUTS — each character of the string occupies one 16-bit
                                                    memory location. For printing, each 16-bit value is converted
                                                    to a char and output one character at a time.
                                                */

                                                /*
                                                    R0 contains the starting address of the string in LC-3 memory.
                                                    memory is an array beginning at address 0, so memory + R0 means
                                                    “start reading from memory[R0]”, i.e., the first character’s location. 
                                                    Note:
                                                        c = address
                                                        *c = data stored at that address
                                                */
                                                uint16_t *c = memory + reg[R_R0];

                                                while(*c) /* loop until we hit the null terminator (value 0) */
                                                {
                                                    putc((char) *c, stdout); /* convert 16-bit word → 8-bit char and print */
                                                    ++c;                     /* move to next memory location */
                                                }

                                                /* flush buffer so output appears immediately */
                                                fflush(stdout);
                                            }
                                            break;
                            case TRAP_IN:
                                            {
                                                /* TRAP_IN reads one character from the keyboard and stores it in R0 */
                                                printf("Enter a character: ");
                                                char c = getchar();
                                                putc(c, stdout);
                                                fflush(stdout);
                                                reg[R_R0] = (uint16_t)c;
                                                update_flags(R_R0);
                                            }
                                            break;
                            case TRAP_PUTSP:
                                            {
                                                /*
                                                    TRAP_PUTSP — Output packed string

                                                    Unlike TRAP_PUTS (where each memory location holds one 16-bit character),
                                                    TRAP_PUTSP packs two characters into one 16-bit memory location.

                                                        • Rightmost 8 bits  → first character
                                                        • Leftmost  8 bits  → second character

                                                    This follows LC-3’s big-endian layout.

                                                    We extract and print:
                                                        1) the rightmost 8 bits (low byte)
                                                        2) then the leftmost 8 bits (high byte, if non-zero)

                                                    This is why the bytes appear to be “swapped” during output.
                                                */

                                                /* extract the 16-bit word consisting 2 characters */
                                                uint16_t *c = memory + reg[R_R0];

                                                while(*c) // loop until no characters remain (0)
                                                {
                                                    /* extract the low/right 8 bits (first character) */
                                                    char char1 = (*c) & 0xFF;
                                                    putc(char1, stdout);
                                                    /* extract the high/left 8 bits (second character) */
                                                    char char2 = (*c) >> 8;
                                                    if(char2) putc(char2, stdout); //do this if char2 != 0
                                                    ++c;
                                                }
                                                fflush(stdout);
                                            }
                                            break;
                            case TRAP_HALT:
                                            {
                                                /* 
                                                    HALT — stop execution of the virtual machine.
                                                    Print "HALT", flush output so it appears immediately,
                                                    then set running = 0 to exit the main execution loop.
                                                */
                                                puts("HALT");
                                                fflush(stdout);
                                                running = 0; // terminate the main while loop
                                            }
                                            break;
                        }
                    }
                    break;
        case OP_RES:
        case OP_RTI:
        default:
                    /* 
                        OP_RES and OP_RTI are reserved / not used in this VM.
                        If execution reaches here, something went wrong → abort.
                    */
                    abort();
                    break;

       }
    }

    /* restore terminal input settings before program exits */
    restore_input_buffering();

    return 0;
}