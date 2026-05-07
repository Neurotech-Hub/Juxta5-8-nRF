/*
 * Destructive test: erases the first flash erase page starting at offset 0,
 * then writes and verifies a pattern. Do not use if low addresses must be preserved.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(juxta5_8_mem, LOG_LEVEL_INF);

#define FLASH_NODE DT_ALIAS(spi_mem)

#define TEST_OFFSET 0
/* Cap pattern length to keep stack/static usage bounded; must fit one erase page after align */
#define TEST_PATTERN_CAP 256U

int main(void)
{
	const struct device *flash = DEVICE_DT_GET(FLASH_NODE);
	struct flash_pages_info page_info;
	const struct flash_parameters *params;
	uint8_t wbuf[TEST_PATTERN_CAP];
	uint8_t rbuf[TEST_PATTERN_CAP];
	size_t test_len;
	size_t wbs;
	int rc;

	LOG_WRN("MEM destroy test: will ERASE + WRITE at offset %u (first erase page)", TEST_OFFSET);

	if (!device_is_ready(flash)) {
		LOG_ERR("[PROBE] FAIL device not ready");
		return -ENODEV;
	}

	params = flash_get_parameters(flash);
	wbs = params->write_block_size;
	LOG_INF("[PROBE] ok write_block_size=%zu erase_value=0x%02x erase_cap=0x%x",
		(size_t)wbs, params->erase_value, (unsigned int)flash_params_get_erase_cap(params));

	uint64_t flash_size = 0;
	rc = flash_get_size(flash, &flash_size);
	if (rc == 0) {
		LOG_INF("[PROBE] flash_size=%llu bytes", (unsigned long long)flash_size);
	} else {
		LOG_INF("[PROBE] flash_get_size rc=%d skip", rc);
	}

	rc = flash_get_page_info_by_offs(flash, TEST_OFFSET, &page_info);
	if (rc != 0) {
		LOG_ERR("[PROBE] FAIL flash_get_page_info_by_offs(%u) rc=%d", TEST_OFFSET, rc);
		return rc;
	}

	LOG_INF("[PROBE] first_page offs=%zu size=%zu index=%u", (size_t)page_info.start_offset,
		(size_t)page_info.size, page_info.index);

	test_len = TEST_PATTERN_CAP;
	if (test_len > page_info.size) {
		test_len = (size_t)page_info.size;
	}
	test_len = (test_len / wbs) * wbs;
	if (test_len == 0U) {
		LOG_ERR("[PROBE] FAIL test_len=0 wbs=%zu page_size=%zu", wbs, (size_t)page_info.size);
		return -EINVAL;
	}

	LOG_INF("[ERASE] offs=%u size=%zu", TEST_OFFSET, (size_t)page_info.size);
	rc = flash_erase(flash, TEST_OFFSET, page_info.size);
	if (rc != 0) {
		LOG_ERR("[ERASE] FAIL rc=%d", rc);
		return rc;
	}
	LOG_INF("[ERASE] PASS");

	for (size_t i = 0; i < test_len; i++) {
		wbuf[i] = (uint8_t)(i & 0xFF);
	}

	LOG_INF("[WRITE] offs=%u len=%zu", TEST_OFFSET, test_len);
	rc = flash_write(flash, TEST_OFFSET, wbuf, test_len);
	if (rc != 0) {
		LOG_ERR("[WRITE] FAIL rc=%d", rc);
		return rc;
	}
	LOG_INF("[WRITE] PASS");

	memset(rbuf, 0, sizeof(rbuf));
	rc = flash_read(flash, TEST_OFFSET, rbuf, test_len);
	if (rc != 0) {
		LOG_ERR("[READ] FAIL rc=%d", rc);
		return rc;
	}

	for (size_t i = 0; i < test_len; i++) {
		if (rbuf[i] != wbuf[i]) {
			LOG_ERR("[VERIFY] FAIL at +%zu exp=0x%02x got=0x%02x", i, wbuf[i], rbuf[i]);
			return -EIO;
		}
	}

	LOG_INF("[VERIFY] PASS len=%zu", test_len);
	return 0;
}
