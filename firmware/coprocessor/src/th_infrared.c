#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/dt-bindings/pwm/pwm.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_core.h>


LOG_MODULE_REGISTER(TH_IR, LOG_LEVEL_INF);

static bool ir_tx_setup_done = false;

// Carrier config
#define IR_CARRIER_HZ   38000U
#define IR_PERIOD_NS    PWM_HZ(IR_CARRIER_HZ)
#define IR_PULSE_NS     (IR_PERIOD_NS / 3U)   /* ~33% duty */


static const struct pwm_dt_spec ir_pwm = PWM_DT_SPEC_GET(DT_NODELABEL(ir_tx_led));


int8_t ir_tx_setup();
int8_t ir_tx(const uint32_t *duration_us, size_t count);
int ir_txt_process_sample_count(char *input);
int ir_txt_process(char *input, uint32_t *raw_frame, int num_samples);

/* ----------------------------- */
/*           dispatch            */
/* ----------------------------- */

int8_t ir_argument_dispatch(char *input)
{
    int err;
    char *unprocessed;
    char *token = strtok_r(input, " ", &unprocessed);

    if (token == NULL)
    {
        LOG_WRN("Incomplete IR command! (%s)", input);
        return -EINVAL;
    }


    if (strcmp(token, "play") == 0)
    {
        if (!ir_tx_setup_done)
        {
            if ((err = ir_tx_setup()) != 0)
            {
                LOG_ERR("IR setup failed! (err: %d)", err);
                return err;
            }
        }

        int num_samples = ir_txt_process_sample_count(unprocessed);
        uint32_t raw_frame[num_samples];
        if ((err = ir_txt_process(unprocessed, raw_frame, num_samples)) != 0)
        {
            LOG_ERR("String processing error! (err: %d)", err);
            return err;
        }

        return ir_tx(raw_frame, num_samples);
    }

    return 0;
}



/* ----------------------------- */
/*              IR TX            */
/* ----------------------------- */



int ir_txt_process(char *input, uint32_t *raw_frame, int num_samples)
{
    int n;
    int consumed = 0;

    for (size_t i = 0; i < num_samples; i++)
    {
        if ((n = sscanf(input, "%u%n", &raw_frame[i], &consumed)) != 1)
        {
            LOG_ERR("Error processing user string, invalid data at index %d", i);
            return -EINVAL;
        }
        input += consumed;
    }


    return 0;
}

int ir_txt_process_sample_count(char *input)
{
    int samples = 0;
    char current;
    char previous = ' ';
    size_t inputlen = strlen(input);

    for (size_t i = 0; i < inputlen; i++)
    {
        current = input[i];

        if (current != ' ' && previous == ' ')
            samples++;

        previous = input[i];
    }
    return samples;
}


int8_t ir_tx_setup()
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

    LOG_INF("IR_TX module ready! Period=%ld ns, pulse=%ld ns", IR_PERIOD_NS, IR_PULSE_NS);

    ir_tx_setup_done = true;

    return 0;
}



// NOTE this function takes a binary input.
// I will either need to change it to take a text input or make
// a different function to translate it (probably the second).
int8_t ir_tx(const uint32_t *duration_us, size_t count)
{

    if (!ir_tx_setup_done)
    {
        LOG_ERR("IR_TX function called before IR pins are setup!");
        return -1;
    }

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

