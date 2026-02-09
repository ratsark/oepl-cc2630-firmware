// -----------------------------------------------------------------------------
//                   CC2630 Startup Code - BARE METAL
// -----------------------------------------------------------------------------
//
// NO driverlib dependency. The previous startup called SetupTrimDevice from
// the cc13x1_cc26x1 driverlib, which has a chip family check that
// IMMEDIATELY HALTS on CC2630 (it checks wafer ID and enters while(1) if
// not cc13x1/cc26x1 silicon).
//
// Instead, we do the absolute minimum needed to get the CPU running:
// 1. Copy .data
// 2. Zero .bss
// 3. Call main
//
// For oscillator/trim configuration, we'll add that later using the
// correct cc26x0 driverlib or direct register access.
//
// -----------------------------------------------------------------------------

#include <stdint.h>

// Linker symbols
extern uint32_t _estack;
extern uint32_t _data;
extern uint32_t _edata;
extern uint32_t _ldata;
extern uint32_t _bss;
extern uint32_t _ebss;

extern int main(void);
extern void NOROM_SetupTrimDevice(void);

// Function declarations
void Reset_Handler(void);
void Default_Handler(void);

// Cortex-M3 core handlers
void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void); // Defined below with RTT diagnostics
void MemManage_Handler(void) __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void) __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void) __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));

// CC2630 peripheral handlers (all default to infinite loop)
void GPIO_Handler(void) __attribute__((weak, alias("Default_Handler")));
void I2C_Handler(void) __attribute__((weak, alias("Default_Handler")));
void RF_CPE0_Handler(void) __attribute__((weak, alias("Default_Handler")));
void RF_CPE1_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SSI0_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SSI1_Handler(void) __attribute__((weak, alias("Default_Handler")));
void UART0_Handler(void) __attribute__((weak, alias("Default_Handler")));
void Timer0A_Handler(void) __attribute__((weak, alias("Default_Handler")));
void Timer0B_Handler(void) __attribute__((weak, alias("Default_Handler")));
void Timer1A_Handler(void) __attribute__((weak, alias("Default_Handler")));
void Timer1B_Handler(void) __attribute__((weak, alias("Default_Handler")));
void Timer2A_Handler(void) __attribute__((weak, alias("Default_Handler")));
void Timer2B_Handler(void) __attribute__((weak, alias("Default_Handler")));
void Timer3A_Handler(void) __attribute__((weak, alias("Default_Handler")));
void Timer3B_Handler(void) __attribute__((weak, alias("Default_Handler")));
void Crypto_Handler(void) __attribute__((weak, alias("Default_Handler")));
void DMA_Handler(void) __attribute__((weak, alias("Default_Handler")));
void DMA_Err_Handler(void) __attribute__((weak, alias("Default_Handler")));
void Flash_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SW_Event0_Handler(void) __attribute__((weak, alias("Default_Handler")));
void AUX_Comb_Handler(void) __attribute__((weak, alias("Default_Handler")));
void AON_Prog_Handler(void) __attribute__((weak, alias("Default_Handler")));

void AON_RTC_Handler(void) __attribute__((weak, alias("Default_Handler")));

// Vector Table
__attribute__((section(".vectors"), used))
void (* const vectors[])(void) = {
    (void (*)(void))(&_estack),
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
    // CC2630 peripheral interrupts (per hw_ints.h)
    GPIO_Handler,       // 16: INT_AON_GPIO_EDGE (IRQ 0)
    I2C_Handler,        // 17: INT_I2C_IRQ (IRQ 1)
    RF_CPE1_Handler,    // 18: INT_RFC_CPE_1 (IRQ 2)
    0,                  // 19: reserved (IRQ 3)
    AON_RTC_Handler,    // 20: INT_AON_RTC_COMB (IRQ 4)
    UART0_Handler,      // 21: INT_UART0_COMB (IRQ 5)
    0,                  // 22: INT_AUX_SWEV0 (IRQ 6)
    SSI0_Handler,       // 23: INT_SSI0_COMB (IRQ 7)
    SSI1_Handler,       // 24: INT_SSI1_COMB (IRQ 8)
    RF_CPE0_Handler,    // 25: INT_RFC_CPE_0 (IRQ 9)
    0, 0, 0, 0, 0,     // 26-30: RFC_HW, RFC_CMD_ACK, I2S, AUX_SWEV1, WDT
    Timer0A_Handler,    // 31
    Timer0B_Handler,    // 32
    Timer1A_Handler,    // 33
    Timer1B_Handler,    // 34
    Timer2A_Handler,    // 35
    Timer2B_Handler,    // 36
    Timer3A_Handler,    // 37
    Timer3B_Handler,    // 38
    Crypto_Handler,     // 39
    DMA_Handler,        // 40
    DMA_Err_Handler,    // 41
    Flash_Handler,      // 42
    SW_Event0_Handler,  // 43
    AUX_Comb_Handler,   // 44
    AON_Prog_Handler,   // 45
};

// Reset Handler - NO SetupTrimDevice!
void Reset_Handler(void)
{
    // CRITICAL: Reset VTOR to flash (0x00000000).
    // The CC2630 ROM bootloader sets VTOR = 0x20000000 and copies its own
    // vector table there. Our .data section also starts at 0x20000000, so
    // when we copy .data below, we'd overwrite the vector table with RTT
    // data, causing any interrupt to crash. Setting VTOR = 0 ensures the
    // CPU uses our vector table in flash.
    *(volatile uint32_t *)0xE000ED08 = 0x00000000;

    // Apply factory trim values - essential for I/O drivers and clocks.
    // Using cc26x0 driverlib (NOT cc13x1_cc26x1 which halts on CC2630).
    NOROM_SetupTrimDevice();

    // Copy .data from flash to RAM
    uint32_t *src = &_ldata;
    uint32_t *dst = &_data;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    // Zero .bss
    dst = &_bss;
    while (dst < &_ebss) {
        *dst++ = 0;
    }

    // Call main
    main();

    while (1) { __asm volatile ("nop"); }
}

// Declared in rtt.c
extern void rtt_puts(const char *s);
extern void rtt_put_hex8(uint8_t v);
extern void rtt_put_hex32(uint32_t v);

void HardFault_Handler(void)
{
    // Read stacked registers from MSP
    uint32_t *sp;
    __asm volatile ("mrs %0, msp" : "=r" (sp));

    rtt_puts("\r\n!!! HARDFAULT !!!\r\n");
    rtt_puts("PC="); rtt_put_hex32(sp[6]); rtt_puts("\r\n");
    rtt_puts("LR="); rtt_put_hex32(sp[5]); rtt_puts("\r\n");
    rtt_puts("SP="); rtt_put_hex32((uint32_t)sp); rtt_puts("\r\n");
    rtt_puts("CFSR="); rtt_put_hex32(*(volatile uint32_t *)0xE000ED28); rtt_puts("\r\n");
    rtt_puts("BFAR="); rtt_put_hex32(*(volatile uint32_t *)0xE000ED38); rtt_puts("\r\n");

    while (1) { __asm volatile ("nop"); }
}

void Default_Handler(void)
{
    rtt_puts("\r\n!!! DEFAULT IRQ !!!\r\n");
    while (1) { __asm volatile ("nop"); }
}
