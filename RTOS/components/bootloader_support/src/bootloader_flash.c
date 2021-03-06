// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sdkconfig.h"

#ifdef CONFIG_TARGET_PLATFORM_ESP32

#include <stddef.h>

#include <bootloader_flash.h>
#include <esp_log.h>
#include <esp_spi_flash.h> /* including in bootloader for error values */
#include <esp_flash_encrypt.h>

#ifndef BOOTLOADER_BUILD
/* Normal app version maps to esp_spi_flash.h operations...
 */
static const char *TAG = "bootloader_mmap";

static spi_flash_mmap_handle_t map;

const void *bootloader_mmap(uint32_t src_addr, uint32_t size)
{
    if (map) {
        ESP_LOGE(TAG, "tried to bootloader_mmap twice");
        return NULL; /* existing mapping in use... */
    }
    const void *result = NULL;
    uint32_t src_page = src_addr & ~(SPI_FLASH_MMU_PAGE_SIZE-1);
    size += (src_addr - src_page);
    esp_err_t err = spi_flash_mmap(src_page, size, SPI_FLASH_MMAP_DATA, &result, &map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_flash_mmap failed: 0x%x", err);
        return NULL;
    }
    return (void *)((intptr_t)result + (src_addr - src_page));
}

void bootloader_munmap(const void *mapping)
{
    if(mapping && map) {
        spi_flash_munmap(map);
    }
    map = 0;
}

esp_err_t bootloader_flash_read(size_t src, void *dest, size_t size, bool allow_decrypt)
{
    if (allow_decrypt && esp_flash_encryption_enabled()) {
        return spi_flash_read_encrypted(src, dest, size);
    } else {
        return spi_flash_read(src, dest, size);
    }
}

esp_err_t bootloader_flash_write(size_t dest_addr, void *src, size_t size, bool write_encrypted)
{
    if (write_encrypted) {
        return spi_flash_write_encrypted(dest_addr, src, size);
    } else {
        return spi_flash_write(dest_addr, src, size);
    }
}

esp_err_t bootloader_flash_erase_sector(size_t sector)
{
    return spi_flash_erase_sector(sector);
}

#else
/* Bootloader version, uses ROM functions only */
#include <soc/dport_reg.h>
#include <rom/spi_flash.h>
#include <rom/cache.h>

static const char *TAG = "bootloader_flash";

/* Use first 50 blocks in MMU for bootloader_mmap,
   50th block for bootloader_flash_read
*/
#define MMU_BLOCK0_VADDR  0x3f400000
#define MMU_BLOCK50_VADDR 0x3f720000
#define MMU_FLASH_MASK    0xffff0000
#define MMU_BLOCK_SIZE    0x00010000

static bool mapped;

// Current bootloader mapping (ab)used for bootloader_read()
static uint32_t current_read_mapping = UINT32_MAX;

const void *bootloader_mmap(uint32_t src_addr, uint32_t size)
{
    if (mapped) {
        ESP_LOGE(TAG, "tried to bootloader_mmap twice");
        return NULL; /* can't map twice */
    }
    if (size > 0x320000) {
        /* Allow mapping up to 50 of the 51 available MMU blocks (last one used for reads) */
        ESP_LOGE(TAG, "bootloader_mmap excess size %x", size);
        return NULL;
    }

    uint32_t src_addr_aligned = src_addr & MMU_FLASH_MASK;
    uint32_t count = (size + (src_addr - src_addr_aligned) + 0xffff) / MMU_BLOCK_SIZE;
    Cache_Read_Disable(0);
    Cache_Flush(0);
    ESP_LOGD(TAG, "mmu set paddr=%08x count=%d", src_addr_aligned, count );
    int e = cache_flash_mmu_set(0, 0, MMU_BLOCK0_VADDR, src_addr_aligned, 64, count);
    if (e != 0) {
        ESP_LOGE(TAG, "cache_flash_mmu_set failed: %d\n", e);
        Cache_Read_Enable(0);
        return NULL;
    }
    Cache_Read_Enable(0);

    mapped = true;

    return (void *)(MMU_BLOCK0_VADDR + (src_addr - src_addr_aligned));
}

void bootloader_munmap(const void *mapping)
{
    if (mapped)  {
        /* Full MMU reset */
        Cache_Read_Disable(0);
        Cache_Flush(0);
        mmu_init(0);
        mapped = false;
        current_read_mapping = UINT32_MAX;
    }
}

