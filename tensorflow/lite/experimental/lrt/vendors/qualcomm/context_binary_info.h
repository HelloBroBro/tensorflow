// Copyright 2024 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TENSORFLOW_LITE_EXPERIMENTAL_LRT_VENDORS_QUALCOMM_CONTEXT_BINARY_INFO_H_
#define TENSORFLOW_LITE_EXPERIMENTAL_LRT_VENDORS_QUALCOMM_CONTEXT_BINARY_INFO_H_

#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "third_party/qairt/include/QNN/QnnInterface.h"
#include "third_party/qairt/include/QNN/QnnTypes.h"
#include "tensorflow/lite/experimental/lrt/vendors/qualcomm/qnn.h"
#include "tensorflow/lite/experimental/lrt/vendors/qualcomm/qnn_tensor.h"

namespace lrt {
namespace qnn {

class GraphInfo {
 public:
  static absl::StatusOr<GraphInfo> Create(
      const QnnSystemContext_GraphInfo_t& graph_info);
  const std::string& name() const { return name_; }
  const std::vector<QnnTensor>& inputs() const { return inputs_; }
  const std::vector<QnnTensor>& outputs() const { return outputs_; }

 private:
  GraphInfo() = default;
  absl::Status Init(const QnnSystemContext_GraphInfo_t& graph_info);
  std::string name_;
  std::vector<QnnTensor> inputs_;
  std::vector<QnnTensor> outputs_;
};

class ContextBinaryInfo {
 public:
  static absl::StatusOr<ContextBinaryInfo> Create(const qnn::Qnn& qnn,
                                                  const void* exec_bytecode_ptr,
                                                  size_t exec_bytecode_size);
  const std::vector<QnnTensor>& context_tensors() const {
    return context_tensors_;
  }
  const std::vector<GraphInfo>& graphs() const { return graphs_; }

 private:
  ContextBinaryInfo() = default;
  absl::Status Init(const QnnSystemContext_BinaryInfo_t& binary_info);
  std::vector<QnnTensor> context_tensors_;
  std::vector<GraphInfo> graphs_;
};

}  // namespace qnn
}  // namespace lrt

#endif  // TENSORFLOW_LITE_EXPERIMENTAL_LRT_VENDORS_QUALCOMM_CONTEXT_BINARY_INFO_H_
