// aws-greengrass-lite - AWS IoT Greengrass runtime for constrained devices
// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include <argp.h>
#include <assert.h>
#include <ggl/buffer.h>
#include <ggl/bump_alloc.h>
#include <ggl/core_bus/client.h>
#include <ggl/error.h>
#include <ggl/log.h>
#include <ggl/object.h>
#include <ggl/vector.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

char *command = NULL;
char *recipe_dir = NULL;
char *artifacts_dir = NULL;
char *component_name = NULL;
char *component_version = NULL;

static char doc[] = "ggl-cli -- Greengrass Lite CLI";

static struct argp_option opts[] = {
    { "recipe-dir", 'r', "path", 0, "Recipe directory to merge", 0 },
    { "artifacts-dir", 'a', "path", 0, "Artifacts directory to merge", 0 },
    { "add-component", 'c', "name=version", 0, "Component to add", 0 },
    { 0 },
};

static error_t arg_parser(int key, char *arg, struct argp_state *state) {
    (void) arg;
    switch (key) {
    case 'r':
        recipe_dir = arg;
        break;
    case 'a':
        artifacts_dir = arg;
        break;
    case 'c': {
        char *eq = strchr(arg, '=');
        if (eq == NULL) {
            // NOLINTNEXTLINE(concurrency-mt-unsafe)
            argp_usage(state);
            break;
        }
        *eq = '\0';
        component_name = arg;
        component_version = &eq[1];
        break;
    }
    case ARGP_KEY_ARG:
        if (command != NULL) {
            // NOLINTNEXTLINE(concurrency-mt-unsafe)
            argp_usage(state);
        }
        if (strcmp(arg, "deploy") == 0) {
            command = arg;
            break;
        }
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        argp_usage(state);
        break;
    case ARGP_KEY_NO_ARGS:
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        argp_usage(state);
        break;
    default:
        break;
    }
    return 0;
}

static struct argp argp = { opts, arg_parser, "deploy", doc, 0, 0, 0 };

int main(int argc, char **argv) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    argp_parse(&argp, argc, argv, 0, 0, NULL);

    GglKVVec args = GGL_KV_VEC((GglKV[3]) { 0 });

    if (recipe_dir != NULL) {
        GglError ret = ggl_kv_vec_push(
            &args,
            (GglKV) { GGL_STR("recipe_directory_path"),
                      GGL_OBJ(ggl_buffer_from_null_term(recipe_dir)) }
        );
        if (ret != GGL_ERR_OK) {
            assert(false);
            return 1;
        }
    }
    if (artifacts_dir != NULL) {
        GglError ret = ggl_kv_vec_push(
            &args,
            (GglKV) { GGL_STR("artifacts_directory_path"),
                      GGL_OBJ(ggl_buffer_from_null_term(artifacts_dir)) }
        );
        if (ret != GGL_ERR_OK) {
            assert(false);
            return 1;
        }
    }
    GglKV component;
    if (component_name != NULL) {
        component
            = (GglKV) { ggl_buffer_from_null_term(component_name),
                        GGL_OBJ(ggl_buffer_from_null_term(component_version)) };
        GglError ret = ggl_kv_vec_push(
            &args,
            (GglKV) { GGL_STR("root_component_versions_to_add"),
                      GGL_OBJ((GglMap) { .pairs = &component, .len = 1 }) }
        );
        if (ret != GGL_ERR_OK) {
            assert(false);
            return 1;
        }
    }

    GglError remote_err = GGL_ERR_OK;
    GglBuffer id_mem = GGL_BUF((uint8_t[36]) { 0 });
    GglBumpAlloc alloc = ggl_bump_alloc_init(id_mem);
    GglObject result;

    GglError ret = ggl_call(
        GGL_STR("/aws/ggl/ggdeploymentd"),
        GGL_STR("create_local_deployment"),
        args.map,
        &remote_err,
        &alloc.alloc,
        &result
    );
    if (ret != GGL_ERR_OK) {
        if (ret == GGL_ERR_REMOTE) {
            GGL_LOGE("cli", "Got error from deployment: %d.", remote_err);
            return 1;
        }
        GGL_LOGE("cli", "Error sending deployment: %d.", ret);
        return 1;
    }

    if (result.type != GGL_TYPE_BUF) {
        GGL_LOGE("cli", "Invalid return type.");
        return 1;
    }

    printf("Deployment id: %.*s.", (int) result.buf.len, result.buf.data);
}