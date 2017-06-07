/*
* Copyright (c) 2016 Baidu, Inc. All Rights Reserved.
*
* Licensed to the Apache Software Foundation (ASF) under one or more
* contributor license agreements.  See the NOTICE file distributed with
* this work for additional information regarding copyright ownership.
* The ASF licenses this file to You under the Apache License, Version 2.0
* (the "License"); you may not use this file except in compliance with
* the License.  You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include "device_management.h"
#include "device_management_conf.h"
#include "device_management_util.h"

#define _GNU_SOURCE

#include <uuid/uuid.h>
#include <MQTTAsync.h>
#include <log4c.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <zconf.h>
#include <stdio.h>

#define SUB_TOPIC_COUNT 7

#define MAX_REQUEST_ID_LENGTH 64

static const char *log4c_category_name = "device_management";

static const char *REQUEST_ID_KEY = "requestId";

static const char *CODE_KEY = "code";

static const char *MESSAGE_KEY = "message";

static const char *REPORTED = "reported";

static const char *TOPIC_PREFIX = "baidu/iot/shadow";

static volatile bool hasInit = false;

static log4c_category_t *category = NULL;

/* Memorize all topics device management uses so that we don't compose them each time. */
typedef struct {
    char *update;
    char *updateAccepted;
    char *updateRejected;
    char *get;
    char *getAccepted;
    char *getRejected;
    char *delete;
    char *deleteAccepted;
    char *deleteRejected;
    char *delta;
    char *deltaRejected;
    char *subTopics[SUB_TOPIC_COUNT];
} TopicContract;

typedef struct {
    const char *key; // key 可以为NULL，表示匹配根。
    ShadowPropertyDeltaCallback cb; // 收到更新之后，会调用这个回调。
} ShadowPropertyDeltaHandler;

/* Manages shadow property handlers. It's a add-only collection. */
typedef struct {
    ShadowPropertyDeltaHandler vault[MAX_SHADOW_PROPERTY_HANDLER];
    int index;
    /* This data is accessed from MQTT client's callback. */
    pthread_mutex_t mutex;
} PropertyHandlerTable;

typedef struct {
    char requestId[MAX_REQUEST_ID_LENGTH];
    ShadowAction action;
    ShadowActionCallback callback;
    void *callbackContext;
    time_t timestamp;
    uint8_t timeout;
    bool free;
} InFlightMessage;

typedef struct {
    InFlightMessage vault[MAX_IN_FLIGHT_MESSAGE];
    /* This data is also accessed from MQTT client's callback. */
    pthread_mutex_t mutex;
} InFlightMessageList;

typedef struct device_management_client_t {
    MQTTAsync mqttClient;
    int errorCode;
    char *errorMessage;
    bool hasSubscribed;
    char *username;
    char *password;
    char *deviceName;
    TopicContract *topicContract;
    PropertyHandlerTable properties;
    InFlightMessageList messages;
    pthread_mutex_t mutex;
} device_management_client_t;

typedef struct {
    device_management_client_t *members[MAX_CLIENT];
    pthread_mutex_t mutex;
} ClientGroup;

static ClientGroup allClients;

static pthread_t inFlightMessageKeeper;

static TopicContract *
topic_contract_create(const char *deviceName);

static void
topic_contract_destroy(TopicContract *topics);

static bool
client_group_add(ClientGroup *group, device_management_client_t *client);

static bool
client_group_remove(ClientGroup *group, device_management_client_t *client);

static void
in_flight_message_house_keep(device_management_client_t *c);

static void *
in_flight_message_house_keep_proc(void *ignore);

static const char *
message_get_request_id(const cJSON *payload);

static bool
device_management_is_connected(DeviceManagementClient client);

static bool
device_management_is_connected2(device_management_client_t *c);

static DmReturnCode
device_management_shadow_send_json(device_management_client_t *c, const char *topic,
                                   const char *requestId, cJSON *payload);

static DmReturnCode
device_management_shadow_send(DeviceManagementClient client, ShadowAction action, cJSON *payload,
                              ShadowActionCallback callback,
                              void *context, uint8_t timeout);

static int
device_management_shadow_handle_response(device_management_client_t *c, const char *requestId, ShadowAction action,
                                         ShadowAckStatus status,
                                         cJSON *payload);

static DmReturnCode
device_management_delta_arrived(device_management_client_t *c, cJSON *payload);

