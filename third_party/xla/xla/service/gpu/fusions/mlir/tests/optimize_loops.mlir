// RUN: mlir_fusions_opt %s -split-input-file -xla-gpu-optimize-loops | FileCheck %s

#map = affine_map<(d0) -> (d0 floordiv 8)>
#map1 = affine_map<(d0) -> (d0 mod 8)>
#map2 = affine_map<(d0, d1)[s0] -> (d1 * 2 + d0 + s0 * 512)>
module {
  func.func @fully_unroll(%arg0: tensor<4x8x4096xf32>, %arg1: tensor<4096xbf16>,
      %arg2: tensor<4x8xf32>, %arg3: tensor<4096xbf16>,
      %arg4: tensor<4x8x4096xbf16>, %arg5: tensor<4x8xf32>,
      %arg6: tensor<4x8x4096xf32>) -> (tensor<4x8x4096xf32>, f32)  {
    %cst = arith.constant 1.000000e+00 : f32
    %cst_1 = arith.constant 1.000000e+00 : bf16
    %c2 = arith.constant 2 : index
    %c8 = arith.constant 8 : index
    %c32 = arith.constant 32 : index
    %c1 = arith.constant 1 : index
    %c0 = arith.constant 0 : index
    %thread_id_x = gpu.thread_id  x {xla.range = [0 : index, 255 : index]}
    %block_id_x = gpu.block_id  x {xla.range = [0 : index, 31 : index]}
    %0 = gpu.lane_id
    %1 = arith.cmpi eq, %0, %c0 : index
    %2 = arith.divui %thread_id_x, %c32 : index
    %3 = arith.cmpi ult, %thread_id_x, %c8 : index
    %4 = xla_gpu.apply_indexing #map(%block_id_x in [0, 32))
    %5 = xla_gpu.apply_indexing #map1(%block_id_x in [0, 32))
    %extracted = tensor.extract %arg2[%4, %5] : tensor<4x8xf32>
    %6 = arith.mulf %extracted, %cst : f32
    %7 = arith.addf %6, %cst : f32
    %8 = math.rsqrt %7 : f32
    %9:2 = scf.for %arg7 = %c0 to %c8 step %c1 iter_args(%arg8 = %arg6, %arg9 = %cst) -> (tensor<4x8x4096xf32>, f32) {
      %18 = xla_gpu.apply_indexing #map2(%c0 in [0, 2), %thread_id_x in [0, 256))[%arg7 in [0, 8)]
      %19 = vector.transfer_read %arg1[%18], %cst_1 {in_bounds = [true]} : tensor<4096xbf16>, vector<2xbf16>
      %20 = xla_gpu.apply_indexing #map2(%c0 in [0, 2), %thread_id_x in [0, 256))[%arg7 in [0, 8)]
      %21 = vector.transfer_read %arg3[%20], %cst_1 {in_bounds = [true]} : tensor<4096xbf16>, vector<2xbf16>
      %22 = xla_gpu.apply_indexing #map2(%c0 in [0, 2), %thread_id_x in [0, 256))[%arg7 in [0, 8)]
      %23 = vector.transfer_read %arg4[%4, %5, %22], %cst_1 {in_bounds = [true]} : tensor<4x8x4096xbf16>, vector<2xbf16>
      %24 = xla_gpu.apply_indexing #map2(%c0 in [0, 2), %thread_id_x in [0, 256))[%arg7 in [0, 8)]
      %25 = vector.transfer_read %arg0[%4, %5, %24], %cst {in_bounds = [true]} : tensor<4x8x4096xf32>, vector<2xf32>
      %26:2 = scf.for %arg10 = %c0 to %c2 step %c1 iter_args(%arg11 = %arg8, %arg12 = %arg9) -> (tensor<4x8x4096xf32>, f32) {
        %27 = xla_gpu.apply_indexing #map2(%arg10 in [0, 2), %thread_id_x in [0, 256))[%arg7 in [0, 8)]
        %28 = vector.extract %25[%arg10] : f32 from vector<2xf32>
        %29 = vector.extract %23[%arg10] : bf16 from vector<2xbf16>
        %30 = arith.extf %29 : bf16 to f32
        %31 = vector.extract %21[%arg10] : bf16 from vector<2xbf16>
        %32 = arith.extf %31 : bf16 to f32
        %33 = arith.mulf %30, %32 : f32
        %34 = arith.mulf %33, %8 : f32
        %35 = vector.extract %19[%arg10] : bf16 from vector<2xbf16>
        %36 = arith.extf %35 : bf16 to f32
        %37 = arith.addf %36, %cst : f32
        %38 = arith.mulf %34, %37 : f32
        %39 = arith.addf %28, %38 : f32
        %40 = arith.mulf %39, %39 : f32
        %41 = arith.addf %arg12, %40 : f32
        %inserted = tensor.insert %39 into %arg11[%4, %5, %27] : tensor<4x8x4096xf32>
        scf.yield %inserted, %41 : tensor<4x8x4096xf32>, f32
      }
      scf.yield %26#0, %26#1 : tensor<4x8x4096xf32>, f32
    }
    return %9#0, %9#1 : tensor<4x8x4096xf32>, f32
  }
}

// CHECK-LABEL: @fully_unroll
// CHECK-NOT: scf.for

// -----

module {
  func.func @unroll_by_factor(%arg0: f32) -> f32 {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c256 = arith.constant 256 : index
    %ret = scf.for %i = %c0 to %c256 step %c1 iter_args (%v = %arg0) -> (f32) {
      %exp = math.exp %v : f32
      %add = arith.addf %v, %exp : f32
      %log = math.log %add : f32
      scf.yield %log : f32
    }
    return %ret : f32
  }
}

