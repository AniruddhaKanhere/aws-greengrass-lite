# aws-greengrass-lite - AWS IoT Greengrass runtime for constrained devices
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

ggl_init_module(
  ggipc-client
  LIBS ggl-lib
       ggl-file
       ggl-socket
       eventstream
       ggl-json
       ggl-constants
       ggipcd
       ggipc-common)
