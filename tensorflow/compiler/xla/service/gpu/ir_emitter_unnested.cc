/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/gpu/ir_emitter_unnested.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/inlined_vector.h"
#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/Function.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/Dialect/mhlo/IR/lhlo_ops.h"
#include "tensorflow/compiler/mlir/utils/name_utils.h"
#include "tensorflow/compiler/mlir/xla/hlo_function_importer.h"
#include "tensorflow/compiler/mlir/xla/hlo_utils.h"
#include "tensorflow/compiler/mlir/xla/mlir_hlo_to_hlo.h"
#include "tensorflow/compiler/mlir/xla/type_to_shape.h"
#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/service/buffer_assignment.h"
#include "tensorflow/compiler/xla/service/dfs_hlo_visitor.h"
#include "tensorflow/compiler/xla/service/gpu/backend_configs.pb.h"
#include "tensorflow/compiler/xla/service/gpu/buffer_allocations.h"
#include "tensorflow/compiler/xla/service/gpu/collective_permute_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/conditional_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/copy_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/cudnn_batchnorm_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/for_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_constants.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_conv_runner.h"
#include "tensorflow/compiler/xla/service/gpu/hlo_to_ir_bindings.h"
#include "tensorflow/compiler/xla/service/gpu/ir_emission_utils.h"
#include "tensorflow/compiler/xla/service/gpu/ir_emitter_context.h"
#include "tensorflow/compiler/xla/service/gpu/kernel_mapping_scheme.h"
#include "tensorflow/compiler/xla/service/gpu/kernel_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/launch_dimensions.h"
#include "tensorflow/compiler/xla/service/gpu/memset_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/nccl_all_reduce_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/parallel_loop_emitter.h"
#include "tensorflow/compiler/xla/service/gpu/replica_id_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/sequential_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/target_util.h"
#include "tensorflow/compiler/xla/service/gpu/thunk.h"
#include "tensorflow/compiler/xla/service/gpu/tuple_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/while_thunk.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/llvm_ir/buffer_assignment_util.h"
#include "tensorflow/compiler/xla/service/llvm_ir/dynamic_update_slice_util.h"
#include "tensorflow/compiler/xla/service/llvm_ir/fused_ir_emitter.h"
#include "tensorflow/compiler/xla/service/llvm_ir/ir_array.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_util.h"
#include "tensorflow/compiler/xla/service/llvm_ir/sort_util.h"
#include "tensorflow/compiler/xla/service/llvm_ir/tuple_ops.h"
#include "tensorflow/compiler/xla/service/name_uniquer.h"
#include "tensorflow/compiler/xla/service/pattern_matcher.h"
#include "tensorflow/compiler/xla/service/shape_inference.h"
#include "tensorflow/compiler/xla/service/while_loop_analysis.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/union_find.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/window_util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/bits.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/logging.h"

namespace xla {
namespace gpu {

namespace {

using absl::InlinedVector;
using absl::nullopt;
using absl::optional;
using absl::StrCat;
using llvm_ir::IrArray;
using llvm_ir::IrName;

const auto kDimX = KernelMappingScheme::DimX;
const auto kDimY = KernelMappingScheme::DimY;
const auto kDimZ = KernelMappingScheme::DimZ;
const auto kDimTot = KernelMappingScheme::DimTot;

const auto kLinearIndexingX = KernelMappingScheme::LinearIndexingX;
const auto kStridedIndexingX = KernelMappingScheme::StridedIndexingX;
const auto kStridedLinearIndexingX =
    KernelMappingScheme::StridedLinearIndexingX;

// If a dimensions is smaller than this, untiled transposition may be more
// efficient.
const int64 kMinDimensionToTransposeTiled = 16;

// Updates the launch dimensions in "thunk" and annotate the launch dimensions
// of the corresponding IR kernel in "llvm_module".
// Precondition: "thunk" must be a KernelThunk.
void UpdateLaunchDimensions(const LaunchDimensions& launch_dims, Thunk* thunk,
                            llvm::Module* llvm_module) {
  CHECK(Thunk::Kind::kKernel == thunk->kind());
  KernelThunk* kernel_thunk = static_cast<KernelThunk*>(thunk);
  kernel_thunk->SetLaunchDimensions(launch_dims);

  // Add __launch_bounds__ to metadata. This limits registers per thread to
  // avoid out-of-resources launching errors.
  llvm::NamedMDNode* nvvm_annotations_node =
      llvm_module->getOrInsertNamedMetadata("nvvm.annotations");
  llvm::Function* ir_kernel =
      llvm_module->getFunction(kernel_thunk->kernel_name().c_str());
  llvm::LLVMContext& llvm_context = llvm_module->getContext();
  llvm::ConstantInt* threads_per_block_ir_value = llvm::ConstantInt::get(
      llvm::IntegerType::get(llvm_context, /*NumBits=*/32),
      launch_dims.thread_counts_per_block().x);
  // Our launch bounds are exact, so we can specify them as reqntidx rather than
  // maxntidx.
  nvvm_annotations_node->addOperand(llvm::MDNode::get(
      llvm_context,
      {llvm::ConstantAsMetadata::get(ir_kernel),
       llvm::MDString::get(llvm_context, "reqntidx"),
       llvm::ConstantAsMetadata::get(threads_per_block_ir_value)}));
}

int64_t GetAllocationIndex(mlir::BlockArgument func_arg) {
  auto func_op =
      mlir::cast<mlir::FuncOp>(func_arg.getParentRegion()->getParentOp());
  return func_op
      .getArgAttrOfType<mlir::IntegerAttr>(func_arg.getArgNumber(),
                                           "lmhlo.alloc")
      .getValue()
      .getSExtValue();
}

StatusOr<BufferAllocation::Slice> GetAllocationSliceForMlir(
    mlir::Value v, absl::Span<const BufferAllocation> allocations) {
  int64 size = v.getType().cast<mlir::MemRefType>().getSizeInBits() / 8;

  if (auto arg = v.dyn_cast<mlir::BlockArgument>()) {
    return BufferAllocation::Slice(&allocations[GetAllocationIndex(arg)], 0,
                                   size);
  }

  // We match the following patterns here:
  //  base := ViewOp(arg) | get_global_memref (global_memref)
  //  root := base | MemRefReinterpretCastOp(base)

  if (mlir::Operation* op = v.getDefiningOp()) {
    if (auto cast = mlir::dyn_cast<mlir::MemRefReinterpretCastOp>(op)) {
      mlir::Value source = cast.getViewSource();
      op = source.getDefiningOp();
      if (!op) {
        return Unimplemented("MemRefReinterpretCastOp has to wrap an op");
      }
    }
    if (auto view = mlir::dyn_cast<mlir::ViewOp>(op)) {
      return BufferAllocation::Slice(
          &allocations[GetAllocationIndex(
              view.source().cast<mlir::BlockArgument>())],
          mlir::cast<mlir::ConstantOp>(view.byte_shift().getDefiningOp())
              .value()
              .cast<mlir::IntegerAttr>()
              .getValue()
              .getSExtValue(),
          size);
    } else if (mlir::isa<mlir::GetGlobalMemrefOp>(op)) {
      int64_t index =
          op->getAttrOfType<mlir::IntegerAttr>("lmhlo.alloc").getInt();
      int64_t offset =
          op->getAttrOfType<mlir::IntegerAttr>("lmhlo.slice_offset").getInt();
      int64_t size =
          op->getAttrOfType<mlir::IntegerAttr>("lmhlo.slice_size").getInt();
      return BufferAllocation::Slice(&allocations[index], offset, size);
    }
    return Unimplemented("MemRefReinterpretCastOp has to wrap a ViewOp");
  }

  return Unimplemented(
      "Operand has to be in the form of ViewOp(arg) or "
      "StaticMemRefCastOp(ViewOp(arg))");
}

static bool WritesMlirBuffer(mlir::Operation* op, mlir::Value operand) {
  llvm::SmallVector<mlir::MemoryEffects::EffectInstance, 2> effects;
  mlir::cast<mlir::MemoryEffectOpInterface>(op).getEffectsOnValue(operand,
                                                                  effects);
  return absl::c_any_of(
      effects, [](const mlir::MemoryEffects::EffectInstance& instance) {
        return mlir::isa<mlir::MemoryEffects::Write>(instance.getEffect());
      });
}

bool BinarySearchDenseElementsAttr(::mlir::DenseIntElementsAttr elements,
                                   int64 v) {
  ::mlir::APInt value(sizeof(int64) * 8, v, /*isSigned=*/true);
  return std::binary_search(
      elements.begin(), elements.end(), value,
      [](const ::mlir::APInt& x, const ::mlir::APInt& y) { return x.slt(y); });
}

// Returns true if the fusion contains any instruction that is likely
// translated to complex LLVM IR, such as loops, and prevent vectorization.
bool MayPreventVectorization(const HloInstruction& hlo) {
  if (hlo.opcode() == HloOpcode::kFusion) {
    return absl::c_any_of(hlo.fused_instructions_computation()->instructions(),
                          [](const HloInstruction* instr) {
                            switch (instr->opcode()) {
                              case HloOpcode::kReduceWindow:
                              case HloOpcode::kSort:
                              case HloOpcode::kDot:
                              case HloOpcode::kSin:
                              case HloOpcode::kCos:
                              case HloOpcode::kPower:
                              case HloOpcode::kAtan2:
                                return true;
                              default:
                                return false;
                            }
                          });
  } else if (hlo.IsElementwise()) {
    // Unfused elementwise operations are usually memory bound, unroll them.
    switch (hlo.opcode()) {
        // The following elementwise operation implementations contain branches.
        // LLVM vectorizer doesn't work in that case.
        // The unrolled code is faster when it isn't vectorized.
      case HloOpcode::kSin:
      case HloOpcode::kCos:
      case HloOpcode::kPower:
      case HloOpcode::kAtan2:
        return true;
      default:
        return false;
    }
  } else if (hlo.opcode() == HloOpcode::kReduce && hlo.shape().IsArray()) {
    // TODO: check if the to_apply() attribute contains instruction
    // that break LLVM vectorization.
    return false;
  }
  return true;
}

}  // namespace

IrEmitterUnnested::IrEmitterUnnested(const HloModuleConfig& hlo_module_config,
                                     const HloComputation* hlo_computation,
                                     IrEmitterContext* ir_emitter_context)
    : IrEmitter(hlo_module_config, ir_emitter_context, /*is_nested=*/false),
      hlo_computation_(hlo_computation),
      mlir_scratch_module_(mlir::ModuleOp::create(
          mlir::Builder(ir_emitter_context->mlir_context()).getUnknownLoc())),
      lhlo_scratch_emitter_(ir_emitter_context_->buffer_assignment(),
                            *hlo_computation, mlir_scratch_module_.get()) {}

StatusOr<std::unique_ptr<IrEmitterUnnested>> IrEmitterUnnested::Create(
    const HloModuleConfig& hlo_module_config,
    const HloComputation* hlo_computation,
    IrEmitterContext* ir_emitter_context) {
  auto emitter = std::unique_ptr<IrEmitterUnnested>(new IrEmitterUnnested(
      hlo_module_config, hlo_computation, ir_emitter_context));
  TF_RETURN_IF_ERROR(emitter->lhlo_scratch_emitter_.Initialize());
  TF_RETURN_IF_ERROR(emitter->EmitConstants(*hlo_computation, true));
  return std::move(emitter);
}

Status IrEmitterUnnested::Postprocess(HloInstruction* hlo) {
  bindings_.UnbindAllLocalIrValues();
  return DfsHloVisitor::Postprocess(hlo);
}

llvm::Function* IrEmitterUnnested::BuildKernelPrototype(
    absl::string_view name, absl::Span<const BufferAllocation* const> args) {
  // Compute the kernel name. The opcode string may contain "-" which cannot be
  // in a PTX function name, so sanitize the name before uniquifying it.
  string kernel_name = ir_emitter_context_->name_uniquer()->GetUniqueName(
      llvm_ir::SanitizeFunctionName(std::string(name)));

  // Create the kernel and add it to the module.
  llvm::Module* module = ir_emitter_context_->llvm_module();
  llvm::LLVMContext& context = module->getContext();
  llvm::FunctionType* kernel_type = llvm::FunctionType::get(
      /*Result=*/llvm::Type::getVoidTy(context),
      std::vector<llvm::Type*>(args.size(), b_.getInt8PtrTy()),
      /*isVarArg=*/false);
  llvm::Function* kernel =
      llvm::Function::Create(kernel_type, llvm::GlobalValue::ExternalLinkage,
                             kernel_name.c_str(), module);

  // Add dereferenceable and alignment information to each of the kernel's
  // parameters.
  auto arg_it = kernel->arg_begin();
  for (size_t arg_no = 0; arg_no < args.size(); ++arg_no) {
    const BufferAllocation* alloc = args[arg_no];
    llvm::Argument* fn_arg = &*arg_it;
    ++arg_it;

    kernel->addDereferenceableAttr(arg_no + 1, alloc->size());

    const int64 alignment = [&] {
      if (alloc->is_entry_computation_parameter()) {
        return kEntryParameterAlignBytes;
      } else if (alloc->is_constant()) {
        return kConstantBufferAlignBytes;
      } else {
        return kXlaAllocatedBufferAlignBytes;
      }
    }();

    kernel->addParamAttr(
        arg_no,
        llvm::Attribute::get(context, llvm::Attribute::Alignment, alignment));

    if (alloc->IsPreallocatedTempBuffer()) {
      fn_arg->setName("temp_buf");
    } else {
      fn_arg->setName(StrCat("alloc", alloc->index()));
    }
  }

  AnnotateFunctionAsGpuKernel(module, kernel, &b_);

  // TODO(b/65380986): Investigate if adding fast math flags for generated
  // kernels makes sense.

  // Update the insert point to the entry basic block.
  llvm::BasicBlock* entry_bb =
      llvm::BasicBlock::Create(context, /*Name=*/"entry", /*Parent=*/kernel);

  // Emit a "return void" at entry_bb's end, and set the insert point before
  // that return instruction.
  b_.SetInsertPoint(llvm::ReturnInst::Create(context, entry_bb));

  return kernel;
}

namespace {
// Computes the maximum valid unroll factor for a given instruction.
int ComputeMaxUnrollFactor(const Shape& shape,
                           const HloModuleConfig& hlo_module_config) {
  int max_unroll_factor =
      hlo_module_config.debug_options().xla_gpu_max_kernel_unroll_factor();

  int64 num_elements = ShapeUtil::ElementsIn(shape);
  for (int i = max_unroll_factor; i > 1; i /= 2) {
    if (num_elements % i == 0) {
      return i;
    }
  }

  // Cannot unroll.
  return 1;
}

// Computes the maximum valid unroll factor for a given instruction.
int ComputeMaxUnrollFactor(const HloInstruction* hlo) {
  // Find the largest possible power of two to unroll by.
  // TODO(kramerb): Make this smarter.
  const Shape& element_shape = hlo->IsMultiOutputFusion()
                                   ? ShapeUtil::GetSubshape(hlo->shape(), {0})
                                   : hlo->shape();
  return ComputeMaxUnrollFactor(element_shape, hlo->GetModule()->config());
}

// Returns the llvm type for the indices used in the kernel that contains the
// hlo instruction. Such indices include the index for the parallel loop and
// the indices for the tensors accessed by the kernel. The return type is i32
// iff the following conditions are met:
//  . The launch_size of the kernel is within the range of i32.
//  . The sizes of all the tensors accessed within the kernel are within the
//    range of i32.
// Otherwise, the return type is i64.
llvm::Type* GetIndexTypeForKernel(const HloInstruction* hlo, int64 launch_size,
                                  llvm::IRBuilder<>* b) {
  // Find the unnested hlo instruction for which the kernel is generated for.
  const HloInstruction* unnested_hlo = hlo;
  const HloComputation* computation = hlo->parent();
  if (computation->IsFusionComputation()) {
    unnested_hlo = computation->FusionInstruction();
  }

  auto shape_in_range = [&](const Shape& s) {
    bool in_range = true;
    ShapeUtil::ForEachSubshape(s, [&](const Shape& sub_shape,
                                      const ShapeIndex& /*index*/) {
      if (sub_shape.IsArray() && !IsInt32(ShapeUtil::ElementsIn(sub_shape))) {
        in_range = false;
      }
    });

    return in_range;
  };

  llvm::Type* i64_ty = b->getInt64Ty();
  // Check launch dimension
  if (!IsInt32(launch_size)) {
    return i64_ty;
  }

  // Check the size of result tensors
  if (!shape_in_range(unnested_hlo->shape())) {
    return i64_ty;
  }

  auto hlo_shape_in_range = [&](const HloInstruction* operand) -> bool {
    return shape_in_range(operand->shape());
  };

  // Check the size of input tensors
  if (!absl::c_all_of(unnested_hlo->operands(), hlo_shape_in_range)) {
    return i64_ty;
  }

  // Check the size of the internal result tensors
  if (unnested_hlo->opcode() == HloOpcode::kFusion) {
    if (!absl::c_all_of(
            unnested_hlo->fused_instructions_computation()->instructions(),
            hlo_shape_in_range)) {
      return i64_ty;
    }
  }

  return b->getInt32Ty();
}

// The same as GetIndexTypeForKernel, but works with MLIR ops.
llvm::Type* GetIndexTypeForKernelFromMlir(mlir::Operation* op,
                                          int64 launch_size,
                                          llvm::IRBuilder<>* b) {
  auto shape_in_range = [&](const Shape& s) {
    bool in_range = true;
    ShapeUtil::ForEachSubshape(s, [&](const Shape& sub_shape,
                                      const ShapeIndex& /*index*/) {
      if (sub_shape.IsArray() && !IsInt32(ShapeUtil::ElementsIn(sub_shape))) {
        in_range = false;
      }
    });

    return in_range;
  };

  llvm::Type* i64_ty = b->getInt64Ty();
  // Check launch dimension
  if (!IsInt32(launch_size)) {
    return i64_ty;
  }

  // Check the size of result tensors
  for (auto result : op->getResults()) {
    if (!shape_in_range(TypeToShape(result.getType()))) {
      return i64_ty;
    }
  }

  auto hlo_shape_in_range = [&](mlir::Value operand) -> bool {
    return shape_in_range(TypeToShape(operand.getType()));
  };

  // Check the size of input tensors
  if (!absl::c_all_of(op->getOperands(), hlo_shape_in_range)) {
    return i64_ty;
  }

  // Check the size of the internal result tensors
  if (auto fusion = mlir::dyn_cast<mlir::lmhlo::FusionOp>(op)) {
    auto result = fusion.region().walk([&](mlir::Operation* op) {
      for (mlir::Value result : op->getResults()) {
        if (!hlo_shape_in_range(result)) {
          return mlir::WalkResult::interrupt();
        }
      }
      return mlir::WalkResult::advance();
    });
    if (result.wasInterrupted()) {
      return i64_ty;
    }
  }

  return b->getInt32Ty();
}

// Gets the input shape of the ROOT slices, which will be used as the kernel
// launch dims. The slice input fusion requires the input shapes of the ROOT
// slices to be the same although the (slice) output shapes can be different.
//
// Returns the input shape of the ROOT slices if all the input shapes of ROOT
// slices are the same and the slices are non-strided. Otherwise, returns
// FailedPrecondition.
StatusOr<Shape> GetConsistentInputShapeForRootSlices(
    const HloInstruction& fusion) {
  if (!IsInputFusibleSlices(fusion, /*verify_no_strides=*/true)) {
    return FailedPrecondition(
        "Unsupported root for slice input fusion. "
        "Only non-strided slices are supported.");
  }

  const HloInstruction& root = *fusion.fused_expression_root();
  if (root.opcode() == HloOpcode::kSlice) {
    return root.operands()[0]->shape();
  }

  CHECK_EQ(root.opcode(), HloOpcode::kTuple);
  const Shape& first_slice_operand_shape =
      root.operands()[0]->operands()[0]->shape();
  for (size_t i = 1; i < root.operands().size(); ++i) {
    const HloInstruction* slice = root.operands()[i];
    const Shape& operand_shape = slice->operands()[0]->shape();
    if (!ShapeUtil::EqualIgnoringElementType(first_slice_operand_shape,
                                             operand_shape)) {
      return FailedPrecondition(
          "Fused slices do not have the same input shape, fused computation = "
          "%s.",
          root.parent()->name());
    }
  }

  return first_slice_operand_shape;
}

}  // namespace

Status IrEmitterUnnested::DefaultAction(HloInstruction* hlo) {
  return IrEmitter::DefaultAction(hlo);
}

Status IrEmitterUnnested::DefaultActionForMlir(MlirEmitterInput input) {
  // Replace unnested op with a fused nested op.
  //
  // TODO(timshen): Ultimately this should be a pass. It's currently not a pass,
  // because we don't have a fully functioning LMHLO graph yet.

  mlir::Location loc = input.op->getLoc();
  mlir::lmhlo::FusionOp fusion = nullptr;
  Shape output_shape;
  if (auto copy = mlir::dyn_cast<mlir::lmhlo::CopyOp>(input.op)) {
    fusion = mlir::OpBuilder(copy).create<mlir::lmhlo::FusionOp>(
        loc, llvm::ArrayRef<mlir::NamedAttribute>());
    copy.getOperation()->moveBefore(&fusion.region().front().back());
    mlir::OpBuilder b(copy);
    auto operand = b.create<mlir::TensorLoadOp>(loc, copy.operand());
    HloFunctionImporter::SetLayoutForMlir(
        operand, TypeToShape(copy.operand().getType()));
    auto fused_copy = b.create<mlir::mhlo::CopyOp>(loc, operand);
    output_shape = TypeToShape(copy.output().getType());
    HloFunctionImporter::SetLayoutForMlir(fused_copy, output_shape);
    b.create<mlir::TensorStoreOp>(loc, fused_copy, copy.output());
    copy.getOperation()->erase();
  } else {
    input.op->dump();
    LOG(FATAL) << "Unimplemented default action for mlir op";
  }
  input.op = fusion;
  auto ret = EmitLoopFusionFromMlir(
      input, output_shape,
      ComputeMaxUnrollFactor(output_shape, hlo_module_config_));
  return ret;
}

Status IrEmitterUnnested::HandleConditional(HloInstruction* conditional) {
  TF_ASSIGN_OR_RETURN(auto thunk, BuildConditionalThunk(conditional));
  AddThunkToThunkSequence(std::move(thunk));
  return Status::OK();
}

Status IrEmitterUnnested::HandleConvolution(HloInstruction* convolution) {
  AddThunkToThunkSequence(
      BuildKernelThunk(convolution, /*implements_whole_instruction=*/true));
  return IrEmitter::HandleConvolution(convolution);
}

// Input = {dynamic array(with dynamic dimension meta data at the end)}
// Output = {static array, dynamic_dim0, dynamic_dim1}
Status IrEmitterUnnested::HandlePadToStatic(HloInstruction* pad_to_static) {
  TF_ASSIGN_OR_RETURN(auto input, GetMlirEmitterInput(pad_to_static));
  return EmitPadToStaticFromMlir(input);
}

Status IrEmitterUnnested::EmitPadToStaticFromMlir(MlirEmitterInput mlir_input) {
  // TODO(jurahul): Create an op to represent PadToStatic.
  auto pad_to_static = ::mlir::cast<::mlir::lmhlo::CustomCallOp>(mlir_input.op);
  int unroll_factor = 1;
  std::string ir_name = mlir::GetNameFromLoc(pad_to_static.getLoc());

  absl::Span<const BufferAllocation> allocations(
      ir_emitter_context_->buffer_assignment().Allocations());
  std::vector<llvm_ir::IrArray> ir_arrays;
  TF_ASSIGN_OR_RETURN(
      auto kernel_thunk,
      BuildKernelThunkForMlir(pad_to_static, mlir_input.thunk_info,
                              mlir_input.extra_slice, &ir_arrays));

  const llvm_ir::IrArray source_array = ir_arrays[0];
  const llvm_ir::IrArray output_array = ir_arrays[1];
  auto output_dim_arrays =
      absl::Span<const llvm_ir::IrArray>(ir_arrays).subspan(2);

  // pseudo code for PadToStatic on a 2d array
  //   int* source_array = input[0];
  //   int* dest_array = output[0];
  const Shape& data_shape =
      TypeToShape(pad_to_static.output().front().getType());
  const Shape& input_shape =
      TypeToShape(pad_to_static.args().front().getType());
  llvm::Value* source_buffer = source_array.GetBasePointer();
  llvm::Value* raw_buffer =
      b_.CreateBitCast(source_buffer, b_.getInt8Ty()->getPointerTo());

  // TODO(jurahul): input_shape here is the static shape of the input (which has
  // a dynamic shape in XLA). Currently, we are mapping that to a static shaped
  // memref. When we change that to a more appropriate representation in MLIR,
  // fix this code to correctly deduce the static shape backing the dynamically
  // shaped memref.
  int64 raw_data_size = ShapeUtil::ByteSizeOf(input_shape);

  //   int* dyn_dim0_size = source_array + meta_data_offset;
  //   int* dyn_dim1_size = source_array + meta_data_offset + sizeof(int);
  std::vector<llvm::Value*> dynamic_dims;
  for (int64 i = 1; i < pad_to_static.output().size(); ++i) {
    // Dynamic size of each dimension is attached at the end of the source
    // array(operand(0)). We need to extract these value.
    const Shape& dim_shape = TypeToShape(pad_to_static.output()[i].getType());
    TF_RET_CHECK(Shape::Equal()(dim_shape, ShapeUtil::MakeScalarShape(S32)));

    const int64 dim_index = i - 1;
    llvm::Value* metadata = b_.CreateConstInBoundsGEP1_32(
        b_.getInt8Ty(), raw_buffer, raw_data_size + dim_index * sizeof(int32));
    llvm::Value* dyn_dim_size = b_.CreateLoad(
        b_.CreateBitCast(metadata, b_.getInt32Ty()->getPointerTo()),
        "dyn_dim_size");
    dynamic_dims.push_back(dyn_dim_size);
  }

  // only one thread need to store the dynamic index
  //   int thread_id = GetThreadId();
  //   int block_id = GetBlockId();
  //   if (thread_id == 0 && block_id == 0) {
  //     *output[1] = *dyn_dim0_size;
  //     *output[2] = *dyn_dim1_size;
  //   }
  KernelSupportLibrary{&b_}.If("is_thred_0", IsBlock0Thread0(&b_), [&] {
    for (int64 i = 1; i < pad_to_static.output().size(); ++i) {
      const int64 dim_index = i - 1;
      llvm::Value* dest_dim_size_address =
          output_dim_arrays[dim_index].GetBasePointer();
      // output[i] stores dynamic_dim_(i-1)
      b_.CreateStore(dynamic_dims[i - 1],
                     b_.CreateBitCast(dest_dim_size_address,
                                      b_.getInt32Ty()->getPointerTo()));
    }
  });

  //     int dyn_element_total = 1;
  //     dyn_element_total *= *dyn_dim0_size;
  //     dyn_element_total *= *dyn_dim1_size;
  llvm::Value* dyn_element_total = llvm::ConstantInt::get(b_.getInt32Ty(), 1);
  for (llvm::Value* dynamic_dim : dynamic_dims) {
    dyn_element_total = b_.CreateMul(dyn_element_total, dynamic_dim,
                                     /*Name=*/"dyn_element_total");
  }

  //   linear_index = block_id * thread_per_block + thread_id;
  //   if (linear_index < max_num_element) {
  //     Index static_index =
  //         delinerized(linerized_index, static_dim0_size, static_dim1_size);
  //     if (linerized_index < dyn_element_total) {
  //       Index dyn_index =
  //           delinerized(linerized_index, *dyn_dim0_size, *dyn_dim1_size);
  //       dest_array[dyn_index.dim0][dyn_index.dim1] =
  //           source_array[static_index.dim0][static_index.dim1];
  //     }
  //   }
  llvm_ir::LoopEmitter::BodyEmitter body_generator =
      [&](const llvm_ir::IrArray::Index& array_index) -> Status {
    llvm::Value* linearIndex =
        array_index.Linearize(input_shape.dimensions(), &b_);
    auto if_in_dyn_bounds = llvm_ir::EmitIfThenElse(
        b_.CreateICmpULT(linearIndex, dyn_element_total),
        llvm_ir::IrName(ir_name, "in_dyn_bounds"), &b_, false);
    // Set IR builder insertion point to the body of the if structure.
    llvm_ir::SetToFirstInsertPoint(if_in_dyn_bounds.true_block, &b_);
    llvm_ir::IrArray::Index dyn_index(linearIndex, input_shape,
                                      absl::MakeSpan(dynamic_dims), &b_);
    output_array.EmitWriteArrayElement(
        dyn_index,
        source_array.EmitReadArrayElement(array_index, &b_, /*name=*/""), &b_,
        /*use_linear_index=*/false);
    return Status::OK();
  };

  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      input_shape, ir_emitter_context_->gpu_device_info(), unroll_factor);
  UpdateLaunchDimensions(launch_dimensions, kernel_thunk.get(),
                         ir_emitter_context_->llvm_module());
  TF_RETURN_IF_ERROR(
      ParallelLoopEmitter(body_generator, data_shape, launch_dimensions, &b_,
                          unroll_factor)
          .EmitLoop(ir_name,
                    GetIndexTypeForKernelFromMlir(
                        pad_to_static, launch_dimensions.launch_bound(), &b_)));
  thunk_sequence_.emplace_back(std::move(kernel_thunk));
  return Status::OK();
}

// Input = {dynamic array(with dynamic dimension meta data at the end)}
// Output = {static array, dynamic_dim0, dynamic_dim1}
Status IrEmitterUnnested::HandleSliceToDynamic(
    HloInstruction* slice_to_dynamic) {
  TF_ASSIGN_OR_RETURN(auto input, GetMlirEmitterInput(slice_to_dynamic));
  return EmitSliceToDynamicFromMlir(input);
}

Status IrEmitterUnnested::EmitSliceToDynamicFromMlir(
    MlirEmitterInput mlir_input) {
  // TODO(jurahul): Create an op to represent SliceToDynamic.
  auto slice_to_dynamic =
      ::mlir::cast<::mlir::lmhlo::CustomCallOp>(mlir_input.op);
  int unroll_factor = 1;
  std::string ir_name = mlir::GetNameFromLoc(slice_to_dynamic.getLoc());
  absl::Span<const BufferAllocation> allocations(
      ir_emitter_context_->buffer_assignment().Allocations());

  std::vector<llvm_ir::IrArray> ir_arrays;
  TF_ASSIGN_OR_RETURN(
      auto kernel_thunk,
      BuildKernelThunkForMlir(slice_to_dynamic, mlir_input.thunk_info,
                              mlir_input.extra_slice, &ir_arrays));

  const Shape& input_shape =
      TypeToShape(slice_to_dynamic.args().front().getType());
  TF_RET_CHECK(slice_to_dynamic.output().size() == 1);
  const Shape& data_shape =
      TypeToShape(slice_to_dynamic.output().front().getType());

  // TODO(jurahul): data_shape here is the static shape of the output (which has
  // a dynamic shape in XLA). Currently, we are mapping that to a static shaped
  // memref. When we change that to a more appropriate representation in MLIR,
  // fix this code to correctly deduce the static shape backing the dynamically
  // shaped memref.

  // calculate the location where metadata needs to be inserted
  //   int* dyn_dim0_size = dest_array + meta_data_offset;
  //   int* dyn_dim1_size = dest_array + meta_data_offset + sizeof(int);
  int32 raw_data_size = ShapeUtil::ByteSizeOf(data_shape);

  // pseudo code for sliceToDynamic on a 2d array
  //   int* source_array = input[0];
  //   int* dest_array = output[0];
  const llvm_ir::IrArray data_array = ir_arrays.back();
  llvm::Value* dest_buffer = data_array.GetBasePointer();
  llvm::Value* raw_buffer =
      b_.CreateBitCast(dest_buffer, b_.getInt8Ty()->getPointerTo());

  // Load dynamic dimensions from memory.
  std::vector<llvm::Value*> dynamic_dims;
  for (int64 i = 1; i < slice_to_dynamic.args().size(); ++i) {
    // const int64 dim_index = i - 1;
    llvm::Value* source_buffer = ir_arrays[i].GetBasePointer();
    llvm::LoadInst* dyn_dim_size = b_.CreateLoad(source_buffer, "dyn_dim_size");
    dynamic_dims.push_back(dyn_dim_size);
  }

  // only one thread need to store the dynamic index
  //   int thread_id = GetThreadId();
  //   int block_id = GetBlockId();
  //   if (thread_id == 0 && block_id == 0) {
  //     *dyn_dim0_size = *output[1];
  //     *dyn_dim1_size = *output[2];
  //   }
  KernelSupportLibrary{&b_}.If("is_thred_0", IsBlock0Thread0(&b_), [&] {
    for (int64 i = 1; i < slice_to_dynamic.args().size(); ++i) {
      const int64 dim_index = i - 1;
      llvm::Value* metadata = b_.CreateConstInBoundsGEP1_32(
          b_.getInt8Ty(), raw_buffer,
          raw_data_size + dim_index * sizeof(int32));
      // output[i] stores dynamic_dim_(i-1)
      b_.CreateStore(
          dynamic_dims[dim_index],
          b_.CreateBitCast(metadata, b_.getInt32Ty()->getPointerTo()));
    }
  });

  //     int dyn_element_total = 1;
  //     dyn_element_total *= dyn_dim0_size;
  //     dyn_element_total *= dyn_dim1_size;
  llvm::Value* dyn_element_total = llvm::ConstantInt::get(b_.getInt32Ty(), 1);
  for (llvm::Value* dynamic_dim : dynamic_dims) {
    dyn_element_total = b_.CreateMul(dyn_element_total, dynamic_dim,
                                     /*Name=*/"dyn_element_total");
  }

  //   linear_index = block_id * thread_per_block + thread_id;
  //   if (linear_index < max_num_element) {
  //     Index static_index =
  //         delinerized(linerized_index, static_dim0_size, static_dim1_size);
  //     if (linerized_index < dyn_element_total) {
  //       Index dyn_index =
  //           delinerized(linerized_index, *dyn_dim0_size, *dyn_dim1_size);
  //       dest_array[static_index.dim0][static_index.di] =
  //           source_array[dyn_index.dim0][dyn_index.dim1];
  //     }
  //   }
  llvm_ir::LoopEmitter::BodyEmitter body_generator =
      [&](const llvm_ir::IrArray::Index& array_index) -> Status {
    llvm::Value* linearIndex =
        array_index.Linearize(input_shape.dimensions(), &b_);
    auto if_in_dyn_bounds = llvm_ir::EmitIfThenElse(
        b_.CreateICmpULT(linearIndex, dyn_element_total),
        llvm_ir::IrName(ir_name, "in_dyn_bounds"), &b_, false);
    // Set IR builder insertion point to the body of the if structure.
    llvm_ir::SetToFirstInsertPoint(if_in_dyn_bounds.true_block, &b_);
    llvm_ir::IrArray::Index dyn_index(linearIndex, input_shape,
                                      absl::MakeSpan(dynamic_dims), &b_);

    data_array.EmitWriteArrayElement(
        array_index,
        ir_arrays[0].EmitReadArrayElement(dyn_index, &b_, /*name=*/"",
                                          /*use_linear_index=*/false),
        &b_);
    return Status::OK();
  };

  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      input_shape, ir_emitter_context_->gpu_device_info(), unroll_factor);
  UpdateLaunchDimensions(launch_dimensions, kernel_thunk.get(),
                         ir_emitter_context_->llvm_module());