static void
mqtt_on_connected(void *context, char *cause);

static void
mqtt_on_connection_lost(void *context, char *cause);

static void
mqtt_on_connect_success(void *context, MQTTAsync_successData *response);

static void
mqtt_on_connect_failure(void *context, MQTTAsync_failureData *response);

static void
mqtt_on_publish_success(void *context, MQTTAsync_successData *response);

static void
mqtt_on_publish_failure(void *context, MQTTAsync_failureData *response);

static int
mqtt_on_message_arrived(void *context, char *topicName, int topicLen, MQTTAsync_message *message);

static void
mqtt_on_delivery_complete(void *context, MQTTAsync_token token);

DmReturnCode
device_management_init() {
    if (hasInit) {
        log4c_category_log(category, LOG4C_PRIORITY_WARN, "already initialized.");
        return SUCCESS;
    }

    if (log4c_init()) {
        fprintf(stderr, "log4c init failed.\n");
        return FAILURE;
    }

    category = log4c_category_new(log4c_category_name);

    log4c_category_log(category, LOG4C_PRIORITY_INFO, "initialized.");

//    dump_log4c_conf();

    pthread_mutex_init(&(allClients.mutex), NULL);
    pthread_create(&inFlightMessageKeeper, NULL, in_flight_message_house_keep_proc, NULL);

    return SUCCESS;
}

DmReturnCode
device_management_fini() {
    if (!hasInit) {
        printf("not initialized. no clean up needed.");
        return SUCCESS;
    }
    hasInit = false;
    // Destroy
    pthread_cancel(inFlightMessageKeeper);

    log4c_category_log(category, LOG4C_PRIORITY_INFO, "cleaned up.");

    // Destroy log4c.
    if (category != NULL) {
        log4c_category_delete(category);
    }
    log4c_fini();
    return SUCCESS;
}

DmReturnCode
device_management_create(DeviceManagementClient *client, const char *broker, const char *deviceName,
                         const char *username, const char *password) {
    int rc;
    int i;

    device_management_client_t *c = malloc(sizeof(device_management_client_t));

    // TODO: validate arguments.
    rc = MQTTAsync_create(&(c->mqttClient), broker, deviceName, MQTTCLIENT_PERSISTENCE_NONE, NULL);

    if (rc == EXIT_FAILURE) {
        log4c_category_log(category, LOG4C_PRIORITY_ERROR, "Failed to create. rc=%d.", rc);
        free(c);
        return FAILURE;
    }

    MQTTAsync_setCallbacks(c->mqttClient, c, mqtt_on_connection_lost, mqtt_on_message_arrived,
                           mqtt_on_delivery_complete);
    MQTTAsync_setConnected(c->mqttClient, c, mqtt_on_connected);

    c->errorMessage = NULL;
    c->username = strdup(username);
    c->password = strdup(password);
    c->deviceName = strdup(deviceName);
    // TODO: check rc for below statements.
    c->topicContract = topic_contract_create(deviceName);
    // TODO: set magic to c
    // TODO: set context.
    c->properties.index = 0;
    pthread_mutex_init(&(c->properties.mutex), NULL);
    for (i = 0; i < MAX_IN_FLIGHT_MESSAGE; ++i) {
        c->messages.vault[i].free = true;
    }
    pthread_mutex_init(&(c->messages.mutex), NULL);
    pthread_mutex_init(&(c->mutex), NULL);
    *client = c;
    client_group_add(&allClients, c);

    log4c_category_log(category, LOG4C_PRIORITY_INFO, "created. broker=%s, deviceName=%s.",
                       broker, deviceName);
    return SUCCESS;
}

