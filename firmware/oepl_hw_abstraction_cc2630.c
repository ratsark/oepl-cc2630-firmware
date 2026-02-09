// -----------------------------------------------------------------------------
//  CC2630 Hardware Abstraction Layer for OEPL display
//  Bare-metal using TI cc26x0 driverlib
// -----------------------------------------------------------------------------

#include "oepl_hw_abstraction_cc2630.h"
#include "rtt.h"

// TI driverlib
#include "ssi.h"
#include "gpio.h"
#include "ioc.h"
#include "prcm.h"
#include "hw_memmap.h"
#include "hw_types.h"  // for HWREG
#include "aon_batmon.h"

// Pin assignments — from STOCK FIRMWARE binary analysis (v29)
// SPI pins: MOSI/MISO swapped vs OEPL HAL! Stock has mosiPin=9, misoPin=8
#define PIN_SPI_MOSI            9   // DIO9  — SSI0_TX (MOSI) — data TO display
#define PIN_SPI_MISO            8   // DIO8  — SSI0_RX (MISO) — data FROM display
#define PIN_SPI_CLK             10  // DIO10 — SSI0_CLK
// Display control pins — from stock firmware binary analysis
#define PIN_DISPLAY_BUSY        13  // DIO13 — BUSY input (HIGH=ready, LOW=busy)
#define PIN_DISPLAY_RST         14  // DIO14 — Reset (active LOW)
#define PIN_DISPLAY_DC          15  // DIO15 — Data/Command
#define PIN_SPI_CS              20  // DIO20 — EPD display CS
#define PIN_EPD_BS              18  // DIO18 — Bus select (LOW=4-wire SPI)
#define PIN_EPD_DIR             12  // DIO12 — SDA direction (LOW=write, HIGH=read)
#define PIN_EPD_POWER           5   // DIO5  — EPD power enable (tentative)
#define PIN_FLASH_CS            11  // DIO11 — SPI flash CS

#define SPI_BITRATE             4000000  // 4 MHz
#define SYSTEM_CLOCK_HZ         48000000

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void oepl_hw_init(void)
{
    // PERIPH power domain and GPIO clock are already enabled in main.c
}

void oepl_hw_spi_init(void)
{
    // Power up SERIAL domain (required for SSI0)
    PRCMPowerDomainOn(PRCM_DOMAIN_SERIAL);
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL) != PRCM_DOMAIN_POWER_ON);

    // Enable SSI0 peripheral clock
    PRCMPeripheralRunEnable(PRCM_PERIPH_SSI0);
    PRCMLoadSet();
    while (!PRCMLoadGet());

    // Configure SPI pins via IOC
    // IOCPinTypeSsiMaster(base, rxPin, txPin, fssPin, clkPin)
    // Stock firmware: DIO9=TX(MOSI), DIO8=RX(MISO), DIO10=CLK
    IOCPinTypeSsiMaster(SSI0_BASE, PIN_SPI_MISO, PIN_SPI_MOSI,
                        IOID_UNUSED, PIN_SPI_CLK);

    // Configure SSI0: SPI Mode 0, Master, 4MHz, 8-bit
    SSIConfigSetExpClk(SSI0_BASE, SYSTEM_CLOCK_HZ, SSI_FRF_MOTO_MODE_0,
                       SSI_MODE_MASTER, SPI_BITRATE, 8);
    SSIEnable(SSI0_BASE);

    // Drain any stale data from RX FIFO
    uint32_t dummy;
    while (SSIDataGetNonBlocking(SSI0_BASE, &dummy));

    rtt_puts("SPI init OK\r\n");
}

void oepl_hw_spi_cs_assert(void)
{
    GPIO_clearDio(PIN_SPI_CS);
}

void oepl_hw_spi_cs_deassert(void)
{
    GPIO_setDio(PIN_SPI_CS);
}

void oepl_hw_spi_send_raw(const uint8_t* data, size_t len)
{
    // Send bytes without toggling CS
    for (size_t i = 0; i < len; i++) {
        SSIDataPut(SSI0_BASE, data[i]);
        while (SSIBusy(SSI0_BASE));
        uint32_t dummy;
        SSIDataGet(SSI0_BASE, &dummy);
    }
}

void oepl_hw_spi_read_raw(uint8_t* data, size_t len)
{
    // Send 0xFF dummy bytes and capture received data (no CS toggle)
    for (size_t i = 0; i < len; i++) {
        SSIDataPut(SSI0_BASE, 0xFF);
        while (SSIBusy(SSI0_BASE));
        uint32_t rx;
        SSIDataGet(SSI0_BASE, &rx);
        data[i] = (uint8_t)rx;
    }
}

void oepl_hw_spi_transfer(const uint8_t* data, size_t len)
{
    GPIO_clearDio(PIN_SPI_CS);
    oepl_hw_spi_send_raw(data, len);
    GPIO_setDio(PIN_SPI_CS);
}

void oepl_hw_spi_transfer_read(uint8_t* data, size_t len)
{
    GPIO_clearDio(PIN_SPI_CS);

    for (size_t i = 0; i < len; i++) {
        SSIDataPut(SSI0_BASE, 0xFF);
        while (SSIBusy(SSI0_BASE));
        uint32_t rx;
        SSIDataGet(SSI0_BASE, &rx);
        data[i] = (uint8_t)rx;
    }

    GPIO_setDio(PIN_SPI_CS);
}

