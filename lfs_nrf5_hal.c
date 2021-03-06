/**
 * @file littlefs_nrf52_hal.c
 *
 * HAL implementation for littlefs for the nRF5 SDK. Uses the fstorage API for
 * low level flash operations.
 *
 */

// Copyright notice from littlefs:
/*
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */


/*======= Includes ==========================================================*/

#include "lfs_nrf5_hal.h"
#include "lfs.h"
#include "lfs_util.h"
#include "nrf_fstorage.h"

#ifdef SOFTDEVICE_PRESENT
#include "nrf_fstorage_sd.h"
#else
#include "nrf_fstorage_nvmc.h"
#endif

/*======= Local Macro Definitions ===========================================*/

#ifndef LFS_NRF5_START_ADDR
#define LFS_NRF5_START_ADDR 0x3e000
#endif

#ifndef LFS_NRF5_END_ADDR
#define LFS_NRF5_END_ADDR 0x3ffff
#endif

// Internal definitions

#define LFS_NRF52_ERR_WAIT_VALUE (-1)
#define N_PAGES_TO_ERASE 1

/*======= Type Definitions ==================================================*/
/*======= Local function prototypes =========================================*/

static int lfs_api_read(const struct lfs_config *c,
                        lfs_block_t block,
                        lfs_off_t off, void *buffer,
                        lfs_size_t size);

static int lfs_api_prog(const struct lfs_config *c,
                        lfs_block_t block,
                        lfs_off_t off, const void *buffer,
                        lfs_size_t size);

static int lfs_api_erase(const struct lfs_config *c, lfs_block_t block);

static int lfs_api_sync(const struct lfs_config *c);

static void fstorage_evt_handler(nrf_fstorage_evt_t *p_evt);

static int errno_to_lfs(uint32_t err);

static void wait_for_flash(void);

static void wait_for_cb(void);

/*======= Local variable declarations =======================================*/

NRF_FSTORAGE_DEF(nrf_fstorage_t fstorage_instance) =
{
    .evt_handler = fstorage_evt_handler,
    .start_addr = LFS_NRF5_START_ADDR,
    .end_addr   = LFS_NRF5_END_ADDR,
};

static volatile int flash_op_ret = LFS_NRF52_ERR_WAIT_VALUE;

static wdt_feed hal_wdt_feed = NULL;

/*======= Global function implementations ===================================*/

uint32_t littlefs_nrf52_init(struct lfs_config *c, wdt_feed wdt_feed_impl)
{
    uint32_t err;

    if (NULL == c)
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    hal_wdt_feed = wdt_feed_impl;

    // Init nRF fstorage
#ifdef SOFTDEVICE_PRESENT
    err = nrf_fstorage_init(&fstorage_instance, &nrf_fstorage_sd, NULL);
#else
    err = nrf_fstorage_init(&fstorage_instance, &nrf_fstorage_nvmc, NULL);
#endif
    if (err)
    {
        return err;
    }

    // Flash operations
    c->read = lfs_api_read;
    c->prog = lfs_api_prog;
    c->erase = lfs_api_erase;
    c->sync = lfs_api_sync;

    return NRF_SUCCESS;
}

// From lfs_util.c
uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size) {
    static const uint32_t rtable[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };

    const uint8_t *data = buffer;

    for (size_t i = 0; i < size; i++) {
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 0)) & 0xf];
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 4)) & 0xf];
    }

    return crc;
}

/*======= Local function implementations ====================================*/

static int lfs_api_read(const struct lfs_config *c,
                        lfs_block_t block,
                        lfs_off_t off, void *buffer,
                        lfs_size_t size)
{
    uint32_t offset = (LFS_NRF5_START_ADDR) + (block * c->block_size) + off;
    uint32_t err = nrf_fstorage_read(&fstorage_instance, offset, buffer, size);
    if (!err)
    {
        wait_for_flash();
    }
    return errno_to_lfs(err);
}

static int lfs_api_prog(const struct lfs_config *c,
                        lfs_block_t block,
                        lfs_off_t off, const void *buffer,
                        lfs_size_t size)
{
    uint32_t offset = (LFS_NRF5_START_ADDR) + (block * c->block_size) + off;
    uint32_t err = nrf_fstorage_write(&fstorage_instance,
                                      offset,
                                      buffer,
                                      size,
                                      NULL);
    if (!err)
    {
        wait_for_flash();
        wait_for_cb();
        err = flash_op_ret;
        flash_op_ret = LFS_NRF52_ERR_WAIT_VALUE;
    }
    return errno_to_lfs(err);
}

static int lfs_api_erase(const struct lfs_config *c, lfs_block_t block)
{
    uint32_t offset = (LFS_NRF5_START_ADDR) + (block * c->block_size);
    uint32_t err = nrf_fstorage_erase(&fstorage_instance,
                                      offset,
                                      N_PAGES_TO_ERASE,
                                      NULL);
    if (!err)
    {
        wait_for_flash();
        wait_for_cb();
        err = flash_op_ret;
        flash_op_ret = LFS_NRF52_ERR_WAIT_VALUE;
    }
    return errno_to_lfs(err);
}

static int lfs_api_sync(const struct lfs_config *c)
{
    return LFS_ERR_OK;
}

static int errno_to_lfs(uint32_t err)
{
    if (err != NRF_SUCCESS)
    {
        return -err;
    }
    return LFS_ERR_OK;
}

static void wait_for_flash(void)
{
    while (nrf_fstorage_is_busy(&fstorage_instance))
    {
        if (hal_wdt_feed != NULL)
        {
            hal_wdt_feed();
        }
    }
}

static void wait_for_cb(void)
{
    while (flash_op_ret < 0)
    {
        if (hal_wdt_feed != NULL)
        {
            hal_wdt_feed();
        }
    }
}

static void fstorage_evt_handler(nrf_fstorage_evt_t *p_evt)
{
    flash_op_ret = p_evt->result;
}
