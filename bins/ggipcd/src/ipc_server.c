// aws-greengrass-lite - AWS IoT Greengrass runtime for constrained devices
// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ipc_server.h"
#include "ipc_components.h"
#include "ipc_dispatch.h"
#include "ipc_subscriptions.h"
#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <ggl/buffer.h>
#include <ggl/bump_alloc.h>
#include <ggl/cleanup.h>
#include <ggl/error.h>
#include <ggl/eventstream/decode.h>
#include <ggl/eventstream/encode.h>
#include <ggl/eventstream/rpc.h>
#include <ggl/eventstream/types.h>
#include <ggl/io.h>
#include <ggl/json_decode.h>
#include <ggl/json_encode.h>
#include <ggl/log.h>
#include <ggl/map.h>
#include <ggl/object.h>
#include <ggl/socket_handle.h>
#include <ggl/socket_server.h>
#include <pthread.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/// Maximum number of GG IPC clients.
/// Can be configured with `-DGGL_IPC_MAX_CLIENTS=<N>`.
#ifndef GGL_IPC_MAX_CLIENTS
#define GGL_IPC_MAX_CLIENTS 50
#endif

static_assert(
    GGL_IPC_MAX_MSG_LEN >= 16, "Minimum EventStream packet size is 16."
);

static uint8_t resp_array[GGL_IPC_MAX_MSG_LEN];
static pthread_mutex_t resp_array_mtx = PTHREAD_MUTEX_INITIALIZER;

static GglComponentHandle client_components[GGL_IPC_MAX_CLIENTS];

static GglError reset_client_state(uint32_t handle, size_t index);
static GglError release_client_subscriptions(uint32_t handle, size_t index);

static GglSocketPool pool = {
    .max_fds = GGL_IPC_MAX_CLIENTS,
    .fds = (int32_t[GGL_IPC_MAX_CLIENTS]) { 0 },
    .generations = (uint16_t[GGL_IPC_MAX_CLIENTS]) { 0 },
    .on_register = reset_client_state,
    .on_release = release_client_subscriptions,
};

__attribute__((constructor)) static void init_client_pool(void) {
    ggl_socket_pool_init(&pool);
}

static GglError reset_client_state(uint32_t handle, size_t index) {
    (void) handle;
    client_components[index] = 0;
    return GGL_ERR_OK;
}

static GglError release_client_subscriptions(uint32_t handle, size_t index) {
    (void) index;
    return ggl_ipc_release_subscriptions_for_conn(handle);
}

static void get_ipc_err_info(
    GglIpcErrorCode error_code,
    GglBuffer *err_str,
    GglBuffer *service_model_type
) {
    switch (error_code) {
    case GGL_IPC_ERR_SERVICE_ERROR:
        *err_str = GGL_STR("ServiceError");
        *service_model_type = GGL_STR("aws.greengrass#ServiceError");
        return;

    case GGL_IPC_ERR_RESOURCE_NOT_FOUND:
        *err_str = GGL_STR("ResourceNotFoundError");
        *service_model_type = GGL_STR("aws.greengrass#ResourceNotFoundError");
        return;

    case GGL_IPC_ERR_INVALID_ARGUMENTS:
        *err_str = GGL_STR("InvalidArgumentsError");
        *service_model_type = GGL_STR("aws.greengrass#InvalidArgumentsError");
        return;

    case GGL_IPC_ERR_COMPONENT_NOT_FOUND:
        *err_str = GGL_STR("ComponentNotFoundError");
        *service_model_type = GGL_STR("aws.greengrass#ComponentNotFoundError");
        return;

    case GGL_IPC_ERR_UNAUTHORIZED_ERROR:
        *err_str = GGL_STR("UnauthorizedError");
        *service_model_type = GGL_STR("aws.greengrass#UnauthorizedError");
        return;

    case GGL_IPC_ERR_CONFLICT_ERROR:
        *err_str = GGL_STR("ConflictError");
        *service_model_type = GGL_STR("aws.greengrass#ConflictError");
        return;

    case GGL_IPC_ERR_FAILED_UPDATE_CONDITION_CHECK_ERROR:
        *err_str = GGL_STR("FailedUpdateConditionCheckError");
        *service_model_type
            = GGL_STR("aws.greengrass#FailedUpdateConditionCheckError");
        return;

    case GGL_IPC_ERR_INVALID_TOKEN_ERROR:
        *err_str = GGL_STR("InvalidTokenError");
        *service_model_type = GGL_STR("aws.greengrass#InvalidTokenError");
        return;

    case GGL_IPC_ERR_INVALID_RECIPE_DIRECTORY_PATH_ERROR:
        *err_str = GGL_STR("InvalidRecipeDirectoryPathError");
        *service_model_type
            = GGL_STR("aws.greengrass#InvalidRecipeDirectoryPathError");
        return;

    case GGL_IPC_ERR_INVALID_ARTIFACTS_DIRECTORY_PATH_ERROR:
        *err_str = GGL_STR("InvalidArtifactsDirectoryPathError");
        *service_model_type
            = GGL_STR("aws.greengrass#InvalidArtifactsDirectoryPathError");
        return;
    }
}