DmReturnCode
device_management_connect(DeviceManagementClient client) {
    int rc;
    device_management_client_t *c = client;
    MQTTAsync_connectOptions connectOptions = MQTTAsync_connectOptions_initializer;
    connectOptions.keepAliveInterval = KEEP_ALIVE;
    connectOptions.cleansession = 1;
    connectOptions.username = c->username;
    connectOptions.password = c->password;
    connectOptions.automaticReconnect = true;
    connectOptions.onSuccess = mqtt_on_connect_success;
    connectOptions.onFailure = mqtt_on_connect_failure;
    connectOptions.context = c;
    connectOptions.connectTimeout = CONNECT_TIMEOUT;

    log4c_category_log(category, LOG4C_PRIORITY_INFO, "connecting to server.");
    rc = MQTTAsync_connect(c->mqttClient, &connectOptions);
    if (rc != MQTTASYNC_SUCCESS) {
        log4c_category_log(category, LOG4C_PRIORITY_ERROR, "Failed to start connecting. rc=%d.", rc);
        return FAILURE;
    }

    while (!MQTTAsync_isConnected(c->mqttClient) && c->errorMessage == NULL) {
        sleep(1);
    }

    if (MQTTAsync_isConnected(c->mqttClient)) {
        log4c_category_log(category, LOG4C_PRIORITY_INFO, "MQTT connected.");
        return SUCCESS;
    } else if (c->errorMessage != NULL) {
        log4c_category_log(category, LOG4C_PRIORITY_ERROR, "MQTT connect failed. code=%d, message=%s.", c->errorCode,
                           c->errorMessage);
        return FAILURE;
    }
}

DmReturnCode
device_management_shadow_update(DeviceManagementClient client, cJSON *reported, ShadowActionCallback callback,
                                void *context, uint8_t timeout) {
    DmReturnCode rc;

    cJSON *payload = cJSON_CreateObject();

    cJSON_AddItemToObject(payload, REPORTED, reported);

    rc = device_management_shadow_send(client, SHADOW_UPDATE, payload, callback, context, timeout);

    cJSON_DetachItemViaPointer(payload, reported);
    cJSON_Delete(payload);

    if (rc != SUCCESS) {
        log4c_category_log(category, LOG4C_PRIORITY_ERROR, "device_management_shadow_update rc=%d", rc);
    }
    return rc;
}

DmReturnCode
device_management_shadow_get(DeviceManagementClient client, ShadowActionCallback callback, void *context,
                             uint8_t timeout) {
    DmReturnCode rc;
    cJSON *payload = cJSON_CreateObject();

    rc = device_management_shadow_send(client, SHADOW_GET, payload, callback, context, timeout);

    cJSON_Delete(payload);

    if (rc != SUCCESS) {
        log4c_category_log(category, LOG4C_PRIORITY_ERROR, "device_management_shadow_get rc=%d", rc);
    }
    return rc;
}

DmReturnCode
device_management_shadow_delete(DeviceManagementClient client, ShadowActionCallback callback, void *context,
                             uint8_t timeout) {
    DmReturnCode rc;
    cJSON *payload = cJSON_CreateObject();

    rc = device_management_shadow_send(client, SHADOW_DELETE, payload, callback, context, timeout);

    cJSON_Delete(payload);

    if (rc != SUCCESS) {
        log4c_category_log(category, LOG4C_PRIORITY_ERROR, "device_management_shadow_delete rc=%d", rc);
    }
    return rc;
}

DmReturnCode
device_management_shadow_register_delta(DeviceManagementClient client, const char *key, ShadowPropertyDeltaCallback cb) {
    DmReturnCode rc = SUCCESS;

    if (client == NULL || cb == NULL) {
        exit_null_pointer();
    }

    if (!device_management_is_connected(client)) {
        return NOT_CONNECTED;
    }

    device_management_client_t *c = client;

    pthread_mutex_lock(&(c->properties.mutex));
    if (c->properties.index >= MAX_SHADOW_PROPERTY_HANDLER) {
        rc = TOO_MANY_SHADOW_PROPERTY_HANDLER;
    } else {
        c->properties.vault[c->properties.index].key = key == NULL ? NULL : strdup(key);
        c->properties.vault[c->properties.index].cb = cb;
        c->properties.index++;
    }
    pthread_mutex_unlock(&(c->properties.mutex));

    if (rc != SUCCESS) {
        log4c_category_log(category, LOG4C_PRIORITY_ERROR, "device_management_shadow_register_delta rc=%d", rc);
    }
    return rc;
}

DmReturnCode device_management_destroy(DeviceManagementClient client) {
    device_management_client_t *c = client;
    if (c == NULL) {
        log4c_category_log(category, LOG4C_PRIORITY_ERROR, "bad client.");
    } else {
        client_group_remove(&allClients, c);
        safe_free(&(c->username));
        safe_free(&(c->password));
        safe_free(&(c->deviceName));
        safe_free(&(c->errorMessage));
        free(c->errorMessage);
        MQTTAsync_disconnect(c->mqttClient, NULL);
        MQTTAsync_destroy(&(c->mqttClient));
        topic_contract_destroy(c->topicContract);
        free(client);
    }

    return SUCCESS;
}

