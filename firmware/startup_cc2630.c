// -----------------------------------------------------------------------------
//                   CC2630 Startup Code and Interrupt Vectors
// -----------------------------------------------------------------------------

#include <stdint.h>
#include <driverlib/setup.h>
#include <driverlib/gpio.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>

// -----------------------------------------------------------------------------
//                          External References
// -----------------------------------------------------------------------------

// Linker symbols (defined in linker script)
extern uint32_t _estack;
extern uint32_t _data;
extern uint32_t _edata;
extern uint32_t _ldata;
extern uint32_t _bss;
extern uint32_t _ebss;

// Main function
extern int main(void);

// TI DriverLib trim function (actual symbol name in library)
extern void NOROM_SetupTrimDevice(void);

// -----------------------------------------------------------------------------
//                          Function Declarations
// -----------------------------------------------------------------------------

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

// CC2630 peripheral handlers
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

// -----------------------------------------------------------------------------
//                          Vector Table
// -----------------------------------------------------------------------------

__attribute__((section(".vectors")))
void (* const vectors[])(void) = {
    (void (*)(void))(&_estack),         // Initial stack pointer
    Reset_Handler,                      // Reset handler
    NMI_Handler,                        // NMI handler
    HardFault_Handler,                  // Hard fault handler
    MemManage_Handler,                  // MPU fault handler
    BusFault_Handler,                   // Bus fault handler
    UsageFault_Handler,                 // Usage fault handler
    0,                                  // Reserved
    0,                                  // Reserved
    0,                                  // Reserved
    0,                                  // Reserved
    SVC_Handler,                        // SVCall handler
    DebugMon_Handler,                   // Debug monitor handler
    0,                                  // Reserved
    PendSV_Handler,                     // PendSV handler
    SysTick_Handler,                    // SysTick handler

    // CC2630 Peripheral Interrupts
    GPIO_Handler,                       // 16: GPIO
    I2C_Handler,                        // 17: I2C
    RF_CPE0_Handler,                    // 18: RF Core Command & Packet Engine 0
    0,                                  // 19: Reserved
    RF_CPE1_Handler,                    // 20: RF Core Command & Packet Engine 1
    0,                                  // 21: Reserved
    0,                                  // 22: Reserved
    SSI0_Handler,                       // 23: SSI0
    SSI1_Handler,                       // 24: SSI1
    UART0_Handler,                      // 25: UART0
    0,                                  // 26: Reserved
    0,                                  // 27: Reserved
    0,                                  // 28: Reserved
    0,                                  // 29: Reserved
    0,                                  // 30: Reserved
    Timer0A_Handler,                    // 31: Timer 0A
    Timer0B_Handler,                    // 32: Timer 0B
    Timer1A_Handler,                    // 33: Timer 1A
    Timer1B_Handler,                    // 34: Timer 1B
    Timer2A_Handler,                    // 35: Timer 2A
    Timer2B_Handler,                    // 36: Timer 2B
    Timer3A_Handler,                    // 37: Timer 3A
    Timer3B_Handler,                    // 38: Timer 3B
    Crypto_Handler,                     // 39: Crypto
    DMA_Handler,                        // 40: uDMA software
    DMA_Err_Handler,                    // 41: uDMA error
    Flash_Handler,                      // 42: Flash
    SW_Event0_Handler,                  // 43: Software Event 0
    AUX_Comb_Handler,                   // 44: AUX combined
    AON_Prog_Handler,                   // 45: AON programmable
};

// -----------------------------------------------------------------------------
//                          Reset Handler
// -----------------------------------------------------------------------------

// Test: Manually set stack pointer like stock firmware does
__attribute__((naked))
void Reset_Handler(void)
{
    __asm volatile (
        // FIRST: Manually set stack pointer (like stock firmware)
        // Stock firmware sets SP to 0x200043A8, we'll use 0x20005000 (top of RAM)
        "movw r0, #0x5000\n"        // Low 16 bits
        "movt r0, #0x2000\n"        // High 16 bits (0x20005000)
        "mov sp, r0\n"              // Set stack pointer

        // Now we have a valid stack, can call functions
        "ldr r0, =NOROM_SetupTrimDevice\n"
        "blx r0\n"

        // Disable watchdog
        "ldr r0, =0x40080000\n"
        "ldr r1, =0x1ACCE551\n"
        "str r1, [r0, #0xC00]\n"
        "movs r1, #0\n"
        "str r1, [r0]\n"
        "str r1, [r0, #0xC00]\n"

        // Pull-up test on MULTIPLE pins
        // Try DIO_0, DIO_1, DIO_2, DIO_10, DIO_13, DIO_27
        // IOC base = 0x40081000, each pin at offset pin_num * 4

        "ldr r0, =0x40081000\n"     // IOC base
        "movw r1, #0x6000\n"        // Pull-up config

        "str r1, [r0, #0x00]\n"     // DIO_0
        "str r1, [r0, #0x04]\n"     // DIO_1
        "str r1, [r0, #0x08]\n"     // DIO_2
        "str r1, [r0, #0x28]\n"     // DIO_10
        "str r1, [r0, #0x34]\n"     // DIO_13
        "str r1, [r0, #0x6C]\n"     // DIO_27

        "loop: b loop\n"
        : : : "r0", "r1", "lr", "memory"
    );
}

// -----------------------------------------------------------------------------
//                          Default Handler
// -----------------------------------------------------------------------------

void Default_Handler(void)
{
    // CRASH DETECTED! If we get here, something caused a fault
    // Try to enable pull-up on DIO_27 to signal the crash

    volatile uint32_t *IOC_IOCFG27 = (volatile uint32_t *)(0x40081000 + (27 * 4));
    *IOC_IOCFG27 = 0x00004000;  // Pull-DOWN instead of up (bits 12-11 = 10)

    // Infinite loop
    while (1) {
        __asm volatile ("nop");
    }
}