  TF_RETURN_IF_ERROR(
      ParallelLoopEmitter(body_generator, data_shape, launch_dimensions, &b_,
                          unroll_factor)
          .EmitLoop(ir_name, GetIndexTypeForKernelFromMlir(
                                 slice_to_dynamic,
                                 launch_dimensions.launch_bound(), &b_)));
  thunk_sequence_.emplace_back(std::move(kernel_thunk));
  return Status::OK();
}

Status IrEmitterUnnested::HandleCustomCall(HloInstruction* custom_call) {
  if (custom_call->custom_call_target() == "PadToStatic") {
    return HandlePadToStatic(custom_call);
  }
  if (custom_call->custom_call_target() == "SliceToDynamic") {
    return HandleSliceToDynamic(custom_call);
  }
  return ThunkEmitter(this).HandleCustomCall(custom_call);
}

Status IrEmitterUnnested::HandleFft(HloInstruction* fft) {
  return ThunkEmitter(this).HandleFft(fft);
}

Status IrEmitterUnnested::HandleTriangularSolve(HloInstruction* hlo) {
  return ThunkEmitter(this).HandleTriangularSolve(hlo);
}

// Convert the following form of fusion region:
//   fusion() {
//     %0 = tensor_load %external_memref0
//     %1 = tensor_load %external_memref1
//     ...
//     tensor_store %ret, %external_memref2
//   }
// to
//   fusion(%external_memref0, %external_memref1) (^bb(%0, %1) {
//     ...
//     mhlo.return %ret
//   })
//
// So that it's suitable for MHLO -> XLA HLO conversion.
// This function won't be needed once ElementalIrEmitter migrates to take MHLO
// instead.
static Status ProcessFusionForConversion(mlir::Region* region,
                                         std::vector<Shape>* operand_shapes) {
  std::vector<mlir::TensorLoadOp> loads;
  std::vector<mlir::TensorStoreOp> stores;

  region->walk([&](mlir::TensorLoadOp load) {
    if (load.memref().getParentRegion() != region) {
      loads.push_back(load);
    }
  });

  region->walk([&](mlir::TensorStoreOp store) {
    if (store.memref().getParentRegion() != region) {
      stores.push_back(store);
    }
  });

  for (auto load : loads) {
    auto arg = region->addArgument(load.getType());
    load.replaceAllUsesWith(arg);
    Shape shape = TypeToShape(load.getType());
    auto attr = mlir::GetLayoutFromMlirHlo(load);
    if (attr) {
      std::vector<int64> minor_to_major;
      absl::c_transform(
          attr, std::back_inserter(minor_to_major),
          std::function<int64(const llvm::APInt&)>(&llvm::APInt::getZExtValue));
      *shape.mutable_layout() = LayoutUtil::MakeLayout(minor_to_major);
    } else {
      *shape.mutable_layout() =
          LayoutUtil::MakeDescendingLayout(load.getType().getShape().size());
    }
    operand_shapes->push_back(std::move(shape));
    load.erase();
  }

  std::vector<mlir::Value> returned_values;
  for (auto store : stores) {
    returned_values.push_back(store.tensor());
    store.erase();
  }

  region->back().back().erase();
  auto b = mlir::OpBuilder::atBlockEnd(&region->back());
  auto loc = returned_values[0].getLoc();
  b.create<mlir::mhlo::ReturnOp>(loc, returned_values);
  return Status::OK();
}

StatusOr<MlirEmitterInput> IrEmitterUnnested::GetMlirEmitterInput(
    HloInstruction* hlo) {
  MlirEmitterInput input;
  TF_ASSIGN_OR_RETURN(input.op, lhlo_scratch_emitter_.EmitOp(hlo));
  input.thunk_info = GetThunkInfo(hlo);
  if (hlo->shape().IsTuple()) {
    const auto& buffer_assignment = ir_emitter_context_->buffer_assignment();
    auto& slice = input.extra_slice.emplace();
    TF_ASSIGN_OR_RETURN(slice.buffer_slice,
                        buffer_assignment.GetUniqueSlice(hlo, {}));
    slice.written = true;
    slice.shape = hlo->shape();
  }
  return input;
}

// TODO(timshen): update the comment once the HandleFusion code path deleted.
//
// This is migrated from IrEmitter::HandleFusion() with IrEmitterUnnested as the
// subclass. The logic is de-virtualized and less scattered.
Status IrEmitterUnnested::EmitLoopFusionFromMlir(MlirEmitterInput input,
                                                 const Shape& output_shape,
                                                 int unroll_factor) {
  auto fusion = mlir::cast<mlir::lmhlo::FusionOp>(input.op);
  MlirEmitterContext context;
  context.SetOperation(fusion);

  std::vector<llvm_ir::IrArray> ir_arrays;
  Thunk* kernel_thunk;
  {
    TF_ASSIGN_OR_RETURN(std::unique_ptr<KernelThunk> kernel_thunk_ptr,
                        BuildKernelThunkForMlir(fusion, input.thunk_info,
                                                input.extra_slice, &ir_arrays));
    kernel_thunk = kernel_thunk_ptr.get();
    thunk_sequence_.emplace_back(std::move(kernel_thunk_ptr));
  }

  auto operand_arrays =
      absl::MakeSpan(ir_arrays).subspan(0, context.operand_shapes.size());
  auto output_element_arrays = absl::MakeSpan(ir_arrays).subspan(
      context.operand_shapes.size(), context.output_shapes.size());
  const llvm_ir::IrArray* tuple_output_array = nullptr;
  if (ir_arrays.size() ==
      context.operand_shapes.size() + context.output_shapes.size() + 1) {
    tuple_output_array = &ir_arrays[context.operand_shapes.size() +
                                    context.output_shapes.size()];
  }

  TF_ASSIGN_OR_RETURN(const HloComputation* fused_computation,
                      GetOrCreateSubComputationFromRegion(&fusion.region(),
                                                          /*is_fusion=*/true));

  GpuElementalIrEmitter elemental_emitter(hlo_module_config_, module_, &b_,
                                          GetNestedComputer());
  FusedIrEmitter fused_emitter(&elemental_emitter);

  for (int i = 0; i < context.operand_shapes.size(); i++) {
    auto* builder = &b_;
    auto ir_array = operand_arrays[i];
    fused_emitter.BindGenerator(
        fused_computation->parameter_instruction(i),
        [builder, ir_array](llvm_ir::IrArray::Index index) {
          return ir_array.EmitReadArrayElement(index, builder);
        });
  }
  TF_ASSIGN_OR_RETURN(
      auto element_generator,
      fused_emitter.GetGenerator(fused_computation->root_instruction()));

  Shape element_shape = context.output_shapes[0];
  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      element_shape, ir_emitter_context_->gpu_device_info(), unroll_factor);
  UpdateLaunchDimensions(launch_dimensions, kernel_thunk,
                         ir_emitter_context_->llvm_module());
  llvm::Type* index_type = GetIndexTypeForKernelFromMlir(
      fusion, launch_dimensions.launch_bound(), &b_);

  if (context.output_shapes.size() > 1) {
    // Emit the tuple pointers in one thread.  We could do this at any point in
    // the kernel, but we do it at the beginning in the hopes of reducing
    // register pressure, since we touch threadIdx.x and blockIdx.x at the
    // beginning of the kernel *anyway*.
    KernelSupportLibrary{&b_}.If("emit_mof_tuple", IsBlock0Thread0(&b_), [&] {
      llvm_ir::EmitTuple(*tuple_output_array, output_element_arrays, &b_);
    });
    // For multioutput fusion, we need to emit each operand and the root.
    TF_RETURN_IF_ERROR(
        ParallelLoopEmitter(element_generator, output_element_arrays,
                            launch_dimensions, &b_, unroll_factor)
            .EmitLoop(context.name, index_type));
  } else {
    TF_RETURN_IF_ERROR(
        ParallelLoopEmitter(element_generator, output_element_arrays[0],
                            launch_dimensions, &b_, unroll_factor)
            .EmitLoop(context.name, index_type));
  }

  b_.SetInsertPoint(b_.GetInsertBlock()->getTerminator());
  return Status::OK();
}

Status IrEmitterUnnested::HandleFusion(HloInstruction* fusion) {
  HloInstruction* root = fusion->fused_expression_root();
  if (fusion->IsInputFusion()) {
    switch (root->opcode()) {
      case HloOpcode::kScatter: {
        std::vector<std::unique_ptr<Thunk>> thunks;
        // The initialization from 'operand' is using different loop bounds, so
        // emit it in a separate kernel. Treat it like a loop fusion, writing to
        // the output buffer.
        {
          thunks.push_back(
              BuildKernelThunk(fusion, /*implements_whole_instruction=*/false));
          GpuElementalIrEmitter operand_elemental_emitter(
              hlo_module_config_, ir_emitter_context_->llvm_module(), &b_,
              GetNestedComputer());
          FusedIrEmitter operand_fused_emitter(&operand_elemental_emitter);
          BindFusionArguments(fusion, &operand_fused_emitter);
          TF_ASSIGN_OR_RETURN(
              auto generator,
              operand_fused_emitter.GetGenerator(root->operand(0)));

          TF_RETURN_IF_ERROR(EmitTargetElementLoopInThunk(
              *fusion, generator,
              static_cast<KernelThunk*>(thunks.back().get()),
              ComputeMaxUnrollFactor(fusion)));
        }

        // Now build the actual scatter, reading and writing to the freshly
        // filled output buffer.
        {
          thunks.push_back(
              BuildKernelThunk(fusion,
                               /*implements_whole_instruction=*/false));
          // Spin up a new fused emitter for the scatter kernel and emit it.
          GpuElementalIrEmitter scatter_elemental_emitter(
              hlo_module_config_, ir_emitter_context_->llvm_module(), &b_,
              GetNestedComputer());
          FusedIrEmitter scatter_fused_emitter(&scatter_elemental_emitter);
          BindFusionArguments(fusion, &scatter_fused_emitter);
          CHECK_EQ(root->parent()->FusionInstruction(), fusion);

          TF_ASSIGN_OR_RETURN(
              const auto dim_numbers,
              lhlo_scratch_emitter_.GetScatterDimensionNumbers(root));

          ScatterDescriptor desc;
          desc.name = IrName(root);
          desc.operand_shape = root->operand(0)->shape();
          desc.scatter_indices_shape = root->operand(1)->shape();
          desc.updates_shape = root->operand(2)->shape();
          desc.dim_numbers = dim_numbers;
          desc.unique_indices = root->unique_indices();
          desc.update_computation = root->called_computations()[0];
          desc.output = GetIrArray(*fusion, *fusion);
          TF_ASSIGN_OR_RETURN(
              desc.scatter_indices_gen,
              scatter_fused_emitter.GetGenerator(root->operand(1)));
          TF_ASSIGN_OR_RETURN(
              desc.updates_gen,
              scatter_fused_emitter.GetGenerator(root->operand(2)));
          desc.get_index_type = [&](int64 launch_size) {
            return GetIndexTypeForKernel(root, launch_size, &b_);
          };

          TF_RETURN_IF_ERROR(EmitScatter(desc, thunks.back().get()));
        }
        AddThunkToThunkSequence(absl::make_unique<SequentialThunk>(
            GetThunkInfo(fusion), std::move(thunks)));
        return Status::OK();
      }
      // In the case of root tuple, it can be either reduce or slice input
      // fusion.
      case HloOpcode::kTuple: {
        if (IsInputFusibleSlices(*fusion)) {
          return EmitInputFusibleNonStridedSlices(fusion);
        }

        CHECK_GE(root->operand_count(), 1);
        return EmitReductionFromOrToContiguousDimensions(fusion,
                                                         root->operands());
      }
      case HloOpcode::kReduce: {
        // HandleFusion specializes reduction from a multi-dimensional array to
        // a 1D array. The specialized version requires a initializer thunk that
        // initializes the output array to the initial value of the reduce.
        if (root->shape().IsTuple()) {
          // TODO(b/129089333): Support tiled vectorized variadic reduce.
          return Unimplemented(
              "Vectorized variadic reduce is not supported on GPU");
        }
        return EmitReductionFromOrToContiguousDimensions(fusion, {root});
      }
      case HloOpcode::kSlice: {
        return EmitInputFusibleNonStridedSlices(fusion);
      }
      default:
        LOG(FATAL) << "Bad opcode for input fusion: "
                   << fusion->fused_expression_root()->opcode();
    }
  } else if (llvm_ir::CanEmitFusedDynamicUpdateSliceInPlace(
                 fusion, ir_emitter_context_->buffer_assignment())) {
    // Fusion node with dynamic-update-slice as the root where the op's input
    // (i.e. array to update) shares the same slice as its output.  In this case
    // we have a special algorithm that modifies the output in place without
    // touching the un-updated elements.

    // Set up kernel thunk and fused ir emitter.
    std::unique_ptr<KernelThunk> fusion_thunk =
        BuildKernelThunk(fusion, /*implements_whole_instruction=*/true);
    GpuElementalIrEmitter elemental_emitter(hlo_module_config_,
                                            ir_emitter_context_->llvm_module(),
                                            &b_, GetNestedComputer());

    // Shape of the dynamic-update-slice's "update" operand.
    Shape update_shape = root->operand(1)->shape();

    // Array to write into.  Because this is an in-place operation, this is the
    // same as operand 0's array.
    IrArray output_array = GetIrArray(*fusion, *fusion);

    LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
        update_shape, ir_emitter_context_->gpu_device_info());
    UpdateLaunchDimensions(launch_dimensions, fusion_thunk.get(),
                           ir_emitter_context_->llvm_module());
    AddThunkToThunkSequence(std::move(fusion_thunk));

    FusedIrEmitter fused_emitter(&elemental_emitter);
    BindFusionArguments(fusion, &fused_emitter);

    return llvm_ir::EmitParallelFusedDynamicUpdateSliceInPlace(
        fusion, output_array, &fused_emitter, launch_dimensions, &b_);
  }

  CHECK_EQ(fusion->fusion_kind(), HloInstruction::FusionKind::kLoop)
      << ": " << fusion->ToString();

  TF_ASSIGN_OR_RETURN(auto input, GetMlirEmitterInput(fusion));
  TF_ASSIGN_OR_RETURN(const bool matched_021,
                      CheckAndEmitHloWithTile021(input));
  if (matched_021) {
    return Status::OK();
  }

  int unroll_factor = 1;
  if (!MayPreventVectorization(*fusion)) {
    unroll_factor = ComputeMaxUnrollFactor(fusion);
  }

  return EmitLoopFusionFromMlir(input, fusion->shape(), unroll_factor);
}

Status IrEmitterUnnested::HandleCopy(HloInstruction* copy) {
  TF_ASSIGN_OR_RETURN(auto input, GetMlirEmitterInput(copy));
  return EmitCopyForMlir(input);
}

Status IrEmitterUnnested::EmitCopyForMlir(MlirEmitterInput input) {
  auto copy = mlir::cast<mlir::lmhlo::CopyOp>(input.op);
  auto operand_shape = TypeToShape(copy.operand().getType());
  auto output_shape = TypeToShape(copy.output().getType());

  CHECK(ShapeUtil::Compatible(operand_shape, output_shape));
  absl::Span<const BufferAllocation> allocations(
      ir_emitter_context_->buffer_assignment().Allocations());

  auto maybe_slice = GetAllocationSliceForMlir(copy.operand(), allocations);
  if (LayoutUtil::Equal(operand_shape.layout(), output_shape.layout()) &&
      maybe_slice.ok()) {
    // Copy the operand into the output if it's not the same buffer already.
    auto operand_buffer = *maybe_slice;
    auto destination_buffer =
        *GetAllocationSliceForMlir(copy.output(), allocations);
    if (operand_buffer != destination_buffer) {
      AddThunkToThunkSequence(absl::make_unique<DeviceToDeviceCopyThunk>(
          input.thunk_info,
          /*source_address=*/operand_buffer,
          /*destination_buffer=*/destination_buffer,
          /*mem_size=*/
          ByteSizeOf(operand_shape)));
    }
    return Status::OK();
  }
  TF_ASSIGN_OR_RETURN(bool matched_021, CheckAndEmitHloWithTile021(input));
  if (matched_021) {
    return Status::OK();
  }

  return DefaultActionForMlir(input);
}

Status IrEmitterUnnested::EmitExtraOutputsForReduce(
    const HloInstruction* unnested_hlo, const IrArray::Index& index,
    bool use_linear_index,
    absl::Span<const std::pair<llvm_ir::ElementGenerator, ShapeIndex>>
        extra_output_gens) {
  // Compute all extra output values before writing them. This avoids
  // overwriting aliased input/output buffers before all reads occured.
  absl::InlinedVector<llvm::Value*, 8> extra_output_ir_values;
  for (int i = 0; i < extra_output_gens.size(); ++i) {
    TF_ASSIGN_OR_RETURN(llvm::Value* const extra_output_ir_value,
                        extra_output_gens[i].first(index));
    extra_output_ir_values.push_back(extra_output_ir_value);
  }
  for (int i = 0; i < extra_output_gens.size(); ++i) {
    GetIrArray(*unnested_hlo, *unnested_hlo, extra_output_gens[i].second)
        .EmitWriteArrayElement(index, extra_output_ir_values[i], &b_,
                               use_linear_index);
  }
  return Status::OK();
}

Status IrEmitterUnnested::HandleReduce(HloInstruction* reduce) {
  if (IsReductionFromOrToContiguousDimensions(*reduce) &&
      reduce->shape().IsArray()) {
    return EmitReductionFromOrToContiguousDimensions(reduce, {reduce});
  }

  return IrEmitter::HandleReduce(reduce);
}

