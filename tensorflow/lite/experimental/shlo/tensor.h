/* Copyright 2024 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_LITE_EXPERIMENTAL_SHLO_TENSOR_H_
#define TENSORFLOW_LITE_EXPERIMENTAL_SHLO_TENSOR_H_

#include <cassert>
#include <cstddef>
#include <variant>

#include "absl/types/span.h"
#include "tensorflow/lite/experimental/shlo/data_type.h"
#include "tensorflow/lite/experimental/shlo/quantized_tensor_element_type.h"
#include "tensorflow/lite/experimental/shlo/shape.h"

namespace shlo_ref {

using TensorElementType = DataType;

struct TensorType {
  Shape shape;
  TensorElementType element_type;
};

struct QuantizedTensorType {
  Shape shape;
  QuantizedTensorElementType element_type;
};

struct Tensor {
  const Shape& shape() const;
  Shape& shape();

  bool IsQuantized() const;
  bool IsPerAxisQuantized() const;
  bool IsPerTensorQuantized() const;

  size_t Rank() const;
  DataType StorageType() const;

  DimensionSize NumElements() const;
  size_t SizeInBytes() const;

  TensorType& tensor_type();
  const TensorType& tensor_type() const;

  QuantizedTensorType& quantized_tensor_type();
  const QuantizedTensorType& quantized_tensor_type() const;

  const TensorElementType& tensor_element_type() const;
  const QuantizedTensorElementType& quantized_tensor_element_type() const;

  template <DataType data_type, typename T = typename Storage<data_type>::Type>
  T* GetDataAs() {
    return reinterpret_cast<T*>(data);
  }

  template <DataType data_type, typename T = typename Storage<data_type>::Type>
  const T* GetDataAs() const {
    return reinterpret_cast<const T*>(data);
  }

  template <DataType data_type, typename T = typename Storage<data_type>::Type>
  absl::Span<const T> Flat() const {
    return absl::MakeConstSpan(GetDataAs<data_type>(),
                               static_cast<size_t>(NumElements()));
  }

  std::variant<TensorType, QuantizedTensorType> type;

  // If type is TensorType, the type should be Storage<type.element_type>::Type.
  // If type is QuantizedTensorType, the type should be
  // Storage<type.element_type.storage_type>::Type.
  // May be nullptr if buffers are not yet available.
  // The size of the array must be equal to Size(shape).
  void* data = nullptr;
};

bool operator==(const TensorType& lhs, const TensorType& rhs);
bool operator!=(const TensorType& lhs, const TensorType& rhs);

bool operator==(const QuantizedTensorType& lhs, const QuantizedTensorType& rhs);
bool operator!=(const QuantizedTensorType& lhs, const QuantizedTensorType& rhs);

}  // namespace shlo_ref

#endif  // TENSORFLOW_LITE_EXPERIMENTAL_SHLO_TENSOR_H_