static esp_err_t spi_to_esp_err(esp_rom_spiflash_result_t r)
{
    switch(r) {
    case ESP_ROM_SPIFLASH_RESULT_OK:
        return ESP_OK;
    case ESP_ROM_SPIFLASH_RESULT_ERR:
        return ESP_ERR_FLASH_OP_FAIL;
    case ESP_ROM_SPIFLASH_RESULT_TIMEOUT:
        return ESP_ERR_FLASH_OP_TIMEOUT;
    default:
        return ESP_FAIL;
    }
}

static esp_err_t bootloader_flash_read_no_decrypt(size_t src_addr, void *dest, size_t size)
{
    Cache_Read_Disable(0);
    Cache_Flush(0);
    esp_rom_spiflash_result_t r = esp_rom_spiflash_read(src_addr, dest, size);
    Cache_Read_Enable(0);

    return spi_to_esp_err(r);
}

static esp_err_t bootloader_flash_read_allow_decrypt(size_t src_addr, void *dest, size_t size)
{
    uint32_t *dest_words = (uint32_t *)dest;

    /* Use the 51st MMU mapping to read from flash in 64KB blocks.
       (MMU will transparently decrypt if encryption is enabled.)
    */
    for (int word = 0; word < size / 4; word++) {
        uint32_t word_src = src_addr + word * 4;  /* Read this offset from flash */
        uint32_t map_at = word_src & MMU_FLASH_MASK; /* Map this 64KB block from flash */
        uint32_t *map_ptr;
        if (map_at != current_read_mapping) {
            /* Move the 64KB mmu mapping window to fit map_at */
            Cache_Read_Disable(0);
            Cache_Flush(0);
            ESP_LOGD(TAG, "mmu set block paddr=0x%08x (was 0x%08x)", map_at, current_read_mapping);
            int e = cache_flash_mmu_set(0, 0, MMU_BLOCK50_VADDR, map_at, 64, 1);
            if (e != 0) {
                ESP_LOGE(TAG, "cache_flash_mmu_set failed: %d\n", e);
                Cache_Read_Enable(0);
                return ESP_FAIL;
            }
            current_read_mapping = map_at;
            Cache_Read_Enable(0);
        }
        map_ptr = (uint32_t *)(MMU_BLOCK50_VADDR + (word_src - map_at));
        dest_words[word] = *map_ptr;
    }
    return ESP_OK;
}

esp_err_t bootloader_flash_read(size_t src_addr, void *dest, size_t size, bool allow_decrypt)
{
    if (src_addr & 3) {
        ESP_LOGE(TAG, "bootloader_flash_read src_addr 0x%x not 4-byte aligned", src_addr);
        return ESP_FAIL;
    }
    if (size & 3) {
        ESP_LOGE(TAG, "bootloader_flash_read size 0x%x not 4-byte aligned", size);
        return ESP_FAIL;
    }
    if ((intptr_t)dest & 3) {
        ESP_LOGE(TAG, "bootloader_flash_read dest 0x%x not 4-byte aligned", (intptr_t)dest);
        return ESP_FAIL;
    }

    if (allow_decrypt) {
        return bootloader_flash_read_allow_decrypt(src_addr, dest, size);
    } else {
        return bootloader_flash_read_no_decrypt(src_addr, dest, size);
    }
}

esp_err_t bootloader_flash_write(size_t dest_addr, void *src, size_t size, bool write_encrypted)
{
    esp_err_t err;
    size_t alignment = write_encrypted ? 32 : 4;
    if ((dest_addr % alignment) != 0) {
        ESP_LOGE(TAG, "bootloader_flash_write dest_addr 0x%x not %d-byte aligned", dest_addr, alignment);
        return ESP_FAIL;
    }
    if ((size % alignment) != 0) {
        ESP_LOGE(TAG, "bootloader_flash_write size 0x%x not %d-byte aligned", size, alignment);
        return ESP_FAIL;
    }
    if (((intptr_t)src % 4) != 0) {
        ESP_LOGE(TAG, "bootloader_flash_write src 0x%x not 4 byte aligned", (intptr_t)src);
        return ESP_FAIL;
    }

    err = spi_to_esp_err(esp_rom_spiflash_unlock());
    if (err != ESP_OK) {
        return err;
    }

    if (write_encrypted) {
        return spi_to_esp_err(esp_rom_spiflash_write_encrypted(dest_addr, src, size));
    } else {
        return spi_to_esp_err(esp_rom_spiflash_write(dest_addr, src, size));
    }
}