Status IrEmitterUnnested::HandleTuple(HloInstruction* tuple) {
  // For the root node of the entry computation we can elide writing the tuple
  // buffer. We can always figure out the contents of the tuples from buffer
  // assignment because we insert copies to ensure non-ambiguous output buffers.
  // GpuExecutable never reads the tuple buffer.
  if (tuple ==
      tuple->parent()->parent()->entry_computation()->root_instruction()) {
    return Status::OK();
  }
  bool all_tuple_elements_have_buffer =
      absl::c_all_of(tuple->operands(), [&](HloInstruction* tuple_element) {
        return ir_emitter_context_->buffer_assignment()
            .GetUniqueTopLevelSlice(tuple_element)
            .ok();
      });
  // TODO(b/111689850): This logic isn't quite correct.
  //
  // Tuples (especially tuples that are the final result of a computation) can
  // be so huge that if we were to emit a kernel that took each tuple element as
  // a parameter, we would exceed the max allowable number of parameters to a
  // GPU kernel, b/31336476. As an optimization, if all tuple elements have a
  // buffer, we collect their buffer addresses in a host array, and then copy
  // that array to the tuple's buffer.
  //
  // Some tuple elements might not have an unambiguous buffer (like the result
  // of a select-tuple). In that case, we fall back to emitting kernels which
  // have access to their buffer addresses in code.
  if (all_tuple_elements_have_buffer) {
    std::vector<BufferAllocation::Slice> tuple_element_buffers;
    for (const HloInstruction* tuple_element : tuple->operands()) {
      tuple_element_buffers.push_back(GetAllocationSlice(*tuple_element));
    }
    AddThunkToThunkSequence(absl::make_unique<TupleThunk>(
        GetThunkInfo(tuple), tuple_element_buffers,
        GetAllocationSlice(*tuple)));
    return Status::OK();
  }
  AddThunkToThunkSequence(
      BuildKernelThunk(tuple, /*implements_whole_instruction=*/true));
  return IrEmitter::HandleTuple(tuple);
}

Status IrEmitterUnnested::HandleGetTupleElement(HloInstruction*) {
  // GetTupleElement IR is emitted in the IR context of the user instruction,
  // and so we do not build a kernel for GetTupleElement instructions.
  return Status::OK();
}

Status IrEmitterUnnested::HandleSelectAndScatter(
    HloInstruction* select_and_scatter) {
  const Window& window = select_and_scatter->window();
  const auto* operand = select_and_scatter->operand(0);
  const auto* source = select_and_scatter->operand(1);
  const int64 rank = operand->shape().rank();
  CHECK_EQ(rank, source->shape().rank());
  CHECK_EQ(rank, window.dimensions_size());

  // TODO(b/31410564): Implement dilation rate for select-and-scatter.
  if (window_util::HasDilation(window)) {
    return Unimplemented(
        "Dilation for SelectAndScatter not implemented on GPU.");
  }

  TF_ASSIGN_OR_RETURN(std::unique_ptr<Thunk> initializer_thunk,
                      BuildInitializerThunk(select_and_scatter));

  TF_ASSIGN_OR_RETURN(auto input, GetMlirEmitterInput(select_and_scatter));
  return EmitSelectAndScatterFromMlir(input, std::move(initializer_thunk));
}

Status IrEmitterUnnested::EmitSelectAndScatterFromMlir(
    MlirEmitterInput mlir_input, std::unique_ptr<Thunk>&& initializer_thunk) {
  auto select_and_scatter_op =
      ::mlir::cast<::mlir::lmhlo::SelectAndScatterOp>(mlir_input.op);

  std::string name = mlir::GetNameFromLoc(select_and_scatter_op.getLoc());

  std::vector<std::unique_ptr<Thunk>> thunks;
  thunks.push_back(std::move(initializer_thunk));

  absl::Span<const BufferAllocation> allocations(
      ir_emitter_context_->buffer_assignment().Allocations());

  std::vector<llvm_ir::IrArray> ir_arrays;
  thunks.emplace_back();
  // Init value is not needed in IR emission.
  TF_ASSIGN_OR_RETURN(
      thunks.back(),
      BuildKernelThunkForMlir(
          select_and_scatter_op,
          {select_and_scatter_op.operand(), select_and_scatter_op.source(),
           select_and_scatter_op.out()},
          Thunk::ThunkInfo(), mlir_input.extra_slice, &ir_arrays));

  CHECK_EQ(ir_arrays.size(), 3);
  const IrArray& operand_array = ir_arrays[0];
  const IrArray& source_array = ir_arrays[1];
  const IrArray& out_array = ir_arrays[2];

  auto select_and_scatter_thunk = absl::make_unique<SequentialThunk>(
      mlir_input.thunk_info, std::move(thunks));

  const Shape source_shape =
      TypeToShape(select_and_scatter_op.source().getType());
  const Shape operand_shape =
      TypeToShape(select_and_scatter_op.operand().getType());
  const int64 rank = operand_shape.rank();

  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      source_shape, ir_emitter_context_->gpu_device_info());
  llvm::Type* index_type = GetIndexTypeForKernelFromMlir(
      select_and_scatter_op, launch_dimensions.launch_bound(), &b_);
  auto index_typed_constant = [&](uint64 c) -> llvm::Constant* {
    return llvm::ConstantInt::get(index_type, c);
  };

  // kSelectAndScatter is implemented as two kernel launches: the first launch
  // initializes the output array to the given initial value,
  // and the second accumulates the "source" matrix to the
  // selected elements in the output array. The first launch is already
  // implemented by the initializer thunk generated earlier, so this function
  // only needs to take care of the select-and-scatter part.
  //
  // Pseudo code for select-and-scatter:
  //
  // for (coordinates S in the source):  # This loop is parallel.
  //   initialized_flag = false
  //   for (coordinates W in the window):
  //     I = S * stride + W - pad_low
  //     if I within bounds of operand:
  //       if !(initialized_flag and select(selected_value, operand(I))):
  //         selected_value = operand(I)
  //         selected_index = I
  //         initialized_flag = true
  //   output(selected_index) = scatter(output(selected_index), source(S))
  auto loop_body_emitter = [&](const IrArray::Index& source_index) -> Status {
    // Allocate space to keep the currently selected value, its index, and a
    // boolean flag if the value is initialized. The initialized_flag is set
    // false.
    llvm::Value* selected_value_address = llvm_ir::EmitAllocaAtFunctionEntry(
        llvm_ir::PrimitiveTypeToIrType(operand_shape.element_type(),
                                       ir_emitter_context_->llvm_module()),
        "selected_value_address", &b_);

    llvm::Value* selected_index_address =
        llvm_ir::EmitAllocaAtFunctionEntryWithCount(
            index_type, index_typed_constant(rank), "selected_index_address",
            &b_);

    llvm::Value* initialized_flag_address = llvm_ir::EmitAllocaAtFunctionEntry(
        b_.getInt1Ty(), "initialized_flag_address", &b_);
    Store(b_.getInt1(false), initialized_flag_address);

    // Create the inner loop to iterate over the window.
    llvm_ir::ForLoopNest window_loops(absl::StrCat(name, "inner"), &b_,
                                      index_type);

    DimensionVector window_size;
    ::mlir::DenseIntElementsAttr window_dimensions =
        select_and_scatter_op.window_dimensions().getValue();
    for (const auto& dim : window_dimensions) {
      window_size.push_back(dim.getSExtValue());
      CHECK_GT(dim.getSExtValue(), 0);
    }

    const IrArray::Index window_index = window_loops.AddLoopsForShape(
        ShapeUtil::MakeShape(operand_shape.element_type(), window_size),
        "window");
    llvm_ir::SetToFirstInsertPoint(window_loops.GetInnerLoopBodyBasicBlock(),
                                   &b_);

    // Compute the operand index to visit and evaluate the condition whether the
    // operand index is within the bounds. The unsigned comparison includes
    // checking whether the operand index >= 0.
    std::vector<llvm::Value*> operand_multi_index(source_index.size());
    llvm::Value* in_bounds_condition = b_.getInt1(true);

    auto strides = *select_and_scatter_op.window_strides();
    auto paddings = *select_and_scatter_op.padding();

    for (auto stride_and_padding :
         llvm::enumerate(llvm::zip(strides, paddings))) {
      const int i = stride_and_padding.index();
      int64 stride = std::get<0>(stride_and_padding.value()).getSExtValue();
      int64 padding = std::get<1>(stride_and_padding.value()).getSExtValue();

      llvm::Value* strided_index =
          NSWMul(source_index[i], index_typed_constant(stride));
      operand_multi_index[i] = NSWSub(NSWAdd(strided_index, window_index[i]),
                                      index_typed_constant(padding));
      llvm::Value* index_condition = ICmpULT(
          operand_multi_index[i],
          index_typed_constant(ShapeUtil::GetDimension(operand_shape, i)));
      in_bounds_condition = And(in_bounds_condition, index_condition);
    }

    // Only need to do something if the operand index is within the bounds.
    // First check if the initialized_flag is set.
    llvm_ir::LlvmIfData if_in_bounds =
        llvm_ir::EmitIfThenElse(in_bounds_condition, "in-bounds", &b_);
    llvm_ir::SetToFirstInsertPoint(if_in_bounds.true_block, &b_);
    llvm_ir::LlvmIfData if_initialized = llvm_ir::EmitIfThenElse(
        Load(initialized_flag_address), "initialized", &b_);

    // If the initialized_flag is false, initialize the selected value and index
    // with the currently visiting operand.
    llvm_ir::SetToFirstInsertPoint(if_initialized.false_block, &b_);
    const auto save_operand_index = [&](const IrArray::Index& operand_index) {
      for (int64 i = 0; i < rank; ++i) {
        llvm::Value* selected_index_address_slot =
            InBoundsGEP(selected_index_address, {b_.getInt32(i)});
        Store(operand_index[i], selected_index_address_slot);
      }
    };
    IrArray::Index operand_index(operand_multi_index, operand_shape,
                                 index_type);
    llvm::Value* operand_data =
        operand_array.EmitReadArrayElement(operand_index, &b_);
    Store(operand_data, selected_value_address);
    save_operand_index(operand_index);
    Store(b_.getInt1(true), initialized_flag_address);

    // If the initialized_flag is true, call the `select` function to
    // potentially update the selected value and index with the currently
    // visiting operand.
    llvm_ir::SetToFirstInsertPoint(if_initialized.true_block, &b_);
    llvm::Value* operand_address =
        operand_array.EmitArrayElementAddress(operand_index, &b_);
    llvm::Value* select_return_buffer = llvm_ir::EmitAllocaAtFunctionEntry(
        llvm_ir::PrimitiveTypeToIrType(PRED,
                                       ir_emitter_context_->llvm_module()),
        "select_return_buffer", &b_);

    TF_ASSIGN_OR_RETURN(
        const HloComputation* select_computation,
        GetOrCreateSubComputationFromRegion(&select_and_scatter_op.select(),
                                            /*is_fusion=*/false));

    TF_RETURN_IF_ERROR(EmitCallToNestedComputation(
        *select_computation, {selected_value_address, operand_address},
        select_return_buffer));
    llvm::Value* result = Load(select_return_buffer);

    // If the 'select' function returns false, update the selected value and the
    // index to the currently visiting operand.
    llvm::Value* cond = ICmpNE(
        result,
        llvm::ConstantInt::get(llvm_ir::PrimitiveTypeToIrType(
                                   PRED, ir_emitter_context_->llvm_module()),
                               0),
        "boolean_predicate");
    llvm_ir::LlvmIfData if_select_lhs =
        llvm_ir::EmitIfThenElse(cond, "if-select-lhs", &b_);
    llvm_ir::SetToFirstInsertPoint(if_select_lhs.false_block, &b_);
    Store(Load(operand_address), selected_value_address);
    save_operand_index(operand_index);

    // After iterating over the window elements, scatter the source element to
    // the selected index of the output. The value we store at the output
    // location is computed by calling the `scatter` function with the source
    // value and the current output value.
    llvm_ir::SetToFirstInsertPoint(window_loops.GetOuterLoopExitBasicBlock(),
                                   &b_);
    std::vector<llvm::Value*> selected_multi_index;
    for (int64 i = 0; i < rank; ++i) {
      llvm::Value* selected_index_address_slot =
          InBoundsGEP(selected_index_address, {b_.getInt32(i)});
      selected_multi_index.push_back(Load(selected_index_address_slot));
    }
    const Shape output_shape =
        TypeToShape(select_and_scatter_op.out().getType());
    llvm::Value* source_value_address =
        source_array.EmitArrayElementAddress(source_index, &b_);
    IrArray::Index selected_index(selected_multi_index, output_shape,
                                  operand_index.GetType());
    llvm::Value* output_value_address =
        out_array.EmitArrayElementAddress(selected_index, &b_);

    TF_ASSIGN_OR_RETURN(
        const HloComputation* scatter_computation,
        GetOrCreateSubComputationFromRegion(&select_and_scatter_op.scatter(),
                                            /*is_fusion=*/false));

    return EmitAtomicOperationForNestedComputation(
        *scatter_computation, output_value_address, source_value_address);
  };

  UpdateLaunchDimensions(
      launch_dimensions,
      // IrEmitterUnnested implements kSelectAndScatter as a SequentialThunk
      // consisting of two thunks, an initializer KernelThunk that initializes
      // the output and another KernelThunk that accumulates the scattered
      // elements.
      select_and_scatter_thunk->thunks().back().get(),
      ir_emitter_context_->llvm_module());
  AddThunkToThunkSequence(std::move(select_and_scatter_thunk));
  return ParallelLoopEmitter(loop_body_emitter, source_shape, launch_dimensions,
                             &b_)
      .EmitLoop(name, index_type);
}

Status IrEmitterUnnested::HandleWhile(HloInstruction* xla_while) {
  HloComputation* condition = xla_while->while_condition();
  TF_RET_CHECK(ShapeUtil::IsScalar(condition->root_instruction()->shape()) &&
               condition->root_instruction()->shape().element_type() == PRED)
      << "While condition computation must return bool";
  // Build ForThunk for conformant while loops, otherwise build WhileThunk.
  auto config = xla_while->backend_config<WhileLoopBackendConfig>();
  if (config.ok() && config.ValueOrDie().has_known_trip_count()) {
    TF_ASSIGN_OR_RETURN(
        auto thunk,
        BuildForThunk(xla_while, config.ValueOrDie().known_trip_count().n()));
    AddThunkToThunkSequence(std::move(thunk));
  } else {
    TF_ASSIGN_OR_RETURN(auto thunk, BuildWhileThunk(xla_while));
    AddThunkToThunkSequence(std::move(thunk));
  }
  return Status::OK();
}

Status IrEmitterUnnested::HandleRng(HloInstruction* rng) {
  return Unimplemented("Rng should be expanded for GPU.");
}

Status IrEmitterUnnested::HandleRngGetAndUpdateState(
    HloInstruction* rng_state) {
  // Emit a kernel to increment the global state for Philox RNG algorithm.
  AddThunkToThunkSequence(
      BuildKernelThunk(rng_state, /*implements_whole_instruction=*/true));

  llvm::Value* old_state = llvm_ir::RngGetAndUpdateState(
      Cast<HloRngGetAndUpdateStateInstruction>(rng_state)->delta(), module_,
      &b_);

  llvm::Value* output_address =
      GetIrArray(*rng_state, *rng_state)
          .EmitArrayElementAddress(
              llvm_ir::IrArray::Index(
                  /*linear=*/b_.getInt64(0), rng_state->shape(), &b_),
              &b_, "rng_state_address");
  output_address = BitCast(
      output_address, llvm::PointerType::get(
                          old_state->getType(),
                          output_address->getType()->getPointerAddressSpace()));
  Store(old_state, output_address);

  return Status::OK();
}

Status IrEmitterUnnested::HandleScatter(HloInstruction* scatter) {
  TF_ASSIGN_OR_RETURN(auto input, GetMlirEmitterInput(scatter));
  return EmitScatterFromMlir(input);
}

Status IrEmitterUnnested::EmitScatterFromMlir(MlirEmitterInput mlir_input) {
  std::vector<std::unique_ptr<Thunk>> thunks;

  absl::Span<const BufferAllocation> allocations(
      ir_emitter_context_->buffer_assignment().Allocations());

  auto scatter_op = ::mlir::cast<::mlir::lmhlo::ScatterOp>(mlir_input.op);

  TF_ASSIGN_OR_RETURN(
      auto operand_buffer,
      GetAllocationSliceForMlir(scatter_op.operand(), allocations));
  TF_ASSIGN_OR_RETURN(
      auto output_buffer,
      GetAllocationSliceForMlir(scatter_op.output(), allocations));

  // Copy the operand into the output if it's not the same buffer already.
  if (operand_buffer != output_buffer) {
    thunks.push_back(absl::make_unique<DeviceToDeviceCopyThunk>(
        Thunk::ThunkInfo(),
        /*source_address=*/operand_buffer,
        /*destination_buffer=*/output_buffer,
        /*mem_size=*/
        ShapeUtil::ByteSizeOf(TypeToShape(scatter_op.output().getType()))));
  }

  // Create kernel thunk for all operands except the first one (`operand`). The
  // code generated for scatter below assumes that the input operand is already
  // copied into the output, so does not use it in codegen.
  std::vector<llvm_ir::IrArray> ir_arrays;
  thunks.emplace_back();
  TF_ASSIGN_OR_RETURN(
      thunks.back(),
      BuildKernelThunkForMlir(scatter_op, scatter_op.getOperands().drop_front(),
                              mlir_input.thunk_info, mlir_input.extra_slice,
                              &ir_arrays));

  CHECK_EQ(ir_arrays.size(), 3);
  const IrArray& scatter_indices = ir_arrays[0];
  const IrArray& updates = ir_arrays[1];
  const IrArray& output = ir_arrays[2];

  auto get_index_type = [&](int64 launch_size) {
    return GetIndexTypeForKernelFromMlir(scatter_op, launch_size, &b_);
  };

  TF_RETURN_IF_ERROR(EmitScatter(
      thunks.back().get(), scatter_op, output,
      /*scatter_indices_gen=*/
      [&](const IrArray::Index& index) {
        return scatter_indices.EmitReadArrayElement(index, &b_,
                                                    "scatter_index");
      },
      /*updates_gen=*/
      [&](const IrArray::Index& index) {
        return updates.EmitReadArrayElement(index, &b_, "update");
      },
      /* get_index_type=*/
      get_index_type));

  // Elide the sequential thunk if there's no copy.
  if (thunks.size() == 1) {
    AddThunkToThunkSequence(std::move(thunks[0]));
  } else {
    AddThunkToThunkSequence(absl::make_unique<SequentialThunk>(
        mlir_input.thunk_info, std::move(thunks)));
  }

  return Status::OK();
}

Status IrEmitterUnnested::EmitScatter(
    Thunk* thunk, mlir::lmhlo::ScatterOp scatter,
    const llvm_ir::IrArray& output,
    const llvm_ir::ElementGenerator& scatter_indices_gen,
    const llvm_ir::ElementGenerator& updates_gen,
    std::function<llvm::Type*(int64)> get_index_type) {
  const Shape operand_shape = TypeToShape(scatter.operand().getType());
  CHECK(
      ShapeUtil::Equal(TypeToShape(scatter.output().getType()), operand_shape));

  TF_ASSIGN_OR_RETURN(
      const HloComputation* update_computation,
      GetOrCreateSubComputationFromRegion(&scatter.update_computation(),
                                          /*is_fusion=*/false));

  ScatterDescriptor desc;
  desc.name = mlir::GetNameFromLoc(scatter.getLoc());
  desc.operand_shape = operand_shape;
  desc.scatter_indices_shape = TypeToShape(scatter.scatter_indices().getType());
  desc.updates_shape = TypeToShape(scatter.updates().getType());
  desc.dim_numbers = scatter.scatter_dimension_numbers();
  desc.unique_indices = scatter.unique_indices();
  desc.update_computation = update_computation;
  desc.output = output;
  desc.scatter_indices_gen = scatter_indices_gen;
  desc.updates_gen = updates_gen;
  desc.get_index_type = get_index_type;
  return EmitScatter(desc, thunk);
}

Status IrEmitterUnnested::EmitScatter(const ScatterDescriptor& desc,
                                      Thunk* thunk) {
  auto loop_body_emitter = [&](const IrArray::Index& index) -> Status {
    std::vector<llvm::Value*> raw_window_multidim;
    std::vector<llvm::Value*> input_scatter_multidim;
    std::vector<int64> raw_window_bounds;

    // Partition the index into window indices and scatter indices.
    for (int64 i = 0, e = index.size(); i != e; ++i) {
      // For window indices also remember the window size, this comes in handy
      // later.
      if (BinarySearchDenseElementsAttr(desc.dim_numbers.update_window_dims(),
                                        i)) {
        raw_window_multidim.push_back(index[i]);
        raw_window_bounds.push_back(desc.updates_shape.dimensions(i));
      } else {
        input_scatter_multidim.push_back(index[i]);
      }
    }
    DCHECK_EQ(raw_window_multidim.size(),
              desc.dim_numbers.update_window_dims().size());

    // Apply inserted_window_dims to the window dimensions.
    int64 raw_window_multidim_idx = 0;
    std::vector<llvm::Value*> input_window_multidim;
    std::vector<int64> input_window_bounds;

    for (int64 i = 0, e = desc.operand_shape.rank(); i != e; ++i) {
      if (BinarySearchDenseElementsAttr(desc.dim_numbers.inserted_window_dims(),
                                        i)) {
        input_window_bounds.push_back(1);  // Trivial dimension.
        input_window_multidim.push_back(index.GetConstantWithIndexType(0));
      } else {
        input_window_bounds.push_back(
            raw_window_bounds[raw_window_multidim_idx]);
        input_window_multidim.push_back(
            raw_window_multidim[raw_window_multidim_idx]);
        ++raw_window_multidim_idx;
      }
    }
    DCHECK_EQ(input_window_multidim.size(), desc.operand_shape.rank());

    // Insert a 1 dimension at the end if index_vector_dim requests one.
    Shape scatter_indices_shape_fixed = desc.scatter_indices_shape;
    if (desc.dim_numbers.index_vector_dim().getInt() ==
        desc.scatter_indices_shape.rank()) {
      scatter_indices_shape_fixed.add_dimensions(1);
      scatter_indices_shape_fixed.mutable_layout()->add_minor_to_major(
          desc.dim_numbers.index_vector_dim().getInt());
    }

    // Now load the indices corresponding to the current window from
    // scatter_indices.
    std::vector<llvm::Value*> raw_scatter_index_multidim =
        input_scatter_multidim;
    raw_scatter_index_multidim.insert(
        raw_scatter_index_multidim.begin() +
            desc.dim_numbers.index_vector_dim().getInt(),
        nullptr);
    llvm::Value* is_in_bounds = b_.getTrue();
    for (int64 i = 0,
               e = desc.dim_numbers.scatter_dims_to_operand_dims().size();
         i != e; ++i) {
      // Our index is stored along index_vector_dim, insert that into the lookup
      // index into scatter_indices.
      raw_scatter_index_multidim[desc.dim_numbers.index_vector_dim().getInt()] =
          index.GetConstantWithIndexType(i);
      llvm_ir::IrArray::Index raw_scatter_index_index(
          raw_scatter_index_multidim, scatter_indices_shape_fixed,
          index.GetType());

      int64 operand_dim =
          desc.dim_numbers.scatter_dims_to_operand_dims().getValue<int64>(i);
      TF_ASSIGN_OR_RETURN(
          llvm::Value* const loaded_scatter_index,
          desc.scatter_indices_gen(raw_scatter_index_index.SourceIndexOfReshape(
              scatter_indices_shape_fixed, desc.scatter_indices_shape, &b_)));
      // And add the index to our window index. This yields the output index.
      llvm::Value* casted_scatter_index =
          IntCast(loaded_scatter_index, index.GetType(),
                  /*isSigned=*/true);
      llvm::Value* dim_offset =
          Add(input_window_multidim[operand_dim], casted_scatter_index);
      input_window_multidim[operand_dim] = dim_offset;

      // Also do the bounds check now.
      int64 max_index = desc.operand_shape.dimensions(operand_dim) -
                        input_window_bounds[operand_dim] + 1;
      // is_in_bounds = index >= 0 && index < dim_size-window_size+1
      //   --> index u< dim_size-window_size+1
      is_in_bounds =
          And(is_in_bounds, ICmpULT(casted_scatter_index,
                                    index.GetConstantWithIndexType(max_index)));
    }

    llvm_ir::LlvmIfData if_window_in_bounds_data = llvm_ir::EmitIfThenElse(
        is_in_bounds, "scatter.in_bounds", &b_, /*emit_else=*/false);
    llvm_ir::SetToFirstInsertPoint(if_window_in_bounds_data.true_block, &b_);
    // All done, now just read from the calculated input from the window, and do
    // an atomic store to the calculated location in the output.
    llvm_ir::IrArray::Index input_window_index(
        input_window_multidim, desc.output.GetShape(), index.GetType());
    llvm::Value* output_address =
        desc.output.EmitArrayElementAddress(input_window_index, &b_);
    llvm::Value* input_address = llvm_ir::EmitAllocaAtFunctionEntry(
        llvm_ir::PrimitiveTypeToIrType(desc.updates_shape.element_type(),
                                       module_),
        "input_address", &b_);
    TF_ASSIGN_OR_RETURN(llvm::Value* const input_ir_value,
                        desc.updates_gen(index));
    Store(input_ir_value, input_address);

    if (!desc.unique_indices) {
      return EmitAtomicOperationForNestedComputation(
          *desc.update_computation, output_address, input_address);
    } else {
      return EmitCallToNestedComputation(*desc.update_computation,
                                         {output_address, input_address},
                                         output_address);
    }
  };

  // Launch a kernel that reads every element in the updates tensor. We could
  // also do one kernel per window instead if bounds checks turn out to be a
  // bottleneck.
  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      desc.updates_shape, ir_emitter_context_->gpu_device_info());
  UpdateLaunchDimensions(launch_dimensions, thunk,
                         ir_emitter_context_->llvm_module());

  return ParallelLoopEmitter(loop_body_emitter, desc.updates_shape,
                             launch_dimensions, &b_)
      .EmitLoop(desc.name,
                desc.get_index_type(launch_dimensions.launch_bound()));
}

Status IrEmitterUnnested::HandleSelect(HloInstruction* select) {
  return IrEmitter::HandleSelect(select);
}

// This transformation should be migrated off. See b/171334474.
StatusOr<const HloComputation*>
IrEmitterUnnested::GetOrCreateSubComputationFromRegion(mlir::Region* region,
                                                       bool is_fusion) {
  std::unique_ptr<HloModule>& module = scratch_nested_computations_[region];
  if (module == nullptr) {
    std::vector<Shape> operand_shapes;
    if (is_fusion) {
      TF_RETURN_IF_ERROR(ProcessFusionForConversion(region, &operand_shapes));
    }

    xla::XlaComputation xla_computation;
    mlir::MlirToHloConversionOptions options;
    options.propagate_layouts = true;
    TF_RETURN_IF_ERROR(
        ConvertRegionToComputation(region, &xla_computation, options));

    TF_ASSIGN_OR_RETURN(auto program_shape, xla_computation.GetProgramShape());
    TF_ASSIGN_OR_RETURN(
        module, HloModule::CreateFromProto(xla_computation.proto(),
                                           HloModuleConfig(program_shape)));

    // Post-process the generated computation:
    // * Sanitize constant names, so that they can be used as LLVM global
    // symbols.
    // * Propagate layouts for tuple types.
    for (HloComputation* computation : module->computations()) {
      for (HloInstruction* instr : computation->MakeInstructionPostOrder()) {
        if (instr->opcode() == HloOpcode::kConstant) {
          instr->SetAndSanitizeName(llvm_ir::SanitizeConstantName(*instr));
        }
        if (instr->shape().IsTuple()) {
          TF_ASSIGN_OR_RETURN(*instr->mutable_shape(),
                              ShapeInference::InferVariadicOpShape(
                                  instr->opcode(), instr->operands()));
        }
      }
    }
    if (is_fusion) {
      HloComputation* fused_computation = module->entry_computation();
      CHECK_EQ(operand_shapes.size(), fused_computation->num_parameters());
      for (int i = 0; i < fused_computation->num_parameters(); i++) {
        *fused_computation->parameter_instruction(i)
             ->mutable_shape()
             ->mutable_layout() = operand_shapes[i].layout();
      }
    }
  }
  return module->entry_computation();
}

