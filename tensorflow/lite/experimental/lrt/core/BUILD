# Copyright 2024 Google LLC.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_visibility = ["//tensorflow/lite/experimental/lrt:__subpackages__"],
)

cc_library(
    name = "api_internal",
    srcs = ["lite_rt_common.cc"],
    hdrs = [
        "//tensorflow/lite/experimental/lrt/c:lite_rt_common.h",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_compiler_plugin.h",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_model.h",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_op_code.h",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_support.h",
        "//tensorflow/lite/experimental/lrt/cc:lite_rt_support.h",
    ],
    deps = [
        "//tensorflow/lite:builtin_ops",
        "//tensorflow/lite/core/c:c_api_types",
    ],
)

cc_library(
    name = "model",
    srcs = ["model.cc"],
    hdrs = [
        "model.h",
    ],
    deps = [
        ":api_internal",
        "//tensorflow/compiler/mlir/lite/core:model_builder_base",
        "//tensorflow/lite/c:c_api_types",
        "//tensorflow/lite/core/c:c_api_types",
        "//tensorflow/lite/schema:schema_fbs",
    ],
)

cc_library(
    name = "lite_rt_model_init",
    srcs = ["lite_rt_model_init.cc"],
    hdrs = ["lite_rt_model_init.h"],
    deps = [
        ":api_internal",
        ":model",
        "//tensorflow/compiler/mlir/lite/core:model_builder_base",
        "//tensorflow/lite:allocation",
        "//tensorflow/lite:framework",
        "//tensorflow/lite:stderr_reporter",
        "//tensorflow/lite/c:c_api_types",
        "//tensorflow/lite/schema:schema_fbs",
        "@com_google_absl//absl/log:check",
        "@flatbuffers//:runtime_cc",
    ],
)

cc_test(
    name = "model_test",
    srcs = ["model_test.cc"],
    tags = ["no_oss"],
    deps = [
        ":api_internal",
        ":graph_tools",
        ":lite_rt_model_init",
        "//tensorflow/lite/experimental/lrt/test:common",
        "//tensorflow/lite/schema:schema_fbs",
        "@com_google_googletest//:gtest_main",
        "@flatbuffers//:runtime_cc",
        "@llvm-project//llvm:Support",
    ],
)

cc_library(
    name = "algo",
    hdrs = ["algo.h"],
    deps = [
        ":api_internal",
        ":model",
        "//tensorflow/lite/schema:schema_fbs",
        "@com_google_absl//absl/log:check",
        "@llvm-project//llvm:Support",
    ],
)

cc_test(
    name = "algo_test",
    srcs = ["algo_test.cc"],
    tags = ["no_oss"],
    deps = [
        ":algo",
        ":api_internal",
        ":graph_tools",
        ":model",
        "//tensorflow/lite/experimental/lrt/test:common",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "graph_tools",
    hdrs = [
        "graph_tools.h",
    ],
    deps = [
        ":api_internal",
        "//tensorflow/compiler/mlir/lite/core:model_builder_base",
        "//tensorflow/lite/c:c_api_types",
        "@llvm-project//llvm:Support",
    ],
)

cc_library(
    name = "dynamic_loading",
    srcs = ["dynamic_loading.cc"],
    hdrs = ["dynamic_loading.h"],
    linkopts = ["-ldl"],
    deps = [
        ":logging",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_c_api",
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_library(
    name = "tensor_buffer",
    srcs = [
        "dmabuf_buffer.cc",
        "fastrpc_buffer.cc",
        "ion_buffer.cc",
        "lite_rt_event.cc",
        "lite_rt_tensor_buffer.cc",
        "lite_rt_tensor_buffer_requirements.cc",
        "tensor_buffer.cc",
    ],
    hdrs = [
        "dmabuf_buffer.h",
        "fastrpc_buffer.h",
        "ion_buffer.h",
        "tensor_buffer.h",
    ],
    deps = [
        ":logging",
        ":utils",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_c_api",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_tensor_buffer_c_api",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:node_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/types:span",
    ],
)

cc_test(
    name = "tensor_buffer_test",
    srcs = [
        "tensor_buffer_test.cc",
    ],
    linkopts = select({
        "//tensorflow:android": ["-landroid"],
        "//conditions:default": [],
    }),
    tags = ["manual"],
    deps = [
        ":tensor_buffer",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_c_api",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_tensor_buffer_c_api",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "dispatch",
    srcs = [
        "lite_rt_dispatch.cc",
    ],
    hdrs = [
    ],
    deps = [
        ":logging",
        ":tensor_buffer",
        ":utils",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_c_api",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_dispatch_c_api",
        "//tensorflow/lite/experimental/lrt/c:lite_rt_tensor_buffer_c_api",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:node_hash_map",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/types:span",
    ],
)

cc_library(
    name = "utils",
    srcs = [
        "utils.cc",
    ],
    hdrs = [
        "utils.h",
    ],
    deps = [
        "//tensorflow/lite/experimental/lrt/c:lite_rt_c_api",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
    ],
)

cc_library(
    name = "logging",
    srcs = [
        "logging.cc",
    ],
    hdrs = [
        "logging.h",
    ],
    deps = ["//tensorflow/lite:minimal_logging"],
)
