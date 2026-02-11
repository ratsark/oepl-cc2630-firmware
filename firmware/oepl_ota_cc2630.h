#ifndef OEPL_OTA_CC2630_H
#define OEPL_OTA_CC2630_H

#include <stdint.h>
#include <stdbool.h>
#include "oepl_radio_cc2630.h"

// OTA flash layout (CC2630F128 = 128KB, 4KB sectors)
#define OTA_STAGING_ADDR    0x10000  // Sector 16
#define OTA_STAGING_END     0x1E000  // Sector 29 (exclusive)
#define OTA_STAGING_SIZE    (OTA_STAGING_END - OTA_STAGING_ADDR)  // 56KB
#define OTA_SECTOR_SIZE     0x1000   // 4KB

// Sector 30 (0x1E000) stores the last applied OTA dataVer to prevent
// re-downloading the same firmware if the AP re-offers it.
#define OTA_DATAVER_ADDR    0x1E000
#define OTA_DATAVER_MAGIC   0x4F544156  // "OTAV"

// Download firmware to staging flash, verify, copy to active area, reboot.
// Does NOT return on success. On failure, returns so caller can retry later.
void oepl_ota_download_and_apply(struct AvailDataInfo *info);

// Check if the offered dataVer matches the last successfully applied OTA.
// Returns true if the tag already has this firmware.
bool oepl_ota_already_applied(uint64_t dataVer);

#endif // OEPL_OTA_CC2630_H