TopicContract *
topic_contract_create(const char *deviceName) {
    int rc;
    TopicContract *t = malloc(sizeof(TopicContract));
    check_malloc_result(t);

    rc = asprintf(&(t->update), "%s/%s/update", TOPIC_PREFIX, deviceName);
    rc = asprintf(&(t->updateAccepted), "%s/%s/update/accepted", TOPIC_PREFIX, deviceName);
    rc = asprintf(&(t->updateRejected), "%s/%s/update/rejected", TOPIC_PREFIX, deviceName);
    t->subTopics[0] = t->updateAccepted;
    t->subTopics[1] = t->updateRejected;

    rc = asprintf(&(t->get), "%s/%s/get", TOPIC_PREFIX, deviceName);
    rc = asprintf(&(t->getAccepted), "%s/%s/get/accepted", TOPIC_PREFIX, deviceName);
    rc = asprintf(&(t->getRejected), "%s/%s/get/rejected", TOPIC_PREFIX, deviceName);
    t->subTopics[2] = t->getAccepted;
    t->subTopics[3] = t->getRejected;

    rc = asprintf(&(t->delete), "%s/%s/delete", TOPIC_PREFIX, deviceName);
    rc = asprintf(&(t->deleteAccepted), "%s/%s/delete/accepted", TOPIC_PREFIX, deviceName);
    rc = asprintf(&(t->deleteRejected), "%s/%s/delete/rejected", TOPIC_PREFIX, deviceName);
    t->subTopics[4] = t->getAccepted;
    t->subTopics[5] = t->getRejected;

    rc = asprintf(&(t->delta), "%s/%s/delta", TOPIC_PREFIX, deviceName);
    rc = asprintf(&(t->deltaRejected), "%s/%s/delta/rejected", TOPIC_PREFIX, deviceName);
    t->subTopics[6] = t->delta;
}

void
topic_contract_destroy(TopicContract *topics) {
    if (topics != NULL) {
        safe_free(&(topics->update));
        safe_free(&(topics->updateAccepted));
        safe_free(&(topics->updateRejected));
        safe_free(&(topics->get));
        safe_free(&(topics->getAccepted));
        safe_free(&(topics->getRejected));
        safe_free(&(topics->delta));
        safe_free(&(topics->deltaRejected));
        free(topics);
    }
}

static void
client_group_iterate(ClientGroup *clients, void (*fp)(device_management_client_t *c)) {
    int i;
    pthread_mutex_lock(&(clients->mutex));
    for (i = 0; i < MAX_CLIENT; ++i) {
        if (clients->members[i] != NULL) {
            fp(clients->members[i]);
        }
    }
    pthread_mutex_unlock(&(clients->mutex));
}

bool
client_group_add(ClientGroup *group, device_management_client_t *client) {
    int i;
    bool rc = false;

    pthread_mutex_lock(&(group->mutex));
    for (i = 0; i < MAX_CLIENT; ++i) {
        if (group->members[i] == NULL) {
            group->members[i] = client;
            rc = true;
            break;
        }
    }
    pthread_mutex_unlock(&(group->mutex));

    return rc;
}

bool
client_group_remove(ClientGroup *group, device_management_client_t *client) {
    int i;
    bool rc = false;

    pthread_mutex_lock(&(group->mutex));
    for (i = 0; i < MAX_CLIENT; ++i) {
        if (group->members[i] == client) {
            group->members[i] = NULL;
            rc = true;
            break;
        }
    }
    pthread_mutex_unlock(&(group->mutex));

    return rc;
}


void
in_flight_message_house_keep(device_management_client_t *c) {
    int i;
    time_t now;
    time(&now);
    pthread_mutex_lock(&(c->messages.mutex));
    for (i = 0; i < MAX_IN_FLIGHT_MESSAGE; ++i) {
        if (!c->messages.vault[i].free) {
            long elipse = difftime(now, c->messages.vault[i].timestamp);
            if (elipse > c->messages.vault[i].timeout) {
                // TODO: how to log the request id.
                log4c_category_log(category, LOG4C_PRIORITY_WARN, "request timed out.");
                if (c->messages.vault[i].callback != NULL) {
                    c->messages.vault[i].callback(c->messages.vault[i].action, SHADOW_ACK_TIMEOUT, NULL,
                                                  c->messages.vault[i].callbackContext);
                }
                c->messages.vault[i].free = true;
            }
        }
    }

    pthread_mutex_unlock(&(c->messages.mutex));

}

