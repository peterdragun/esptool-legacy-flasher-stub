/* Command handlers for writing out to flash.
 *
 *  Called from stub_flasher.c
 *
 * Copyright (c) 2016 Cesanta Software Limited & Espressif Systems (Shanghai) PTE LTD.
 * All rights reserved
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later version.
 *
 */
#include "soc_support.h"
#include "stub_write_flash.h"
#include "stub_flasher.h"
#include "rom_functions.h"
#include "miniz.h"

/* local flashing state

   This is wrapped in a structure because gcc 4.8
   generates significantly more code for ESP32
   if they are static variables (literal pool, I think!)
*/
static struct {
  /* set by flash_begin, cleared by flash_end */
  bool in_flash_mode;
  /* offset of next SPI write */
  uint32_t next_write;
  /* sector number for next erase */
  int next_erase_sector;
  /* number of output bytes remaining to write */
  uint32_t remaining;
  /* number of sectors remaining to erase */
  int remaining_erase_sector;
  /* last error generated by a data packet */
  esp_command_error last_error;

  /* inflator state for deflate write */
  tinfl_decompressor inflator;
  /* number of compressed bytes remaining to read */
  uint32_t remaining_compressed;
} fs;

bool is_in_flash_mode(void)
{
  return fs.in_flash_mode;
}

esp_command_error get_flash_error(void)
{
  return fs.last_error;
}

esp_command_error handle_flash_begin(uint32_t total_size, uint32_t offset) {
  fs.in_flash_mode = true;
  fs.next_write = offset;
  fs.next_erase_sector = offset / FLASH_SECTOR_SIZE;
  fs.remaining = total_size;
  fs.remaining_erase_sector = (total_size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
  fs.last_error = ESP_OK;

  if (SPIUnlock() != 0) {
	return ESP_FAILED_SPI_UNLOCK;
  }

  return ESP_OK;
}

esp_command_error handle_flash_deflated_begin(uint32_t uncompressed_size, uint32_t compressed_size, uint32_t offset) {
  esp_command_error err = handle_flash_begin(uncompressed_size, offset);
  tinfl_init(&fs.inflator);
  fs.remaining_compressed = compressed_size;
  return err;
}


/* Returns true if the spiflash is ready for its next write
   operation.

   Doesn't block, except for the SPI state machine to finish
   any previous SPI host operation.
*/
static bool spiflash_is_ready(void)
{
  /* Wait for SPI state machine ready */
  while((REG_READ(SPI_EXT2_REG(SPI_IDX)) & SPI_ST))
	{ }
  REG_WRITE(SPI_RD_STATUS_REG(SPI_IDX), 0);
  /* Issue read status command */
  REG_WRITE(SPI_CMD_REG(SPI_IDX), SPI_FLASH_RDSR);
  while(REG_READ(SPI_CMD_REG(SPI_IDX)) != 0)
	{ }
  uint32_t status_value = REG_READ(SPI_RD_STATUS_REG(SPI_IDX));
  const uint32_t STATUS_WIP_BIT = 1;
  return (status_value & STATUS_WIP_BIT) == 0;
}

/* Erase the next sector or block (depending if we're at a block boundary).

   Does nothing if SPI flash not yet ready for a write. Also does not wait
   for any existing SPI flash operation to complete.
 */
static void start_next_erase(void)
{
  if(fs.remaining_erase_sector == 0)
	return; /* nothing left to erase */
  if(!spiflash_is_ready())
	return; /* don't wait for flash to be ready, caller will call again if needed */

  uint32_t command = SPI_FLASH_SE; /* sector erase, 4KB */
  uint32_t sectors_to_erase = 1;
  if(fs.remaining_erase_sector >= SECTORS_PER_BLOCK
	 && fs.next_erase_sector % SECTORS_PER_BLOCK == 0) {
	/* perform a 32KB block erase if we have space for it */
	command = SPI_FLASH_BE;
	sectors_to_erase = SECTORS_PER_BLOCK;
  }

  uint32_t addr = fs.next_erase_sector * FLASH_SECTOR_SIZE;
  REG_WRITE(SPI_ADDR_REG(SPI_IDX), addr & 0xffffff);
  REG_WRITE(SPI_CMD_REG(SPI_IDX), command);
  while(REG_READ(SPI_CMD_REG(SPI_IDX)) != 0)
	{ }
  fs.remaining_erase_sector -= sectors_to_erase;
  fs.next_erase_sector += sectors_to_erase;
}

/* Write data to flash (either direct for non-compressed upload, or decompressed.
   erases as it goes.

   Updates fs.remaining_erase_sector, fs.next_write, and fs.remaining
*/
void handle_flash_data(void *data_buf, uint32_t length) {
  /* what sector is this write going to end in?
	 make sure we've erased at least that far.
  */
  int last_sector = (fs.next_write + length + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
  while(fs.next_erase_sector < last_sector) {
	start_next_erase();
  }

  /* do the actual write */
  if (SPIWrite(fs.next_write, data_buf, length)) {
	fs.last_error = ESP_FAILED_SPI_OP;
  }
  fs.next_write += length;
  fs.remaining -= length;
}

void handle_flash_deflated_data(void *data_buf, uint32_t length) {
  static uint8_t out_buf[32768];
  static uint8_t *next_out = out_buf;
  int status = TINFL_STATUS_NEEDS_MORE_INPUT;

  while(length > 0 && fs.remaining > 0 && status > TINFL_STATUS_DONE) {
	size_t in_bytes = length; /* input remaining */
	size_t out_bytes = out_buf + sizeof(out_buf) - next_out; /* output space remaining */
	int flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
	if(fs.remaining_compressed > length) {
	  flags |= TINFL_FLAG_HAS_MORE_INPUT;
	}

	/* start an opportunistic erase: decompressing takes time, so might as
	   well be running a SPI erase in the background. */
	start_next_erase();

	status = tinfl_decompress(&fs.inflator, data_buf, &in_bytes,
					 out_buf, next_out, &out_bytes,
					 flags);

	fs.remaining_compressed -= in_bytes;
	length -= in_bytes;
	data_buf += in_bytes;

	next_out += out_bytes;
	size_t bytes_in_out_buf = next_out - out_buf;
	if (status <= TINFL_STATUS_DONE || bytes_in_out_buf == sizeof(out_buf)) {
	  // Output buffer full, or done
	  handle_flash_data(out_buf, bytes_in_out_buf);
	  next_out = out_buf;
	}
  } // while

  if (status < TINFL_STATUS_DONE) {
	/* error won't get sent back to esptool.py until next block is sent */
	fs.last_error = ESP_INFLATE_ERROR;
  }

  if (status == TINFL_STATUS_DONE && fs.remaining > 0) {
	fs.last_error = ESP_NOT_ENOUGH_DATA;
  }
  if (status != TINFL_STATUS_DONE && fs.remaining == 0) {
	fs.last_error = ESP_TOO_MUCH_DATA;
  }
}

esp_command_error handle_flash_end(void)
{
  if (!fs.in_flash_mode) {
	return ESP_NOT_IN_FLASH_MODE;
  }

  if (fs.remaining > 0) {
	return ESP_NOT_ENOUGH_DATA;
  }

  fs.in_flash_mode = false;
  return fs.last_error;
}
