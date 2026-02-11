/* Linker script for Texas Instruments CC2630F128 */
/* Based on TI SimpleLink SDK linker scripts */

/* Memory Configuration */
MEMORY
{
    /* CC2630F128: 128KB Flash, 20KB RAM */

    /* Flash memory (128KB total) */
    FLASH (RX) : ORIGIN = 0x00000000, LENGTH = 0x0001FFA8

    /* CCFG (Customer Configuration) at end of flash */
    FLASH_CCFG (RX) : ORIGIN = 0x0001FFA8, LENGTH = 88

    /* SRAM (20KB total) */
    /* Reserve space for stack at top of RAM */
    SRAM (RWX) : ORIGIN = 0x20000000, LENGTH = 0x00004800  /* 18KB for data/bss */
    STACK (RW) : ORIGIN = 0x20004800, LENGTH = 0x00000800  /* 2KB for stack */
}

/* Section Configuration */
SECTIONS
{
    /* Code and read-only data */
    .text :
    {
        _text = .;
        KEEP(*(.vectors))        /* Interrupt vectors */
        *(.text*)                /* Code */
        *(.rodata*)              /* Read-only data */

        /* C++ exception handling (not used, but keep for compatibility) */
        *(.ARM.extab*)
        *(.ARM.exidx*)

        _etext = .;
    } > FLASH

    /* Initialized data (copied from flash to RAM at startup) */
    .data :
    {
        _data = .;
        *(.data*)
        _edata = .;
    } > SRAM AT > FLASH

    /* Load address of .data in flash */
    _ldata = LOADADDR(.data);

    /* RAM-resident functions (copied from flash to RAM at startup) */
    .ramfunc :
    {
        _ramfunc = .;
        *(.ramfunc*)
        _eramfunc = .;
    } > SRAM AT > FLASH

    /* Load address of .ramfunc in flash */
    _lramfunc = LOADADDR(.ramfunc);

    /* Uninitialized data (zeroed at startup) */
    .bss :
    {
        _bss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > SRAM

    /* Stack */
    .stack :
    {
        _stack_start = .;
        . = . + 0x800;  /* 2KB stack */
        _stack_end = .;
    } > STACK

    /* Heap (if needed) */
    .heap :
    {
        _heap_start = .;
        end = .;  /* Required by newlib */
        _end = .;
        __end = .;
        . = ORIGIN(SRAM) + LENGTH(SRAM);
        _heap_end = .;
    } > SRAM

    /* Customer Configuration (CCFG) */
    .ccfg :
    {
        KEEP(*(.ccfg))
    } > FLASH_CCFG

    /* Discard unnecessary sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.debug*)
    }
}

/* Entry point */
ENTRY(Reset_Handler)

/* Stack pointer initial value (top of stack) */
_estack = ORIGIN(STACK) + LENGTH(STACK);

/* Provide symbols for startup code */
PROVIDE(_stack = _estack);
PROVIDE(__stack = _estack);
