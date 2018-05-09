/*
 * Copyright (C) 2017 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup    
 * @ingroup     
 * @brief       
 * @{
 * @file		umdk-adxl345.c
 * @brief       umdk-adxl345 module implementation
 * @author		Oleg Artamonov
 */

#ifdef __cplusplus
extern "C" {
#endif

/* define is autogenerated, do not change */
#undef _UMDK_MID_
#define _UMDK_MID_ UNWDS_ADXL345_MODULE_ID

/* define is autogenerated, do not change */
#undef _UMDK_NAME_
#define _UMDK_NAME_ "adxl345"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "xtimer.h"
#include "periph/gpio.h"
#include "periph/i2c.h"

#include "board.h"

#include "adxl345.h"
#include "adxl345_params.h"

#include "unwds-common.h"
#include "umdk-adxl345.h"

#include "thread.h"
#include "rtctimers-millis.h"

adxl345_t dev;
adxl345_data_t adxl345_data;

static int event_detected = 0;
static int event_detected_time;

static uwnds_cb_t *callback;

static kernel_pid_t timer_pid;

static msg_t timer_msg = {};
static rtctimers_millis_t timer;

static bool is_polled = false;

static struct {
	uint8_t measure_period_sec;
} adxl345_config;

static bool init_sensor(void) {
	printf("[umdk-" _UMDK_NAME_ "] Initializing ADXL345\n");
    
    dev.i2c = I2C_DEV(UMDK_ADXL345_I2C);
    dev.addr = ADXL345_PARAM_ADDR;
	return adxl345_init(&dev, (adxl345_params_t*)adxl345_params) == ADXL345_OK;
}

static void prepare_result(module_data_t *data) {
    adxl345_set_measure(&dev);
    xtimer_spin(xtimer_ticks_from_usec(20000));
	adxl345_read(&dev, &adxl345_data);
    adxl345_set_sleep(&dev);
    
    int x = (int)adxl345_data.x;
    int y = (int)adxl345_data.y;
    int z = (int)adxl345_data.z;
    
	printf("Acceleration: X %d mg, Y %d mg, Z %d mg\n", x, y, z);
    
    int theta = 0;
    if (z != 0) {
        theta = 573*atanf(sqrtf(x*x + y*y)/((float)z));
    }
    
    char angle[10];
    int_to_float_str(angle, theta, 1);
    
    printf("Angle: %s degrees\n", angle);

    data->data[0] = _UMDK_MID_;
    
    if ((theta > 150) || (theta < -150))  {
        printf("Significant tilt detected: %d\n", theta);
        data->data[1] = 1;
    } else {
        data->data[1] = 0;
    }
    
    data->length = 2;
}

static void *timer_thread(void *arg) {
    (void) arg;
    
    msg_t msg;
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);

    puts("[umdk-" _UMDK_NAME_ "] Periodic publisher thread started");

    while (1) {
        msg_receive(&msg);

        module_data_t data = {};
        data.as_ack = is_polled;
        is_polled = false;

        prepare_result(&data);

        if ((!event_detected) && (data.data[1] == 1)) {
            /* Notify the application */
            callback(&data);
            event_detected = 1;
        }
        
        if ((event_detected) && (data.data[1] == 0)) {
            event_detected = 0;
            event_detected_time = 0;
        }
        
        if ((event_detected) && (data.data[1] == 1)) {
            event_detected_time++;
        }
        
        if (((event_detected_time % 300) == 0) && (data.data[1] == 1)) {
            callback(&data);
        }
            
        
        /* Restart after delay */
        rtctimers_millis_set_msg(&timer, 1000 * adxl345_config.measure_period_sec, &timer_msg, timer_pid);
    }

    return NULL;
}

static void reset_config(void) {
	adxl345_config.measure_period_sec = UMDK_ADXL345_MEASURE_PERIOD_SEC;
}

static void init_config(void) {
	reset_config();

	if (!unwds_read_nvram_config(_UMDK_MID_, (uint8_t *) &adxl345_config, sizeof(adxl345_config))) {
        reset_config();
    }

}

static inline void save_config(void) {
	unwds_write_nvram_config(_UMDK_MID_, (uint8_t *) &adxl345_config, sizeof(adxl345_config));
}

