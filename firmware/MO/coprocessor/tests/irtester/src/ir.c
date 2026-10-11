#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/time_units.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>

#include "ir.h"

LOG_MODULE_REGISTER(ir, LOG_LEVEL_INF);

static const struct pwm_dt_spec pwm_ir = PWM_DT_SPEC_GET(DT_PATH(zephyr_user));
static const struct gpio_dt_spec ir_rx = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ir_rx_gpios);

static struct gpio_callback ir_rx_cb_data;
static struct ir_edge capture_buf[IR_CAPTURE_MAX];
static volatile int capture_count;

static inline void ir_carrier_on(void)
{
	pwm_set_dt(&pwm_ir, pwm_ir.period, pwm_ir.period / 2);
}

static inline void ir_carrier_off(void)
{
	pwm_set_dt(&pwm_ir, pwm_ir.period, 0);
}

static void ir_mark(uint32_t us)
{
	ir_carrier_on();
	k_busy_wait(us);
	ir_carrier_off();
}

static void ir_space(uint32_t us)
{
	k_busy_wait(us);
}

static void ir_rx_isr(const struct device *port, struct gpio_callback *cb,
		       gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (capture_count >= IR_CAPTURE_MAX) {
		return;
	}

	capture_buf[capture_count].timestamp_cycles = k_cycle_get_32();
	capture_buf[capture_count].active = gpio_pin_get_dt(&ir_rx) == 1;
	capture_count++;
}

int ir_tx_init(void)
{
	if (!pwm_is_ready_dt(&pwm_ir)) {
		LOG_ERR("PWM device for IR TX is not ready");
		return -ENODEV;
	}

	return pwm_set_dt(&pwm_ir, pwm_ir.period, 0);
}

int ir_rx_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&ir_rx)) {
		LOG_ERR("GPIO device for IR RX is not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&ir_rx, GPIO_INPUT);
	if (err) {
		LOG_ERR("Failed to configure IR RX pin as input (err: %d)", err);
		return err;
	}

	gpio_init_callback(&ir_rx_cb_data, ir_rx_isr, BIT(ir_rx.pin));

	err = gpio_add_callback_dt(&ir_rx, &ir_rx_cb_data);
	if (err) {
		LOG_ERR("Failed to add IR RX gpio callback (err: %d)", err);
		return err;
	}

	return 0;
}

void ir_send_byte(uint8_t byte)
{
	ir_mark(IR_HEADER_MARK_US);
	ir_space(IR_HEADER_SPACE_US);

	for (int i = 0; i < 8; i++) {
		ir_mark(IR_BIT_MARK_US);
		if (byte & BIT(i)) {
			ir_space(IR_ONE_SPACE_US);
		} else {
			ir_space(IR_ZERO_SPACE_US);
		}
	}

	ir_mark(IR_BIT_MARK_US);
	ir_carrier_off();
}

void ir_rx_capture_start(void)
{
	capture_count = 0;
	gpio_pin_interrupt_configure_dt(&ir_rx, GPIO_INT_EDGE_BOTH);
}

int ir_rx_capture_stop(void)
{
	gpio_pin_interrupt_configure_dt(&ir_rx, GPIO_INT_DISABLE);
	return capture_count;
}

static uint32_t edge_delta_us(int idx)
{
	uint32_t delta_cycles = capture_buf[idx + 1].timestamp_cycles -
				 capture_buf[idx].timestamp_cycles;

	return k_cyc_to_us_floor32(delta_cycles);
}

/* Finds a header mark at or after idx and returns the index of the start of
 * bit 0's mark (i.e. just past the header space), or -1 if no complete
 * header is found. Generous tolerance on the header mark's duration since
 * the two halves of this board are not aimed at each other and some
 * corruption is expected. */
static int find_header(int idx, int n)
{
	while (idx < n - 1) {
		if (capture_buf[idx].active &&
		    edge_delta_us(idx) > IR_HEADER_MARK_US / 2 &&
		    edge_delta_us(idx) < IR_HEADER_MARK_US * 2) {
			break;
		}
		idx++;
	}

	if (idx >= n - 1) {
		return -1;
	}

	idx++; /* the header space */

	if (idx >= n - 1 || capture_buf[idx].active) {
		return -1;
	}

	return idx + 1; /* start of bit 0's mark */
}

/* Decodes one 8 bit frame starting at idx (the start of bit 0's mark).
 * Returns the index just past the frame (its stop mark), and writes the
 * decoded byte to *out_byte. Stops early, returning a partial byte, if the
 * capture runs out of edges mid-frame. */
static int decode_frame(int idx, int n, uint8_t *out_byte)
{
	uint8_t byte = 0;

	for (int bit = 0; bit < 8 && idx < n - 1; bit++) {
		if (!capture_buf[idx].active) {
			break;
		}
		idx++; /* the bit's space */
		if (idx >= n - 1 || capture_buf[idx].active) {
			break;
		}

		if (edge_delta_us(idx) > (IR_ZERO_SPACE_US + IR_ONE_SPACE_US) / 2) {
			byte |= BIT(bit);
		}

		idx++;
	}

	*out_byte = byte;
	return idx;
}

int ir_decode_capture(uint8_t *out_bytes, int max_bytes)
{
	int n = capture_count;
	int idx = 0;
	int bytes_decoded = 0;

	LOG_INF("Captured %d edge(s)", n);

	for (int i = 0; i < n - 1; i++) {
		LOG_INF("  [%d] %s for %u us", i,
			capture_buf[i].active ? "mark" : "space", edge_delta_us(i));
	}

	if (n < 3) {
		LOG_WRN("Not enough edges captured to attempt a decode");
		return 0;
	}

	while (bytes_decoded < max_bytes) {
		int bit_start = find_header(idx, n);

		if (bit_start < 0) {
			break;
		}

		idx = decode_frame(bit_start, n, &out_bytes[bytes_decoded]);
		LOG_INF("Decoded byte %d: 0x%02x", bytes_decoded, out_bytes[bytes_decoded]);
		bytes_decoded++;
	}

	if (bytes_decoded < max_bytes) {
		LOG_WRN("Only decoded %d of %d expected byte(s)", bytes_decoded, max_bytes);
	}

	return bytes_decoded;
}
