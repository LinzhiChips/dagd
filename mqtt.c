/*
 * mqtt.c - MQTT interface
 *
 * Copyright (C) 2021, 2022 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <mosquitto.h>

#include "linzhi/alloc.h"
#include "linzhi/common.h"
#include "linzhi/dagalgo.h"

#include "debug.h"
#include "mqtt.h"


#define	POLL_WAIT_MS		200

#define	HOLD_STATE		"epoch_upload "

#define	MQTT_HOST		"localhost"
#define	MQTT_PORT		1883
#define	MQTT_TOPIC_EPOCH	"/mine/epoch"
#define	MQTT_TOPIC_SLOT_EPOCH	"/mine/+/epoch"
#define	MQTT_TOPIC_SLOT0_EPOCH	"/mine/0/epoch"
#define	MQTT_TOPIC_SLOT1_EPOCH	"/mine/1/epoch"
#define	MQTT_TOPIC_CACHE	"/mine/dag-cache"
#define	MQTT_TOPIC_SHUTDOWN	"/sys/shutdown"
#define	MQTT_TOPIC_MINE_STATE	"/mine/+/state"
#define	MQTT_TOPIC_MINE_STATE_0	"/mine/0/state"
#define	MQTT_TOPIC_MINE_STATE_1	"/mine/1/state"
#define	MQTT_TOPIC_MINE_RUNNING	"/mine/running"
#define	MQTT_TOPIC_MINE_RUNNING_0 \
				"/mine/0/running"
#define	MQTT_TOPIC_MINE_RUNNING_1 \
				"/mine/1/running"
#define	MQTT_CLIENT		"dagd"

enum mqtt_qos {
	qos_be		= 0,
	qos_ack		= 1,
	qos_once	= 2
};


bool shutdown_pending = 0;
bool hold = 0;
int curr_algo = -1;
int curr_epoch = -1;
int alt_epoch = -1;
uint64_t curr_block = 0;

static bool limit_subscriptions = 0;


/* ----- Notifications ----------------------------------------------------- */


struct subscription {
	enum mqtt_notify_type type;
	void (*fn)(void *user);
	void *user;
};


static struct subscription *sub = NULL;
static unsigned subs = 0;


static void notify(enum mqtt_notify_type type)
{
	const struct subscription *s;

	for (s  = sub; s != sub + subs; s++)
		if (s->type == type)
			s->fn(s->user);
}


void mqtt_subscribe(enum mqtt_notify_type type,
    void (*fn)(void *user), void *user)
{
	sub = realloc_type_n(sub, subs + 1);
	sub[subs].type = type;
	sub[subs].fn = fn;
	sub[subs].user = user;
	subs++;
}


/* ----- MQTT transmission ------------------------------------------------- */


void mqtt_status(mqtt_handle mqtt, const char *s, bool flush)
{
	static time_t last = 0;
	time_t t;
	int res;

	time(&t);
	if (t == last && !flush)	/* rate-limit to ~1 per second */
		return;
	last = t;
	res = mosquitto_publish(mqtt, NULL, MQTT_TOPIC_CACHE, strlen(s), s,
	    qos_ack, 1);
	if (res != MOSQ_ERR_SUCCESS)
		fprintf(stderr, "mosquitto_publish (%s): %d\n",
		    MQTT_TOPIC_CACHE, res);
}


/* ----- Epoch change ------------------------------------------------------ */


static void process_epoch(unsigned n, const char *names)
{
	const char *next;
	int algo;

	if ((int) n == alt_epoch) {
		debug(0, "selected alternate epoch\n");
		return;
	}
	if (names) {
		next = strchr(names, ' ');
		if (!next) {
			fprintf(stderr, "algorithm name missing in epoch\n");
			return;
		}
		algo = dagalgo_code(next + 1);
		if (algo < 0) {
			fprintf(stderr, "unknown algorithm \"%s\"\n", next + 1);
			return;
		}
	} else {
		algo = da_ethash;
	}
	if (algo == curr_algo && (int) n == curr_epoch)
		return;
	curr_algo = algo;
	curr_epoch = n;
	notify(mqtt_notify_epoch);
}