Status IrEmitterUnnested::HandleSort(HloInstruction* sort) {
  MlirEmitterInput result;

  TF_ASSIGN_OR_RETURN(auto sort_op, lhlo_scratch_emitter_.EmitOp(sort));
  result.op = sort_op;
  const auto& buffer_assignment = ir_emitter_context_->buffer_assignment();
  auto& slice = result.extra_slice.emplace();
  TF_ASSIGN_OR_RETURN(slice.buffer_slice,
                      buffer_assignment.GetUniqueSlice(sort, {}));
  slice.written = true;
  slice.shape = sort->shape();

  result.thunk_info = GetThunkInfo(sort);

  return EmitSortFromMlir(result);
}

Status IrEmitterUnnested::EmitSortFromMlir(MlirEmitterInput mlir_input) {
  absl::Span<const BufferAllocation> allocations(
      ir_emitter_context_->buffer_assignment().Allocations());
  auto sort_op = mlir::cast<mlir::lmhlo::SortOp>(mlir_input.op);
  MlirEmitterContext context;
  context.SetOperation(sort_op);

  std::vector<std::unique_ptr<Thunk>> thunks;

  const Shape& keys_shape = context.operand_shapes[0];
  int64 dimension_to_sort = sort_op.dimension();
  for (int64 i = 0; i < context.operand_shapes.size(); ++i) {
    // We assume that the layout of all involved operands and outputs is the
    // same.
    TF_RET_CHECK(LayoutUtil::LayoutsInShapesEqual(keys_shape,
                                                  context.operand_shapes[i]));
    TF_RET_CHECK(
        LayoutUtil::LayoutsInShapesEqual(keys_shape, context.output_shapes[i]));

    // If possible, we share buffers. If that is not possible, we need to copy
    // the values, because the emitter does the sorting in-place.
    TF_ASSIGN_OR_RETURN(
        auto destination_buffer,
        GetAllocationSliceForMlir(sort_op.output()[i], allocations));
    TF_ASSIGN_OR_RETURN(
        auto source_address,
        GetAllocationSliceForMlir(sort_op.operands()[i], allocations));
    if (destination_buffer != source_address) {
      // TODO(b/26783907): Figure out why we never seem to share buffers for
      // key/value sort.
      VLOG(2) << context.name << " requires initial D2D copy for operand " << i;
      thunks.push_back(absl::make_unique<DeviceToDeviceCopyThunk>(
          Thunk::ThunkInfo(),
          /*source_address=*/source_address,
          /*destination_buffer=*/destination_buffer,
          /*mem_size=*/ShapeUtil::ByteSizeOf(context.operand_shapes[i])));
    }
  }

  uint64 dimension_to_sort_bound = keys_shape.dimensions(dimension_to_sort);
  int64 num_stages = tensorflow::Log2Ceiling(dimension_to_sort_bound);
  VLOG(2) << context.name << " requires " << num_stages << " stages.";
  CHECK_GE(1ULL << num_stages, dimension_to_sort_bound);
  CHECK_LT(1ULL << (num_stages - 1), dimension_to_sort_bound);

  // Naive C++ code for the outer loops:
  //
  // for (int64 stage = 0; stage < Log2Ceiling(dimension_to_sort_bound);
  //     ++stage) {
  //   int64 first_xor_mask = (1LL << (stage + 1)) - 1;
  //   SortInPlace(first_xor_mask);
  //   for (int64 mask = stage - 1; mask >= 0; --mask) {
  //     int64 later_xor_mask = 1LL << mask;
  //     SortInPlace(later_xor_mask);
  //   }
  // }
  //
  // This follows the alternative representation of the algorithm described on
  // Wikipedia: https://en.wikipedia.org/wiki/Bitonic_sorter
  //
  // Each mask specifies how to derive from one position in the array the
  // position with which it should be compared (we calculate the xor of the
  // position with the mask).
  // As an optimization, we can move the 'mask' loop to inside the
  // sorting/comparison loop if the comparisons happen within a small block of
  // the array. To make this work, we collect all consecutive masks that are
  // smaller than our chosen power of 2 tile size, and pass them to SortInPlace.
  // Each thread then processes one tile of data.

  const uint64 kTileSize = std::min(2048ULL, 1ULL << num_stages);

  // If we cannot combine several xor masks together, we don't use tiling, so we
  // calculate the standard launch dimensions for the shape. However we only
  // need to iterate through ~half of the dimension to sort (rounded up to the
  // next highest power of 2), because each iteration compares one pair of
  // elements.
  Shape standard_iteration_shape = keys_shape;
  uint64 standard_num_iterations_in_sort_dim = 1ULL << (num_stages - 1);
  standard_iteration_shape.set_dimensions(dimension_to_sort,
                                          standard_num_iterations_in_sort_dim);
  LaunchDimensions standard_launch_dimensions = CalculateLaunchDimensions(
      standard_iteration_shape, ir_emitter_context_->gpu_device_info());

  // Calculate the launch dimensions for the case where we use tiling. We split
  // the dimension that should be sorted into tiles of size 'kTileSize'. This
  // means we first need to round 'dimension_to_sort_bound' up to be a multiple
  // of the tile size.
  int64 rounded_bound = RoundUpToNearest(dimension_to_sort_bound, kTileSize);
  Shape iteration_shape = keys_shape;

  // We iterate through the element pairs that should be compared.
  uint64 num_iterations_in_sort_dim = rounded_bound / 2;
  iteration_shape.set_dimensions(dimension_to_sort, num_iterations_in_sort_dim);
  uint64 num_iterations = ShapeUtil::ElementsIn(iteration_shape);

  // For correctness reasons we need exactly 'kTileSize' / 2 many threads per
  // block. Each thread is responsible for copying exactly two adjacent elements
  // into shared memory, and then does a comparison of two possibly different
  // elements taken from shared memory.
  const uint64 kThreadsPerBlock = kTileSize / 2;

  // Check whether we should use any tiling. We might not be able to use it if
  // we have not enough threads, or not enough shared memory. Also it does not
  // give a speedup if the tile size is < 128.
  int64 total_shared_memory_needed = 0;
  for (int64 i = 0; i < context.operand_shapes.size(); ++i) {
    total_shared_memory_needed +=
        kTileSize * ShapeUtil::ByteSizeOfPrimitiveType(
                        context.operand_shapes[i].element_type());
  }
  bool no_tiling =
      kTileSize < 128 ||
      kThreadsPerBlock >
          ir_emitter_context_->gpu_device_info().threads_per_block_limit ||
      total_shared_memory_needed >
          ir_emitter_context_->gpu_device_info().shared_memory_per_block;
  VLOG(2) << absl::StreamFormat(
      "%s %s use tiling. No tiling if any of the following is true: "
      "kTileSize=%d < 128, "
      "kThreadsPerBlock=%d > threads_per_block_limit=%d, "
      "total_shared_memory_needed=%d > shared_memory_per_block=%d",
      context.name, (no_tiling ? "won't" : "will"), kTileSize, kThreadsPerBlock,
      ir_emitter_context_->gpu_device_info().threads_per_block_limit,
      total_shared_memory_needed,
      ir_emitter_context_->gpu_device_info().shared_memory_per_block);

  uint64 num_blocks = CeilOfRatio(num_iterations, kThreadsPerBlock);
  LaunchDimensions tiled_launch_dimensions(num_blocks, kThreadsPerBlock);
  VLOG(2) << absl::StreamFormat("%s launch dims: %d blocks, %d threads/block",
                                context.name, num_blocks, kThreadsPerBlock);

  std::vector<llvm_ir::IrArray> ir_arrays;
  auto emit_kernel = [&](absl::Span<const int64> xor_masks) {
    VLOG(2) << absl::StreamFormat(
        "%s uses kernel for xor masks [%s]", context.name,
        absl::StrJoin(xor_masks, ", ", [](std::string* out, int64 xor_mask) {
          absl::StrAppendFormat(out, "0x%x", xor_mask);
        }));
    thunks.emplace_back();
    TF_ASSIGN_OR_RETURN(
        thunks.back(),
        BuildKernelThunkForMlir(sort_op, sort_op.output(), Thunk::ThunkInfo(),
                                mlir_input.extra_slice, &ir_arrays));
    LaunchDimensions launch_dimensions = xor_masks.size() > 1
                                             ? tiled_launch_dimensions
                                             : standard_launch_dimensions;
    UpdateLaunchDimensions(launch_dimensions, thunks.back().get(),
                           ir_emitter_context_->llvm_module());
    std::vector<IrArray> values_arrays;
    values_arrays.reserve(context.operand_shapes.size());
    for (int64 i = 0; i < context.operand_shapes.size(); ++i) {
      values_arrays.push_back(ir_arrays[i]);
    }
    TF_ASSIGN_OR_RETURN(const HloComputation* comparator,
                        GetOrCreateSubComputationFromRegion(
                            &sort_op.comparator(), /*is_fusion=*/false));
    return llvm_ir::EmitSortInPlace(
        dimension_to_sort, values_arrays, IrName(context.name), xor_masks, &b_,
        launch_dimensions,
        xor_masks.size() > 1 ? num_iterations_in_sort_dim
                             : standard_num_iterations_in_sort_dim,
        kTileSize,
        [&](absl::Span<llvm::Value* const> operands, llvm::Value* output) {
          return EmitCallToNestedComputation(*comparator, operands, output);
        });
  };
  std::vector<int64> xor_masks;
  for (int64 stage = 0; stage < num_stages; ++stage) {
    for (int64 mask = stage; mask >= 0; --mask) {
      int64 xor_mask;
      if (mask == stage) {
        xor_mask = (1LL << (stage + 1)) - 1;
      } else {
        xor_mask = 1LL << mask;
      }
      if (xor_mask >= kTileSize || no_tiling) {
        if (!xor_masks.empty()) {
          TF_RETURN_IF_ERROR(emit_kernel(xor_masks));
          xor_masks.clear();
        }
        TF_RETURN_IF_ERROR(emit_kernel({xor_mask}));
      } else {
        xor_masks.push_back(xor_mask);
      }
    }
  }
  if (!xor_masks.empty()) {
    TF_RETURN_IF_ERROR(emit_kernel(xor_masks));
  }
  VLOG(2) << absl::StreamFormat(
      "%s requires %d thunks (including any D2D copies)", context.name,
      thunks.size());

  AddThunkToThunkSequence(absl::make_unique<SequentialThunk>(
      mlir_input.thunk_info, std::move(thunks)));
  if (context.operand_shapes.size() > 1) {
    // Emit the tuple as part of the last stage of sorting.
    // We are currently in the block sorted.in_bounds.after.
    b_.SetInsertPoint(b_.GetInsertBlock()->getTerminator());
    llvm_ir::EmitTuple(
        ir_arrays.back(),
        absl::MakeSpan(ir_arrays).subspan(0, ir_arrays.size() - 1), &b_);
  }
  return Status::OK();
}

Status IrEmitterUnnested::HandleReplicaId(HloInstruction* hlo) {
  AddThunkToThunkSequence(absl::make_unique<ReplicaIdThunk>(
      GetThunkInfo(hlo), GetAllocationSlice(*hlo)));
  return Status::OK();
}

Status IrEmitterUnnested::HandleCollectivePermute(HloInstruction* hlo) {
  CollectivePermuteConfig config = GetCollectivePermuteConfig(hlo);
  AddThunkToThunkSequence(absl::make_unique<CollectivePermuteThunk>(
      GetThunkInfo(hlo), std::move(config),
      GetAllocationSlice(*hlo->operand(0)), GetAllocationSlice(*hlo)));
  return Status::OK();
}

Status IrEmitterUnnested::HandleAllReduce(HloInstruction* crs) {
  VLOG(2) << "AllReduce; replica count: " << hlo_module_config_.replica_count()
          << "; operand count: " << crs->operand_count()
          << "; NCCL is enabled: " << NcclAllReduceThunk::NcclIsEnabled();

  // Note the replica_count == 1 case is handled via device-to-device copy
  // below.
  bool should_use_nccl_thunk = hlo_module_config_.replica_count() > 1 &&
                               NcclAllReduceThunk::CanImplement(crs);

  if (should_use_nccl_thunk) {
    std::vector<NcclAllReduceThunk::Buffer> buffers;
    std::vector<BufferAllocation::Slice> tuple_element_buffers;
    buffers.resize(crs->operand_count());
    tuple_element_buffers.reserve(crs->operand_count());
    CHECK(crs->shape().IsArray() && crs->operand_count() == 1 ||
          crs->shape().IsTuple() &&
              crs->shape().tuple_shapes_size() == crs->operand_count());
    for (int i = 0; i < crs->operand_count(); ++i) {
      CHECK(crs->operand(i)->shape().IsArray())
          << "Operands to all-reduce must be arrays: " << crs->ToString();
      buffers[i].element_count =
          ShapeUtil::ElementsIn(crs->operand(i)->shape());
      buffers[i].source_buffer = GetAllocationSlice(*crs->operand(i));
      buffers[i].destination_buffer = GetAllocationSlice(
          *crs, crs->shape().IsTuple() ? ShapeIndex({i}) : ShapeIndex({}));
      tuple_element_buffers.push_back(buffers[i].destination_buffer);
    }
    NcclAllReduceConfig config =
        GetNcclAllReduceConfig(crs, hlo_module_config_.replica_count());
    auto all_reduce_thunk = absl::make_unique<NcclAllReduceThunk>(
        GetThunkInfo(crs), std::move(config),
        /*buffers=*/std::move(buffers));
    if (crs->shape().IsTuple()) {
      std::vector<std::unique_ptr<Thunk>> thunks;
      thunks.push_back(std::move(all_reduce_thunk));
      thunks.push_back(absl::make_unique<TupleThunk>(
          Thunk::ThunkInfo(), tuple_element_buffers, GetAllocationSlice(*crs)));
      AddThunkToThunkSequence(absl::make_unique<SequentialThunk>(
          GetThunkInfo(crs), std::move(thunks)));
    } else {
      AddThunkToThunkSequence(std::move(all_reduce_thunk));
    }

    return Status::OK();
  }

  if (hlo_module_config_.replica_count() != 1) {
    // TODO(b/33011107): Support more AllReduce configurations on GPU.
    string message = absl::StrFormat(
        "Requested AllReduce not implemented on GPU; replica_count: %d; "
        "operand_count: %d; IsCrossReplicaAllReduce: %d; NCCL support: %d",
        hlo_module_config_.replica_count(), crs->operand_count(),
        crs->IsCrossReplicaAllReduce(), NcclAllReduceThunk::NcclIsEnabled());
    if (crs->operand_count() > 0) {
      absl::StrAppendFormat(
          &message, "; first operand array element-type: %s",
          PrimitiveType_Name(crs->operand(0)->shape().element_type()));
    }
    return Unimplemented("%s", message);
  }

  // CRS with one operand and one replica is simply the identity function.
  // Buffer assignment expects a copy, so that's what we do.
  //
  // TODO(b/80100934): We would like to eliminate one-replica CRS nodes entirely
  // in algebraic-simplifier, but currently on some platforms
  // HloModuleConfig::num_replicas changes between when the module is compiled
  // and when it's run.
  if (crs->operand_count() == 1) {
    CHECK(crs->operand(0)->shape().IsArray())
        << "Operands to all-reduce must be arrays: " << crs->ToString();
    AddThunkToThunkSequence(absl::make_unique<DeviceToDeviceCopyThunk>(
        GetThunkInfo(crs),
        /*source_address=*/GetAllocationSlice(*crs->operand(0)),
        /*destination_buffer=*/GetAllocationSlice(*crs),
        /*mem_size=*/ShapeUtil::ByteSizeOf(crs->shape())));
    return Status::OK();
  }

  // One-replica CRS with multiple operands produces a tuple of the inputs.
  // Again, buffer assignment expects us to copy each.
  std::vector<std::unique_ptr<Thunk>> thunks;
  std::vector<BufferAllocation::Slice> tuple_element_buffers;
  for (int64 i = 0; i < crs->operand_count(); ++i) {
    tuple_element_buffers.push_back(ir_emitter_context_->buffer_assignment()
                                        .GetUniqueSlice(crs, {i})
                                        .ValueOrDie());
    thunks.push_back(absl::make_unique<DeviceToDeviceCopyThunk>(
        Thunk::ThunkInfo(),
        /*source_address=*/GetAllocationSlice(*crs->operand(i)),
        /*destination_buffer=*/tuple_element_buffers.back(),
        /*mem_size=*/ShapeUtil::ByteSizeOf(crs->operand(i)->shape())));
  }

  // Output a tuple of the buffers above.
  thunks.push_back(absl::make_unique<TupleThunk>(
      Thunk::ThunkInfo(), tuple_element_buffers, GetAllocationSlice(*crs)));
  AddThunkToThunkSequence(
      absl::make_unique<SequentialThunk>(GetThunkInfo(crs), std::move(thunks)));
  return Status::OK();
}

Status IrEmitterUnnested::HandleInfeed(HloInstruction* xla_infeed) {
  return ThunkEmitter(this).HandleInfeed(xla_infeed);
}

Status IrEmitterUnnested::HandleOutfeed(HloInstruction* outfeed) {
  return ThunkEmitter(this).HandleOutfeed(outfeed);
}

Status IrEmitterUnnested::HandleAfterAll(HloInstruction* after_all) {
  return Status::OK();
}

// Figures out how to access the buffers for all subshapes of hlo's operands and
// for hlo itself (i.e. all the buffers produced by HLO).
//
// Returns a vector of `HloBufferSlice`s, one for each HLO subshape `hlo` needs
// to access (including one or more for itself).
//
// This function conservatively assumes that we'll touch all sub-buffers of
// every operand and of the output.
static std::vector<HloBufferSlice> GetHloBufferSlices(
    const HloInstruction* hlo, const BufferAssignment& buffer_assn) {
  std::vector<HloBufferSlice> result;
  absl::flat_hash_set<std::pair<const HloInstruction*, ShapeIndex>>
      inserted_buffer_slices;

  // Tries to find a slice plus an array of indices i1, ..., iN such that the
  // sub-buffer for instr at index can be found at slice[i1]...[iN].
  auto find_slice_for = [&](const HloInstruction* instr,
                            const ShapeIndex& index)
      -> optional<std::pair<BufferAllocation::Slice, ShapeIndex>> {
    // Simple, common case: Is the buffer for instr known at runtime?  If so,
    // we're done.
    auto slice = buffer_assn.GetUniqueSlice(instr, index);
    if (slice.ok()) {
      return {{slice.ValueOrDie(), ShapeIndex()}};
    }

    // If that didn't work, walk up any bitcasts that we might see.  These must
    // appear before any GTE instructions, because it's illegal to bitcast to a
    // tuple type.
    const HloInstruction* parent = instr;
    while (parent->IsEffectiveBitcast()) {
      parent = parent->operand(0);

      auto slice = buffer_assn.GetUniqueSlice(parent, {});
      if (slice.ok()) {
        return {{slice.ValueOrDie(), ShapeIndex()}};
      }
    }

    // Check whether instr is a GTE instruction.  If it is, see if we can get a
    // buffer for its parent, and continue walking up parents until we find a
    // defined buffer or we hit something that's not a GTE.
    ShapeIndex gte_indices;
    while (parent->opcode() == HloOpcode::kGetTupleElement) {
      gte_indices.push_front(parent->tuple_index());
      parent = parent->operand(0);

      auto slice = buffer_assn.GetUniqueSlice(parent, {});
      if (slice.ok()) {
        return {{slice.ValueOrDie(), gte_indices}};
      }
    }

    // Finally, if we don't know the buffer for instr at index, see if we know
    // the buffer for instr at index without its last element.  If so, we can
    // dynamically find the buffer for instr by dereferencing a pointer in that
    // buffer.  Continue looking this way until we run out of elements in
    // 'index'.
    //
    // We can almost always get a buffer without resorting to this.  The only
    // exception is for cases where the relevant sub-buffer is truly unknowable,
    // for example the sub-buffer of a tuple-shaped select.
    ShapeIndex new_index = index;
    while (!new_index.empty()) {
      gte_indices.push_front(new_index.back());
      new_index.pop_back();
      auto slice = buffer_assn.GetUniqueSlice(instr, new_index);
      if (slice.ok()) {
        return {{slice.ValueOrDie(), gte_indices}};
      }
    }

    return nullopt;
  };

  // Adds entries for all subshapes of instr to `slices`.
  auto add_slices_for = [&](const HloInstruction* instr) {
    ShapeUtil::ForEachSubshape(
        instr->shape(), [&](const Shape& /*shape*/, const ShapeIndex& index) {
          if (!inserted_buffer_slices.insert({instr, index}).second) {
            // HLOs can have duplicate operands; don't bother redoing work.
            return;
          }
          auto maybe_slice = find_slice_for(instr, index);
          if (maybe_slice.has_value()) {
            HloBufferSlice hlo_buffer_slice;
            hlo_buffer_slice.instr = instr;
            hlo_buffer_slice.hlo_index = index;
            hlo_buffer_slice.buffer_slice = maybe_slice->first;
            hlo_buffer_slice.gte_index = maybe_slice->second;
            result.push_back(hlo_buffer_slice);
          } else {
            VLOG(1) << "Couldn't find buffer for " << instr->ToString()
                    << " at index " << index.ToString();
          }
        });
  };

  add_slices_for(hlo);
  for (const HloInstruction* operand : hlo->operands()) {
    // Conservatively assume we'll need the buffers for all subshapes of the
    // operand.
    add_slices_for(operand);
  }

  return result;
}

std::unique_ptr<KernelThunk>
IrEmitterUnnested::BuildKernelThunkFromBufferSlices(
    absl::string_view name, Thunk::ThunkInfo thunk_info,
    absl::Span<const BufferSlice* const> slices,
    std::function<void(const BufferSlice*, llvm::Value*)>
        bind_slice_to_ir_value) {
  const auto& buffer_assn = ir_emitter_context_->buffer_assignment();

  // Figure out which buffer allocations need to be passed as arguments to our
  // kernel.  This is simply all of the allocations referenced in slices,
  // plus the XLA temp buffer (if we have it).  We always include the temp
  // buffer because even if the kernel itself doesn't use it, a nested
  // subcomputation within the kernel (e.g. a kMap's computation) might.
  std::unordered_set<const BufferAllocation*> buffers_needed;
  for (auto* slice : slices) {
    buffers_needed.insert(slice->buffer_slice.allocation());
  }
  absl::optional<const BufferAllocation*> temp_buffer;
  for (const BufferAllocation& alloc : buffer_assn.Allocations()) {
    if (alloc.IsPreallocatedTempBuffer()) {
      if (!temp_buffer.has_value()) {
        // Retrieve the first seen temp buffer.
        temp_buffer = &alloc;
      }
    }
  }
  if (temp_buffer.has_value()) {
    buffers_needed.insert(*temp_buffer);
  }

  // We'll pass a pointer to each of the elements of `buffers` to our kernel, in
  // this order.
  std::vector<const BufferAllocation*> non_constant_buffers;
  absl::c_copy_if(buffers_needed, std::back_inserter(non_constant_buffers),
                  [](const BufferAllocation* allocation) {
                    return !allocation->is_constant();
                  });

  absl::c_sort(non_constant_buffers,
               [](const BufferAllocation* a, const BufferAllocation* b) {
                 return a->index() < b->index();
               });

  llvm::Function* kernel = BuildKernelPrototype(name, non_constant_buffers);

  // Build a map from a BufferAllocation to the corresponding argument in our
  // kernel.
  std::unordered_map<const BufferAllocation*, llvm::Value*> kernel_args;
  {
    auto arg_it = kernel->arg_begin();
    auto buffers_it = non_constant_buffers.begin();
    for (; arg_it != kernel->arg_end(); ++arg_it, ++buffers_it) {
      kernel_args[*buffers_it] = arg_it;

      // Annotate all allocations with LLVM's `noalias`.
      // There are three kinds of allocations:
      // * Read-only allocations, aka input parameters that are not aliased with
      // outputs.
      // * Read-write allocations, including all output buffers, some of which
      // may alias with input HLO parameters, but aliased HLO buffers are always
      // assigned with the same allocation.
      // * The temp buffer.
      //
      // Read-only allocations may overlap with each other, but since they are
      // not mutated, they can always be annotated with `noalias` per LLVM
      // semantics.
      //
      // Read-write allocations and the temp buffer don't overlap with any
      // allocations, therefore they can also be annotated with `noalias`.
      kernel->addParamAttr(
          arg_it->getArgNo(),
          llvm::Attribute::get(arg_it->getContext(), llvm::Attribute::NoAlias));
    }
  }

  // For each buffer our kernel might want to touch, bind it to a value derived
  // from our kernel args.
  for (auto* slice : slices) {
    const BufferAllocation::Slice& buffer_slice = slice->buffer_slice;
    const ShapeIndex& gte_index = slice->gte_index;

    llvm::Value* loc;
    if (buffer_slice.allocation()->is_constant()) {
      loc = ir_emitter_context_->llvm_module()->getGlobalVariable(
          llvm_ir::ConstantBufferAllocationToGlobalName(
              *buffer_slice.allocation()));
      CHECK_NE(loc, nullptr);
    } else {
      loc = InBoundsGEP(kernel_args.at(buffer_slice.allocation()),
                        {b_.getInt64(buffer_slice.offset())});
    }

    // If gte_index is nonempty, we have to dereference `loc` to get to the
    // value we're ultimately interested in.
    llvm::Type* int8_double_pointer =
        llvm::PointerType::get(b_.getInt8PtrTy(), /*AddressSpace=*/0);
    for (int64 idx : gte_index) {
      loc = b_.CreatePointerBitCastOrAddrSpaceCast(loc, int8_double_pointer);
      loc = Load(InBoundsGEP(loc, {b_.getInt64(idx)}));
    }

    bind_slice_to_ir_value(slice, loc);
  }

  // Bind the temp buffer so that nested subcomputations can find it if they
  // need.
  if (temp_buffer.has_value()) {
    bindings_.SetTempBufferBase(kernel_args.at(*temp_buffer));
  } else {
    bindings_.SetTempBufferBase(
        llvm::ConstantPointerNull::get(b_.getInt8PtrTy()));
  }

  return absl::make_unique<KernelThunk>(thunk_info, non_constant_buffers,
                                        std::string(kernel->getName()));
}