void *
in_flight_message_house_keep_proc(void *ignore) {
    while (1) {
        client_group_iterate(&allClients, in_flight_message_house_keep);
        sleep(1);
    }
}

const char *
message_get_request_id(const cJSON *payload) {
    cJSON *requestId = cJSON_GetObjectItemCaseSensitive(payload, "requestId");
    return requestId->valuestring;
}

void
exit_null_pointer() {
    log4c_category_log(category, LOG4C_PRIORITY_ERROR, "NULL POINTER");
    exit(NULL_POINTER);
}

DmReturnCode
in_flight_message_add(InFlightMessageList *table, const char *requestId, ShadowAction action,
                      ShadowActionCallback callback,
                      void *context, uint8_t timeout) {
    int rc = TOO_MANY_IN_FLIGHT_MESSAGE;
    int i;

    pthread_mutex_lock(&(table->mutex));
    for (i = 0; i < MAX_IN_FLIGHT_MESSAGE; ++i) {
        if (table->vault[i].free) {
            table->vault[i].free = false;
            table->vault[i].action = action;
            table->vault[i].callback = callback;
            table->vault[i].callbackContext = context;
            table->vault[i].timeout = timeout;
            time(&(table->vault[i].timestamp));
            strncpy(table->vault[i].requestId, requestId, MAX_REQUEST_ID_LENGTH);
            rc = SUCCESS;
            break;
        }
    }
    pthread_mutex_unlock(&(table->mutex));

    return rc;
}

bool
device_management_is_connected(DeviceManagementClient client) {
    if (client == NULL) {
        exit_null_pointer();
        return false;
    }

    device_management_client_t *c = client;

    return device_management_is_connected2(c);
}

bool
device_management_is_connected2(device_management_client_t *c) {
    if (c->mqttClient == NULL) {
        return false;
    }

    return MQTTAsync_isConnected(c->mqttClient) && c->hasSubscribed ? true : false;
}

DmReturnCode
device_management_shadow_send_json(device_management_client_t *c, const char *topic,
                                   const char *requestId, cJSON *payload) {
    DmReturnCode dmrc = SUCCESS;
    MQTTAsync_message message = MQTTAsync_message_initializer;
    MQTTAsync_responseOptions *responseOptions;
    char *string;
    int rc;

    cJSON_AddStringToObject(payload, REQUEST_ID_KEY, requestId);
    string = cJSON_PrintUnformatted(payload);

    message.payload = string;
    message.payloadlen = strlen(string) + 1;
    message.qos = QOS;
    message.retained = 0;

    responseOptions = malloc(sizeof(MQTTAsync_responseOptions));
    responseOptions->onSuccess = mqtt_on_publish_success;
    responseOptions->onFailure = mqtt_on_publish_failure;
    responseOptions->context = responseOptions;

    rc = MQTTAsync_sendMessage(c->mqttClient, topic, &message, responseOptions);
    if (rc != MQTTASYNC_SUCCESS) {
        log4c_category_log(category, LOG4C_PRIORITY_ERROR, "failed to send message. rc=%d, requestId=%s.", rc,
                           requestId);
        dmrc = FAILURE;
    } else {
        log4c_category_log(category, LOG4C_PRIORITY_TRACE, "\n[>>>>>>\ntopic:\n%s\npayload:\n%s\n >>>>>>]", topic,
                           string);

    }
    free(string);

    return dmrc;
}

DmReturnCode
device_management_shadow_send(DeviceManagementClient client, ShadowAction action, cJSON *payload,
                              ShadowActionCallback callback,
                              void *context, uint8_t timeout) {
    const char *topic;

    int rc;
    device_management_client_t *c = client;

    uuid_t uuid;
    char requestId[MAX_REQUEST_ID_LENGTH];
    uuid_generate(uuid);
    uuid_unparse(uuid, requestId);

    if (action == SHADOW_UPDATE) {
        topic = client->topicContract->update;
    } else if (action == SHADOW_GET) {
        topic = client->topicContract->get;
    } else if (action == SHADOW_DELETE) {
        topic = client->topicContract->delete;
    } else {
        log4c_category_log(category, LOG4C_PRIORITY_ERROR, "Unsupported action.");
        return BAD_ARGUMENT;
    }

    rc = in_flight_message_add(&(c->messages), requestId, action, callback, context, timeout);
    if (rc != SUCCESS) {
        return rc;
    }

    device_management_shadow_send_json(c, topic, requestId, payload);

    return SUCCESS;
}

