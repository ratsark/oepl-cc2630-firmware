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
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
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
    // CC2630 peripheral interrupts
    GPIO_Handler,       // 16
    I2C_Handler,        // 17
    RF_CPE0_Handler,    // 18
    0,                  // 19
    RF_CPE1_Handler,    // 20
    0, 0,               // 21-22
    SSI0_Handler,       // 23
    SSI1_Handler,       // 24
    UART0_Handler,      // 25
    0, 0, 0, 0, 0,     // 26-30
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

void Default_Handler(void)
{
    while (1) { __asm volatile ("nop"); }
}