std::unique_ptr<KernelThunk> IrEmitterUnnested::BuildKernelThunk(
    const HloInstruction* inst, bool implements_whole_instruction) {
  std::vector<HloBufferSlice> hlo_slices =
      GetHloBufferSlices(inst, ir_emitter_context_->buffer_assignment());

  std::vector<BufferSlice*> slice_ptrs;
  slice_ptrs.reserve(hlo_slices.size());
  for (auto& slice : hlo_slices) {
    slice_ptrs.push_back(&slice);
  }

  return BuildKernelThunkFromBufferSlices(
      inst->name(),
      implements_whole_instruction ? GetThunkInfo(inst) : Thunk::ThunkInfo(),
      slice_ptrs, [this](const BufferSlice* slice, llvm::Value* value) {
        const HloBufferSlice* hlo_buffer_slice =
            static_cast<const HloBufferSlice*>(slice);
        const HloInstruction* instr = hlo_buffer_slice->instr;
        const ShapeIndex& index = hlo_buffer_slice->hlo_index;
        VLOG(3) << "Buffer for " << instr->ToString() << " at "
                << index.ToString() << " is found in slice "
                << hlo_buffer_slice->buffer_slice.ToString() << " at GTE index "
                << hlo_buffer_slice->gte_index.ToString();

        bindings_.BindHloToIrValue(*instr, value, index);
      });
}

std::unique_ptr<KernelThunk> IrEmitterUnnested::BuildKernelThunkForMlirImpl(
    absl::string_view name, Thunk::ThunkInfo thunk_info,
    absl::Span<const MlirBufferSlice> slices,
    std::vector<llvm_ir::IrArray>* ir_arrays) {
  absl::flat_hash_set<BufferAllocation::Slice> buffers_written;
  std::vector<const BufferSlice*> slice_ptrs;
  slice_ptrs.reserve(slices.size());
  for (auto& slice : slices) {
    slice_ptrs.push_back(&slice);
    if (slice.written) {
      buffers_written.insert(slice.buffer_slice);
    }
  }

  ir_arrays->clear();
  return BuildKernelThunkFromBufferSlices(
      name, thunk_info, slice_ptrs,
      [&](const BufferSlice* slice, llvm::Value* value) {
        const auto& mlir_slice = static_cast<const MlirBufferSlice&>(*slice);

        llvm_ir::IrArray ir_array(
            CastToTypedValue(mlir_slice.shape, value, &b_), mlir_slice.shape);
        if (!buffers_written.contains(slice->buffer_slice)) {
          ir_array.MarkInvariantOverWholeProgram(&value->getContext());
        }

        ir_arrays->push_back(ir_array);
      });
}

static void GetFusionOperandsAndOutputs(mlir::lmhlo::FusionOp fusion,
                                        std::vector<mlir::Value>* operands,
                                        std::vector<mlir::Value>* outputs) {
  fusion.region().walk([&](mlir::TensorLoadOp load) {
    CHECK(load.memref().getParentRegion() != &fusion.region())
        << "TensorLoadOp shows should be only expected for accessing captured "
           "memrefs.";
    operands->push_back(load.memref());
  });
  fusion.region().walk([&](mlir::TensorStoreOp store) {
    CHECK(store.memref().getParentRegion() != &fusion.region())
        << "TensorStoreOp shows should be only expected for accessing captured "
           "memrefs.";
    outputs->push_back(store.memref());
  });
}

StatusOr<std::unique_ptr<KernelThunk>>
IrEmitterUnnested::BuildKernelThunkForMlir(
    mlir::Operation* op, mlir::ValueRange operands, Thunk::ThunkInfo thunk_info,
    absl::optional<MlirBufferSlice> extra_slice,
    std::vector<llvm_ir::IrArray>* ir_arrays) {
  absl::Span<const BufferAllocation> allocations(
      ir_emitter_context_->buffer_assignment().Allocations());
  std::vector<MlirBufferSlice> slices;
  for (mlir::Value operand : operands) {
    slices.emplace_back();
    auto& slice = slices.back();
    TF_ASSIGN_OR_RETURN(slice.buffer_slice,
                        GetAllocationSliceForMlir(operand, allocations));
    slice.written = WritesMlirBuffer(op, operand);
    slice.shape = TypeToShape(operand.getType());
  }
  if (extra_slice) {
    slices.push_back(*extra_slice);
  }
  std::string name = mlir::GetNameFromLoc(op->getLoc());
  return BuildKernelThunkForMlirImpl(name, thunk_info, slices, ir_arrays);
}

StatusOr<std::unique_ptr<KernelThunk>>
IrEmitterUnnested::BuildKernelThunkForMlir(
    mlir::Operation* op, Thunk::ThunkInfo thunk_info,
    absl::optional<MlirBufferSlice> extra_slice,
    std::vector<llvm_ir::IrArray>* ir_arrays) {
  if (auto fusion = mlir::dyn_cast<mlir::lmhlo::FusionOp>(op)) {
    absl::Span<const BufferAllocation> allocations(
        ir_emitter_context_->buffer_assignment().Allocations());
    std::vector<mlir::Value> operands, outputs;
    GetFusionOperandsAndOutputs(fusion, &operands, &outputs);

    std::vector<MlirBufferSlice> slices;
    for (auto operand : operands) {
      slices.emplace_back();
      auto& slice = slices.back();
      TF_ASSIGN_OR_RETURN(slice.buffer_slice,
                          GetAllocationSliceForMlir(operand, allocations));
      slice.written = false;
      slice.shape = TypeToShape(operand.getType());
    }
    for (auto output : outputs) {
      slices.emplace_back();
      auto& slice = slices.back();
      TF_ASSIGN_OR_RETURN(slice.buffer_slice,
                          GetAllocationSliceForMlir(output, allocations));
      slice.written = true;
      slice.shape = TypeToShape(output.getType());
    }
    std::string name = mlir::GetNameFromLoc(op->getLoc());
    if (extra_slice) {
      slices.push_back(*extra_slice);
    }
    return BuildKernelThunkForMlirImpl(name, thunk_info, slices, ir_arrays);
  }
  return BuildKernelThunkForMlir(op, op->getOperands(), thunk_info, extra_slice,
                                 ir_arrays);
}

StatusOr<std::unique_ptr<Thunk>> IrEmitterUnnested::BuildInitializerThunk(
    HloInstruction* hlo, const ShapeIndex& index) {
  bool fused = HloOpcode::kFusion == hlo->opcode();
  HloInstruction* inst = fused ? hlo->fused_expression_root() : hlo;
  HloInstruction* init_value_operand = [&] {
    switch (inst->opcode()) {
      case HloOpcode::kSelectAndScatter:
        return inst->mutable_operand(2);
      case HloOpcode::kReduce:
        return inst->mutable_operand(1);
      case HloOpcode::kTuple:
        CHECK(hlo->IsMultiOutputFusion())
            << ": " << hlo->ToString() << " is not a multi-output fusion.";
        CHECK(inst->operand(index.back())->opcode() == HloOpcode::kReduce)
            << ": Found '" << inst->operand(index.back())->opcode() << "' in "
            << inst->ToString() << " but expected 'reduce'.";
        // For multi-output fusion look through the tuple.
        return inst->mutable_operand(index.back())->mutable_operand(1);
      default:
        LOG(FATAL) << "Opcode " << inst->opcode()
                   << " should not need an initializer.";
    }
  }();

  const HloInstruction* init_value = init_value_operand;
  if (fused && init_value->opcode() == HloOpcode::kParameter) {
    init_value = hlo->operand(init_value->parameter_number());
  }

  // Initializer thunks don't implement a whole instruction, and we want to
  // profile the whole instruction instead of the individual thunks it consists
  // of. Therefore we pass nullptr as the HloInstruction* to the thunks we
  // generate below.
  //
  // In the common case, the initializer is a constant.  In this case, emit a
  // device-memset call if we can.  Currently StreamExecutor only supports
  // zeroing and 32-bit memsets.
  if (init_value->IsConstant()) {
    CHECK(ShapeUtil::IsScalar(init_value->shape()));
    int64 num_bytes = ShapeUtil::ByteSizeOfElements(init_value->shape());
    const auto& literal = init_value->literal();

    // Are all the bytes of this scalar equal to 0?  If so, we can create a
    // MemzeroThunk.
    absl::Span<const uint8> literal_bytes(
        reinterpret_cast<const uint8*>(literal.untyped_data()), num_bytes);
    if (absl::c_all_of(literal_bytes, [](uint8 byte) { return byte == 0; })) {
      return {absl::make_unique<MemzeroThunk>(Thunk::ThunkInfo(),
                                              GetAllocationSlice(*hlo, index))};
    }

    // If the literal is 8 or 16 bits wide, we can emit a 32-bit memset by
    // repeating the literal 4 or 2 times, so long as the destination buffer is
    // an even multiple of 32 bits long.
    const Shape& output_shape = ShapeUtil::GetSubshape(hlo->shape(), index);
    if ((num_bytes == 1 || num_bytes == 2) &&
        ShapeUtil::ByteSizeOf(output_shape) % 4 == 0) {
      uint16 pattern16;
      if (num_bytes == 1) {
        uint8 b = literal_bytes.front();
        pattern16 = uint16{b} | (uint16{b} << 8);
      } else {
        memcpy(&pattern16, literal_bytes.data(), sizeof(pattern16));
      }
      uint32 pattern32 = uint32{pattern16} | (uint32{pattern16} << 16);
      return {absl::make_unique<Memset32BitValueThunk>(
          Thunk::ThunkInfo(), pattern32, GetAllocationSlice(*hlo, index))};
    }

    // If the literal is an even multiple of 32 bits wide, we can emit a 32-bit
    // memset so long as all 32-bit words of the scalar are equal to each other.
    if (num_bytes >= 4 && num_bytes % 4 == 0 &&
        memcmp(literal_bytes.data(), literal_bytes.data() + 4,
               literal_bytes.size() - 4) == 0) {
      uint32 word;
      memcpy(&word, literal_bytes.data(), sizeof(word));
      return {absl::make_unique<Memset32BitValueThunk>(
          Thunk::ThunkInfo(), word, GetAllocationSlice(*hlo, index))};
    }
  }

  // Otherwise fall back to our slow initializer code.
  std::unique_ptr<KernelThunk> kernel_thunk =
      BuildKernelThunk(hlo, /*implements_whole_instruction=*/false);
  LaunchDimensions launch_dimensions =
      CalculateLaunchDimensions(ShapeUtil::GetSubshape(hlo->shape(), index),
                                ir_emitter_context_->gpu_device_info());
  UpdateLaunchDimensions(launch_dimensions, kernel_thunk.get(),
                         ir_emitter_context_->llvm_module());

  if (fused) {
    // If init_value was fused into this reduce we have to generate it first.
    GpuElementalIrEmitter elemental_emitter(hlo_module_config_,
                                            ir_emitter_context_->llvm_module(),
                                            &b_, GetNestedComputer());

    FusedIrEmitter fused_emitter(&elemental_emitter);
    BindFusionArguments(hlo, &fused_emitter);
    TF_ASSIGN_OR_RETURN(auto generator,
                        fused_emitter.GetGenerator(init_value_operand));
    TF_RETURN_IF_ERROR(ParallelLoopEmitter(generator,
                                           GetIrArray(*hlo, *hlo, index),
                                           launch_dimensions, &b_)
                           .EmitLoop(IrName(hlo)));
  } else {
    // In the unfused case the element is already there, just read from it.
    TF_RETURN_IF_ERROR(ParallelLoopEmitter(
                           [=](const IrArray::Index& index) {
                             return GetIrArray(*init_value, *hlo)
                                 .EmitReadArrayElement(index, &b_);
                           },
                           GetIrArray(*hlo, *hlo, index), launch_dimensions,
                           &b_)
                           .EmitLoop(IrName(hlo)));
  }

  // Clean up state left behind by emitting the loop above.  (This is normally
  // done in IrEmitterUnnested::Postprocess().)
  bindings_.UnbindAllLocalIrValues();

  // Convert unique_ptr<KernelThunk> to StatusOr<unique_ptr<Thunk>>.
  return {std::move(kernel_thunk)};
}

namespace {

// Checks that the buffers corresponding to the given two HLOs share the same
// allocation.
Status CheckHloBuffersShareAllocation(
    const HloInstruction* a, const HloInstruction* b, const ShapeIndex& index,
    const BufferAssignment& buffer_assignment) {
  const BufferAllocation::Slice slice_a =
      buffer_assignment.GetUniqueSlice(a, index).ConsumeValueOrDie();
  const BufferAllocation::Slice slice_b =
      buffer_assignment.GetUniqueSlice(b, index).ConsumeValueOrDie();
  if (slice_a != slice_b) {
    return InternalError(
        "instruction %s %s does not share allocation with instruction %s %s",
        a->ToString(), slice_a.ToString(), b->ToString(), slice_b.ToString());
  }
  return Status::OK();
}

// Checks that all buffers used during while loop iteration share the same
// buffer allocation. This includes buffers for while result, while init
// operand, condition parameter, body parameter and body result.
// Returns OK on success, error status otherwise.
Status CheckWhileBuffersShareAllocation(
    const HloInstruction* xla_while,
    const BufferAssignment& buffer_assignment) {
  return ShapeUtil::ForEachSubshapeWithStatus(
      xla_while->shape(),
      [&](const Shape& /*subshape*/, const ShapeIndex& index) -> Status {
        const HloInstruction* condition_parameter =
            xla_while->while_condition()->parameter_instruction(0);
        const HloComputation* body = xla_while->while_body();
        const HloInstruction* body_parameter = body->parameter_instruction(0);
        const HloInstruction* body_result = body->root_instruction();
        TF_RETURN_IF_ERROR(CheckHloBuffersShareAllocation(
            xla_while, xla_while->operand(0), index, buffer_assignment));
        TF_RETURN_IF_ERROR(CheckHloBuffersShareAllocation(
            xla_while, condition_parameter, index, buffer_assignment));
        TF_RETURN_IF_ERROR(CheckHloBuffersShareAllocation(
            xla_while, body_parameter, index, buffer_assignment));
        TF_RETURN_IF_ERROR(CheckHloBuffersShareAllocation(
            xla_while, body_result, index, buffer_assignment));
        return Status::OK();
      });
}

// Checks that the buffers used in a conditional instruction are shared with the
// operands and result as follows:
//   * The result buffer of the conditional should share the allocation with the
//     result buffers of each branch computation.
//   * The buffer of operand b+1 should share the allocation with the buffer of
//     the parameter 0 instruction of the b'th computation.
Status CheckConditionalBuffersShareAllocation(
    const HloInstruction* conditional,
    const BufferAssignment& buffer_assignment) {
  TF_RETURN_IF_ERROR(ShapeUtil::ForEachSubshapeWithStatus(
      conditional->shape(),
      [&](const Shape& /*subshape*/, const ShapeIndex& index) -> Status {
        for (auto branch_computation : conditional->branch_computations()) {
          TF_RETURN_IF_ERROR(CheckHloBuffersShareAllocation(
              conditional, branch_computation->root_instruction(), index,
              buffer_assignment));
        }
        return Status::OK();
      }));
  for (int j = 0; j < conditional->branch_count(); ++j) {
    TF_RETURN_IF_ERROR(ShapeUtil::ForEachSubshapeWithStatus(
        conditional->operand(j + 1)->shape(),
        [&](const Shape& /*subshape*/, const ShapeIndex& index) -> Status {
          return CheckHloBuffersShareAllocation(
              conditional->operand(j + 1),
              conditional->branch_computation(j)->parameter_instruction(0),
              index, buffer_assignment);
        }));
  }
  return Status::OK();
}

}  // namespace

StatusOr<std::unique_ptr<Thunk>> IrEmitterUnnested::BuildWhileThunk(
    const HloInstruction* hlo) {
  // Check that all while-related buffers share an allocation.
  TF_CHECK_OK(CheckWhileBuffersShareAllocation(
      hlo, ir_emitter_context_->buffer_assignment()));

  // Generate thunk sequence for while 'condition'.
  HloComputation* condition = hlo->while_condition();
  TF_ASSIGN_OR_RETURN(auto ir_emitter_condition,
                      IrEmitterUnnested::Create(hlo_module_config_, condition,
                                                ir_emitter_context_));
  TF_RETURN_IF_ERROR(condition->Accept(ir_emitter_condition.get()));

  // Generate thunk sequence for while 'body'.
  HloComputation* body = hlo->while_body();
  TF_ASSIGN_OR_RETURN(
      auto ir_emitter_body,
      IrEmitterUnnested::Create(hlo_module_config_, body, ir_emitter_context_));
  TF_RETURN_IF_ERROR(body->Accept(ir_emitter_body.get()));

  const auto* index_map = ir_emitter_context_->profile_index_map();
  absl::optional<size_t> condition_profile_index, body_profile_index;
  if (index_map) {
    condition_profile_index = index_map->GetProfileIndexFor(*condition);
    body_profile_index = index_map->GetProfileIndexFor(*body);
  }

  return std::unique_ptr<Thunk>(new WhileThunk(
      GetThunkInfo(hlo),
      GetAllocationSlice(*condition->root_instruction()),  // cond result
      ir_emitter_condition->ConsumeThunkSequence(),
      ir_emitter_body->ConsumeThunkSequence(), condition_profile_index,
      body_profile_index));
}

StatusOr<std::unique_ptr<Thunk>> IrEmitterUnnested::BuildForThunk(
    const HloInstruction* hlo, const int64 loop_limit) {
  // Check that all while-related buffers share an allocation.
  TF_CHECK_OK(CheckWhileBuffersShareAllocation(
      hlo, ir_emitter_context_->buffer_assignment()));

  // Generate thunk sequence for while 'body' (will be used a For loop body).
  HloComputation* body = hlo->while_body();
  TF_ASSIGN_OR_RETURN(
      auto ir_emitter_body,
      IrEmitterUnnested::Create(hlo_module_config_, body, ir_emitter_context_));
  TF_RETURN_IF_ERROR(body->Accept(ir_emitter_body.get()));

  const auto* index_map = ir_emitter_context_->profile_index_map();
  absl::optional<size_t> body_profile_index;
  if (index_map) {
    body_profile_index = index_map->GetProfileIndexFor(*body);
  }

  return std::unique_ptr<Thunk>(new ForThunk(
      GetThunkInfo(hlo), loop_limit, ir_emitter_body->ConsumeThunkSequence(),
      body_profile_index));
}

StatusOr<std::unique_ptr<Thunk>> IrEmitterUnnested::BuildConditionalThunk(
    const HloInstruction* hlo) {
  // Check that the buffers used in conditional are shared with the operands and
  // result appropriately.
  TF_CHECK_OK(CheckConditionalBuffersShareAllocation(
      hlo, ir_emitter_context_->buffer_assignment()));

  std::vector<BufferAllocation::Slice> branch_operands;
  std::vector<ThunkSequence> branch_thunks;
  std::vector<absl::optional<size_t>> branch_profile_indices;

  int branch_count = hlo->branch_count();
  branch_thunks.reserve(branch_count);
  branch_profile_indices.reserve(branch_count);

  const auto* index_map = ir_emitter_context_->profile_index_map();

  for (int j = 0; j < branch_count; ++j) {
    branch_operands.emplace_back(GetAllocationSlice(*hlo->operand(j + 1)));
    HloComputation* branch_computation = hlo->branch_computation(j);
    TF_ASSIGN_OR_RETURN(
        auto ir_emitter,
        IrEmitterUnnested::Create(hlo_module_config_, branch_computation,
                                  ir_emitter_context_));
    TF_CHECK_OK(branch_computation->Accept(ir_emitter.get()));
    branch_thunks.push_back(std::move(*ir_emitter->ConsumeThunkSequence()));

    absl::optional<size_t> profile_index;
    if (index_map) {
      profile_index = index_map->GetProfileIndexFor(*branch_computation);
    }
    branch_profile_indices.push_back(profile_index);
  }

  ConditionalThunkConfig config = GetConditionalThunkConfig(
      hlo, std::move(branch_thunks), std::move(branch_profile_indices));
  return std::unique_ptr<Thunk>(new ConditionalThunk(
      GetThunkInfo(hlo), std::move(config),
      GetAllocationSlice(*hlo->operand(0)), branch_operands));
}

Status IrEmitterUnnested::EmitTargetElementLoopInThunk(
    const HloInstruction& hlo,
    const llvm_ir::ElementGenerator& element_generator, KernelThunk* thunk,
    int unroll_factor, bool few_waves) {
  VLOG(3) << bindings_.ToString();

  bool multi_output = hlo.shape().IsTuple();

  const Shape& element_shape =
      multi_output ? ShapeUtil::GetSubshape(hlo.shape(), {0}) : hlo.shape();
  VLOG(3) << "EmitTargetElementLoopInThunk "
          << ShapeUtil::HumanStringWithLayout(hlo.shape())
          << " for unroll_factor " << unroll_factor;
  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      element_shape, ir_emitter_context_->gpu_device_info(), unroll_factor,
      few_waves);
  UpdateLaunchDimensions(launch_dimensions, thunk,
                         ir_emitter_context_->llvm_module());
  if (!multi_output) {
    return ParallelLoopEmitter(element_generator, GetIrArray(hlo, hlo),
                               launch_dimensions, &b_, unroll_factor)
        .EmitLoop(
            IrName(&hlo),
            GetIndexTypeForKernel(&hlo, launch_dimensions.launch_bound(), &b_));
  }

  // Emit the tuple pointers in one thread.  We could do this at any point in
  // the kernel, but we do it at the beginning in the hopes of reducing register
  // pressure, since we touch threadIdx.x and blockIdx.x at the beginning of the
  // kernel *anyway*.
  std::vector<IrArray> output_arrays = ConstructIrArrayForOutputs(hlo);
  KernelSupportLibrary{&b_}.If("emit_mof_tuple", IsBlock0Thread0(&b_), [&] {
    llvm_ir::EmitTuple(GetIrArray(hlo, hlo), output_arrays, &b_);
  });

  // For multioutput fusion, we need to emit each operand and the root.
  TF_RETURN_IF_ERROR(
      ParallelLoopEmitter(element_generator, output_arrays, launch_dimensions,
                          &b_, unroll_factor)
          .EmitLoop(IrName(&hlo),
                    GetIndexTypeForKernel(
                        &hlo, launch_dimensions.launch_bound(), &b_)));

  b_.SetInsertPoint(b_.GetInsertBlock()->getTerminator());
  return Status::OK();
}

Status IrEmitterUnnested::EmitTargetElementLoop(
    const HloInstruction& hlo, const llvm_ir::ElementGenerator& body_emitter) {
  int unroll_factor = 1;
  if (!MayPreventVectorization(hlo)) {
    unroll_factor = ComputeMaxUnrollFactor(&hlo);
  }

  std::unique_ptr<KernelThunk> kernel_thunk =
      BuildKernelThunk(&hlo, /*implements_whole_instruction=*/true);

  // Check if we want to schedule grid size that has fewer SM waves.
  // This speed up computations in some cases.
  bool few_waves = false;
  auto few_waves_allow_instr = [](const HloInstruction* instr) {
    return instr->IsElementwise() || instr->opcode() == HloOpcode::kParameter ||
           // We need to make the codegen broadcast aware before enabling
           // more broadcast pattern.
           (instr->opcode() == HloOpcode::kBroadcast &&
            instr->dimensions().empty());
  };
  if (hlo.opcode() == HloOpcode::kFusion) {
    few_waves =
        absl::c_all_of(hlo.fused_instructions_computation()->instructions(),
                       few_waves_allow_instr);
  } else {
    few_waves = few_waves_allow_instr(&hlo);
  }

  Status emit_status = EmitTargetElementLoopInThunk(
      hlo, body_emitter, kernel_thunk.get(), unroll_factor, few_waves);
  thunk_sequence_.emplace_back(std::move(kernel_thunk));

  return emit_status;
}

// Gets the output offset as calculated from thread_id.x (to be applied to the
// offset calculated from block_id and thread_id.y).
static llvm::Value* GetStartOffsetX(const KernelMappingScheme& mapping_scheme,
                                    llvm::Value* thread_id_x,
                                    llvm::Type* index_ty,
                                    llvm::IRBuilder<>* b) {
  auto constant = [&](int64 val) {
    return llvm::ConstantInt::get(index_ty, val);
  };
  if (mapping_scheme.GetIndexingOrder() == kStridedIndexingX) {
    return thread_id_x;
  } else if (mapping_scheme.GetIndexingOrder() == kStridedLinearIndexingX) {
    return b->CreateMul(thread_id_x, constant(mapping_scheme.GetVectorSize()));
  }
  CHECK_EQ(mapping_scheme.GetIndexingOrder(), kLinearIndexingX);
  int64 x_num_steps =
      mapping_scheme.GetTileSizeX() / mapping_scheme.GetNumThreadsX();
  return b->CreateMul(thread_id_x, constant(x_num_steps));
}

// Calls `emit_elem_function()` `x_num_steps` times.  If
// `vector_size`==1, then each element index passed to
// `emit_elem_function()` will be separated by `step_x`. If `vector_size`>1,
// then it must be a multiple of `x_num_steps`.  In that case, it
// triggers a different indexing order that is vectorizable by
// LLVM. It generates many groups of calls to `emit_elem_function`. Each
// group is separated by `step_x` elements.  Inside a group, elements
// are consecutive. If `check_x_tile_bounds` is true, then it will check
// if the element index is in bound compared to `tile_width` before
// calling `emit_elem_function`.
static void UnrollInnerTileLoop(
    bool check_x_tile_bounds, int64 x_num_steps, int64 step_x,
    int64 vector_size, const string& loop_name, KernelSupportLibrary* ksl,
    llvm::Value* start_offset_x, llvm::Value* y_loc, llvm::Value* tile_width,
    const IrArray::Index& source_idx, llvm::IRBuilder<>* b,
    const IrEmitterUnnested::EmitElementFunction* emit_elem_function) {
  llvm::Type* index_ty = tile_width->getType();
  auto constant = [&](int64 val) {
    return llvm::ConstantInt::get(index_ty, val);
  };
  IrArray::Index source_idx_x_base = source_idx.AddOffsetToDim(y_loc, kDimY, b);
  for (int64 j = 0; j < x_num_steps / vector_size; j++) {
    for (int64 i = 0; i < vector_size; i++) {
      int64 linear_index = j * vector_size + i;
      llvm::Value* x_loc = b->CreateAdd(constant(j * step_x * vector_size + i),
                                        start_offset_x, "x_loc");
      IrArray::Index source_idx_x = source_idx_x_base.AddOffsetToDim(
          constant(j * step_x * vector_size + i), kDimX, b);
      auto emit_element = [&] {
        return (*emit_elem_function)(source_idx_x, y_loc, x_loc, linear_index);
      };
      if (check_x_tile_bounds) {
        ksl->If(loop_name + "_x_in_tile", b->CreateICmpULT(x_loc, tile_width),
                emit_element);
      } else {
        emit_element();
      }
    }
  }
}

