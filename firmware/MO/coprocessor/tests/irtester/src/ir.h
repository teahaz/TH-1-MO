#ifndef IR_H_
#define IR_H_

#include <stdint.h>

/* NEC-style timing unit, in microseconds. Standard for 38kHz AGC receivers
 * such as the VS1838B fitted on this board (S1). */
#define IR_UNIT_US        560
#define IR_HEADER_MARK_US  (16 * IR_UNIT_US)
#define IR_HEADER_SPACE_US (8  * IR_UNIT_US)
#define IR_BIT_MARK_US     (1  * IR_UNIT_US)
#define IR_ZERO_SPACE_US   (1  * IR_UNIT_US)
#define IR_ONE_SPACE_US    (3  * IR_UNIT_US)

/* 4 frames (header + 8 bit mark/space pairs + stop mark = 20 timestamps
 * each), plus headroom. */
#define IR_CAPTURE_MAX 96

struct ir_edge {
	uint32_t timestamp_cycles;
	uint8_t active; /* 1 = mark (receiver output active), 0 = space */
};

int ir_tx_init(void);
int ir_rx_init(void);

/* Encodes and sends one byte, LSB first, as a NEC-style header + 8 bit
 * mark/space pairs + stop mark. Blocks for the duration of the transmission. */
void ir_send_byte(uint8_t byte);

/* Clears the capture buffer and (re)arms the RX interrupt. */
void ir_rx_capture_start(void);

/* Disarms the RX interrupt and returns how many edges were captured. */
int ir_rx_capture_stop(void);

/* Logs the raw captured edges and attempts a best-effort decode of up to
 * max_bytes NEC-style frames, stopping early if a header can't be found or
 * a frame runs out of edges. Returns the number of bytes fully decoded
 * (0 to max_bytes); out_bytes holds that many decoded bytes. */
int ir_decode_capture(uint8_t *out_bytes, int max_bytes);

#endif /* IR_H_ */
