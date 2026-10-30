#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include "tests/bringup/gt02_flash_selftest.h"

LOG_MODULE_REGISTER(gt02_flash_test, LOG_LEVEL_INF);

#define TEST_NODE DT_NODELABEL(flash_test_partition)
#define CHUNK_SIZE 512U

/*
 * 这些常量是本轮用户授权的硬边界，不是第二份可随意改动的板级配置。
 * 分区位置仍来自DTS；误改分区时编译即失败，运行时也再次检查。
 * 如需移动测试区，必须先获得新的覆盖授权，再同步修改这些保护。
 */
#define APPROVED_OFFSET 0x00fff000U
#define APPROVED_SIZE   4096U
#define APPROVED_CAPACITY 0x01000000U

BUILD_ASSERT(PARTITION_EXISTS(flash_test_partition), "Flash test partition is required");
BUILD_ASSERT(DT_SAME_NODE(DT_GPARENT(TEST_NODE), DT_ALIAS(ext_flash0)),
	     "Test partition must be on ext-flash0, not internal Flash");
BUILD_ASSERT(PARTITION_OFFSET(flash_test_partition) == APPROVED_OFFSET,
	     "Flash test offset differs from the approved region");
BUILD_ASSERT(PARTITION_SIZE(flash_test_partition) == APPROVED_SIZE,
	     "Flash test size differs from the approved region");
BUILD_ASSERT(!DT_PROP_OR(TEST_NODE, read_only, 0), "Test partition is read-only");
BUILD_ASSERT(APPROVED_SIZE % CHUNK_SIZE == 0, "Test chunks must cover the whole sector");

/* 静态缓冲区共1 KiB，不把整个扇区或大数组放到主线程栈。 */
static uint8_t write_buffer[CHUNK_SIZE];
static uint8_t read_buffer[CHUNK_SIZE];
static bool attempted;

enum test_pattern {
	PATTERN_ZERO,
	PATTERN_ALTERNATING,
	PATTERN_ADDRESS,
	PATTERN_ERASED,
};

static const char *const pattern_names[] = { "zero", "aa55", "address" };

static uint8_t expected_byte(enum test_pattern pattern, size_t offset)
{
	switch (pattern) {
	case PATTERN_ZERO:
		return 0x00;
	case PATTERN_ALTERNATING:
		return (offset & 1U) ? 0x55 : 0xaa;
	case PATTERN_ADDRESS:
		/* 混入页位置，使不同256字节页不只是重复同一组数据。 */
		return (uint8_t)(offset ^ (offset >> 8) ^ 0x5aU);
	case PATTERN_ERASED:
	default:
		return 0xff;
	}
}

static int validate_area(const struct flash_area *area)
{
	struct flash_pages_info page;
	uint64_t capacity;
	size_t alignment;
	int ret;

	if (!flash_area_device_is_ready(area)) {
		return -ENODEV;
	}
	if (area->fa_dev != DEVICE_DT_GET(DT_ALIAS(ext_flash0)) ||
	    area->fa_off != APPROVED_OFFSET || area->fa_size != APPROVED_SIZE) {
		LOG_ERR("Refusing test: device/partition differs from approved region");
		return -EPERM;
	}
	ret = flash_get_size(area->fa_dev, &capacity);
	if (ret != 0) {
		return ret;
	}
	/* 先比较再相减，避免范围校验本身发生整数溢出。 */
	if (capacity != APPROVED_CAPACITY || (uint64_t)area->fa_off > capacity ||
	    area->fa_size > capacity - (uint64_t)area->fa_off) {
		LOG_ERR("Refusing test: unexpected device capacity or area bounds");
		return -EINVAL;
	}
	ret = flash_get_page_info_by_offs(area->fa_dev, area->fa_off, &page);
	if (ret != 0) {
		return ret;
	}
	if (page.start_offset != area->fa_off || page.size != area->fa_size) {
		LOG_ERR("Refusing test: region must be exactly one aligned erase sector");
		return -EINVAL;
	}
	alignment = flash_area_align(area);
	if (alignment == 0 || CHUNK_SIZE % alignment != 0 ||
	    area->fa_off % alignment != 0 || flash_area_erased_val(area) != 0xff) {
		LOG_ERR("Refusing test: unsupported write alignment or erased value");
		return -ENOTSUP;
	}
	return 0;
}