void IrEmitterUnnested::EmitTile(
    const KernelMappingScheme& mapping_scheme,
    const IrArray::Index& tile_origin_index, const string& loop_name,
    KernelSupportLibrary* ksl, const ThreadIdInfo& thread_id_info,
    llvm::Value* tile_height, llvm::Value* tile_width,
    const IrEmitterUnnested::EmitElementFunction& emit_elem_function) {
  llvm::Type* index_ty = tile_width->getType();
  auto constant = [&](int64 val) {
    return llvm::ConstantInt::get(index_ty, val);
  };
  int64 num_threads_x = mapping_scheme.GetNumThreadsX();
  llvm::Value* num_threads_y = constant(mapping_scheme.GetNumThreadsY());
  int64 tile_size_x = mapping_scheme.GetTileSizeX();

  int64 x_num_steps = tile_size_x / num_threads_x;
  llvm::Value* start_offset_x = GetStartOffsetX(
      mapping_scheme, thread_id_info.thread_id_x, index_ty, &b_);

  // Using dilated mapping scheme, each thread steps with a stride of number
  // of threads.
  // Otherwise, the stride is one, but we multiply each offset by the limit of
  // number of steps which can be made.
  int64 step_x =
      mapping_scheme.GetIndexingOrder() == kLinearIndexingX ? 1 : num_threads_x;
  int64 vector_size = mapping_scheme.GetVectorSize();

  IrArray::Index source_idx =
      tile_origin_index.AddOffsetToDim(start_offset_x, kDimX, &b_);

  auto ceil_of_ratio = [&](llvm::Value* a, llvm::Value* b) {
    return b_.CreateUDiv(b_.CreateAdd(b_.CreateAdd(a, b), constant(-1)), b);
  };

  // True iff all threads always execute all instructions in the tiling
  // dimension X.
  bool x_tile_fits =
      mapping_scheme.GetDimsInElems()[kDimX] % tile_size_x == 0 &&
      mapping_scheme.GetRowContiguous();

  // The outer loop below is simply doing:
  //
  // for (int y_loc=thread_id_y; y_loc<tile_height; y_loc+=num_threads_y)
  //
  //
  // However, in order to avoid an LLVM optimization triggering the ptxas bug,
  // we write this loop in a convoluted way:
  //
  // y_bound = ceil_of_ratio(tile_height - thread_id_y, num_threads_y)
  // for (int y_indvar=0; y_indvar<y_bound; y_indvar+=1)
  //    y_loc = thread_id_y + y_indvar * num_threads_y
  //
  // TODO(cheshire): Once ptxas is fixed and TF switches to it, remove the
  // workaround.
  ksl->For(
      loop_name + "_y_in_tile",
      /*start=*/constant(0),
      /*end=*/
      ceil_of_ratio(b_.CreateSub(tile_height, thread_id_info.thread_id_y),
                    num_threads_y),
      /*step=*/constant(1), [&](llvm::Value* y_indvar) {
        llvm::Value* y_loc = b_.CreateAdd(
            thread_id_info.thread_id_y, b_.CreateMul(y_indvar, num_threads_y));
        auto unroll_inner_tile_loop = [&](bool check_x_tile_bounds) {
          return UnrollInnerTileLoop(check_x_tile_bounds, x_num_steps, step_x,
                                     vector_size, loop_name, ksl,
                                     start_offset_x, y_loc, tile_width,
                                     source_idx, &b_, &emit_elem_function);
        };

        // Only take this path when we unroll in a way vectorizable by
        // LLVM. Special case when the tile doesn't fit completely for even
        // row size. For odd row size every other row isn't aligned to the
        // vectorized size, so it can't be vectorized by LLVM.
        if (!x_tile_fits &&
            mapping_scheme.GetIndexingOrder() == kStridedLinearIndexingX) {
          ksl->If(
              loop_name + "_is_full_tile",
              // For the last block, tile_width will be the number of
              // elements left.
              b_.CreateICmpEQ(constant(mapping_scheme.GetTileSizeX()),
                              tile_width),
              [&] { unroll_inner_tile_loop(/*check_x_tile_bounds=*/false); },
              [&] { unroll_inner_tile_loop(/*check_x_tile_bounds=*/true); });
        } else {
          unroll_inner_tile_loop(/*check_x_tile_bounds=*/!x_tile_fits);
        }
      });
}

// Emits code to process a tensor element in a tile for the given kCopy HLO that
// performs a 0-2-1 transpose.
//
// index: The index for the first output element in the normalized tensor. The
//   normalized tensor is the resulting tensor after collapsing contiguous
//   dimensions that play the same role in the transpose.
// mapping_scheme: Kernel mapping scheme specifying the tiling
void IrEmitterUnnested::EmitTileElementForCopy(
    const Shape& output_shape, const llvm_ir::IrArray& output_array,
    const llvm_ir::IrArray::Index& index,
    const KernelMappingScheme& mapping_scheme, llvm::Value* y_loc,
    llvm::Value* x_loc, absl::Span<llvm::Value* const> param_shmem_buffers) {
  // TODO(jlebar): Add AA metadata to this load.
  llvm::Instruction* load_from_shmem_buffer =
      Load(GEP(param_shmem_buffers[0], {b_.getInt64(0), x_loc, y_loc}),
           "output_element");
  Shape output_reduced_shape = ShapeUtil::MakeShapeWithDescendingLayout(
      output_shape.element_type(), mapping_scheme.GetDimsInElems());
  // When the output_reduced_shape is a 0-2-1 transpose of the input shape,
  // the 0-2-1 transpose is achieved through EmitWriteArrayElement.
  output_array.CastToShape(output_reduced_shape, &b_)
      .EmitWriteArrayElement(index, load_from_shmem_buffer, &b_);
}

static IrArray::Index GetUnnormalizedIndex(
    const IrArray::Index& normalized_shape_index,
    const Shape& unnormalized_shape, llvm::IRBuilder<>* b_,
    const KernelMappingScheme& kernel_mapping_scheme) {
  DCHECK_EQ(normalized_shape_index.size(), 3);
  // If the normalization only add a new dimensions of size 1,
  // generate simpler indexing. LLVM doesn't always simplify the more
  // complicated indexing and this prevents it from vectorizing some
  // cases. We do this only for major_to_minor memory layout.
  if (unnormalized_shape.rank() == 2 && unnormalized_shape.has_layout() &&
      unnormalized_shape.dimensions()[0] == normalized_shape_index.dims()[1] &&
      unnormalized_shape.dimensions()[1] == normalized_shape_index.dims()[2] &&
      unnormalized_shape.layout().minor_to_major(1) == 0) {
    CHECK_EQ(normalized_shape_index.dims()[0], 1);
    auto multidim = normalized_shape_index.multidim();
    return IrArray::Index({multidim[1], multidim[2]}, unnormalized_shape,
                          normalized_shape_index.GetType());
  }
  llvm::Value* linear = normalized_shape_index.Linearize(
      kernel_mapping_scheme.GetDimsInElems(), b_);
  return IrArray::Index(linear, unnormalized_shape, b_);
}

// Emits code to process a tensor element in a tile for the given kLoop fusion
// HLO containing parameters that are 0-2-1 transpose of its outputs.
//
// index: The index for the first output element in the normalized tensor, that
//   is the resulting tensor after collapsing contiguous dimensions that play
//   the same role in the transpose.
// kernel_info: Other information to support the kernel code generation.
void IrEmitterUnnested::EmitTileElementForFusion(
    mlir::lmhlo::FusionOp fusion,
    absl::Span<const llvm_ir::IrArray> operand_arrays,
    absl::Span<const llvm_ir::IrArray> output_arrays,
    const llvm_ir::IrArray::Index& index,
    const KernelMappingScheme& mapping_scheme, llvm::Value* y_loc,
    llvm::Value* x_loc, absl::Span<llvm::Value* const> param_shmem_buffers) {
  const HloComputation* fused_computation =
      *GetOrCreateSubComputationFromRegion(&fusion.region(),
                                           /*is_fusion*/ true);
  GpuElementalIrEmitter elem_emitter(hlo_module_config_, module_, &b_,
                                     GetNestedComputer());
  FusedIrEmitter fused_emitter(&elem_emitter);
  for (int i = 0; i < operand_arrays.size(); i++) {
    llvm_ir::ElementGenerator gen;
    if (llvm::Value* param_tile_buffer = param_shmem_buffers[i]) {
      gen = [this, param_tile_buffer, x_loc,
             y_loc](llvm_ir::IrArray::Index index) {
        // TODO(jlebar): Add AA metadata to this load.  Tile buffers are
        // global variables, so LLVM's points-to analysis doesn't help us
        // much.  And we want the AA info to be present before address
        // spaces are inferred (which is pretty late in the pipeline), so
        // even if we had address-space-based AA in LLVM, it wouldn't help
        // us much here.
        return b_.CreateLoad(
            b_.CreateGEP(param_tile_buffer,
                         {index.GetConstantWithIndexType(0), x_loc, y_loc}),
            "tiled_buffer");
      };
    } else {
      auto array = operand_arrays[i];
      gen = [this, array](llvm_ir::IrArray::Index index) {
        return array.EmitReadArrayElement(index, &b_);
      };
    }
    fused_emitter.BindGenerator(fused_computation->parameter_instruction(i),
                                std::move(gen));
  }
  IrArray::Index untiled_index = GetUnnormalizedIndex(
      index, output_arrays[0].GetShape(), &b_, mapping_scheme);
  llvm_ir::ElementGenerator output_generator =
      *fused_emitter.GetGenerator(fused_computation->root_instruction());
  llvm::Value* output_value = output_generator(untiled_index).ValueOrDie();
  if (output_arrays.size() > 1) {
    DCHECK(output_value->getType()->isStructTy());
    DCHECK_EQ(output_value->getType()->getStructNumElements(),
              output_arrays.size() - 1);
    for (int64 i = 0; i < output_arrays.size() - 1; ++i) {
      output_arrays[i].EmitWriteArrayElement(
          untiled_index, ExtractValue(output_value, i), &b_);
    }
  } else {
    output_arrays[0].EmitWriteArrayElement(untiled_index, output_value, &b_);
  }
}

void IrEmitterUnnested::EmitPrologueForReduction(
    HloInstruction* unnested_hlo, ReductionCodegenInfo* reduction_info,
    absl::Span<HloInstruction* const> reduce_instructions,
    llvm::Type* index_type) {
  VLOG(10) << "Emit prologue for reduction: " << unnested_hlo->ToString();
  GpuElementalIrEmitter elemental_emitter(hlo_module_config_,
                                          ir_emitter_context_->llvm_module(),
                                          &b_, GetNestedComputer());
  const HloInstruction* first_reduce = nullptr;
  for (int i = 0; i < reduce_instructions.size(); i++) {
    HloInstruction* reduce_inst = reduce_instructions[i];
    VLOG(10) << "Emit prologue for reduction: " << reduce_inst->ToString();
    if (first_reduce == nullptr) {
      first_reduce = reduce_inst;
    } else {
      CHECK(first_reduce->dimensions() == reduce_inst->dimensions());
    }

    AddressVector* reduction_input_addresses =
        reduction_info->GetMutableReductionInputAddresses();
    llvm::Type* element_type =
        llvm_ir::PrimitiveTypeToIrType(reduce_inst->shape().element_type(),
                                       ir_emitter_context_->llvm_module());
    llvm::AllocaInst* reduction_input_address =
        llvm_ir::EmitAllocaAtFunctionEntry(element_type,
                                           "reduction_input_address", &b_);
    reduction_input_addresses->push_back(reduction_input_address);

    int num_partial_results = reduction_info->GetNumPartialResults();
    AddressVector* partial_result_addresses =
        reduction_info->GetMutablePartialResultAddresses();
    llvm::AllocaInst* partial_result_address =
        llvm_ir::EmitAllocaAtFunctionEntryWithCount(
            element_type, /*ArraySize=*/b_.getInt32(num_partial_results),
            ("partial_reduction_result." + llvm::Twine(i)).str(), &b_);
    partial_result_addresses->push_back(partial_result_address);

    // Initialize the partial result with the initial value of the reduction.
    llvm::Value* init_ir_value;
    const HloInstruction* init_value = reduce_inst->operand(1);
    if (unnested_hlo->opcode() == HloOpcode::kFusion) {
      FusedIrEmitter fused_emitter(&elemental_emitter);
      BindFusionArguments(unnested_hlo, &fused_emitter);

      init_ir_value = (*fused_emitter.GetGenerator(init_value))(
                          IrArray::Index(b_.getInt32Ty()))
                          .ValueOrDie();
    } else {
      init_ir_value =
          GetIrArray(*init_value, *unnested_hlo)
              .EmitReadArrayElement(IrArray::Index(b_.getInt32Ty()), &b_);
    }

    for (int i = 0; i < num_partial_results; ++i) {
      Store(init_ir_value,
            InBoundsGEP(partial_result_address, {b_.getInt32(i)}));
    }
    reduction_info->GetMutableInitialValues()->push_back(init_ir_value);

    auto& mapping_scheme = reduction_info->GetKernelMappingScheme();
    int64 num_threads_x = mapping_scheme.GetNumThreadsX();
    llvm::Type* primitive_type = llvm_ir::PrimitiveTypeToIrType(
        reduce_inst->shape().element_type(), module_);
    llvm::Type* buffer_type = [&] {
      if (reduction_info->IsRowReduction()) {
        // Allocate __shared__ cache[num_partial_results][kWarpSize].
        return llvm::ArrayType::get(
            llvm::ArrayType::get(primitive_type, kWarpSize),
            num_partial_results);
      } else {
        // Allocate __shared__
        // cache[num_partial_results][num_threads][num_threads + 1], where
        // num_threads == num_threads_x == num_threads_y.  The "+1" is used to
        // avoid bank conflicts.
        CHECK_EQ(num_threads_x, mapping_scheme.GetNumThreadsY());
        return llvm::ArrayType::get(
            llvm::ArrayType::get(
                llvm::ArrayType::get(primitive_type, num_threads_x + 1),
                num_threads_x),
            num_partial_results);
      }
    }();
    llvm::GlobalVariable* shared_cache_per_reduce =
        llvm_ir::AllocateSharedMemoryTile(b_.GetInsertBlock()->getModule(),
                                          buffer_type,
                                          absl::StrCat("shared_cache_", i));
    reduction_info->GetMutableSharedCache()->push_back(shared_cache_per_reduce);
  }
}

void IrEmitterUnnested::EmitFullWarpShuffleDownLoopForAllReduces(
    absl::Span<HloComputation* const> reducers,
    absl::Span<llvm::AllocaInst* const> partial_result_addresses) {
  CHECK_EQ(reducers.size(), partial_result_addresses.size());
  for (int i = 0; i != reducers.size(); i++) {
    EmitFullWarpShuffleDownLoopForReduce(
        reducers[i], partial_result_addresses[i]->getType()->getElementType(),
        partial_result_addresses[i]);
  }
}

void IrEmitterUnnested::EmitFullWarpShuffleDownLoopForReduce(
    HloComputation* reducer, llvm::Type* element_type,
    llvm::Value* partial_result_address) {
  for (int distance = 16; distance >= 1; distance /= 2) {
    int bit_width = llvm_ir::GetSizeInBits(element_type);
    llvm::Value* result_from_other_lane = llvm_ir::EmitAllocaAtFunctionEntry(
        element_type, "result_from_other_lane", &b_);
    // Bitcast cannot be applied to aggregate types (even packed ones), so
    // we bitcast addresses of load/store to intN* of the same bit-width.
    llvm::Type* shuffled_value_type =
        element_type->isStructTy() ? b_.getIntNTy(bit_width) : element_type;
    auto convert_pointer_for_shuffle = [&](llvm::Value* ptr) {
      return b_.CreatePointerBitCastOrAddrSpaceCast(
          ptr, shuffled_value_type->getPointerTo());
    };
    llvm::Value* partial_result =
        Load(convert_pointer_for_shuffle(partial_result_address),
             "partial_reduction_result");
    Store(EmitFullWarpShuffleDown(partial_result, b_.getInt32(distance), &b_),
          convert_pointer_for_shuffle(result_from_other_lane));
    TF_CHECK_OK(EmitCallToNestedComputation(
        *reducer, {partial_result_address, result_from_other_lane},
        partial_result_address));
  }
}

// Given the IrArray index of a reduction input, returns the linear address of
// the reduction output as if the reduction were going to keep the input shape
// with the dimensions being reduced moved.
static llvm::Value* GetUntransposedOutputLinearAddress(
    llvm::IRBuilder<>* b, const llvm_ir::IrArray::Index& index,
    const ReductionCodegenInfo& reduction_info) {
  const KernelMappingScheme& kernel_mapping_scheme =
      reduction_info.GetKernelMappingScheme();
  if (reduction_info.IsRowReduction()) {
    // For row-reduction, y-coordinate determines which row we write into.
    return index[kDimY];
  }
  // For column reduction, we get the transposed address.
  absl::Span<const int64> dims_in_elem = kernel_mapping_scheme.GetDimsInElems();
  llvm::Value* x_dim_size = index.GetConstantWithIndexType(dims_in_elem[kDimX]);
  llvm::Value* x_block_offset = b->CreateMul(index[kDimZ], x_dim_size);
  return b->CreateAdd(x_block_offset, index[kDimX]);
}

void IrEmitterUnnested::EmitEpilogueForReduction(
    llvm::Type* index_ty, HloInstruction* unnested_hlo,
    const ReductionCodegenInfo& reduction_info,
    absl::Span<const HloInstruction* const> reduce_instructions,
    absl::Span<const ShapeIndex> reduction_output_shape_indices,
    absl::Span<HloComputation* const> reducers,
    const TilingKernelInfo& tiling_kernel_info) {
  const KernelMappingScheme& mapping_scheme =
      reduction_info.GetKernelMappingScheme();
  auto constant = [&](uint64 c) -> llvm::Constant* {
    return llvm::ConstantInt::get(index_ty, c);
  };

  IrEmitterUnnested::ThreadIdInfo thread_id_info =
      EmitThreadIdInfo(mapping_scheme.GetThreadsPerBlock(), index_ty,
                       mapping_scheme.GetNumThreadsX());

  IrArray::Index start_offset = [&] {
    llvm::Value* x_loc = thread_id_info.thread_id_x;
    llvm::Value* y_loc = thread_id_info.thread_id_y;
    if (!reduction_info.IsRowReduction()) {
      std::swap(x_loc, y_loc);
    }
    llvm::Value* start_offset_x =
        GetStartOffsetX(mapping_scheme, x_loc, index_ty, &b_);
    return tiling_kernel_info.tile_origin.AddOffsetToDim(y_loc, kDimY, &b_)
        .AddOffsetToDim(start_offset_x, kDimX, &b_);
  }();

  int num_reduces = reducers.size();
  absl::Span<llvm::AllocaInst* const> partial_result_addresses =
      reduction_info.GetPartialResultAddresses();

  int num_partial_results = reduction_info.GetNumPartialResults();

  // Emit an atomic operation that accumulates the partial reduction to the
  // output element. For row reduction, this is only for lane 0 due to the
  // if-statement emitted above.
  for (int i = 0; i != num_reduces; ++i) {
    const HloInstruction* reduce_hlo = reduce_instructions[i];
    Shape reduction_kept_element_shape = ShapeUtil::FilterDimensions(
        [&](int64 dim) {
          return !absl::c_linear_search(reduce_hlo->dimensions(), dim);
        },
        reduce_hlo->operand(0)->shape());
    for (int j = 0; j < num_partial_results; ++j) {
      llvm::Value* untransposed_output_linear_address =
          GetUntransposedOutputLinearAddress(
              &b_, start_offset.AddOffsetToDim(constant(j), kDimX, &b_),
              reduction_info);

      // A reduction is allowed to transpose its output.  For example, suppose
      // we are reducing the second dimension of f32[10,20,30]{3,2,1}.  We are
      // allowed to produce as output either f32[10,30]{1,0} (no transpose) or
      // f32[10,30]{0,1} (transposing the two output dims).
      //
      // At this point in the function we have a "partial sum" of input elements
      // (stored in partial_result_addresses), and we need to accumulate it into
      // the correct output element.
      auto output_array = GetIrArray(*unnested_hlo, *unnested_hlo,
                                     reduction_output_shape_indices[i]);
      IrArray::Index element_index(
          /*linear=*/untransposed_output_linear_address,
          reduction_kept_element_shape, &b_);
      IrArray::Index output_index(element_index.multidim(),
                                  output_array.GetShape(),
                                  element_index.GetType());
      llvm::Value* output_address = output_array.EmitArrayElementAddress(
          output_index, &b_, "output_element_address");
      llvm::Value* current_output = b_.CreateInBoundsGEP(
          partial_result_addresses[i], {constant(j)}, "current_output");

      llvm::GlobalVariable* shared_cache = reduction_info.GetSharedCache()[i];

      // __shared__ memory uses a different address space, so we cast it to
      // global address space before writing or reading.
      auto shared_to_global = [&](llvm::Value* input, llvm::Twine name = "") {
        return b_.CreateAddrSpaceCast(
            input,
            llvm::PointerType::get(input->getType()->getPointerElementType(),
                                   /*AddressSpace=*/0),
            name);
      };

      auto is_zero = [&](llvm::Value* value) {
        return b_.CreateICmpEQ(value, constant(0));
      };

      KernelSupportLibrary ksl(&b_);
      llvm::Type* element_type =
          partial_result_addresses[i]->getType()->getElementType();
      if (reduction_info.IsRowReduction()) {
        EmitFullWarpShuffleDownLoopForReduce(reducers[i], element_type,
                                             current_output);
        llvm::Value* warp_id =
            b_.CreateUDiv(thread_id_info.thread_id_x, constant(kWarpSize));
        ksl.If("intra_warp_reduce_write", is_zero(thread_id_info.lane_id), [&] {
          llvm::Value* shmem_output_addr =
              shared_to_global(b_.CreateInBoundsGEP(
                  shared_cache, {b_.getInt32(0), constant(j), warp_id}));
          b_.CreateStore(b_.CreateLoad(current_output), shmem_output_addr);
        });

        EmitSyncThreads();
        ksl.If("inter_warp_reduce", is_zero(warp_id), [&] {
          llvm::Value* block_accum_addr = shared_to_global(b_.CreateInBoundsGEP(
              shared_cache,
              {b_.getInt32(0), constant(j), thread_id_info.lane_id}));
          llvm::Value* initial_value = reduction_info.GetInitialValues()[i];
          llvm::Value* initial_value_addr =
              shared_to_global(llvm_ir::EmitAllocaAtFunctionEntry(
                  element_type, "initial_value_addr", &b_));
          b_.CreateStore(initial_value, initial_value_addr);

          llvm::Value* warp_exists = b_.CreateICmpULT(
              thread_id_info.thread_id_x,
              constant(mapping_scheme.GetNumThreadsX() / kWarpSize));

          llvm::Value* selected_value = b_.CreateSelect(
              warp_exists, block_accum_addr, initial_value_addr);

          EmitFullWarpShuffleDownLoopForReduce(
              reducers[i], element_type,
              /*block_accum_addr*/ selected_value);
          ksl.If("reduction_atomic_update", is_zero(thread_id_info.thread_id_x),
                 [&] {
                   TF_CHECK_OK(EmitAtomicOperationForNestedComputation(
                       *reducers[i], output_address, block_accum_addr));
                 });
        });

      } else {
        llvm::Value* shmem_output_addr = shared_to_global(
            b_.CreateInBoundsGEP(shared_cache, {b_.getInt32(0), constant(j),
                                                thread_id_info.thread_id_x,
                                                thread_id_info.thread_id_y}),
            "shmem_output_address");
        llvm::Value* current_output_value = b_.CreateLoad(current_output);
        b_.CreateStore(current_output_value, shmem_output_addr);

        EmitSyncThreads();

        // Get transposed element from shared memory.
        llvm::Value* shmem_transposed_addr =
            shared_to_global(b_.CreateInBoundsGEP(
                shared_cache,
                {b_.getInt32(0), constant(j), thread_id_info.thread_id_y,
                 thread_id_info.thread_id_x},
                "shmem_transposed_addr"));

        EmitFullWarpShuffleDownLoopForReduce(reducers[i], element_type,
                                             shmem_transposed_addr);

        // Some threads in the block are completely outside of the bound of the
        // tensor, so they should not write any output at all.
        llvm::Value* has_output = b_.CreateAnd(
            b_.CreateICmpULT(
                GetStartOffsetX(mapping_scheme, thread_id_info.thread_id_y,
                                index_ty, &b_),
                tiling_kernel_info.output_tile_bounds[kDimX]),
            b_.CreateICmpULT(thread_id_info.thread_id_x,
                             tiling_kernel_info.output_tile_bounds[kDimY]));

        ksl.If("reduction_atomic_update",
               b_.CreateAnd(has_output, is_zero(thread_id_info.lane_id)), [&] {
                 TF_CHECK_OK(EmitAtomicOperationForNestedComputation(
                     *reducers[i], output_address, shmem_transposed_addr));
               });
      }
    }
  }
}

llvm::Value* IrEmitterUnnested::EmitBlockId() {
  return gpu::EmitCallToTargetIntrinsic(gpu::TargetIntrinsicID::kBlockIdx, {},
                                        {}, &b_);
}

void IrEmitterUnnested::EmitPrintfWithThreadId(
    absl::string_view fmt, absl::Span<llvm::Value* const> arguments,
    absl::optional<int64> thread_id_filter,
    absl::optional<int64> block_id_filter) {
  llvm::Value* thread_id = EmitThreadId(1024, b_.getInt32Ty());
  llvm::Value* block_id = EmitBlockId();
  std::vector<llvm::Value*> updated_arguments = {thread_id, block_id};
  updated_arguments.insert(updated_arguments.end(), arguments.begin(),
                           arguments.end());
  llvm::Value* constraint = b_.getTrue();
  if (thread_id_filter) {
    constraint = b_.CreateAnd(
        constraint, b_.CreateICmpEQ(thread_id, b_.getInt32(*thread_id_filter)));
  }
  if (block_id_filter) {
    constraint = b_.CreateAnd(
        constraint, b_.CreateICmpEQ(block_id, b_.getInt32(*block_id_filter)));
  }
  KernelSupportLibrary ksl(&b_, llvm_ir::UnrollMode::kDefaultUnroll);
  ksl.If(constraint, [&] {
    ::xla::gpu::EmitPrintf(absl::StrCat("[TID=%d,BID=%d] ", fmt, "\n"),
                           updated_arguments, &b_);
  });
}

