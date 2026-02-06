// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include "oepl_hw_abstraction_cc2630.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// TI SimpleLink SDK includes (adjust based on actual SDK structure)
// These will need to be verified against the installed SDK
#ifdef SIMPLELINK_SDK_AVAILABLE
#include "ti/devices/cc13x1_cc26x1/driverlib/ssi.h"
#include "ti/devices/cc13x1_cc26x1/driverlib/gpio.h"
#include "ti/devices/cc13x1_cc26x1/driverlib/ioc.h"
#include "ti/devices/cc13x1_cc26x1/driverlib/prcm.h"
#include "ti/devices/cc13x1_cc26x1/driverlib/aon_batmon.h"
#include "ti/devices/cc13x1_cc26x1/driverlib/sys_ctrl.h"
#include "ti/devices/cc13x1_cc26x1/driverlib/watchdog.h"
#include "ti/devices/cc13x1_cc26x1/driverlib/uart.h"
#include "ti/devices/cc13x1_cc26x1/inc/hw_memmap.h"
#include "ti/devices/cc13x1_cc26x1/inc/hw_ints.h"
#endif

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

// SPI peripheral configuration
#define SPI_BASE                SSI0_BASE
#define SPI_BITRATE             4000000  // 4 MHz

// Pin assignments (typical for CC2630 e-paper displays)
// These are DIO pin numbers - adjust based on actual hardware
#define PIN_SPI_CLK             10  // DIO10 - SPI CLK
#define PIN_SPI_MOSI            9   // DIO9 - SPI MOSI
#define PIN_SPI_CS              11  // DIO11 - Chip Select
#define PIN_DISPLAY_DC          12  // DIO12 - Data/Command
#define PIN_DISPLAY_RST         13  // DIO13 - Reset
#define PIN_DISPLAY_BUSY        14  // DIO14 - Busy (input)
#define PIN_UART_TX             3   // DIO3 - UART TX for debug

// GPIO base addresses (from GPIO_PINOUT.md)
#define GPIO_BASE_ADDR          0x20000670
#define GPIO_DC_RST_OFFSET      0x10
#define GPIO_BUSY_OFFSET        0x12

// Direct GPIO access (memory-mapped)
#define GPIO_DC_RST             (*(volatile uint8_t*)(GPIO_BASE_ADDR + GPIO_DC_RST_OFFSET))
#define GPIO_BUSY               (*(volatile uint8_t*)(GPIO_BASE_ADDR + GPIO_BUSY_OFFSET))

// RST bit mask (shares register with DC)
#define RST_BIT                 0x40

// Debug output configuration
#define DEBUG_UART_ENABLE       1  // Set to 1 to enable UART debug output
#define DEBUG_UART_BAUD         115200

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------
static bool hw_initialized = false;
static uint32_t system_time_ms = 0;

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void configure_clocks(void);
static void configure_gpio_ioc(void);
static void timer_init(void);
static void uart_init(void);
static void uart_write(const char* str, size_t len);

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void oepl_hw_init(void)
{
    if (hw_initialized) {
        return;
    }

    // Configure system clocks
    configure_clocks();

    // Initialize GPIO and IOC
    configure_gpio_ioc();

    // Initialize UART for debug output (do this early!)
    uart_init();

    // Initialize system timer
    timer_init();

    // Initialize watchdog (disabled for now)
    // oepl_hw_watchdog_init();

    hw_initialized = true;

    oepl_hw_debugprint(DBG_SYSTEM, "\n\n=== CC2630 HAL initialized ===\n");
}

void oepl_hw_spi_init(void)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    // Enable SSI0 peripheral clock
    PRCMPeripheralRunEnable(PRCM_PERIPH_SSI0);
    PRCMLoadSet();
    while (!PRCMLoadGet());

    // Configure SPI pins via IOC
    IOCPortConfigureSet(PIN_SPI_MOSI, IOC_PORT_MCU_SSI0_TX, IOC_STD_OUTPUT);
    IOCPortConfigureSet(PIN_SPI_CLK, IOC_PORT_MCU_SSI0_CLK, IOC_STD_OUTPUT);

    // Configure chip select as GPIO output
    IOCPortConfigureSet(PIN_SPI_CS, IOC_PORT_GPIO, IOC_STD_OUTPUT);
    GPIO_setOutputEnableDio(PIN_SPI_CS, GPIO_OUTPUT_ENABLE);
    GPIO_setDio(PIN_SPI_CS);  // CS high (inactive)

    // Configure SSI0 in SPI master mode
    SSIConfigSetExpClk(SPI_BASE,
                       SysCtrlClockGet(),
                       SSI_FRF_MOTO_MODE_0,
                       SSI_MODE_MASTER,
                       SPI_BITRATE,
                       8);  // 8-bit data

    // Enable SSI
    SSIEnable(SPI_BASE);

    oepl_hw_debugprint(DBG_SYSTEM, "SPI initialized\n");