static int verify_area(const struct flash_area *area, enum test_pattern pattern,
		       unsigned int round)
{
	const char *stage = pattern == PATTERN_ERASED ? "erase verify" : "write verify";
	int ret;

	for (size_t offset = 0; offset < area->fa_size; offset += CHUNK_SIZE) {
		ret = flash_area_read(area, (off_t)offset, read_buffer, sizeof(read_buffer));
		if (ret != 0) {
			LOG_ERR("round=%u %s read failed: offset=0x%x, err=%d", round,
				stage, (unsigned int)offset, ret);
			return ret;
		}
		for (size_t i = 0; i < sizeof(read_buffer); ++i) {
			uint8_t expected = expected_byte(pattern, offset + i);

			if (read_buffer[i] != expected) {
				LOG_ERR("round=%u %s mismatch: offset=0x%x expected=0x%02x actual=0x%02x",
					round, stage, (unsigned int)(offset + i),
					(unsigned int)expected, (unsigned int)read_buffer[i]);
				return -EILSEQ;
			}
		}
	}
	return 0;
}

static int erase_and_verify(const struct flash_area *area, unsigned int round)
{
	int ret = flash_area_erase(area, 0, area->fa_size);

	if (ret != 0) {
		LOG_ERR("round=%u erase failed: offset=0x0, size=%u, err=%d", round,
			(unsigned int)area->fa_size, ret);
		return ret;
	}
	return verify_area(area, PATTERN_ERASED, round);
}

static int program_and_verify(const struct flash_area *area,
			      enum test_pattern pattern, unsigned int round)
{
	int ret;

	for (size_t offset = 0; offset < area->fa_size; offset += CHUNK_SIZE) {
		for (size_t i = 0; i < sizeof(write_buffer); ++i) {
			write_buffer[i] = expected_byte(pattern, offset + i);
		}
		/* 每次512字节覆盖多个编程页，由官方SPI NOR驱动负责拆页。 */
		ret = flash_area_write(area, (off_t)offset, write_buffer, sizeof(write_buffer));
		if (ret != 0) {
			LOG_ERR("round=%u write failed: offset=0x%x, err=%d", round,
				(unsigned int)offset, ret);
			return ret;
		}
	}
	return verify_area(area, pattern, round);
}

int gt02_flash_selftest_run(void)
{
	const struct flash_area *area;
	int ret;

	/* 仅启动线程调用；不允许同一次启动反复调用而持续磨损扇区。 */
	if (attempted) {
		return -EALREADY;
	}
	attempted = true;
	ret = flash_area_open(PARTITION_ID(flash_test_partition), &area);
	if (ret != 0) {
		LOG_ERR("Self-test partition open failed: %d", ret);
		return ret;
	}
	ret = validate_area(area);
	if (ret != 0) {
		goto done;
	}
	LOG_WRN("DESTRUCTIVE self-test: external Flash [0x%08x, 0x%08x], size=%u; old data will be lost",
		(unsigned int)area->fa_off,
		(unsigned int)(area->fa_off + area->fa_size - 1), (unsigned int)area->fa_size);

	for (unsigned int round = 0; round < ARRAY_SIZE(pattern_names); ++round) {
		ret = erase_and_verify(area, round + 1);
		if (ret != 0) {
			goto done;
		}
		ret = program_and_verify(area, (enum test_pattern)round, round + 1);
		if (ret != 0) {
			goto done;
		}
		LOG_INF("round=%u pattern=%s: erase/write/readback PASS (%u bytes)",
			round + 1, pattern_names[round], (unsigned int)area->fa_size);
	}
	/* 成功时额外擦除一次；失败不清理，保留当时内容供排查。 */
	ret = erase_and_verify(area, 4);
	if (ret == 0) {
		LOG_INF("Flash self-test PASS: 3 patterns; final sector is all 0xff");
	}

done:
	flash_area_close(area);
	if (ret != 0) {
		LOG_ERR("Flash self-test FAILED: %d; no retry or further erase", ret);
	}
	return ret;
}