int
device_management_shadow_handle_response(device_management_client_t *c, const char *requestId, ShadowAction action,
                                         ShadowAckStatus status,
                                         cJSON *payload) {
    int rc = NO_MATCHING_IN_FLIGHT_MESSAGE;
    int i;
    ShadowActionAck ack;

    pthread_mutex_lock(&(c->messages.mutex));
    for (i = 0; i < MAX_IN_FLIGHT_MESSAGE; ++i) {
        if (!c->messages.vault[i].free &&
            strncasecmp(c->messages.vault[i].requestId, requestId, MAX_REQUEST_ID_LENGTH) == 0) {
            if (status == SHADOW_ACK_ACCEPTED) {
                ack.accepted.document = payload;
            } else if (status == SHADOW_ACK_REJECTED) {
                ack.rejected.code = cJSON_GetObjectItem(payload, CODE_KEY)->valuestring;
                ack.rejected.message = cJSON_GetObjectItem(payload, MESSAGE_KEY)->valuestring;
            }
            c->messages.vault[i].callback(action, status, &ack, c->messages.vault[i].callbackContext);
            c->messages.vault[i].free = true;
            rc = SUCCESS;
            break;
        }
    }
    pthread_mutex_unlock(&(c->messages.mutex));

    if (rc == NO_MATCHING_IN_FLIGHT_MESSAGE) {
        log4c_category_log(category, LOG4C_PRIORITY_WARN, "no in flight payload matching %s.", requestId);
    }
    return rc;
}

DmReturnCode
device_management_delta_arrived(device_management_client_t *c, cJSON *payload) {
    uint32_t i = 0;
    UserDefinedError *error = NULL;
    cJSON *desired;
    cJSON *property;

    const char *requestId = message_get_request_id(payload);
    log4c_category_log(category, LOG4C_PRIORITY_DEBUG, "received delta. requestId=%s.", requestId);
    desired = cJSON_GetObjectItemCaseSensitive(payload, "desired");

    pthread_mutex_lock(&(c->properties.mutex));
    for (i = 0; i < c->properties.index; ++i) {
        if (c->properties.vault[i].key == NULL) {
            error = c->properties.vault[i].cb(NULL, desired);
        } else {
            property = cJSON_GetObjectItemCaseSensitive(desired, c->properties.vault[i].key);
            if (property != NULL) {
                error = c->properties.vault[i].cb(c->properties.vault[i].key, property);
            }
        }

        if (error != NULL) {
            break;
        }
    }

    pthread_mutex_unlock(&(c->properties.mutex));

    if (error != NULL) {
        cJSON *responsePayload = cJSON_CreateObject();
        cJSON_AddStringToObject(responsePayload, CODE_KEY, error->code);
        cJSON_AddStringToObject(responsePayload, MESSAGE_KEY, error->message);
        device_management_shadow_send_json(c, c->topicContract->deltaRejected, requestId, responsePayload);
        if (error->destroyer != NULL) {
            error->destroyer(error);
        }
        cJSON_Delete(responsePayload);
    }

    return SUCCESS;
}

void
mqtt_on_connected(void *context, char *cause) {
    int i;
    int rc;
    MQTTAsync_responseOptions responseOptions;
    responseOptions.onSuccess = NULL;
    responseOptions.onFailure = NULL;
    device_management_client_t *c = context;

    // TODO: on-demand subscribe
    int qos[SUB_TOPIC_COUNT];
    for (i = 0; i < SUB_TOPIC_COUNT; ++i) {
        qos[i] = 1;
    }
    rc = MQTTAsync_subscribeMany(c->mqttClient, SUB_TOPIC_COUNT, c->topicContract->subTopics, qos, &responseOptions);
    if (rc != MQTTASYNC_SUCCESS) {
        log4c_category_log(category, LOG4C_PRIORITY_ERROR, "Failed to subscribe. rc=%d.", rc);
        return;
    }
    MQTTAsync_waitForCompletion(c->mqttClient, responseOptions.token, SUBSCRIBE_TIMEOUT * 1000);
    pthread_mutex_lock(&(c->mutex));
    c->hasSubscribed = true;
    pthread_mutex_unlock(&(c->mutex));
    log4c_category_log(category, LOG4C_PRIORITY_DEBUG, "MQTT subscribed.");
}