void oepl_hw_gpio_init(void)
{
    // EPD_BS1 (Bus Select 1) - output, LOW for 4-wire SPI mode
    IOCPinTypeGpioOutput(PIN_EPD_BS);
    GPIO_setOutputEnableDio(PIN_EPD_BS, GPIO_OUTPUT_ENABLE);
    GPIO_clearDio(PIN_EPD_BS);

    // EPD_DIR (SDA direction) - output, LOW for write mode
    IOCPinTypeGpioOutput(PIN_EPD_DIR);
    GPIO_setOutputEnableDio(PIN_EPD_DIR, GPIO_OUTPUT_ENABLE);
    GPIO_clearDio(PIN_EPD_DIR);

    // EPD_POWER - output, HIGH to enable display boost converter
    IOCPinTypeGpioOutput(PIN_EPD_POWER);
    GPIO_setOutputEnableDio(PIN_EPD_POWER, GPIO_OUTPUT_ENABLE);
    GPIO_setDio(PIN_EPD_POWER);

    // DC (Data/Command) - output, start LOW (command mode)
    IOCPinTypeGpioOutput(PIN_DISPLAY_DC);
    GPIO_setOutputEnableDio(PIN_DISPLAY_DC, GPIO_OUTPUT_ENABLE);
    GPIO_clearDio(PIN_DISPLAY_DC);

    // RST (Reset) - output, start HIGH (not in reset)
    IOCPinTypeGpioOutput(PIN_DISPLAY_RST);
    GPIO_setOutputEnableDio(PIN_DISPLAY_RST, GPIO_OUTPUT_ENABLE);
    GPIO_setDio(PIN_DISPLAY_RST);

    // BUSY - input (HIGH=ready, LOW=busy — UC8159 standard)
    IOCPinTypeGpioInput(PIN_DISPLAY_BUSY);
    GPIO_setOutputEnableDio(PIN_DISPLAY_BUSY, GPIO_OUTPUT_DISABLE);

    // EPD CS - output, HIGH (deselected)
    IOCPinTypeGpioOutput(PIN_SPI_CS);
    GPIO_setOutputEnableDio(PIN_SPI_CS, GPIO_OUTPUT_ENABLE);
    GPIO_setDio(PIN_SPI_CS);

    // Flash CS - output, HIGH (deselected, prevent interference)
    IOCPinTypeGpioOutput(PIN_FLASH_CS);
    GPIO_setOutputEnableDio(PIN_FLASH_CS, GPIO_OUTPUT_ENABLE);
    GPIO_setDio(PIN_FLASH_CS);

    // Wait 100ms for EPD boost converter to stabilize
    oepl_hw_delay_ms(100);

    rtt_puts("GPIO OK\r\n");
}

void oepl_hw_gpio_set(uint8_t pin, bool level)
{
    if (level) {
        GPIO_setDio(pin);
    } else {
        GPIO_clearDio(pin);
    }
}

bool oepl_hw_gpio_get(uint8_t pin)
{
    return (GPIO_readDio(pin) != 0);
}

void oepl_hw_delay_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        oepl_hw_delay_us(1000);
    }
}

void oepl_hw_delay_us(uint32_t us)
{
    // CC2630 at 48 MHz. Volatile loop body is ~8 cycles (LDR+SUB+STR+NOP+CMP+BNE).
    // 48 cycles/us / 8 cycles/iter = 6 iterations/us
    volatile uint32_t delay = us * 6;
    while (delay--) {
        __asm volatile ("nop");
    }
}

uint32_t oepl_hw_get_time_ms(void)
{
    return 0;  // Not implemented — not needed for display driver
}

bool oepl_hw_get_temperature(int8_t* temp_degc)
{
    AONBatMonEnable();
    int32_t temp = AONBatMonTemperatureGetDegC();
    *temp_degc = (int8_t)temp;
    return true;
}

bool oepl_hw_get_voltage(uint16_t* voltage_mv)
{
    AONBatMonEnable();
    uint32_t raw = AONBatMonBatteryVoltageGet();
    // Raw format: bits [10:8] = integer volts, bits [7:0] = fraction (0-255)
    uint32_t int_v = (raw >> 8) & 0x7;
    uint32_t frac = raw & 0xFF;
    *voltage_mv = (uint16_t)(int_v * 1000 + (frac * 1000) / 256);
    return true;
}

void oepl_hw_set_led(uint8_t color, bool on)
{
    (void)color;
    (void)on;
}

void oepl_hw_enter_deepsleep(void)
{
    rtt_puts("Deep sleep (stub)\r\n");
}

uint8_t oepl_hw_get_hwid(void)
{
    return 0x35;  // SOLUM_M3_BWR_60
}

bool oepl_hw_get_screen_properties(size_t* x, size_t* y, size_t* bpp)
{
    *x = 600;
    *y = 448;
    *bpp = 1;
    return true;
}

void oepl_hw_debugprint(debug_level_t level, const char* fmt, ...)
{
    (void)level;
    (void)fmt;
    // We use RTT directly, no UART debug print needed
}

void oepl_hw_crash(const char* message)
{
    rtt_puts("CRASH: ");
    rtt_puts(message);
    rtt_puts("\r\n");
    __asm volatile ("cpsid i");
    while (1) {
        __asm volatile ("nop");
    }
}

void oepl_hw_watchdog_init(void)
{
}

void oepl_hw_watchdog_feed(void)
{
}
