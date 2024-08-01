// aws-greengrass-lite - AWS IoT Greengrass runtime for constrained devices
// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ggl/socket_handle.h"
#include "ggl/socket.h"
#include <assert.h>
#include <ggl/alloc.h>
#include <ggl/defer.h>
#include <ggl/error.h>
#include <ggl/log.h>
#include <ggl/object.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>

// Handles are 32 bits, with the high 16 bits being a generation counter, and
// the low 16 bits being an index. The generation counter is incremented on
// close, to prevent reuse.
//
// Use of the index and generation count must be done with a mutex held to
// prevent concurrent incrementing of the generation counter.

static const int32_t FD_FREE = -0x55555556; // Alternating bits for debugging

GGL_DEFINE_DEFER(
    unlock_pool_mtx, GglSocketPool *, pool, pthread_mutex_unlock(&(*pool)->mtx)
)

static GglError validate_handle(
    GglSocketPool *pool, uint32_t handle, uint16_t *index, const char *location
) {
    uint16_t handle_index = (uint16_t) (handle & UINT16_MAX);
    uint16_t handle_generation = (uint16_t) (handle >> 16);

    if (handle_generation != pool->generations[handle_index]) {
        GGL_LOGD("socket", "Generation mismatch in %s.", location);
        return GGL_ERR_NOENTRY;
    }

    *index = handle_index;
    return GGL_ERR_OK;
}

void ggl_socket_pool_init(GglSocketPool *pool) {
    assert(pool != NULL);
    assert(pool->fds != NULL);
    assert(pool->generations != NULL);

    for (size_t i = 0; i < pool->max_fds; i++) {
        pool->fds[i] = FD_FREE;
    }
}

GglError ggl_socket_pool_register(
    GglSocketPool *pool, int fd, uint32_t *handle
) {
    if (fd < 0) {
        return GGL_ERR_INVALID;
    }

    pthread_mutex_lock(&pool->mtx);
    GGL_DEFER(unlock_pool_mtx, pool);

    for (uint16_t i = 0; i < pool->max_fds; i++) {
        if (pool->fds[i] == FD_FREE) {
            pool->fds[i] = fd;
            uint32_t new_handle = (uint32_t) pool->generations[i] << 16 | i;

            if (pool->on_register != NULL) {
                GglError ret = pool->on_register(new_handle, i);
                if (ret != GGL_ERR_OK) {
                    pool->fds[i] = FD_FREE;
                    return ret;
                }
            }

            *handle = new_handle;

            GGL_LOGD(
                "socket",
                "Registered fd %d at index %u, generation %u.",
                fd,
                i,
                pool->generations[i]
            );

            return GGL_ERR_OK;
        }
    }

    return GGL_ERR_NOMEM;
}

GglError ggl_socket_pool_release(
    GglSocketPool *pool, uint32_t handle, int *fd
) {
    pthread_mutex_lock(&pool->mtx);
    GGL_DEFER(unlock_pool_mtx, pool);

    uint16_t index = 0;
    GglError ret = validate_handle(pool, handle, &index, __func__);
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    if (pool->on_release != NULL) {
        ret = pool->on_release(handle, index);
        if (ret != GGL_ERR_OK) {
            return ret;
        }
    }

    if (fd != NULL) {
        *fd = pool->fds[index];
    }

    GGL_LOGD(
        "socket",
        "Releasing fd %d at index %u, generation %u.",
        pool->fds[index],
        index,
        pool->generations[index]
    );

    pool->generations[index] += 1;
    pool->fds[index] = FD_FREE;

    return GGL_ERR_OK;
}

GglError ggl_socket_handle_read(
    GglSocketPool *pool, uint32_t handle, GglBuffer buf
) {
    GglBuffer rest = buf;

    while (rest.len > 0) {
        pthread_mutex_lock(&pool->mtx);
        GGL_DEFER(unlock_pool_mtx, pool);

        uint16_t index = 0;
        GglError ret = validate_handle(pool, handle, &index, __func__);
        if (ret != GGL_ERR_OK) {
            return ret;
        }

        ret = ggl_read(pool->fds[index], &rest);
        if (ret != GGL_ERR_OK) {
            return ret;
        }
    }

    return GGL_ERR_OK;
}

GglError ggl_socket_handle_write(
    GglSocketPool *pool, uint32_t handle, GglBuffer buf
) {
    GglBuffer rest = buf;

    while (rest.len > 0) {
        pthread_mutex_lock(&pool->mtx);
        GGL_DEFER(unlock_pool_mtx, pool);

        uint16_t index = 0;
        GglError ret = validate_handle(pool, handle, &index, __func__);
        if (ret != GGL_ERR_OK) {
            return ret;
        }

        ret = ggl_write(pool->fds[index], &rest);
        if (ret != GGL_ERR_OK) {
            return ret;
        }
    }

    return GGL_ERR_OK;
}

GglError ggl_socket_handle_close(GglSocketPool *pool, uint32_t handle) {
    int fd = -1;

    GglError ret = ggl_socket_pool_release(pool, handle, &fd);
    if (ret == GGL_ERR_OK) {
        close(fd);
    }

    return ret;
}

GglError ggl_with_socket_handle_index(
    void (*action)(void *ctx, size_t index),
    void *ctx,
    GglSocketPool *pool,
    uint32_t handle
) {
    pthread_mutex_lock(&pool->mtx);
    GGL_DEFER(unlock_pool_mtx, pool);

    uint16_t index = 0;
    GglError ret = validate_handle(pool, handle, &index, __func__);
    if (ret != GGL_ERR_OK) {
        return ret;
    }

    action(ctx, index);

    return GGL_ERR_OK;
}