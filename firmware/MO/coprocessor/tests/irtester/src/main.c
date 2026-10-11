#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>

#include "ir.h"

LOG_MODULE_REGISTER(irtester, LOG_LEVEL_INF);

#define IR_MESSAGE "TH-1"
#define IR_MESSAGE_LEN (sizeof(IR_MESSAGE) - 1)

int main(void)
{
	int err;
	uint8_t decoded[IR_MESSAGE_LEN] = {0};
	int bytes_decoded;
	int edges_captured;

	LOG_INF("irtester starting");

	err = ir_tx_init();
	if (err) {
		return err;
	}

	err = ir_rx_init();
	if (err) {
		return err;
	}

	LOG_INF("Sending message \"%s\" over IR", IR_MESSAGE);

	ir_rx_capture_start();
	for (size_t i = 0; i < IR_MESSAGE_LEN; i++) {
		ir_send_byte((uint8_t)IR_MESSAGE[i]);
	}
	k_msleep(5);
	edges_captured = ir_rx_capture_stop();

	if (edges_captured == 0) {
		LOG_WRN("No response seen on the IR receiver at all");
		return 0;
	}

	LOG_INF("Response detected on IR receiver (%d edge(s))", edges_captured);

	bytes_decoded = ir_decode_capture(decoded, (int)IR_MESSAGE_LEN);
	if (bytes_decoded == (int)IR_MESSAGE_LEN &&
	    memcmp(decoded, IR_MESSAGE, IR_MESSAGE_LEN) == 0) {
		LOG_INF("Decoded message matches what was sent: \"%.*s\"",
			(int)IR_MESSAGE_LEN, decoded);
	} else {
		LOG_WRN("Only decoded %d of %zu byte(s):", bytes_decoded, IR_MESSAGE_LEN);
		for (int i = 0; i < bytes_decoded; i++) {
			char c = (decoded[i] >= 32 && decoded[i] < 127) ? (char)decoded[i] : '?';

			LOG_WRN("  byte %d: 0x%02x ('%c')", i, decoded[i], c);
		}
	}

	return 0;
}