namespace {

// Obtains the corresponding index of the out_instr in the outputs of the
// `unnested_hlo`.
ShapeIndex CreateShapeIndexForOutputInstruction(
    const HloInstruction& unnested_hlo, const HloInstruction& out_instr) {
  if (!unnested_hlo.IsMultiOutputFusion()) {
    return ShapeIndex({});
  }
  const auto& all_outputs = unnested_hlo.fused_expression_root()->operands();
  for (size_t i = 0; i < all_outputs.size(); ++i) {
    if (all_outputs[i] == &out_instr) {
      return ShapeIndex({static_cast<int64>(i)});
    }
  }
  LOG(FATAL) << " Fusion root does not contain output instruction; "
             << " fusion: " << unnested_hlo.ToString()
             << ", output instruction: " << out_instr.ToString();
}

}  // namespace

void IrEmitterUnnested::EmitTileElementForReduction(
    HloInstruction* unnested_hlo, const Shape& reduction_operand_shape,
    absl::Span<HloInstruction* const> output_instructions,
    const llvm_ir::IrArray::Index& index,
    const ReductionCodegenInfo& reduction_info,
    absl::Span<HloComputation* const> reducers, int64 x_iter_num) {
  VLOG(10) << "Emit tile element for reduce " << unnested_hlo->ToString();
  int partial_result_index = reduction_info.IsRowReduction() ? 0 : x_iter_num;

  InlinedVector<llvm_ir::ElementGenerator, 1> input_gens;
  std::vector<std::pair<llvm_ir::ElementGenerator, ShapeIndex>>
      extra_output_gens;
  GpuElementalIrEmitter elem_emitter(hlo_module_config_, module_, &b_,
                                     GetNestedComputer());
  FusedIrEmitter fused_emitter(&elem_emitter);

  // Construct the ElementGenerator for each reduction and extra output in the
  // the group of output instructions.
  if (unnested_hlo->opcode() == HloOpcode::kFusion) {
    BindFusionArguments(unnested_hlo, &fused_emitter);

    for (int i = 0, e = output_instructions.size(); i != e; ++i) {
      const HloInstruction* inst = output_instructions[i];
      ShapeIndex idx =
          CreateShapeIndexForOutputInstruction(*unnested_hlo, *inst);
      if (IsReductionFromOrToContiguousDimensions(*inst)) {
        input_gens.push_back(*fused_emitter.GetGenerator(inst->operand(0)));
      } else {
        extra_output_gens.emplace_back(*fused_emitter.GetGenerator(inst),
                                       std::move(idx));
      }
    }
  } else {
    input_gens.push_back([&](const IrArray::Index& index) {
      return GetIrArray(*unnested_hlo->operand(0), *unnested_hlo)
          .EmitReadArrayElement(index, &b_);
    });
  }

  IrArray::Index input_index =
      GetUnnormalizedIndex(index, reduction_operand_shape, &b_,
                           reduction_info.GetKernelMappingScheme());
  // Clear the linear index field of the IrArray::Index to enable the use of
  // GetElementPointer with array types. This enables the vectorization of
  // the computation for different partial results. Use this index if
  // 'num_partial_results > 1'.
  int num_partial_results = reduction_info.GetNumPartialResults();
  auto index_without_linear = IrArray::Index(
      input_index.multidim(), reduction_operand_shape, input_index.GetType());

  // Emit code to generate the input and perform the reduction computation for
  // each reduction instruction.
  for (int i = 0; i != reducers.size(); ++i) {
    llvm::AllocaInst* input_address =
        reduction_info.GetReductionInputAddresses()[i];
    llvm::AllocaInst* partial_reduction_result_address =
        reduction_info.GetPartialResultAddresses()[i];
    llvm::Value* const input_ir_value =
        input_gens[i](num_partial_results > 1 ? index_without_linear
                                              : input_index)
            .ValueOrDie();
    Store(input_ir_value, input_address);
    llvm::Value* partial_result_address = InBoundsGEP(
        partial_reduction_result_address, {b_.getInt32(partial_result_index)});
    TF_CHECK_OK(EmitCallToNestedComputation(
        *reducers[i], {partial_result_address, input_address},
        partial_result_address));
  }

  // Emit code to generate the output for the non-reduction instructions in the
  // fusion, if any.
  TF_CHECK_OK(EmitExtraOutputsForReduce(
      unnested_hlo, input_index,
      /*use_linear_index=*/num_partial_results == 1, extra_output_gens));
}

llvm::Value* IrEmitterUnnested::EmitThreadId(int64 threads_per_block,
                                             llvm::Type* index_ty) {
  // Calculate (y, x) coordinates respectively in the 2D view of thread block,
  // defined by (num_thread_y, num_thread_x) from thread_id.
  llvm::CallInst* thread_id_raw = gpu::EmitCallToTargetIntrinsic(
      gpu::TargetIntrinsicID::kThreadIdx, {}, {}, &b_);
  llvm_ir::AddRangeMetadata(0, threads_per_block, thread_id_raw);
  return b_.CreateIntCast(thread_id_raw, index_ty,
                          /*isSigned=*/true, "thread.id.x");
}

IrEmitterUnnested::ThreadIdInfo IrEmitterUnnested::EmitThreadIdInfo(
    int64 threads_per_block, llvm::Type* index_ty, int64 num_threads_x) {
  auto constant = [&](uint64 c) -> llvm::Constant* {
    return llvm::ConstantInt::get(index_ty, c);
  };
  llvm::Value* thread_id = EmitThreadId(threads_per_block, index_ty);
  llvm::Value* num_threads_x_v = constant(num_threads_x);
  return {
      /*thread_id=*/thread_id,
      /*thread_id_x=*/b_.CreateURem(thread_id, num_threads_x_v, "thread_id.x"),
      /*thread_id_y=*/b_.CreateUDiv(thread_id, num_threads_x_v, "thread_id.y"),
      /*lane_id=*/b_.CreateURem(thread_id, constant(kWarpSize), "lane_id")};
}

IrEmitterUnnested::TilingKernelInfo IrEmitterUnnested::EmitTilingKernel(
    const KernelMappingScheme& mapping_scheme, llvm::Type* index_ty,
    const TileElementGenerator& tile_element_generator) {
  absl::Span<const int64> dims_in_elems = mapping_scheme.GetDimsInElems();
  std::vector<int64> dims_in_blocks = {
      CeilOfRatio(dims_in_elems[0], mapping_scheme.GetTileSizeZ()),
      CeilOfRatio(dims_in_elems[1], mapping_scheme.GetTileSizeY()),
      CeilOfRatio(dims_in_elems[2], mapping_scheme.GetTileSizeX())};
  auto constant = [&](uint64 c) -> llvm::Constant* {
    return llvm::ConstantInt::get(index_ty, c);
  };

  IrEmitterUnnested::ThreadIdInfo thread_id_info =
      EmitThreadIdInfo(mapping_scheme.GetThreadsPerBlock(), index_ty,
                       mapping_scheme.GetNumThreadsX());

  KernelSupportLibrary ksl(&b_, llvm_ir::UnrollMode::kDefaultUnroll);

  const IrArray::Index block_coords = [&] {
    llvm::Value* block_id = EmitBlockId();
    llvm_ir::AddRangeMetadata(0, mapping_scheme.GetNumberOfBlocks(),
                              llvm::cast<llvm::Instruction>(block_id));
    llvm::Value* linear_block_id =
        b_.CreateIntCast(block_id, index_ty, /*isSigned=*/true, "block.id.x");
    IrArray::Index starting_block(linear_block_id,
                                  ShapeUtil::MakeShapeWithDescendingLayout(
                                      PRED /*arbitrary*/, dims_in_blocks),
                                  &b_);

    std::vector<llvm::Value*> multidim = {
        b_.CreateMul(starting_block[0], constant(mapping_scheme.GetTileSizeZ()),
                     "block_origin.z"),
        starting_block[1], starting_block[2]};
    return IrArray::Index(multidim, dims_in_blocks, index_ty);
  }();

  std::array<llvm::Value*, 3> output_tile_bounds;
  for (int i = kDimY; i < kDimTot; ++i) {
    int64 tile_size_for_dim = mapping_scheme.GetTileSizeFor(i);
    // Only last row or column may not have full size.
    llvm::Value* is_last =
        b_.CreateICmpEQ(block_coords[i], constant(dims_in_blocks[i] - 1));
    int64 partial_row =
        dims_in_elems[i] - (dims_in_blocks[i] - 1) * tile_size_for_dim;
    output_tile_bounds[i] =
        b_.CreateSelect(is_last, constant(partial_row),
                        constant(tile_size_for_dim), "tile_bound");
  }

  IrArray::Index tile_origin = [&] {
    std::vector<llvm::Value*> elem_multi_index = block_coords.multidim();
    llvm::Type* index_ty = block_coords.GetType();
    for (int i = kDimY; i < kDimTot; ++i) {
      elem_multi_index[i] = b_.CreateMul(
          block_coords[i],
          llvm::ConstantInt::get(index_ty, mapping_scheme.GetTileSizeFor(i)),
          "tile_origin." + std::to_string(i));
    }
    return IrArray::Index(elem_multi_index, mapping_scheme.GetDimsInElems(),
                          index_ty);
  }();

  auto emit_tile = [&](const IrArray::Index& tile) {
    tile_element_generator(thread_id_info, tile, "output",
                           output_tile_bounds[1], output_tile_bounds[2], &ksl);
  };

  if (mapping_scheme.GetTileSizeZ() == 1) {
    emit_tile(tile_origin);
  } else {
    llvm::Value* starting_tile_index_for_dim = tile_origin[kDimZ];
    llvm::Value* block_size_for_dim = constant(mapping_scheme.GetTileSizeZ());
    llvm::Value* block_id_for_dim =
        b_.CreateUDiv(starting_tile_index_for_dim, block_size_for_dim);
    llvm::Value* last_block_for_dim = constant(dims_in_blocks[kDimZ] - 1);
    llvm::Value* last_block_size_for_dim =
        constant(dims_in_elems[kDimZ] -
                 (dims_in_blocks[kDimZ] - 1) * mapping_scheme.GetTileSizeZ());

    llvm::Value* num_tiles_in_block =
        b_.CreateSelect(b_.CreateICmpEQ(last_block_for_dim, block_id_for_dim),
                        last_block_size_for_dim, block_size_for_dim);
    ksl.For("loop_z",
            /*start=*/constant(0),
            /*end=*/num_tiles_in_block,
            /*step=*/1, [&](llvm::Value* block_dim_induction_var) {
              IrArray::Index tile_index = tile_origin.AddOffsetToDim(
                  block_dim_induction_var, kDimZ, &b_);
              emit_tile(tile_index);
            });
  }
  return {output_tile_bounds, tile_origin};
}

llvm::CallInst* IrEmitterUnnested::EmitSyncThreads() {
  return EmitCallToTargetIntrinsic(TargetIntrinsicID::kBarrierId, {}, {}, &b_);
}

// Emits a kernel for the given hlo instruction using a tiled 0-2-1 transpose
// algorithm to improve the memory access patterns for the input parameters
// with a shape that is a 0-2-1 transpose of the output tensor shape. The caller
// is responsible for making sure that it is safe to apply the shared memory
// transpose on the input parameters.
//
//
// For the purpose of tiling, the output tensors have a logical shape of three
// components 0-2-1 while the relevant input parameters have a logical shape
// of three components 0-1-2 in the order major to minor. The x- and y-
// dimensions of the tensors are tiled in square tiles with an edge length
// `kTileSize`. Each thread block of `kTileSize` x `kNumRows` threads
// transposes one tile: each thread copies kTileSize/kNumRows elements from
// the input to a shared memory tile, then the otherwise "regular HLO kernel"
// reads from the shared memory instead of the original input.
//
// This is similar to the following CUDA algorithm in TensorFlow:
// https://goo.gl/MStRV6.
//
// `kTileSize` should usually be same as warp size. We currently choose 32 for
// `kTileSize` and 4 for `kNumRows`. The CUDA algorithm uses 8 for `kNumRows`.
//
// TODO(b/33320379): Here each block transposes 1 tile. It may be more
// efficient to launch fewer blocks so each transposes many tiles.
void IrEmitterUnnested::EmitHlo021Tile(
    mlir::Operation* op, Thunk* kernel_thunk, const MlirEmitterContext& context,
    absl::Span<const llvm_ir::IrArray> operand_arrays,
    absl::Span<const llvm_ir::IrArray> output_arrays,
    absl::Span<const int64> reduced_output_dims,
    absl::Span<const int64> tiled_param_ids) {
  constexpr int kNumRows = 4;

  std::string name = mlir::GetNameFromLoc(op->getLoc());

  KernelMappingScheme mapping_scheme(reduced_output_dims,
                                     /*tile_sizes=*/{1, kWarpSize, kWarpSize},
                                     /*num_threads_y=*/kNumRows,
                                     /*num_threads_x=*/kWarpSize,
                                     /*indexing_order=*/kLinearIndexingX,
                                     /*vector_size=*/1,
                                     /*is_row_contiguous=*/false);
  LaunchDimensions launch_dimensions(mapping_scheme.GetNumberOfBlocks(),
                                     mapping_scheme.GetThreadsPerBlock());

  llvm::Type* index_type =
      GetIndexTypeForKernelFromMlir(op, launch_dimensions.launch_bound(), &b_);
  std::vector<IrArray> param_arrays;

  // For each tiled parameter, cast its input IrArray to the corresponding
  // reduced shape and keep the reduced shape live during IR emission.
  std::vector<IrArray> param_in_reduced_shape_arrays;
  std::vector<llvm::Value*> param_shmem_buffers(context.operand_shapes.size(),
                                                nullptr);

  auto get_shared_memory_buffer = [&](llvm::Type* elem_ty,
                                      absl::string_view buffer_name) {
    // For Nvidia GPUs, the warp size is 32 threads and the shared memory bank
    // is organized into 32-way. We usually use the warp size or a multiplier or
    // a the warp size as the size for tiling. This may cause all elements in
    // the same column of a tile use the same memory bank and therefore shared
    // memory bank conflicts. Adding 1 to the minor dimension of the shared
    // memory buffer can reduce such shared memory bank conflicts.
    llvm::Type* buffer_type = llvm::ArrayType::get(
        llvm::ArrayType::get(elem_ty, mapping_scheme.GetTileSizeX() + 1),
        mapping_scheme.GetTileSizeY());
    return llvm_ir::AllocateSharedMemoryTile(b_.GetInsertBlock()->getModule(),
                                             buffer_type, buffer_name);
  };

  for (int64 id = 0; id < context.operand_shapes.size(); id++) {
    const Shape& param_shape = context.operand_shapes[id];
    param_arrays.push_back(operand_arrays[id]);

    if (absl::c_linear_search(tiled_param_ids, id)) {
      param_shmem_buffers[id] = get_shared_memory_buffer(
          llvm_ir::PrimitiveTypeToIrType(param_shape.element_type(), module_),
          IrName(name, StrCat("tile", id)));
      VLOG(3) << "Added shmem buffer for parameter " << id << ": "
              << llvm_ir::DumpToString(*param_shmem_buffers[id]);
      Shape reduced_shape = ShapeUtil::MakeShapeWithDescendingLayout(
          param_shape.element_type(), Permute({0, 2, 1}, reduced_output_dims));
      param_in_reduced_shape_arrays.push_back(
          param_arrays[id].CastToShape(reduced_shape, &b_));
    } else {
      param_in_reduced_shape_arrays.push_back(IrArray());
    }
  }

  EmitElementFunction element_generator =
      [&](const llvm_ir::IrArray::Index& index, llvm::Value* y_loc,
          llvm::Value* x_loc, int64 x_iter_num) {
        if (auto copy = mlir::dyn_cast<mlir::lmhlo::CopyOp>(op)) {
          CHECK_EQ(1, context.output_shapes.size());
          EmitTileElementForCopy(context.output_shapes[0], output_arrays[0],
                                 index, mapping_scheme, y_loc, x_loc,
                                 param_shmem_buffers);
        } else if (auto fusion = mlir::dyn_cast<mlir::lmhlo::FusionOp>(op)) {
          EmitTileElementForFusion(fusion, operand_arrays, output_arrays, index,
                                   mapping_scheme, y_loc, x_loc,
                                   param_shmem_buffers);
        } else {
          op->dump();
          LOG(FATAL) << "Unexpected op type";
        }
      };

  TileElementGenerator tile_generator =
      [&](const ThreadIdInfo& thread_id_info, const IrArray::Index& index,
          const string& loop_name, llvm::Value* tile_height,
          llvm::Value* tile_width, KernelSupportLibrary* ksl) {
        // If shared memory transpose is needed, wait for all threads to reach
        // this point, lest we copy a value from tile to output before the other
        // thread copies it from input to tile. This is `__syncthreads` in CUDA.
        if (!tiled_param_ids.empty()) {
          // Calculate the input tile origin from the output tile origin.
          const IrArray::Index input_tile_origin(
              Permute({0, 2, 1}, index.multidim()),
              Permute({0, 2, 1}, index.dims()), index.GetType());

          // Copy input parameter values to shared memory buffers:
          // tile[thread_id_y, thread_id_x] = input[index]
          // Note that tile_width and tile_height are flipped here because we
          // are reading a transposed tile.
          EmitTile(mapping_scheme, input_tile_origin, "input", ksl,
                   thread_id_info, tile_width, tile_height,
                   [&](const IrArray::Index& index, llvm::Value* y_loc,
                       llvm::Value* x_loc, int64 /*x_iter_num*/) {
                     for (int64 id : tiled_param_ids) {
                       IrArray& input_in_logical_shape =
                           param_in_reduced_shape_arrays.at(id);

                       llvm::Value* shmem_buffer = param_shmem_buffers.at(id);
                       llvm::Value* zero =
                           llvm::ConstantInt::get(index_type, 0);
                       // TODO(jlebar): Add AA metadata to this store.  Tile
                       // buffers are global variables, so LLVM can't infer much
                       // about it.
                       auto value = input_in_logical_shape.EmitReadArrayElement(
                           index, &b_, "input_element");
                       auto addr = GEP(shmem_buffer, {zero, y_loc, x_loc});
                       Store(value, addr);
                     }
                   });

          // Wait for all threads to reach this point using `__syncthreads` in
          // CUDA.
          EmitSyncThreads();
        }

        EmitTile(mapping_scheme, index, loop_name, ksl, thread_id_info,
                 tile_height, tile_width, element_generator);
        bool block_contains_multi_tiles = mapping_scheme.GetTileSizeZ() > 1;

        // If a tile block contains multiple tiles and shared memory buffers are
        // used, we need to wait for all threads to finish using the shared
        // memory buffer for the current tile before we move on to process the
        // next tile and overwrite the shared memory buffers.
        if (block_contains_multi_tiles && !tiled_param_ids.empty()) {
          EmitSyncThreads();
        }
      };

  // For multioutput fusion, one thread needs to output a tuple
  // with pointers to all the individual outputs.  We could do this
  // at any point in the kernel, but we do it at the beginning in
  // the hopes of reducing register pressure, since we touch
  // threadIdx.x and blockIdx.x at the beginning of the kernel
  // *anyway*.
  if (output_arrays.size() > 1) {
    KernelSupportLibrary{&b_}.If("emit_mof_tuple", IsBlock0Thread0(&b_), [&] {
      llvm_ir::EmitTuple(output_arrays.back(),
                         output_arrays.subspan(0, output_arrays.size() - 1),
                         &b_);
    });
  }

  EmitTilingKernel(mapping_scheme, index_type, tile_generator);
  UpdateLaunchDimensions(launch_dimensions, kernel_thunk,
                         ir_emitter_context_->llvm_module());
}

namespace {

// A recursive function to inspect the users of a parameter to determine
// whether it's safe for a parameter to participate in a shared-memory
// transpose.
//
// Consider a fusion parameter P for which we might want to use a shmem
// transpose.  If we do, we use a GPU thread block to preload a tile of P with
// indices [z, y..y+31, x..x+31] to compute an output tile with the same indices
// cooperatively, where z, y, x are the indices for the normalized input/output
// tensor (see the document for FindTranspose021 for the definition of
// normalized tensor for 0-2-1 transpose). This shmem transpose implementation
// requires that the computation of the output tile only read elements within
// the preload tile. If this is not true, we can't use a shmem transpose for P.
//
// If the computation of output element [z, y, x] only requires the element of
// P with the same indices, the shmem transpose implementation can be applied
// to P safely. This is a sufficient but not necessary condition. We check all
// the transitive users of P to see if we can find a user that may cause an
// exception to the situation. If such a user is not found, we conclude that P
// is safe for shmem transpose.
//
// This is trivially true for elementwise operations and some "data-movement"
// ops like kTuple. However, it's not true for operations that can change the
// dimensions of the inputs (e.g. pad, slice) and bitcast operation.
// For example:
//
// fused_computation {
//   param_0 = f32[64,64]{1,0} parameter(0)
//   ROOT bitcast = f32[64,64]{0,1} bitcast(param_0)
// }
// The output element at logical address [0, 63] depends on the input element
// at logical address [63, 0], which would not be within the shared-memory
// block.
//
// TODO(bixia): In order to extend this for kInput fusion, that is reduction
// with transpose, we only need to end the use-chain checking with the input of
// a reduce operations. In this case, the above description on "output" apply
// to the result of such a use-chain, which provides the input to the reduce
// operation.
bool IsInstructionSafeForShmemTranspose(mlir::Operation* op) {
  if (mlir::isa<mlir::TensorStoreOp>(op)) {
    return true;
  }

  HloOpcode opcode;
  if (mlir::isa<mlir::TensorLoadOp>(op)) {
    opcode = HloOpcode::kParameter;
  } else {
    opcode = *mlir::MhloToHloOpcode(op);
  }
  if (HloInstruction::IsOpElementwise(opcode)) {
    for (mlir::Value v : op->getResults()) {
      for (mlir::OpOperand use : v.getUsers()) {
        if (!IsInstructionSafeForShmemTranspose(use.getOwner())) {
          return false;
        }
      }
    }
    return true;
  }

  switch (opcode) {
    // Non-elementwise instructions that don't cause the shmem transpose
    // to be unsafe, including the instructions that don't currently fuse.
    case HloOpcode::kGetDimensionSize:
      // The result of the operation doesn't rely on the content of the
      // tensor. As such, there is no need to further inspect its users.
      return true;
    case HloOpcode::kGetTupleElement:
    case HloOpcode::kMap:
    case HloOpcode::kParameter:
    case HloOpcode::kTuple:
    case HloOpcode::kTupleSelect:
      for (mlir::Value v : op->getResults()) {
        for (mlir::OpOperand use : v.getUsers()) {
          if (!IsInstructionSafeForShmemTranspose(use.getOwner())) {
            return false;
          }
        }
      }
      return true;

    default:
      return false;
  }
}

// Given a group of input parameters that are 0-2-1 transpose of the outputs of
// a fusion kernel, returns the input parameters that are safe for the shared
// memory transpose implementation.
//
// When a tile based shared memory transpose is used to implement an input with
// 0-2-1 transpose, we preload a tile of the input elements
// [z, y..y+31, x..x+31] to compute the output tile elements of the same
// indices. Preloading the input tile this way is only safe when the computation
// of the output tile elements do not need any input element outside the
// preloaded tile. We inspect all the transitive users of the input parameter
// up to the fusion root instruction to see if we can find any instruction
// that can make preloading the input tile unsafe.
std::vector<int64> FilterInputsForShmemTranspose(mlir::lmhlo::FusionOp fusion,
                                                 std::vector<int64> input_ids) {
  std::vector<mlir::Value> params;
  fusion.region().walk([&](mlir::TensorLoadOp load) {
    CHECK(load.memref().getParentRegion() != &fusion.region())
        << "TensorLoadOp shows should be only expected for accessing captured "
           "memrefs.";
    params.push_back(load);
  });

  std::vector<int64> filtered_input_ids;
  for (int64 input_id : input_ids) {
    mlir::Value input = params.at(input_id);
    if (IsInstructionSafeForShmemTranspose(input.getDefiningOp())) {
      filtered_input_ids.push_back(input_id);
    }
  }
  return filtered_input_ids;
}

}  // namespace

StatusOr<bool> IrEmitterUnnested::CheckAndEmitHloWithTile021(
    MlirEmitterInput input) {
  CHECK(mlir::isa<mlir::lmhlo::FusionOp>(input.op) ||
        mlir::isa<mlir::lmhlo::CopyOp>(input.op));

  MlirEmitterContext context;
  context.SetOperation(input.op);

  // If the output_shape is reduced to 021 shape, find all the parameters of
  // the HLO that are in the corresponding 012 shape.
  std::vector<int64> params_012;
  optional<std::vector<int64>> reduced_dims_021;
  for (int64 operand_idx = 0; operand_idx < context.operand_shapes.size();
       ++operand_idx) {
    const Shape& operand_shape = context.operand_shapes[operand_idx];
    auto find_transpose_result =
        ShapeUtil::FindTranspose021(operand_shape, context.output_shapes[0]);
    if (!find_transpose_result.has_value()) {
      continue;
    }
    const std::vector<int64>& curr_reduced_dims_021 = *find_transpose_result;
    if (!reduced_dims_021.has_value()) {
      reduced_dims_021 = curr_reduced_dims_021;
    }
    if (!absl::c_equal(*reduced_dims_021, curr_reduced_dims_021)) {
      // There is more than one possible transpose. Instead of picking one
      // transpose, we simply give up here.
      return false;
    }
    params_012.push_back(operand_idx);
  }

  if (!reduced_dims_021.has_value()) {
    return false;
  }

  if ((*reduced_dims_021)[1] < kMinDimensionToTransposeTiled ||
      (*reduced_dims_021)[2] < kMinDimensionToTransposeTiled) {
    return false;
  }

  if (auto fusion_op = mlir::dyn_cast<mlir::lmhlo::FusionOp>(input.op)) {
    params_012 = FilterInputsForShmemTranspose(fusion_op, params_012);
    if (params_012.empty()) {
      return false;
    }
  }

  // Each of our shared memory tiles has 32*33 elements (so ~4kb, if the
  // elements are of size 4 bytes), and CUDA has an architectural limit of
  // 48kb shared memory per SM.  (This is increased to 96kb in Volta, but we
  // don't use this, in part because it eats into our L1 cache space.)
  //
  // For correctness we need to ensure that we don't make more than 48kb worth
  // of shmem tiles per block.  And for performance, we'd probably like to use
  // significantly less, so that we can fit more than one block at a time on a
  // gpu core.
  //
  // We say without benchmarks that we want at least 3 threads/block,
  // corresponding to 3 shmem tiles if the elements are 32 bits wide.  We
  // choose which params get the shmem transpose treatment arbitrarily; it's
  // not clear if there's a Right Choice.
  //
  // This is only sound if tiled transposes are the only place where we use
  // shared memory in fusions.  If in the future other fusible ops use shared
  // memory, we'll have to adjust this heuristic.
  constexpr int kMinBlocksPerCore = 3;
  constexpr int64 kShmemPerCore = 48 * 1024;
  int64 shmem_used = 0;
  for (int64 i = 0; i < params_012.size(); ++i) {
    const Shape& operand_shape = context.operand_shapes[params_012[i]];
    shmem_used +=
        32 * 33 *
        ShapeUtil::ByteSizeOfPrimitiveType(operand_shape.element_type());

    if (kMinBlocksPerCore * shmem_used > kShmemPerCore) {
      // Erase this element and everything after it from params_012.
      params_012.resize(i);
      break;
    }
  }

  if (params_012.empty()) {
    return false;
  }

  std::vector<llvm_ir::IrArray> ir_arrays;
  TF_ASSIGN_OR_RETURN(std::unique_ptr<KernelThunk> kernel_thunk,
                      BuildKernelThunkForMlir(input.op, input.thunk_info,
                                              input.extra_slice, &ir_arrays));
  EmitHlo021Tile(
      input.op, kernel_thunk.get(), context,
      absl::MakeSpan(ir_arrays).subspan(0, context.operand_shapes.size()),
      absl::MakeSpan(ir_arrays).subspan(context.operand_shapes.size()),
      *reduced_dims_021, params_012);
  AddThunkToThunkSequence(std::move(kernel_thunk));
  return true;
}

