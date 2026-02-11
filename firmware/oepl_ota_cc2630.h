#ifndef OEPL_OTA_CC2630_H
#define OEPL_OTA_CC2630_H

#include <stdint.h>
#include "oepl_radio_cc2630.h"

// OTA flash layout (CC2630F128 = 128KB, 4KB sectors)
#define OTA_STAGING_ADDR    0x10000  // Sector 16
#define OTA_STAGING_END     0x1F000  // Sector 30 (exclusive, CCFG at 0x1F000)
#define OTA_STAGING_SIZE    (OTA_STAGING_END - OTA_STAGING_ADDR)  // 60KB
#define OTA_SECTOR_SIZE     0x1000   // 4KB

// Download firmware to staging flash, verify, copy to active area, reboot.
// Does NOT return on success. On failure, returns so caller can retry later.
void oepl_ota_download_and_apply(struct AvailDataInfo *info);

#endif // OEPL_OTA_CC2630_H
