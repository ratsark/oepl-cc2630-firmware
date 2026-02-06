// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include "oepl_display_driver_common_cc2630.h"
#include "../oepl_hw_abstraction_cc2630.h"

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------
static const oepl_display_driver_desc_t* current_driver = NULL;

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void oepl_display_driver_register(const oepl_display_driver_desc_t* driver)
{
    if (driver == NULL) {
        oepl_hw_debugprint(DBG_DISPLAY, "Cannot register NULL driver\n");
        return;
    }

    oepl_hw_debugprint(DBG_DISPLAY, "Registering display driver: %s\n", driver->name);

    current_driver = driver;

    // Initialize the driver
    if (current_driver->init != NULL) {
        current_driver->init();
    }
}

const oepl_display_driver_desc_t* oepl_display_driver_get_current(void)
{
    return current_driver;
}

void oepl_display_init(void)
{
    if (current_driver == NULL) {
        oepl_hw_debugprint(DBG_DISPLAY, "No display driver registered\n");
        return;
    }

    if (current_driver->init != NULL) {
        current_driver->init();
    }
}

void oepl_display_update(const uint8_t* framebuffer, size_t len)
{
    if (current_driver == NULL) {
        oepl_hw_debugprint(DBG_DISPLAY, "No display driver registered\n");
        return;
    }

    if (current_driver->draw != NULL) {
        current_driver->draw(framebuffer, len);
    }
}

void oepl_display_sleep(void)
{
    if (current_driver == NULL) {
        oepl_hw_debugprint(DBG_DISPLAY, "No display driver registered\n");
        return;
    }

    if (current_driver->sleep != NULL) {
        current_driver->sleep();
    }
}

void oepl_display_wake(void)
{
    if (current_driver == NULL) {
        oepl_hw_debugprint(DBG_DISPLAY, "No display driver registered\n");
        return;
    }

    if (current_driver->wake != NULL) {
        current_driver->wake();
    }
}
