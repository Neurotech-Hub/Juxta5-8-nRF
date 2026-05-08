#ifndef JUXTA_BLE_SERVICE_H_
#define JUXTA_BLE_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>

#include "juxta_log.h"

#define JUXTA_HUBLINK_SERVICE_UUID                                                                  \
	0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x01, 0x00, 0x01, 0x55, 0x68, 0x73,       \
		0x61, 0x57
#define JUXTA_NODE_CHAR_UUID                                                                        \
	0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x01, 0x00, 0x05, 0x55, 0x68, 0x73,       \
		0x61, 0x57
#define JUXTA_GATEWAY_CHAR_UUID                                                                     \
	0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x01, 0x00, 0x04, 0x55, 0x68, 0x73,       \
		0x61, 0x57
#define JUXTA_FILENAME_CHAR_UUID                                                                    \
	0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x01, 0x00, 0x02, 0x55, 0x68, 0x73,       \
		0x61, 0x57
#define JUXTA_FILE_TRANSFER_CHAR_UUID                                                               \
	0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x01, 0x00, 0x03, 0x55, 0x68, 0x73,       \
		0x61, 0x57

#define BT_UUID_JUXTA_HUBLINK_SERVICE BT_UUID_DECLARE_128(JUXTA_HUBLINK_SERVICE_UUID)
#define BT_UUID_JUXTA_NODE_CHAR BT_UUID_DECLARE_128(JUXTA_NODE_CHAR_UUID)
#define BT_UUID_JUXTA_GATEWAY_CHAR BT_UUID_DECLARE_128(JUXTA_GATEWAY_CHAR_UUID)
#define BT_UUID_JUXTA_FILENAME_CHAR BT_UUID_DECLARE_128(JUXTA_FILENAME_CHAR_UUID)
#define BT_UUID_JUXTA_FILE_TRANSFER_CHAR BT_UUID_DECLARE_128(JUXTA_FILE_TRANSFER_CHAR_UUID)

int juxta_ble_service_init(struct juxta_log_context *log_ctx);
int juxta_ble_get_device_id(char *device_id);
void juxta_ble_connection_established(struct bt_conn *conn);
void juxta_ble_connection_terminated(void);
int juxta_ble_get_status(uint16_t *mtu, bool *connected, bool *transfer_active);

/* Set a getter that returns current battery voltage in millivolts.
 * Called once from main() before advertising starts. */
void juxta_ble_set_battery_mv_source(int32_t (*getter)(void));
void juxta_ble_set_production_ready(void);

void juxta_ble_timing_update_trigger(void);
void juxta_ble_datetime_synchronized(void);

#endif /* JUXTA_BLE_SERVICE_H_ */
