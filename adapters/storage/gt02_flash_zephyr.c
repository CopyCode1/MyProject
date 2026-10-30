#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>

#include "adapters/storage/gt02_flash_zephyr.h"

LOG_MODULE_REGISTER(gt02_flash, LOG_LEVEL_INF);

#if !DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(ext_flash0))
#error "GT02 board must provide an enabled ext-flash0 alias"
#endif

/* Board topology stays in DTS; SPI commands are owned by the Zephyr driver. */
static const struct device *const ext_flash = DEVICE_DT_GET(DT_ALIAS(ext_flash0));

int gt02_flash_zephyr_probe(void)
{
	struct flash_pages_info page;
	uint64_t capacity;
	uint8_t id[3];
	uint8_t scratch[32];
	int ret;

	if (!device_is_ready(ext_flash)) {
		LOG_ERR("External Flash driver is not ready; check SPI NOR startup logs");
		return -ENODEV;
	}

	ret = flash_read_jedec_id(ext_flash, id);
	if (ret < 0) {
		LOG_ERR("JEDEC ID read failed: %d", ret);
		return ret;
	}
	LOG_INF("External Flash JEDEC ID: %02x %02x %02x", id[0], id[1], id[2]);
	/* Match both W25Q128JV identities accepted by the legacy efs_port_w25qxx. */
	if (id[0] != 0xef || (id[1] != 0x40 && id[1] != 0x70) || id[2] != 0x18) {
		LOG_ERR("External Flash is not a supported W25Q128JV variant");
		return -ENODEV;
	}

	ret = flash_get_size(ext_flash, &capacity);
	if (ret < 0) {
		LOG_ERR("Flash capacity query failed: %d", ret);
		return ret;
	}
	/* With SFDP_RUNTIME this size comes from the chip, not just a DTS constant. */
	if (capacity != (uint64_t)DT_PROP(DT_ALIAS(ext_flash0), size) / 8U) {
		LOG_ERR("Unexpected Flash capacity: %llu bytes", (unsigned long long)capacity);
		return -EINVAL;
	}
	ret = flash_get_page_info_by_offs(ext_flash, 0, &page);
	if (ret < 0) {
		LOG_ERR("Flash erase layout query failed: %d", ret);
		return ret;
	}
	LOG_INF("W25Q128JV ready: size=%llu bytes, erase_block=%u, write_align=%u",
		(unsigned long long)capacity, (unsigned int)page.size,
		(unsigned int)flash_get_write_block_size(ext_flash));

	/* Exercise low/high addresses without exposing existing product data in logs. */
	ret = flash_read(ext_flash, 0, scratch, sizeof(scratch));
	if (ret < 0) {
		LOG_ERR("Flash first-address read failed: %d", ret);
		return ret;
	}
	ret = flash_read(ext_flash, (off_t)(capacity - sizeof(scratch)),
			 scratch, sizeof(scratch));
	if (ret < 0) {
		LOG_ERR("Flash last-address read failed: %d", ret);
		return ret;
	}
	LOG_INF("External Flash first/last 32-byte reads passed; no erase or program");
	return 0;
}
