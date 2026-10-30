#ifndef GT02_FLASH_ZEPHYR_H_
#define GT02_FLASH_ZEPHYR_H_

/* Startup diagnostics only: query identity/geometry and read the first/last bytes.
 * Does not erase, program data, or expose a storage partition to the application.
 * Call from thread context; returns 0 on success or a negative errno on failure.
 */
int gt02_flash_zephyr_probe(void);

#endif /* GT02_FLASH_ZEPHYR_H_ */