// CHECK-LABEL: @unroll_by_factor
// CHECK: %[[C8:.*]] = arith.constant 8 : index
// CHECK: scf.for {{.*}} step %[[C8]]

// -----

module {
  func.func @do_not_unroll(%arg0: f32) -> f32 {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c31 = arith.constant 31 : index
    %ret = scf.for %i = %c0 to %c31 step %c1 iter_args (%v = %arg0) -> (f32) {
      %exp = math.exp %v : f32
      %add = arith.addf %v, %exp : f32
      %log = math.log %add : f32
      scf.yield %log : f32
    }
    return %ret : f32
  }
}

// CHECK-LABEL: @do_not_unroll
// CHECK: %[[C1:.*]] = arith.constant 1 : index
// CHECK: scf.for {{.*}} step %[[C1]]

// -----

module {
  func.func @pipeline_extract(%arg: tensor<31xf32>) -> f32 {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c31 = arith.constant 31 : index
    %cst = arith.constant 0.0 : f32
    %ret = scf.for %i = %c0 to %c31 step %c1 iter_args (%iter = %cst) -> (f32) {
      %val = tensor.extract %arg[%i] : tensor<31xf32>
      %log = math.log %val : f32
      %add = arith.addf %log, %iter : f32
      scf.yield %add : f32
    }
    return %ret : f32
  }
}

// CHECK: #[[$MAP:.*]] = affine_map<(d0) -> (d0 + 1)>
// CHECK-LABEL: @pipeline_extract
// CHECK-DAG:  %[[C0:.*]] = arith.constant 0 : index
// CHECK-DAG:  %[[C30:.*]] = arith.constant 30 : index
// CHECK:      %[[VAL0:.*]] = tensor.extract %[[ARG0:.*]][%[[C0]]]
// CHECK:      scf.for %[[I:.*]] = %[[C0]] {{.*}} iter_args(%[[ITER:.*]] = {{.*}}, %[[VAL:.*]] = %[[VAL0]])
// CHECK-DAG:  %[[NEXT_I_EXISTS:.*]] = arith.cmpi ult, %[[I]], %[[C30]]
// CHECK-DAG:  %[[NEXT_I:.*]] = xla_gpu.apply_indexing #[[$MAP]](%[[I]]
// CHECK:      %[[NEXT_VAL:.*]] = scf.if %[[NEXT_I_EXISTS]]
// CHECK-NEXT:   tensor.extract %[[ARG0]][%[[NEXT_I]]]
// CHECK-NEXT:   yield
// CHECK-NEXT: else
// CHECK-NEXT:   yield %[[VAL]]
// CHECK:     math.log %[[VAL]]
// CHECK:     %[[ADD:.*]] = arith.addf
// CHECK:     yield %[[ADD]], %[[NEXT_VAL]]

// -----

module {
  func.func @pipeline_transfer(%arg: tensor<34xf32>) -> vector<2xf32> {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c17 = arith.constant 17 : index
    %cst = arith.constant dense<[0.0, 0.0]> : vector<2xf32>
    %cst0  = arith.constant 0.0 : f32
    %ret = scf.for %i = %c0 to %c17 step %c1 iter_args (%iter = %cst) -> (vector<2xf32>) {
      %base = xla_gpu.apply_indexing affine_map<(d0) -> (d0 * 2)>(%i in [0, 16))
      %val = vector.transfer_read %arg[%base], %cst0 : tensor<34xf32>, vector<2xf32>
      %log = math.log %val : vector<2xf32>
      %add = arith.addf %log, %iter : vector<2xf32>
      scf.yield %add : vector<2xf32>
    }
    return %ret : vector<2xf32>
  }
}

// CHECK-DAG: #[[$MAP0:.*]] = affine_map<(d0) -> (d0 * 2)>
// CHECK-DAG: #[[$MAP1:.*]] = affine_map<(d0) -> (d0 + 1)>
// CHECK-LABEL: @pipeline_transfer
// CHECK-DAG:  %[[C0:.*]] = arith.constant 0 : index
// CHECK-DAG:  %[[C16:.*]] = arith.constant 16 : index
// CHECK:      %[[BASE0:.*]] = xla_gpu.apply_indexing #[[$MAP0]](%[[C0]]
// CHECK:      %[[VAL0:.*]] = vector.transfer_read %[[ARG0:.*]][%[[BASE0]]]
// CHECK:      scf.for %[[I:.*]] = %[[C0]] {{.*}} iter_args(%[[ITER:.*]] = {{.*}}, %[[VAL:.*]] = %[[VAL0]])
// CHECK-DAG:  %[[NEXT_I_EXISTS:.*]] = arith.cmpi ult, %[[I]], %[[C16]]
// CHECK-DAG:  %[[NEXT_I:.*]] = xla_gpu.apply_indexing #[[$MAP1]](%[[I]]
// CHECK-DAG:  %[[NEXT_BASE:.*]] = xla_gpu.apply_indexing #[[$MAP0]](%[[NEXT_I]]
// CHECK:      %[[NEXT_VAL:.*]] = scf.if %[[NEXT_I_EXISTS]]
// CHECK-NEXT:   vector.transfer_read %[[ARG0]][%[[NEXT_BASE]]]
// CHECK-NEXT:   yield
// CHECK-NEXT: else
// CHECK-NEXT:   yield %[[VAL]]
// CHECK:     math.log %[[VAL]]
// CHECK:     %[[ADD:.*]] = arith.addf
// CHECK:     yield %[[ADD]], %[[NEXT_VAL]]