static GglError deserialize_payload(GglBuffer payload, GglMap *out) {
    GglObject obj;

    static uint8_t
        json_decode_mem[GGL_IPC_PAYLOAD_MAX_SUBOBJECTS * sizeof(GglObject)];
    GglBumpAlloc balloc = ggl_bump_alloc_init(GGL_BUF(json_decode_mem));

    GGL_LOGT(
        "Deserializing payload %.*s", (int) payload.len, (char *) payload.data
    );

    GglError ret = ggl_json_decode_destructive(payload, &balloc.alloc, &obj);
    if (ret != GGL_ERR_OK) {
        GGL_LOGE("Failed to decode msg payload.");
        return ret;
    }

    if (obj.type != GGL_TYPE_MAP) {
        GGL_LOGE("Message payload is not a JSON object.");
        return GGL_ERR_INVALID;
    }

    *out = obj.map;
    return GGL_ERR_OK;
}

static void set_conn_component(void *ctx, size_t index) {
    GglComponentHandle *component_handle = ctx;
    assert(*component_handle != 0);

    client_components[index] = *component_handle;
}

static GglError complete_conn_init(
    uint32_t handle,
    GglComponentHandle component_handle,
    bool send_svcuid,
    GglBuffer svcuid
) {
    GGL_LOGT("Setting %d as connected.", handle);

    GglError ret = ggl_socket_handle_protected(
        set_conn_component, &component_handle, &pool, handle
    );
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    GGL_MTX_SCOPE_GUARD(&resp_array_mtx);
    GglBuffer resp_buffer = GGL_BUF(resp_array);

    ret = eventstream_encode(
        &resp_buffer,
        (EventStreamHeader[]) {
            { GGL_STR(":message-type"),
              { EVENTSTREAM_INT32, .int32 = EVENTSTREAM_CONNECT_ACK } },
            { GGL_STR(":message-flags"),
              { EVENTSTREAM_INT32, .int32 = EVENTSTREAM_CONNECTION_ACCEPTED } },
            { GGL_STR(":stream-id"), { EVENTSTREAM_INT32, .int32 = 0 } },
            { GGL_STR("svcuid"), { EVENTSTREAM_STRING, .string = svcuid } },
        },
        send_svcuid ? 4 : 3,
        GGL_NULL_READER
    );
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    ret = ggl_socket_handle_write(&pool, handle, resp_buffer);
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    GGL_LOGD("Successful connection.");
    return GGL_ERR_OK;
}

static GglError handle_authentication_request(uint32_t handle) {
    GGL_LOGD("Client %d requesting svcuid.", handle);

    pid_t pid = 0;
    GglError ret = ggl_socket_handle_get_peer_pid(&pool, handle, &pid);
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    GglComponentHandle component_handle = 0;
    uint8_t svcuid_buf[GGL_IPC_SVCUID_LEN];
    GglBuffer svcuid = GGL_BUF(svcuid_buf);
    ret = ggl_ipc_components_register(pid, &component_handle, &svcuid);
    if (ret != GGL_ERR_OK) {
        GGL_LOGE("Client %d failed authentication.", handle);
        return ret;
    }

    return complete_conn_init(handle, component_handle, true, svcuid);
}

