/*
 * Control channel before capture: the SPI control binding (spec 9.2.1) on SPIS1, the
 * hello, and configuration messages.
 */

#ifndef CONTROL_H
#define CONTROL_H

#include <stdbool.h>

#include "capture.h"


void control_init(void);
bool control_service(void);
const struct capture_config *control_config(void);

#endif
