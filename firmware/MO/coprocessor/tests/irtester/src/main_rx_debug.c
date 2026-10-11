#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>

#include "ir.h"

LOG_MODULE_REGISTER(irtester, LOG_LEVEL_INF);

int main(void)
{
	int err;
	uint8_t unused[1];

	LOG_INF("irtester (rx debug) starting");

	err = ir_rx_init();
	if (err) {
		return err;
	}

	LOG_INF("Receiver armed. Point a remote at it.");

	while (1) {
		ir_rx_capture_start();
		k_msleep(200);
		int edges = ir_rx_capture_stop();

		if (edges > 0) {
			/* max_bytes=0: dumps the raw captured edges without
			 * attempting our own NEC frame decode, since a real
			 * remote won't necessarily use our timing. */
			ir_decode_capture(unused, 0);
		}
	}
}