static GglError handle_conn_init(
    uint32_t handle,
    EventStreamMessage *msg,
    EventStreamCommonHeaders common_headers
) {
    GGL_LOGD("Handling connect for %d.", handle);

    if (common_headers.message_type != EVENTSTREAM_CONNECT) {
        GGL_LOGE("Client initial message not of type connect.");
        return GGL_ERR_INVALID;
    }
    if (common_headers.stream_id != 0) {
        GGL_LOGE("Connect message has non-zero :stream-id.");
        return GGL_ERR_INVALID;
    }
    if ((common_headers.message_flags & EVENTSTREAM_FLAGS_MASK) != 0) {
        GGL_LOGE("Connect message has flags set.");
        return GGL_ERR_INVALID;
    }

    bool request_auth = false;

    {
        EventStreamHeaderIter iter = msg->headers;
        EventStreamHeader header;

        while (eventstream_header_next(&iter, &header) == GGL_ERR_OK) {
            if (ggl_buffer_eq(header.name, GGL_STR(":version"))) {
                if (header.value.type != EVENTSTREAM_STRING) {
                    GGL_LOGE(":version header not string.");
                    return GGL_ERR_INVALID;
                }
                if (!ggl_buffer_eq(header.value.string, GGL_STR("0.1.0"))) {
                    GGL_LOGE("Client protocol version not 0.1.0.");
                    return GGL_ERR_INVALID;
                }
            } else if (ggl_buffer_eq(header.name, GGL_STR("authenticate"))) {
                if (header.value.type != EVENTSTREAM_INT32) {
                    GGL_LOGE("request_svcuid header not an int.");
                    return GGL_ERR_INVALID;
                }
                if (header.value.int32 == 1) {
                    request_auth = true;
                }
            }
        }
    }

    if (request_auth) {
        return handle_authentication_request(handle);
    }

    GglMap payload_data = { 0 };
    GglError ret = deserialize_payload(msg->payload, &payload_data);
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    GglObject *value;
    bool found = ggl_map_get(payload_data, GGL_STR("authToken"), &value);
    if (!found) {
        GGL_LOGE("Connect message payload missing authToken.");
        return GGL_ERR_INVALID;
    }
    if (value->type != GGL_TYPE_BUF) {
        GGL_LOGE("Connect message authToken not a string.");
        return GGL_ERR_INVALID;
    }
    GglBuffer auth_token = value->buf;

    GGL_LOGD("Client connecting.");

    GglComponentHandle component_handle = 0;
    ret = ggl_ipc_components_get_handle(auth_token, &component_handle);
    if (ret != GGL_ERR_OK) {
        GGL_LOGE("Client failed authentication.");
        return ret;
    }

    return complete_conn_init(
        handle, component_handle, false, (GglBuffer) { 0 }
    );
}

static GglError send_stream_error(
    uint32_t handle, int32_t stream_id, GglIpcError ipc_error
) {
    GGL_LOGE("Sending error on client %u stream %d.", handle, stream_id);

    GGL_MTX_SCOPE_GUARD(&resp_array_mtx);
    GglBuffer resp_buffer = GGL_BUF(resp_array);

    GglBuffer service_model_type;
    GglBuffer error_code;

    get_ipc_err_info(ipc_error.error_code, &error_code, &service_model_type);

    // TODO: Match classic error response
    EventStreamHeader resp_headers[] = {
        { GGL_STR(":message-type"),
          { EVENTSTREAM_INT32, .int32 = EVENTSTREAM_APPLICATION_ERROR } },
        { GGL_STR(":message-flags"),
          { EVENTSTREAM_INT32, .int32 = EVENTSTREAM_TERMINATE_STREAM } },
        { GGL_STR(":stream-id"), { EVENTSTREAM_INT32, .int32 = stream_id } },
        { GGL_STR(":content-json"),
          { EVENTSTREAM_STRING, .string = GGL_STR("application/json") } },
        { GGL_STR("service-model-type"),
          { EVENTSTREAM_STRING, .string = service_model_type } },
    };
    size_t resp_headers_len = sizeof(resp_headers) / sizeof(resp_headers[0]);

    GglError ret = eventstream_encode(
        &resp_buffer,
        resp_headers,
        resp_headers_len,
        ggl_json_reader(&GGL_OBJ_MAP(GGL_MAP(
            { GGL_STR("_message"), GGL_OBJ_BUF(ipc_error.message) },
            { GGL_STR("_errorCode"), GGL_OBJ_BUF(error_code) }
        )))
    );
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    return ggl_socket_handle_write(&pool, handle, resp_buffer);
}

static GglError handle_stream_operation(
    uint32_t handle,
    EventStreamMessage *msg,
    EventStreamCommonHeaders common_headers,
    GglIpcError *ipc_error
) {
    if (common_headers.message_type != EVENTSTREAM_APPLICATION_MESSAGE) {
        GGL_LOGE("Client sent unhandled message type.");
        return GGL_ERR_INVALID;
    }
    if ((common_headers.message_flags & EVENTSTREAM_FLAGS_MASK) != 0) {
        GGL_LOGE("Client request has flags set.");
        return GGL_ERR_INVALID;
    }

    GglBuffer operation = { 0 };

    {
        bool operation_set = false;
        EventStreamHeaderIter iter = msg->headers;
        EventStreamHeader header;

        while (eventstream_header_next(&iter, &header) == GGL_ERR_OK) {
            if (ggl_buffer_eq(header.name, GGL_STR("operation"))) {
                if (header.value.type != EVENTSTREAM_STRING) {
                    GGL_LOGE("operation header not string.");
                    return GGL_ERR_INVALID;
                }
                operation = header.value.string;
                operation_set = true;
            }
        }

        if (!operation_set) {
            GGL_LOGE("Client request missing operation header.");
            return GGL_ERR_INVALID;
        }
    }

    GglMap payload_data = { 0 };
    GglError ret = deserialize_payload(msg->payload, &payload_data);
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    return ggl_ipc_handle_operation(
        operation, payload_data, handle, common_headers.stream_id, ipc_error
    );
}