/* ----- Hold logic -------------------------------------------------------- */


static bool hold_slot[2] = { 0, 0 };
static bool running[2] = { 0, 0 };


static void update_hold(void)
{
	bool next =
	    (hold_slot[0] && running[0]) || (hold_slot[1] && running[1]);

	if (next != hold)
		debug(2, "%s holding", hold ? "end" : "begin");
	hold = next;
	notify(mqtt_notify_mined_state);
}


static double parse_progress(const char *state, char tag)
{
	char find[3] = { tag, ':', 0 };
	const char *s;
	char *end;
	double value;

	s = strstr(state, find);
	if (!s)
		return 0;
	value = strtod(s + 2, &end);
	if (*end && *end != ' ')
		return -1;
	return value;
}


static void process_mine_state(unsigned slot, const char *state)
{
	double done_d, done_a;

	debug(2, "process_mine_state(slot %u, state %s)", slot, state);

	done_d = parse_progress(state, 'D');
	done_a = parse_progress(state, 'A');
	if (done_d < 0 || done_a < 0) {
		debug(0, "process_mine_state: bad progress in \"%s\"", state);
		return;
	}

	bool next_hold = (done_d != 0 && done_d != 1) ||
	    (done_a != 0 && done_a != 1);

	debug(3,
	    "process_mine_state: slot %u, state \"%s\", hold %u,%u, next %u",
	    slot, state, hold_slot[0], hold_slot[1], next_hold);

	hold_slot[slot] = next_hold;
	update_hold();
}


static void process_running(const char *topic, unsigned runs)
{
	if (!strcmp(topic, MQTT_TOPIC_MINE_RUNNING))
		running[0] = running[1] = runs;
	else if (!strcmp(topic, MQTT_TOPIC_MINE_RUNNING_0))
		running[0] = runs;
	else if (!strcmp(topic, MQTT_TOPIC_MINE_RUNNING_1))
		running[1] = runs;
	update_hold();
}


/* ----- MQTT reception ---------------------------------------------------- */


static void message(struct mosquitto *mosq, void *user,
    const struct mosquitto_message *msg)
{
	enum mqtt_notify_type type;
	char *buf, *end;
	unsigned n;

	if (!strcmp(msg->topic, MQTT_TOPIC_EPOCH) ||
	    !strcmp(msg->topic, MQTT_TOPIC_SLOT0_EPOCH) ||
	    !strcmp(msg->topic, MQTT_TOPIC_SLOT1_EPOCH)) {
		type = mqtt_notify_epoch;
	} else if (!strcmp(msg->topic, MQTT_TOPIC_MINE_STATE_0) ||
	    !strcmp(msg->topic, MQTT_TOPIC_MINE_STATE_1)) {
		type = mqtt_notify_mined_state;
	} else if (!strcmp(msg->topic, MQTT_TOPIC_SHUTDOWN)) {
		type = mqtt_notify_shutdown;
	} else if (!strcmp(msg->topic, MQTT_TOPIC_MINE_RUNNING) ||
	    !strcmp(msg->topic, MQTT_TOPIC_MINE_RUNNING_0) ||
	    !strcmp(msg->topic, MQTT_TOPIC_MINE_RUNNING_1)) {
		type = mqtt_notify_running;
	} else {
		fprintf(stderr, "unrecognized topic '%s'\n", msg->topic);
		return;
	}

	buf = alloc_size(msg->payloadlen + 1);
	memcpy(buf, msg->payload, msg->payloadlen);
	buf[msg->payloadlen] = 0;

	if (type == mqtt_notify_mined_state) {
		process_mine_state(!strcmp(msg->topic,
		    MQTT_TOPIC_MINE_STATE_1), buf);
		free(buf);
		return;
	}

	n = strtoul(buf, &end, 0);
	if (*end && *end != ' ') {
		fprintf(stderr, "bad number '%s'\n", buf);
		free(buf);
		return;
	}

	switch (type) {
	case mqtt_notify_epoch:
		if (*end)
			process_epoch(n, end + 1);
		else
			process_epoch(n, NULL);
		break;
	case mqtt_notify_shutdown:
		free(buf);
		shutdown_pending = n;
		notify(mqtt_notify_shutdown);
		break;
	case mqtt_notify_running:
		free(buf);
		process_running(msg->topic, n);
		break;
	default:
		abort();
	}
}