static void set_period (int period) {
    rtctimers_millis_remove(&timer);
    adxl345_config.measure_period_sec = period;
	save_config();

	/* Don't restart timer if new period is zero */
	if (adxl345_config.measure_period_sec) {
		rtctimers_millis_set_msg(&timer, 1000*adxl345_config.measure_period_sec, &timer_msg, timer_pid);
		printf("[umdk-" _UMDK_NAME_ "] Period set to %d sec\n", adxl345_config.measure_period_sec);
	} else {
		puts("[umdk-" _UMDK_NAME_ "] Timer stopped");
	}
}

int umdk_adxl345_shell_cmd(int argc, char **argv) {
    if (argc == 1) {
        puts (_UMDK_NAME_ " get - get results now");
        puts (_UMDK_NAME_ " send - get and send results now");
        puts (_UMDK_NAME_ " period <N> - set period to N seconds");
        puts (_UMDK_NAME_ " - reset settings to default");
        return 0;
    }
    
    char *cmd = argv[1];
	
    if (strcmp(cmd, "get") == 0) {
        module_data_t data = {};
        prepare_result(&data);
    }
    
    if (strcmp(cmd, "send") == 0) {
        is_polled = true;
		/* Send signal to publisher thread */
		msg_send(&timer_msg, timer_pid);
    }
    
    if (strcmp(cmd, "period") == 0) {
        char *val = argv[2];
        set_period(atoi(val));
    }
    
    if (strcmp(cmd, "reset") == 0) {
        reset_config();
        save_config();
    }
    
    return 1;
}

static void btn_connect(void* arg) {
    (void) arg;
    
    is_polled = false;
    msg_send(&timer_msg, timer_pid);
}


void umdk_adxl345_init(uint32_t *non_gpio_pin_map, uwnds_cb_t *event_callback) {
	(void) non_gpio_pin_map;

	callback = event_callback;

	init_config();
	printf("[umdk-" _UMDK_NAME_ "] Period: %d sec\n", adxl345_config.measure_period_sec);

	if (!init_sensor()) {
		puts("[umdk-" _UMDK_NAME_ "] Unable to init sensor!");
        return;
	}

	/* Create handler thread */
	char *stack = (char *) allocate_stack(UMDK_ADXL345_STACK_SIZE);
	if (!stack) {
		puts("[umdk-" _UMDK_NAME_ "] unable to allocate memory. Is too many modules enabled?");
		return;
	}
    
    unwds_add_shell_command(_UMDK_NAME_, "type '" _UMDK_NAME_ "' for commands list", umdk_adxl345_shell_cmd);
    
    gpio_init_int(UNWD_CONNECT_BTN, GPIO_IN_PU, GPIO_FALLING, btn_connect, NULL);
    
	timer_pid = thread_create(stack, UMDK_ADXL345_STACK_SIZE, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, timer_thread, NULL, "adxl345 thread");

    /* Start publishing timer */
	rtctimers_millis_set_msg(&timer, 1000 * adxl345_config.measure_period_sec, &timer_msg, timer_pid);
}

static void reply_fail(module_data_t *reply) {
	reply->length = 2;
	reply->data[0] = _UMDK_MID_;
	reply->data[1] = 255;
}

static void reply_ok(module_data_t *reply) {
	reply->length = 2;
	reply->data[0] = _UMDK_MID_;
	reply->data[1] = 0;
}

bool umdk_adxl345_cmd(module_data_t *cmd, module_data_t *reply) {
	if (cmd->length < 1) {
		reply_fail(reply);
		return true;
	}

	umdk_adxl345_cmd_t c = cmd->data[0];
	switch (c) {
	case UMDK_ADXL345_CMD_SET_PERIOD: {
		if (cmd->length != 2) {
			reply_fail(reply);
			break;
		}

		uint8_t period = cmd->data[1];
		set_period(period);

		reply_ok(reply);
		break;
	}

	case UMDK_ADXL345_CMD_POLL:
		is_polled = true;

		/* Send signal to publisher thread */
		msg_send(&timer_msg, timer_pid);

		return false; /* Don't reply */

	default:
		reply_fail(reply);
		break;
	}

	return true;
}

#ifdef __cplusplus
}
#endif