namespace {

// Returns true if all the transitive users of hlo before hitting users in
// use_chain_endings are elementwise operations.
bool AreUsersElementwise(const HloInstruction* hlo,
                         const ConstHloInstructionSet& use_chain_endings) {
  return absl::c_all_of(hlo->users(), [&](const HloInstruction* user) {
    return use_chain_endings.count(user) ||
           (user->IsElementwise() &&
            AreUsersElementwise(user, use_chain_endings));
  });
}

// Returns the number of fusion inputs that have the same dimension as the
// given shape, and involve in only elementwise operations.
int64 NumInputsInvolveInOnlyElementwiseOps(
    const HloInstruction* unnested_hlo, const Shape& op_shape,
    const ConstHloInstructionSet& use_chain_endings) {
  return absl::c_count_if(
      unnested_hlo->fused_parameters(), [&](const HloInstruction* parameter) {
        const Shape& parameter_shape = parameter->shape();
        return ShapeUtil::SameDimensions(op_shape, parameter_shape) &&
               AreUsersElementwise(parameter, use_chain_endings);
      });
}

// Returns the number of fusion inputs that have more elements than the given
// shape.
int64 NumInputsWithMoreElementsThan(const HloInstruction* unnested_hlo,
                                    const Shape& shape) {
  int64 num_elements = ShapeUtil::ElementsIn(shape);
  return absl::c_count_if(
      unnested_hlo->fused_parameters(), [&](const HloInstruction* parameter) {
        return ShapeUtil::ElementsIn(parameter->shape()) > num_elements;
      });
}

// The benefit of unrolling a kInput fusion that is a column reduction comes
// from the vectorization of non-reduction fusion outputs and fusion inputs.
// On the other hand, unrolling can also introduce factors that can cause
// the kernel to run slower. This routine uses a simple heuristic to estimate
// the benefit as well as the overhead of unrolling in order to decide whether
// unrolling is beneficial for the given kInput fusion.
bool IsUnrollingColumnReductionBeneficial(const HloInstruction* unnested_hlo,
                                          const Shape& input_shape,
                                          int64 num_kept_minor) {
  // TODO(b/122468062): Need further investigate to see whether we can
  // remove the constraint on IsPowerOfTwo.
  if (!IsPowerOfTwo(static_cast<uint64>(num_kept_minor))) {
    return false;
  }

  if (IsReductionFromOrToContiguousDimensions(*unnested_hlo)) {
    return true;
  }

  CHECK_EQ(unnested_hlo->opcode(), HloOpcode::kFusion);
  int64 can_be_vectorized = 0;
  int64 cannot_be_vectorized = 0;
  const HloInstruction* fused_root = unnested_hlo->fused_expression_root();
  ConstHloInstructionSet use_chain_endings;
  if (IsReductionFromOrToContiguousDimensions(*fused_root)) {
    use_chain_endings.insert(fused_root);
    // Atomic.add of the reduction result can't be vectorized.
    cannot_be_vectorized++;
  } else {
    CHECK_EQ(fused_root->opcode(), HloOpcode::kTuple);
    for (const HloInstruction* instr : fused_root->operands()) {
      if (IsReductionFromOrToContiguousDimensions(*instr)) {
        // Atomic.add of the reduction result can't be vectorized.
        cannot_be_vectorized++;
      } else {
        // Write of the non-reduction result can be vectorized.
        can_be_vectorized++;
      }
      use_chain_endings.insert(instr);
    }
  }
  // Fusion inputs that have the same dimension as the reduce input and
  // only involve in elementwise operations can be vectorized.
  can_be_vectorized += NumInputsInvolveInOnlyElementwiseOps(
      unnested_hlo, input_shape, use_chain_endings);
  // Fusion inputs with more elements than the reduce op input must participate
  // in non-elementwise operations and we assume that they are not vectorizable
  // for the purpose of estimating the benefit of unrolling. If the kernel is
  // unrolled even with such an assumption,  and the accesses to those inputs
  // turn out to be vectorizable, the compiler will still vectorize them.
  cannot_be_vectorized +=
      NumInputsWithMoreElementsThan(unnested_hlo, input_shape);
  return can_be_vectorized >= cannot_be_vectorized;
}

int64 NearestPowerOfTwo(int64 v) {
  if (v < 0) {
    return 0;
  }
  int64 upper = tensorflow::NextPowerOfTwo64(v);
  int64 lower = upper >> 1;
  return upper - v < v - lower ? upper : lower;
}

}  // namespace

ReductionCodegenInfo IrEmitterUnnested::ComputeReductionCodegenInfo(
    const HloInstruction* unnested_hlo, const HloInstruction* first_reduce) {
  const Shape& input_shape = first_reduce->operand(0)->shape();
  ReductionDimensions reduction_dimensions =
      GetReductionKindAndContiguousComponents(*first_reduce);
  VLOG(10) << "is_row_reduction " << reduction_dimensions.is_row_reduction
           << " " << reduction_dimensions.dimensions[0] << " "
           << reduction_dimensions.dimensions[1] << " "
           << reduction_dimensions.dimensions[2];
  auto get_dtype_bits = [](const HloInstruction* i) {
    return primitive_util::BitWidth(i->shape().element_type());
  };

  // For fusion with multiple inputs, use the smallest input dtype to
  // select the reduction_tiling.
  int smallest_input_dtype_bits = get_dtype_bits(first_reduce->operand(0));
  for (xla::HloInstruction* input : unnested_hlo->operands()) {
    smallest_input_dtype_bits =
        std::min(get_dtype_bits(input), smallest_input_dtype_bits);
  }
  std::array<int64, 3> reduction_tiling =
      GetReductionTiling(reduction_dimensions, smallest_input_dtype_bits,
                         ir_emitter_context_->cuda_compute_capability());

  int64 num_threads_y = reduction_dimensions.is_row_reduction ? 1 : kWarpSize;
  int64 num_threads_x = [&] {
    if (reduction_dimensions.is_row_reduction) {
      // Use 512 as default block size (threads per block) for row reductions.
      // For multi-output fusions, reduce the block size further to decrease
      // register pressure when multiple outputs are computed by each thread.
      int64 fan_out =
          unnested_hlo->IsMultiOutputFusion()
              ? unnested_hlo->fused_expression_root()->operand_count()
              : 1;
      int64 max_block_size = std::max(64LL, 512LL / NearestPowerOfTwo(fan_out));
      return std::min(
          max_block_size,
          RoundUpToNearest(CeilOfRatio(reduction_dimensions.dimensions[2],
                                       reduction_tiling[2]),
                           kWarpSize));
    }
    return kWarpSize;
  }();

  bool tile_fit = reduction_dimensions.dimensions[kDimX] %
                      (reduction_tiling[2] * num_threads_x) ==
                  0;

  int cc_major = 0;
  if (ir_emitter_context_->cuda_compute_capability()) {
    cc_major = ir_emitter_context_->cuda_compute_capability()->cc_major;
  }

  int num_partial_results = 1;
  KernelMappingScheme::IndexingOrder indexing_order = [&]() {
    if (reduction_dimensions.is_row_reduction &&
        // P100, only try to vectorize+coales memory access when the
        // tile size fits exactly and dtypes <= 32 bits
        ((cc_major == 6 && smallest_input_dtype_bits <= 32 && tile_fit) ||
         // On V100, only try to vectorize+coales memory access for
         // rows of even size.  For odd row sizes, every other row
         // isn't aligned, so it can't be vectorized.
         (cc_major >= 7 && reduction_dimensions.dimensions[2] % 2 == 0))) {
      return kStridedLinearIndexingX;
    } else if (!reduction_dimensions.is_row_reduction &&
               IsUnrollingColumnReductionBeneficial(
                   unnested_hlo, input_shape,
                   reduction_dimensions.dimensions[2])) {
      num_partial_results = 2;
      reduction_tiling[2] *= num_partial_results;
      return kLinearIndexingX;
    } else {
      return kStridedIndexingX;
    }
  }();

  int vector_size = 1;
  if (indexing_order == kStridedLinearIndexingX) {
    if (reduction_dimensions.dimensions[2] % 2 == 0 &&
        // Assuming XLA will perform the unrolling and LLVM will vectorize,
        // disable the unroll for the cases that LLVM doesn't vectorize.
        !MayPreventVectorization(*unnested_hlo)) {
      vector_size = 2;
    } else {
      indexing_order = kStridedIndexingX;
    }
  }
  KernelMappingScheme mapping_scheme(
      reduction_dimensions.dimensions,
      {reduction_tiling[0], reduction_tiling[1] * num_threads_y,
       reduction_tiling[2] * num_threads_x},
      num_threads_y, num_threads_x, indexing_order, vector_size);
  return ReductionCodegenInfo(mapping_scheme, num_partial_results,
                              reduction_dimensions.is_row_reduction);
}

void IrEmitterUnnested::EmitIRForReduction(
    HloInstruction* unnested_hlo,
    absl::Span<HloInstruction* const> output_instructions,
    ReductionCodegenInfo* reduction_info, const Shape& input_shape) {
  std::vector<HloInstruction*> reduce_instructions;
  InlinedVector<ShapeIndex, 1> reduction_output_shape_indices;
  InlinedVector<HloComputation*, 1> reducers;
  for (size_t i = 0; i < output_instructions.size(); ++i) {
    if (!IsReductionFromOrToContiguousDimensions(*output_instructions[i])) {
      continue;
    }

    HloInstruction* output_instruction = output_instructions[i];
    reduce_instructions.push_back(output_instruction);
    reduction_output_shape_indices.push_back(
        CreateShapeIndexForOutputInstruction(*unnested_hlo,
                                             *output_instruction));
    reducers.push_back(output_instruction->to_apply());
  }
  CHECK(reduce_instructions.size() != 0)
      << " expect at least one reduce instructions.";

  const KernelMappingScheme& mapping_scheme =
      reduction_info->GetKernelMappingScheme();
  LaunchDimensions launch_dimensions(mapping_scheme.GetNumberOfBlocks(),
                                     mapping_scheme.GetThreadsPerBlock());
  llvm::Type* index_ty = GetIndexTypeForKernel(
      unnested_hlo, launch_dimensions.launch_bound(), &b_);
  EmitPrologueForReduction(unnested_hlo, reduction_info, reduce_instructions,
                           index_ty);
  EmitElementFunction emit_reduction_tile =
      [&](const llvm_ir::IrArray::Index& index, llvm::Value* y_loc,
          llvm::Value* x_loc, int64 x_iter_num) {
        EmitTileElementForReduction(unnested_hlo, input_shape,
                                    output_instructions, index, *reduction_info,
                                    reducers, x_iter_num);
      };

  TilingKernelInfo tiling_kernel_info = EmitTilingKernel(
      mapping_scheme, index_ty,
      [&](const ThreadIdInfo& thread_id_info, const IrArray::Index& index,
          const string& loop_name, llvm::Value* tile_height,
          llvm::Value* tile_width, KernelSupportLibrary* ksl) {
        EmitTile(reduction_info->GetKernelMappingScheme(), index, loop_name,
                 ksl, thread_id_info, tile_height, tile_width,
                 emit_reduction_tile);
      });
  EmitEpilogueForReduction(index_ty, unnested_hlo, *reduction_info,
                           reduce_instructions, reduction_output_shape_indices,
                           reducers, tiling_kernel_info);
}

namespace {

// Returns whether the `instr` is either a constant, a scalar, or a
// broadcasted constant/scalar.
bool IsBroadcastedConstantOrScalar(const HloInstruction& instr) {
  return instr.IsConstant() || ShapeUtil::IsScalar(instr.shape()) ||
         (HloOpcode::kBroadcast == instr.opcode() &&
          (instr.operand(0)->IsConstant() ||
           ShapeUtil::IsScalar(instr.operand(0)->shape())));
}

// Divides output_instructions into groups. Different groups will be executed
// in parallel. Generally speaking, we'd like to run the reduce instructions
// in parallel without incurring too much recomputation overhead. The current
// heuristic is to place reduce instructions who share nothing or only
// (broadcasted) scalars/constants into different groups; otherwise, they are
// placed in the same group. Non-reduce instructions always go with the reduce
// instructions into the same group so long as they share any predecessors.
std::vector<std::vector<HloInstruction*>> DivideOutputInstructionsIntoGroups(
    HloInstruction* unnested_hlo,
    absl::Span<HloInstruction* const> output_instructions) {
  CHECK(!output_instructions.empty());
  if (output_instructions.size() == 1) {
    return {{output_instructions[0]}};
  }

  std::vector<tensorflow::UnionFind<HloInstruction*>> disjoint_sets(
      output_instructions.size());
  for (size_t i = 0; i < output_instructions.size(); ++i) {
    disjoint_sets[i].Get() = output_instructions[i];
  }

  std::unique_ptr<HloReachabilityMap> reachability_map =
      HloReachabilityMap::Build(unnested_hlo->fused_instructions_computation());
  for (auto* instr : unnested_hlo->fused_instructions()) {
    std::vector<int64> reached_output_ids;
    for (size_t oid = 0; oid < output_instructions.size(); ++oid) {
      if (HloOpcode::kReduce == output_instructions[oid]->opcode() &&
          (IsBroadcastedConstantOrScalar(*instr))) {
        // Do not group output reduce instructions through broadcasted
        // constants or scalars, as the recomputation should be acceptable.
        VLOG(3) << "Skip broadcasted constant or scalar " << instr->ToString();
        continue;
      }
      // Now group output instructions if they have common predecessors.
      if (reachability_map->IsReachable(instr, output_instructions[oid])) {
        VLOG(3) << "Reaching " << output_instructions[oid]->ToString()
                << " from " << instr->ToString();
        reached_output_ids.push_back(oid);
      }
    }
    for (size_t j = 1; j < reached_output_ids.size(); ++j) {
      disjoint_sets[reached_output_ids[0]].Merge(
          &disjoint_sets[reached_output_ids[j]]);
    }
  }
  // Place output instructions in the same set into the same group.
  absl::flat_hash_map<HloInstruction*, std::vector<HloInstruction*>> groups;
  for (size_t oid = 0; oid < output_instructions.size(); ++oid) {
    groups[disjoint_sets[oid].Get()].push_back(output_instructions.at(oid));
  }

  std::vector<std::vector<HloInstruction*>> ret;
  absl::c_for_each(
      groups, [&](auto& iter) { ret.emplace_back(std::move(iter.second)); });
  return ret;
}

}  // namespace

Status IrEmitterUnnested::EmitReductionFromOrToContiguousDimensions(
    HloInstruction* unnested_hlo,
    absl::Span<HloInstruction* const> output_instructions) {
  bool returns_tuple = output_instructions.size() > 1;
  VLOG(10) << "Emitting reduction to vector " << unnested_hlo->ToString();

  // Build an initializer thunk to initialize each reduction output.
  std::vector<std::unique_ptr<Thunk>> thunks;
  for (int i = 0; i < output_instructions.size(); ++i) {
    if (!IsReductionFromOrToContiguousDimensions(*output_instructions[i])) {
      continue;
    }

    ShapeIndex idx = returns_tuple ? ShapeIndex({i}) : ShapeIndex({});
    TF_ASSIGN_OR_RETURN(std::unique_ptr<Thunk> initializer_thunk,
                        BuildInitializerThunk(unnested_hlo, idx));
    thunks.push_back(std::move(initializer_thunk));
  }

  // Build a kernel thunk to compute all the outputs.
  const HloInstruction* first_reduce = nullptr;
  for (int i = 0; i < output_instructions.size(); ++i) {
    if (IsReductionFromOrToContiguousDimensions(*output_instructions[i])) {
      first_reduce = output_instructions[i];
      break;
    }
  }
  CHECK(first_reduce);
  if (output_instructions.size() > 1) {
    if (!AreFusedReductionOutputsConsistent(output_instructions,
                                            first_reduce)) {
      return InternalError("Inconsistent reduction fusion outputs");
    }
  }
  const Shape& input_shape = first_reduce->operand(0)->shape();
  // The layout of a reduction input is either set by LayoutAssignment for
  // unnested kReduce or by InstructionFusion for fused kReduce.
  CHECK(input_shape.has_layout()) << "LayoutAssignment or InstructionFusion "
                                     "doesn't set the input layout of "
                                  << first_reduce->ToString();

  // Group output instructions. Each group will be executed in parallel.
  std::vector<std::vector<HloInstruction*>> instr_groups =
      DivideOutputInstructionsIntoGroups(unnested_hlo, output_instructions);
  VLOG(2) << StrCat("Generate in ", instr_groups.size(), " groups for ",
                    unnested_hlo->ToString());
  std::unique_ptr<KernelThunk> kernel_thunk =
      BuildKernelThunk(unnested_hlo, /*implements_whole_instruction=*/false);
  KernelSupportLibrary ksl(&b_, llvm_ir::UnrollMode::kDefaultUnroll);
  for (size_t i = 0; i < instr_groups.size(); ++i) {
    // Create a new ReductionCodegenInfo instance as it contains states for
    // code generation per reduction group. For now, let's always use the very
    // first reduce as representative to construct ReductionCodegenInfo, since
    // all the reductions are required to have the same shape and layout as
    // verified by `AreFusedReductionOutputsConsistent()`. We can loosen the
    // constraint later when the needs arise.
    ReductionCodegenInfo reduction_info =
        ComputeReductionCodegenInfo(unnested_hlo, first_reduce);
    auto emit_reduction_func = [&] {
      EmitIRForReduction(unnested_hlo, instr_groups[i], &reduction_info,
                         input_shape);
    };
    // Use raw block_id_y to select the i-th parallel reduction to run. Using
    // block_id_y instead of block_id_x simplifies the index calculation
    // for reduction code generation as the block_id_y is orthogonal to
    // the indices used within the reductions.
    llvm::CallInst* raw_block_id_y = gpu::EmitCallToTargetIntrinsic(
        gpu::TargetIntrinsicID::kBlockIdy, {}, {}, &b_);
    llvm_ir::AddRangeMetadata(0, instr_groups.size(),
                              llvm::cast<llvm::Instruction>(raw_block_id_y));
    llvm::Value* guarding_cond =
        b_.CreateICmpEQ(raw_block_id_y, b_.getInt32(i));
    ksl.If(StrCat("reduce-group-", i), guarding_cond, emit_reduction_func);
  }
  ReductionCodegenInfo reduction_info =
      ComputeReductionCodegenInfo(unnested_hlo, first_reduce);
  const KernelMappingScheme& mapping_scheme =
      reduction_info.GetKernelMappingScheme();
  // block_y_count is set to instr_groups.size(), so that each reduction group
  // can be run in parallel by a different BlockIdy.
  LaunchDimensions launch_dimensions(
      {/*x=*/mapping_scheme.GetNumberOfBlocks(),
       /*y=*/static_cast<int64>(instr_groups.size()),
       /*z=*/1},
      {/*x=*/mapping_scheme.GetThreadsPerBlock(), /*y=*/1, /*z=*/1});
  VLOG(3) << "Launch dimensions of " << unnested_hlo->name()
          << ": number of blocks: " << mapping_scheme.GetNumberOfBlocks()
          << " - threads per block: " << mapping_scheme.GetThreadsPerBlock();
  UpdateLaunchDimensions(launch_dimensions, kernel_thunk.get(),
                         ir_emitter_context_->llvm_module());

  thunks.push_back(std::move(kernel_thunk));
  std::unique_ptr<SequentialThunk> sequential_thunk =
      absl::make_unique<SequentialThunk>(GetThunkInfo(unnested_hlo),
                                         std::move(thunks));
  AddThunkToThunkSequence(std::move(sequential_thunk));

  return Status::OK();
}

// Emits code for slices based on the below structure. An if statement with
// a guarding condition is generated for each ROOT slice.
//
// Pseudo code:
//
// Compute values of slice input operands
//
// Compute guarding_cond0
// if (guarding_cond0) {
//   Write to output of slice0
// }
//
// Compute guarding_cond1
// if (guarding_cond1) {
//   Write to output of slice1
// }
//
void IrEmitterUnnested::EmitElementForInputFusibleSlices(
    HloInstruction* unnested_hlo, const llvm_ir::IrArray::Index& index) {
  VLOG(10) << "Emitting slice input fusion for " << unnested_hlo->ToString();

  HloInstruction* slice_or_tuple = unnested_hlo->fused_expression_root();
  auto slice_instructions = [&]() -> absl::Span<HloInstruction* const> {
    if (slice_or_tuple->opcode() == HloOpcode::kSlice) {
      return absl::Span<HloInstruction* const>(&slice_or_tuple, 1);
    }
    CHECK_EQ(slice_or_tuple->opcode(), HloOpcode::kTuple);
    return slice_or_tuple->operands();
  }();

  // Emit input operand values of slices.
  std::vector<llvm::Value*> input_ir_values;
  GpuElementalIrEmitter elem_emitter(hlo_module_config_, module_, &b_,
                                     GetNestedComputer());
  FusedIrEmitter fused_emitter(&elem_emitter);
  BindFusionArguments(unnested_hlo, &fused_emitter);
  for (const HloInstruction* slice : slice_instructions) {
    auto input_generator = *fused_emitter.GetGenerator(slice->operand(0));
    input_ir_values.push_back(input_generator(index).ValueOrDie());
  }

  // Emit for slice_instructions.
  KernelSupportLibrary ksl(&b_, llvm_ir::UnrollMode::kDefaultUnroll);
  for (int64 i = 0; i < slice_instructions.size(); ++i) {
    HloInstruction* slice = slice_instructions[i];

    // guarding_cond := index >= start && index < limit, for each dim.
    std::vector<llvm::Value*> index_within_ranges;
    for (size_t dim = 0; dim < slice->slice_starts().size(); ++dim) {
      CHECK_EQ(slice->slice_strides(dim), 1);
      auto larger_or_equal_than_start = b_.CreateICmpSGE(
          index.multidim()[dim],
          index.GetConstantWithIndexType(slice->slice_starts(dim)));
      llvm::Value* smaller_than_limit = b_.CreateICmpSLT(
          index.multidim()[dim],
          index.GetConstantWithIndexType(slice->slice_limits(dim)));
      llvm::Value* within_range =
          b_.CreateAnd(larger_or_equal_than_start, smaller_than_limit);
      index_within_ranges.push_back(within_range);
    }
    llvm::Value* guarding_cond = b_.CreateAnd(index_within_ranges);

    auto emit_slice_elem_func = [&] {
      const std::vector<llvm::Value*>& src_multidim = index.multidim();
      std::vector<llvm::Value*> dst_multidim(src_multidim.size());
      for (size_t dim = 0; dim < src_multidim.size(); ++dim) {
        dst_multidim[dim] =
            Sub(src_multidim[dim],
                index.GetConstantWithIndexType(slice->slice_starts(dim)));
      }
      ShapeIndex shape_index = (slice_or_tuple->opcode() == HloOpcode::kSlice)
                                   ? ShapeIndex()
                                   : ShapeIndex({i});
      llvm_ir::IrArray src_ir_array =
          GetIrArray(*unnested_hlo, *unnested_hlo, shape_index);
      IrArray::Index slice_dst_index(dst_multidim, slice->shape(),
                                     index.GetType());
      src_ir_array.EmitWriteArrayElement(slice_dst_index, input_ir_values[i],
                                         &b_);
    };

    ksl.If(StrCat("slice", i), guarding_cond, emit_slice_elem_func);
  }
}

Status IrEmitterUnnested::EmitInputFusibleNonStridedSlices(
    HloInstruction* unnested_hlo) {
  constexpr int unroll_factor = 1;
  std::unique_ptr<KernelThunk> kernel_thunk =
      BuildKernelThunk(unnested_hlo, /*implements_whole_instruction=*/true);

  TF_ASSIGN_OR_RETURN(Shape element_shape,
                      GetConsistentInputShapeForRootSlices(*unnested_hlo));
  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      element_shape, ir_emitter_context_->gpu_device_info(), unroll_factor);
  UpdateLaunchDimensions(launch_dimensions, kernel_thunk.get(),
                         ir_emitter_context_->llvm_module());

  Status emit_status =
      ParallelLoopEmitter(
          [&](const llvm_ir::IrArray::Index index) -> Status {
            EmitElementForInputFusibleSlices(unnested_hlo, index);
            return Status::OK();
          },
          element_shape, launch_dimensions, &b_)
          .EmitLoop(IrName(unnested_hlo),
                    GetIndexTypeForKernel(
                        unnested_hlo, launch_dimensions.launch_bound(), &b_));

  thunk_sequence_.emplace_back(std::move(kernel_thunk));

  return emit_status;
}

Thunk::ThunkInfo IrEmitterUnnested::GetThunkInfo(
    const HloInstruction* hlo) const {
  auto info = ThunkEmitter::EmissionContext::GetThunkInfo(hlo);
  if (const auto* index_map = ir_emitter_context_->profile_index_map()) {
    info.profile_index.emplace(
        static_cast<int64>(index_map->GetProfileIndexFor(*hlo)));
  }
  return info;
}

void MlirEmitterContext::SetOperation(mlir::Operation* op) {
  this->name = mlir::GetNameFromLoc(op->getLoc());

  std::vector<mlir::Value> operands, outputs;
  if (auto fusion = mlir::dyn_cast<mlir::lmhlo::FusionOp>(op)) {
    GetFusionOperandsAndOutputs(fusion, &operands, &outputs);
  } else {
    for (auto buffer : op->getOperands()) {
      if (WritesMlirBuffer(op, buffer)) {
        outputs.push_back(buffer);
      } else {
        operands.push_back(buffer);
      }
    }
  }
  for (auto operand : operands) {
    operand_shapes.push_back(TypeToShape(operand.getType()));
  }
  for (auto output : outputs) {
    output_shapes.push_back(TypeToShape(output.getType()));
  }
}

}  // namespace gpu
}  // namespace xla