void mqtt_on_connection_lost(void *context, char *cause) {
    log4c_category_log(category, LOG4C_PRIORITY_ERROR, "connection lost. cause=%s.", cause);
    device_management_client_t *c = context;
    pthread_mutex_lock(&(c->mutex));
    c->hasSubscribed = false;
    pthread_mutex_unlock(&(c->mutex));
    // TODO: I've set auto-connect to true. Should I manually re-connect here?
}

void mqtt_on_connect_success(void *context, MQTTAsync_successData *response) {
    device_management_client_t *c = context;
    if (c->errorMessage != NULL) {
        free(c->errorMessage);
        c->errorMessage = NULL;
    }
}

void mqtt_on_connect_failure(void *context, MQTTAsync_failureData *response) {
    device_management_client_t *c = context;
    if (c->errorMessage != NULL) {
        free(c->errorMessage);
    }
    c->errorCode = response->code;
    c->errorMessage = strdup(response->message);
}

void
mqtt_on_publish_success(void *context, MQTTAsync_successData *response) {
    free(context);
}

void
mqtt_on_publish_failure(void *context, MQTTAsync_failureData *response) {
    log4c_category_log(category, LOG4C_PRIORITY_ERROR, "failed to send json. code=%d, message=%s.",
                       response->code, response->message);
    free(context);
}

void
mqtt_on_delivery_complete(void *context, MQTTAsync_token dt) {
    // TODO: anything to do?
    device_management_client_t *c = context;
}

int
mqtt_on_message_arrived(void *context, char *topicName, int topicLen, MQTTAsync_message *message) {
    device_management_client_t *c = context;
    char *json = message->payload;
    if (message->payloadlen < 3) {
        return -1;
    }
    if (json[message->payloadlen - 1] != '\0') {
        // Make a copy
        json = malloc(message->payloadlen + 1);
        strncpy(json, message->payload, message->payloadlen);
        json[message->payloadlen] = '\0';
    }
    log4c_category_log(category, LOG4C_PRIORITY_TRACE, "\n[<<<<<<\ntopic:\n%s\npayload:\n%s\n <<<<<<]",
                       topicName, json);
    cJSON *payload = cJSON_Parse(json);
    if (json != message->payload) {
        free(json);
    }
    ShadowAckStatus status = SHADOW_ACK_ACCEPTED;
    ShadowAction action = SHADOW_INVALID;

    if (strncasecmp(c->topicContract->delta, topicName, strlen(c->topicContract->delta)) == 0) {
        device_management_delta_arrived(c, payload);
    } else {
        if (strncasecmp(c->topicContract->updateAccepted, topicName, strlen(c->topicContract->updateAccepted)) ==
            0) {
            action = SHADOW_UPDATE;
        } else if (strncasecmp(c->topicContract->updateRejected, topicName,
                               strlen(c->topicContract->updateRejected)) == 0) {
            action = SHADOW_UPDATE;
            status = SHADOW_ACK_REJECTED;
        } else if (strncasecmp(c->topicContract->getAccepted, topicName, strlen(c->topicContract->getAccepted)) ==
                   0) {
            action = SHADOW_GET;
        } else if (strncasecmp(c->topicContract->getRejected, topicName, strlen(c->topicContract->getRejected)) ==
                   0) {
            action = SHADOW_GET;
            status = SHADOW_ACK_REJECTED;
        } else {
            log4c_category_log(category, LOG4C_PRIORITY_ERROR, "Unexpected topic %s.", topicName);
        }

        if (action != SHADOW_INVALID) {
            cJSON *requestId = cJSON_GetObjectItem(payload, REQUEST_ID_KEY);

            if (requestId == NULL) {
                log4c_category_log(category, LOG4C_PRIORITY_ERROR, "cannot find request id.");
            } else {

                device_management_shadow_handle_response(c, requestId->valuestring, action, status, payload);
            }
        }
    }

    cJSON_Delete(payload);
    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topicName);

    return true;
}