#else
    // Placeholder for when SDK is not available
    oepl_hw_debugprint(DBG_SYSTEM, "SPI init (stub)\n");
#endif
}

void oepl_hw_spi_transfer(const uint8_t* data, size_t len)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    // Assert chip select (active low)
    GPIO_clearDio(PIN_SPI_CS);

    for (size_t i = 0; i < len; i++) {
        // Write data
        SSIDataPut(SPI_BASE, data[i]);

        // Wait for transfer to complete
        while (SSIBusy(SPI_BASE));

        // Read dummy byte to clear RX FIFO
        uint32_t dummy;
        SSIDataGet(SPI_BASE, &dummy);
    }

    // Deassert chip select
    GPIO_setDio(PIN_SPI_CS);
#else
    (void)data;
    (void)len;
#endif
}

void oepl_hw_spi_transfer_read(uint8_t* data, size_t len)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    for (size_t i = 0; i < len; i++) {
        // Write dummy byte to generate clock
        SSIDataPut(SPI_BASE, 0xFF);

        // Wait for transfer to complete
        while (SSIBusy(SPI_BASE));

        // Read received byte
        uint32_t rx_data;
        SSIDataGet(SPI_BASE, &rx_data);
        data[i] = (uint8_t)rx_data;
    }
#else
    (void)data;
    (void)len;
#endif
}

void oepl_hw_gpio_init(void)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    // Enable GPIO peripheral clock
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMLoadSet();
    while (!PRCMLoadGet());

    // Configure display control pins via IOC
    // DC (Data/Command) - output
    IOCPortConfigureSet(PIN_DISPLAY_DC, IOC_PORT_GPIO, IOC_STD_OUTPUT);
    GPIO_setOutputEnableDio(PIN_DISPLAY_DC, GPIO_OUTPUT_ENABLE);
    GPIO_clearDio(PIN_DISPLAY_DC);  // Start in command mode

    // RST (Reset) - output
    IOCPortConfigureSet(PIN_DISPLAY_RST, IOC_PORT_GPIO, IOC_STD_OUTPUT);
    GPIO_setOutputEnableDio(PIN_DISPLAY_RST, GPIO_OUTPUT_ENABLE);
    GPIO_setDio(PIN_DISPLAY_RST);  // Start with reset inactive (high)

    // BUSY - input
    IOCPortConfigureSet(PIN_DISPLAY_BUSY, IOC_PORT_GPIO, IOC_INPUT_ENABLE);
    GPIO_setOutputEnableDio(PIN_DISPLAY_BUSY, GPIO_OUTPUT_DISABLE);

    oepl_hw_debugprint(DBG_SYSTEM, "Display GPIO pins configured\n");
#endif
}

void oepl_hw_gpio_set(uint8_t pin, bool level)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    if (level) {
        GPIO_setDio(pin);
    } else {
        GPIO_clearDio(pin);
    }
#else
    (void)pin;
    (void)level;
#endif
}

bool oepl_hw_gpio_get(uint8_t pin)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    return (GPIO_readDio(pin) != 0);
#else
    (void)pin;
    return false;
#endif
}

void oepl_hw_delay_ms(uint32_t ms)
{
    // Simple delay loop
    // For production, use SysCtrlDelay() from TI driverlib
    for (uint32_t i = 0; i < ms; i++) {
        oepl_hw_delay_us(1000);
    }
}

void oepl_hw_delay_us(uint32_t us)
{
    // Simple delay loop
    // CC2630 runs at 48 MHz
    // Each loop iteration takes approximately 3 cycles
    // So we need roughly 16 iterations per microsecond
    volatile uint32_t delay = us * 16;
    while (delay--) {
        __asm volatile ("nop");
    }
}

uint32_t oepl_hw_get_time_ms(void)
{
    return system_time_ms;
}

bool oepl_hw_get_temperature(int8_t* temp_degc)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    // Read temperature from CC2630 internal sensor
    int32_t temp = AONBatMonTemperatureGetDegC();
    *temp_degc = (int8_t)temp;
    return true;
#else
    *temp_degc = 25;  // Default room temperature
    return true;
#endif
}

bool oepl_hw_get_voltage(uint16_t* voltage_mv)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    // Read battery voltage
    uint32_t voltage = AONBatMonBatteryVoltageGet();
    // Convert to millivolts (AON returns in units of (1/256)V)
    *voltage_mv = (uint16_t)((voltage * 1000) / 256);
    return true;
#else
    *voltage_mv = 3000;  // Default 3.0V
    return true;
#endif
}