esp_err_t bootloader_flash_erase_sector(size_t sector)
{
    return spi_to_esp_err(esp_rom_spiflash_erase_sector(sector));
}

#endif

#endif

#ifdef CONFIG_TARGET_PLATFORM_ESP8266

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"

#ifndef BOOTLOADER_BUILD
#include "esp_spi_flash.h"
#endif

extern void Cache_Read_Disable();
extern void Cache_Read_Enable(uint8_t map, uint8_t p, uint8_t v);

static const char *TAG = "bootloader_flash";

typedef enum { SPI_FLASH_RESULT_OK = 0,
               SPI_FLASH_RESULT_ERR = 1,
               SPI_FLASH_RESULT_TIMEOUT = 2 } SpiFlashOpResult;

SpiFlashOpResult SPIRead(uint32_t addr, void *dst, uint32_t size);
SpiFlashOpResult SPIWrite(uint32_t addr, const uint8_t *src, uint32_t size);
SpiFlashOpResult SPIEraseSector(uint32_t sector_num);

static bool mapped;

const void *bootloader_mmap(uint32_t src_addr, uint32_t size)
{
    if (mapped) {
        ESP_LOGE(TAG, "tried to bootloader_mmap twice");
        return NULL; /* can't map twice */
    }

    /* ToDo: Improve the map policy! */

    Cache_Read_Disable();

    /* 0 and 0x100000 address use same mmap addresss 0x40200000 */
    if (src_addr < 0x100000) {
        Cache_Read_Enable(0, 0, 0);
    } else {
        Cache_Read_Enable(1, 0, 0);
        src_addr -= 0x100000;
    }

    mapped = true;

    return (void *)(0x40200000 + src_addr);
}

void bootloader_munmap(const void *mapping)
{
    if (mapped)  {
        Cache_Read_Disable();
        mapped = false;
    }
}

static esp_err_t bootloader_flash_read_no_decrypt(size_t src_addr, void *dest, size_t size)
{
#ifdef BOOTLOADER_BUILD
    SPIRead(src_addr, dest, size);
#else
    spi_flash_read(src_addr, dest, size);
#endif

    return ESP_OK;
}

esp_err_t bootloader_flash_read(size_t src_addr, void *dest, size_t size, bool allow_decrypt)
{
    if (src_addr & 3) {
        ESP_LOGE(TAG, "bootloader_flash_read src_addr 0x%x not 4-byte aligned", src_addr);
        return ESP_FAIL;
    }
    if (size & 3) {
        ESP_LOGE(TAG, "bootloader_flash_read size 0x%x not 4-byte aligned", size);
        return ESP_FAIL;
    }
    if ((intptr_t)dest & 3) {
        ESP_LOGE(TAG, "bootloader_flash_read dest 0x%x not 4-byte aligned", (intptr_t)dest);
        return ESP_FAIL;
    }

        return bootloader_flash_read_no_decrypt(src_addr, dest, size);
}

esp_err_t bootloader_flash_write(size_t dest_addr, void *src, size_t size, bool write_encrypted)
{
    size_t alignment = write_encrypted ? 32 : 4;
    if ((dest_addr % alignment) != 0) {
        ESP_LOGE(TAG, "bootloader_flash_write dest_addr 0x%x not %d-byte aligned", dest_addr, alignment);
        return ESP_FAIL;
    }
    if ((size % alignment) != 0) {
        ESP_LOGE(TAG, "bootloader_flash_write size 0x%x not %d-byte aligned", size, alignment);
        return ESP_FAIL;
    }
    if (((intptr_t)src % 4) != 0) {
        ESP_LOGE(TAG, "bootloader_flash_write src 0x%x not 4 byte aligned", (intptr_t)src);
        return ESP_FAIL;
    }


    SPIWrite(dest_addr, src, size);

    return ESP_OK;
}

esp_err_t bootloader_flash_erase_sector(size_t sector)
{
    SPIEraseSector(sector);

    return ESP_OK;
}

#endif