static GglError handle_operation(
    uint32_t handle,
    EventStreamMessage *msg,
    EventStreamCommonHeaders common_headers
) {
    if (common_headers.stream_id == 0) {
        GGL_LOGE("Application message has zero :stream-id.");
        return GGL_ERR_INVALID;
    }

    GglIpcError ipc_error = { 0 };

    GglError ret
        = handle_stream_operation(handle, msg, common_headers, &ipc_error);
    if (ret == GGL_ERR_FATAL) {
        return GGL_ERR_FAILURE;
    }

    if (ret != GGL_ERR_OK) {
        return send_stream_error(handle, common_headers.stream_id, ipc_error);
    }

    return GGL_ERR_OK;
}

static void get_conn_component(void *ctx, size_t index) {
    GglComponentHandle *handle = ctx;
    *handle = client_components[index];
}

GglError ggl_ipc_get_component_name(
    uint32_t handle, GglBuffer *component_name
) {
    GglComponentHandle component_handle;
    GglError ret = ggl_socket_handle_protected(
        get_conn_component, &component_handle, &pool, handle
    );
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    *component_name = ggl_ipc_components_get_name(component_handle);
    return GGL_ERR_OK;
}

static GglError client_ready(void *ctx, uint32_t handle) {
    (void) ctx;

    static uint8_t payload_array[GGL_IPC_MAX_MSG_LEN];
    GglBuffer recv_buffer = GGL_BUF(payload_array);
    GglBuffer prelude_buf = ggl_buffer_substr(recv_buffer, 0, 12);
    assert(prelude_buf.len == 12);

    GglError ret = ggl_socket_handle_read(&pool, handle, prelude_buf);
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    EventStreamPrelude prelude;
    ret = eventstream_decode_prelude(prelude_buf, &prelude);
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    if (prelude.data_len > recv_buffer.len) {
        GGL_LOGE(
            "EventStream packet does not fit in configured IPC buffer size."
        );
        return ENOMEM;
    }

    GglBuffer data_section
        = ggl_buffer_substr(recv_buffer, 0, prelude.data_len);

    ret = ggl_socket_handle_read(&pool, handle, data_section);
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    EventStreamMessage msg;

    ret = eventstream_decode(&prelude, data_section, &msg);
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    EventStreamCommonHeaders common_headers;
    ret = eventstream_get_common_headers(&msg, &common_headers);
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    GGL_LOGT("Retrieving connection state for %d.", handle);
    GglComponentHandle component_handle = 0;
    ret = ggl_socket_handle_protected(
        get_conn_component, &component_handle, &pool, handle
    );
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    if (component_handle == 0) {
        return handle_conn_init(handle, &msg, common_headers);
    }

    return handle_operation(handle, &msg, common_headers);
}

GglError ggl_ipc_listen(const char *socket_name, const char *socket_path) {
    return ggl_socket_server_listen(
        &(GglBuffer) { .data = (uint8_t *) socket_name,
                       .len = strlen(socket_name) },
        (GglBuffer) { .data = (uint8_t *) socket_path,
                      .len = strlen(socket_path) },
        0666,
        &pool,
        client_ready,
        NULL
    );
}

GglError ggl_ipc_response_send(
    uint32_t handle,
    int32_t stream_id,
    GglBuffer service_model_type,
    GglObject response
) {
    GGL_MTX_SCOPE_GUARD(&resp_array_mtx);
    GglBuffer resp_buffer = GGL_BUF(resp_array);

    EventStreamHeader resp_headers[] = {
        { GGL_STR(":message-type"),
          { EVENTSTREAM_INT32, .int32 = EVENTSTREAM_APPLICATION_MESSAGE } },
        { GGL_STR(":message-flags"), { EVENTSTREAM_INT32, .int32 = 0 } },
        { GGL_STR(":stream-id"), { EVENTSTREAM_INT32, .int32 = stream_id } },

        { GGL_STR(":content-type"),
          { EVENTSTREAM_STRING, .string = GGL_STR("application/json") } },
        { GGL_STR("service-model-type"),
          { EVENTSTREAM_STRING, .string = service_model_type } },
    };
    size_t resp_headers_len = sizeof(resp_headers) / sizeof(resp_headers[0]);

    if (service_model_type.len == 0) {
        resp_headers_len -= 1;
    }

    GglError ret = eventstream_encode(
        &resp_buffer, resp_headers, resp_headers_len, ggl_json_reader(&response)
    );
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    return ggl_socket_handle_write(&pool, handle, resp_buffer);
}