void oepl_hw_set_led(uint8_t color, bool on)
{
    // LED control - depends on hardware
    // TG-GR6000N may not have user-controllable LED
    (void)color;
    (void)on;
}

void oepl_hw_enter_deepsleep(void)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    // Enter deep sleep mode
    PRCMDeepSleep();
#else
    oepl_hw_debugprint(DBG_SYSTEM, "Entering deep sleep (stub)\n");
#endif
}

uint8_t oepl_hw_get_hwid(void)
{
    // Return hardware ID for TG-GR6000N
    return HWID_TG_GR6000N;
}

bool oepl_hw_get_screen_properties(size_t* x, size_t* y, size_t* bpp)
{
    *x = 600;
    *y = 448;
    *bpp = 1;  // Monochrome
    return true;
}

void oepl_hw_debugprint(debug_level_t level, const char* fmt, ...)
{
#if DEBUG_UART_ENABLE
    char buffer[256];
    va_list args;

    // Prefix based on debug level
    const char* prefix[] = {
        "[SYS] ",
        "[RADIO] ",
        "[DISP] ",
        "[NVM] ",
        "[APP] "
    };

    if (level < sizeof(prefix) / sizeof(prefix[0])) {
        strcpy(buffer, prefix[level]);
        size_t offset = strlen(buffer);

        va_start(args, fmt);
        vsnprintf(buffer + offset, sizeof(buffer) - offset, fmt, args);
        va_end(args);

        // Output to UART
        uart_write(buffer, strlen(buffer));
    }
#else
    (void)level;
    (void)fmt;
#endif
}

void oepl_hw_crash(const char* message)
{
    oepl_hw_debugprint(DBG_SYSTEM, "CRASH: %s\n", message);

    // Disable interrupts
    __asm volatile ("cpsid i");

    // Infinite loop
    while (1) {
        oepl_hw_delay_ms(1000);
    }
}

void oepl_hw_watchdog_init(void)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    // Initialize watchdog timer
    // Note: CC26x1 API doesn't use base address
    WatchdogReloadSet(0xFFFFFFFF);
    WatchdogEnable();
#endif
}

void oepl_hw_watchdog_feed(void)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    // Feed watchdog
    WatchdogReloadSet(0xFFFFFFFF);
#endif
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

static void configure_clocks(void)
{
#ifdef SIMPLELINK_SDK_AVAILABLE
    // SetupTrimDevice() already configured clocks and power domains
    // Just ensure peripheral domain is powered on
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);

    // Brief delay to let power domain stabilize (don't use infinite loop!)
    for (volatile uint32_t i = 0; i < 10000; i++) {
        __asm volatile ("nop");
    }
#endif
}

static void configure_gpio_ioc(void)
{
    // GPIO and IOC configuration based on TG-GR6000N pinout
    // This uses direct memory access as documented in GPIO_PINOUT.md

    // The TG-GR6000N uses memory-mapped GPIO control at:
    // - 0x20000670 + 0x10 for DC/RST
    // - 0x20000670 + 0x12 for BUSY

    // Initialize to known state
    GPIO_DC_RST = 0x00;  // DC=0, RST=0

    // Set RST high (normal operation)
    GPIO_DC_RST |= RST_BIT;
}

static void timer_init(void)
{
    // Initialize system timer for millisecond tracking
    // This would typically use RTC or AON timer
    system_time_ms = 0;
}

static void uart_init(void)
{
#if DEBUG_UART_ENABLE && defined(SIMPLELINK_SDK_AVAILABLE)
    // Enable UART peripheral clock
    PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
    PRCMLoadSet();
    while (!PRCMLoadGet());

    // Configure UART TX pin via IOC
    IOCPortConfigureSet(PIN_UART_TX, IOC_PORT_MCU_UART0_TX, IOC_STD_OUTPUT);

    // Disable UART
    UARTDisable(UART0_BASE);

    // Configure UART: 115200 baud, 8N1
    UARTConfigSetExpClk(UART0_BASE,
                       SysCtrlClockGet(),
                       DEBUG_UART_BAUD,
                       UART_CONFIG_WLEN_8 |
                       UART_CONFIG_STOP_ONE |
                       UART_CONFIG_PAR_NONE);

    // Enable UART
    UARTEnable(UART0_BASE);

    // Send startup message
    const char* msg = "\n\n*** TG-GR6000N Firmware Starting ***\n";
    uart_write(msg, strlen(msg));
#endif
}

static void uart_write(const char* str, size_t len)
{
#if DEBUG_UART_ENABLE && defined(SIMPLELINK_SDK_AVAILABLE)
    for (size_t i = 0; i < len; i++) {
        // Wait for space in TX FIFO
        while (UARTBusy(UART0_BASE));

        // Send character
        UARTCharPut(UART0_BASE, str[i]);
    }
#else
    (void)str;
    (void)len;
#endif
}
