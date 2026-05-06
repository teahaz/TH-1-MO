#include <stdint.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/dt-bindings/pwm/pwm.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>


LOG_MODULE_REGISTER(ir_replay, LOG_LEVEL_INF);


// Carrier config
#define IR_CARRIER_HZ   38000U
#define IR_PERIOD_NS    PWM_HZ(IR_CARRIER_HZ)
#define IR_PULSE_NS     (IR_PERIOD_NS / 3U)   /* ~33% duty */


static const struct pwm_dt_spec ir_pwm = PWM_DT_SPEC_GET(DT_NODELABEL(ir_tx_led));


// This is just a test sequence, it turns on an IR controlled
// light I own.
static const uint32_t raw_data[] = {
    8995, 4461,  638,  512,  588,  528,  595,  532,  569,  563,
     598,  533,  599,  534,  598,  529,  590,  567,  551, 1635,
     617, 1611,  621, 1612,  598, 1638,  592, 1660,  606, 1608,
     619, 1615,  638, 1601,  592, 1654,  581,  536,  588, 1638,
     598,  530,  600,  534,  594,  537,  589, 1644,  589,  562,
     580,  534,  593, 1639,  592,  536,  611, 1627,  583, 1647,
     594, 1663,  574,  534,  590, 1646,  607, 39810, 9035, 2205
};

static const uint32_t raw_data1[] = {
    9015, 4467,  622,  527,  590,  526,  605,  524,  577,  554,
     624,  506,  572,  610,  559,  523,  594,  533,  599, 1622,
     643, 1572,  601, 1626,  662, 1601,  615, 1593,  601, 1634,
     622, 1607,  652, 1606,  607, 1608,  649, 1573,  634, 1607,
     621,  536,  598,  533,  598,  552,  558, 1629,  625,  528,
     594,  541,  593,  509,  594,  538,  641, 1597,  584, 1665,
     582, 1636,  589,  536,  593, 1632,  600, 39817, 9048, 2198
};

static int ir_transmit(const uint32_t *duration_us, size_t count);



int main(void)
{
    int err;

    if (!(err = pwm_is_ready_dt(&ir_pwm)))
    {
        LOG_ERR("PWM device %s is not ready! (err: %d)", ir_pwm.dev->name, err);
        return err;
    }


    /*
     * Make sure the carrier starts off. Setting period+pulse with
     * pulse=0 establishes the period without producing output.
     */
    if ((err = pwm_set_dt(&ir_pwm, IR_PERIOD_NS, 0)) != 0)
    {
        LOG_ERR("PWM initial set failed! (err: %d)", err);
        return err;
    }

    LOG_INF("IR replay ready. Period=%ld ns, pulse=%ld ns, %zu symbols", IR_PERIOD_NS, IR_PULSE_NS, ARRAY_SIZE(raw_data));



    while (1)
    {
        LOG_INF("Transmitting...");
        if ((err = ir_transmit(raw_data, ARRAY_SIZE(raw_data))) != 0)
        {
            LOG_ERR("Transmit failed! (err: %d)", err);
            return err;
        }
        k_sleep(K_SECONDS(1));

        if ((err = ir_transmit(raw_data1, ARRAY_SIZE(raw_data1))) != 0)
        {
            LOG_ERR("Transmit failed! (err: %d)", err);
            return err;
        }
        k_sleep(K_SECONDS(1));
    }
}



static int ir_transmit(const uint32_t *duration_us, size_t count)
{
    int err;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t us = duration_us[i];


        // Even offsets in the array are marks, odd ones are 0s
        if ((i & 1U) == 0U)
        {
            if ((err = pwm_set_pulse_dt(&ir_pwm, IR_PULSE_NS)) != 0)
            {
                LOG_ERR("PWM pulse generation failed! (err: %d)", err);
                return err;
            }
            k_busy_wait(us);

            if ((err = pwm_set_pulse_dt(&ir_pwm, 0)) != 0)
            {
                LOG_ERR("PWM off failed! (err: %d)", err);
                return err;
            }
        }
        else
        {
            k_busy_wait(us);
        }
    }
    return 0;
}















/*
 * Notes on BLE coexistence -- courtesy of Dario himself
 * ------------------------
 * This implementation runs from the main (preemptible) thread and uses
 * k_busy_wait() between PWM transitions. It will NOT disrupt BLE timing,
 * but BLE events WILL preempt it and insert gaps (typically <3 ms) into
 * the IR frame.
 *
 * If decode reliability with BLE active is poor, in order of escalating
 * effort:
 *   1. Re-send the frame 2-3x (NEC remotes do this anyway).
 *   2. Move the transmit loop into a cooperative-priority thread
 *      (K_PRIO_COOP(7) or so). This stops other application threads
 *      from preempting, but BLE interrupts still will.
 *   3. Switch the carrier driver to nrfx_pwm sequence playback, which
 *      runs entirely in the PWM peripheral via EasyDMA. The CPU only
 *      sets it up and gets a "done" interrupt — BLE can't disturb it.
 */
