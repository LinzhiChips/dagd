/*
 * mqtt.h - MQTT interface
 *
 * Copyright (C) 2021 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#ifndef DAGD_MQTT_H
#define	DAGD_MQTT_H

#include <stdbool.h>
#include <stdint.h>


struct mosquitto;

typedef struct mosquitto *mqtt_handle;

enum mqtt_notify_type {
	mqtt_notify_epoch,
	mqtt_notify_mined_state,
	mqtt_notify_shutdown,
};


extern bool shutdown_pending;
extern bool hold;
extern int curr_algo;
extern uint16_t curr_epoch;
extern uint64_t curr_block;


void mqtt_subscribe(enum mqtt_notify_type type,
    void (*fn)(void *user), void *user);

void mqtt_status(mqtt_handle mqtt, const char *s, bool flush);

void mqtt_poll(mqtt_handle mqtt, bool do_wait);
int mqtt_fd(mqtt_handle mqtt);

mqtt_handle mqtt_init(const char *broker);

#endif /* !DAGD_MQTT_H */