/* ----- MQTT setup -------------------------------------------------------- */


static void connected(struct mosquitto *mosq, void *data, int result)
{
	int res;

	if (result) {
		fprintf(stderr, "connect failed: %d\n", result);
		exit(1);
	}

	res = mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_SHUTDOWN, qos_ack);
	if (res < 0) {
		fprintf(stderr, "mosquitto_subscribe: %d\n", res);
		exit(1);
	}

	if (limit_subscriptions)
		return;

	res = mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_EPOCH, qos_ack);
	if (res < 0) {
		fprintf(stderr, "mosquitto_subscribe: %d\n", res);
		exit(1);
	}
	res = mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_SLOT_EPOCH, qos_ack);
	if (res < 0) {
		fprintf(stderr, "mosquitto_subscribe: %d\n", res);
		exit(1);
	}
	res = mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_MINE_STATE, 0);
	if (res < 0) {
		fprintf(stderr, "mosquitto_subscribe: %d\n", res);
		exit(1);
	}
	res = mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_MINE_RUNNING, qos_ack);
	if (res < 0) {
		fprintf(stderr, "mosquitto_subscribe: %d\n", res);
		exit(1);
	}
	res = mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_MINE_RUNNING_0,
	    qos_ack);
	if (res < 0) {
		fprintf(stderr, "mosquitto_subscribe: %d\n", res);
		exit(1);
	}
	res = mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_MINE_RUNNING_1,
	    qos_ack);
	if (res < 0) {
		fprintf(stderr, "mosquitto_subscribe: %d\n", res);
		exit(1);
	}
}


static void disconnected(struct mosquitto *mosq, void *data, int result)
{
	int res;

	fprintf(stderr, "warning: reconnecting MQTT (disconnect reason %d)",
	    result);
	res = mosquitto_reconnect(mosq);
	if (res != MOSQ_ERR_SUCCESS)
		fprintf(stderr, "mosquitto_reconnect: %s\n",
		    mosquitto_strerror(res));
}


static struct mosquitto *setup_mqtt(const char *broker)
{
	struct mosquitto *mosq;
	char *host = NULL;
	const char *colon;
	char *end;
	int port = MQTT_PORT;
	int res;

	if (broker) {
		colon = strchr(broker, ':');
		if (colon) {
			port = strtoul(colon + 1, &end, 0);
			if (*end) {
				fprintf(stderr, "invalid port \"%s\"\n",
				    colon + 1);
				exit(1);
			}
		}
		host = strndup(broker,
		    colon ? (size_t) (colon - broker) : strlen(broker));
		if (!host) {
			perror("strndup");
			exit(1);
		}
	}
	mosquitto_lib_init();
	mosq = mosquitto_new(NULL, 1, NULL);
	if (!mosq) {
		fprintf(stderr, "mosquitto_new failed\n");
		exit(1);
	}
	res = mosquitto_connect(mosq, host ? host : MQTT_HOST, port, 3600);
	if (res < 0) {
		fprintf(stderr, "mosquitto_connect: %d\n", res);
		exit(1);
	}
	free(host);
	mosquitto_connect_callback_set(mosq, connected);
	mosquitto_disconnect_callback_set(mosq, disconnected);
	mosquitto_message_callback_set(mosq, message);
	return mosq;
}


void mqtt_poll(mqtt_handle mqtt, bool do_wait)
{
	int res;

	res = mosquitto_loop(mqtt, do_wait ? POLL_WAIT_MS : 0, 1);
	if (res < 0) {
		fprintf(stderr, "mosquitto_loop: %d\n", res);
		exit(1);
	}
}


int mqtt_fd(mqtt_handle mqtt)
{
	return mosquitto_socket(mqtt);
}


/* ----- Initialization ---------------------------------------------------- */


mqtt_handle mqtt_init(const char *broker, bool just_one)
{
	limit_subscriptions = just_one;
	return setup_mqtt(broker);
}
