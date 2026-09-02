/*
 * ota_metadata.h
 *
 *  Created on: Aug 24, 2026
 *      Author: Sahil
 */

#ifndef OTA_METADATA_H
#define OTA_METADATA_H

#include <stdint.h>

#define METADATA_BASE_ADDRESS 0x08060000UL
#define METADATA_MAGIC 0xB2074ADEUL

#define SLOT_A_BASE_ADDRESS 0x08020000UL
#define SLOT_B_BASE_ADDRESS 0x08040000UL
#define SLOT_SIZE (128UL * 1024UL)

#define SLOT_A 0UL
#define SLOT_B 1UL

#define BOOT_TRIAL 0xFFFFFFFFUL   //erased state = unconfirmed
#define BOOT_CONFIRMED 0x00000000UL

#define BOOT_ATTEMPTS_MAX 3U
#define BOOT_ATTEMPTS_FRESH 0xFFFFFFFFUL

typedef struct
{
    uint32_t magic;
    uint32_t active_slot;
    uint32_t boot_confirmed;
    uint32_t boot_attempts;

    uint32_t slot_a_size;
    uint32_t slot_a_crc32;
    uint32_t slot_b_size;
    uint32_t slot_b_crc32;

    uint32_t slot_a_version;
    uint32_t slot_b_version;
    uint32_t reserved0;
    uint32_t reserved1;
} ota_metadata_t;

#endif /* OTA_METADATA_H */
