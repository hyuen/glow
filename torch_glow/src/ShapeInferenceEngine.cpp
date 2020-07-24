// Copyright 2004-present Facebook. All Rights Reserved.

#include <iostream>
#include <string>
#include <torch/script.h>
#include <unordered_set>
#include <vector>

#include "ShapeInferenceEngine.h"

#include "glow/Support/Error.h"
#include "glow/Support/Support.h"

namespace glow {

ShapeInferenceEngine::ShapeInferenceEngine(
    const torch::jit::Graph &graph, const at::ArrayRef<at::IValue> &inputs)
    : graph_(graph), inputs_(inputs){};

void ShapeInferenceEngine::getNodeInputShape(const torch::jit::Node *node,
                                             MetaStack &inputMetas) {
  for (auto input : node->inputs()) {
    auto it = shapeMap_.find(input);
    CHECK(it != shapeMap_.end());
    inputMetas.emplace_back(shapeMap_[input]);
  }
}

std::vector<std::vector<int64_t>> &ShapeInferenceEngine::getGraphOutputShape() {
  return outputShape_;
}

Error ShapeInferenceEngine::shapeOnNode(const torch::jit::Node *node) {

  /// Get op symbol
  const auto kind = node->kind();
  const std::string symbol = kind.toQualString();
  /// Extract shapes of inputs from shape mapping
  MetaStack inputMetas;

  /// The output of each Op shape function could be either the shape or int
  /// value generated by prim::consant or prim::ListContruct.
  /// The \p outputShapesOrValues is to store outputs of ops shape function.
  std::vector<std::vector<int64_t>> outputShapesOrValues(1);

  getNodeInputShape(node, inputMetas);

  // Get output shape or int value of the ops without actual computation
  if (symbol == "glow::fused_stack") {
    int64_t dim = node->i(at::attr::dim);
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                               fusedStack(inputMetas, dim));
  } else if (symbol == "fb::embedding_bag_byte_rowwise_offsets") {
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                               embeddingBagByteRowwiseOffsets(inputMetas));
  } else {
    switch (kind) {
    case c10::prim::Constant: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], primConstant(node));
      break;
    }
    case c10::aten::tanh:
    case c10::aten::relu:
    case c10::aten::sigmoid: {
      RETURN_ERR_IF_NOT(inputMetas.size() == 1,
                        "Expected 1 input shape for operators.");
      outputShapesOrValues[0] = inputMetas[0].shape;
      break;
    }
    case c10::aten::sub:
    case c10::aten::pow:
    case c10::aten::mul:
    case c10::aten::add: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], binaryOp(inputMetas));
      break;
    }
    case c10::aten::mm: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], mm(inputMetas));
      break;
    }
    case c10::aten::addmm: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], addmm(inputMetas));
      break;
    }
    case c10::aten::bmm: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], bmm(inputMetas));
      break;
    }
    case c10::prim::FusedConcat: {
      int64_t dim = node->i(at::attr::dim);
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                                 fusedConcat(inputMetas, dim));
      break;
    }
    case c10::prim::ConstantChunk: {
      int64_t chunks = node->i(at::attr::chunks);
      int64_t dim = node->i(at::attr::dim);
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues,
                                 constantChunk(inputMetas, chunks, dim));
      break;
    }
    case c10::prim::ListConstruct: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                                 listConstruct(inputMetas));
      break;
    }
    case c10::aten::slice: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], slice(inputMetas));
      break;
    }
    case c10::aten::reshape: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], reshape(inputMetas));
      break;
    }
    case c10::aten::permute: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], permute(inputMetas));
      break;
    }
    case c10::aten::embedding_bag: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                                 embeddingBag(inputMetas));
      break;
    }
    default: {
      return MAKE_ERR(strFormat("Node's operator %s is not supported",
                                kind.toQualString()));
    }
    }
  }

  /// Put output into map
  /// For \p prim::Constant, the output could be either Tensor or NumberType.
  /// If the output is TensorType, store the \p outputShapesOrValues
  /// into VariableMeta.shape;
  /// Else store the \p outputShapesOrValues into VariableMeta.intValue.
  /// For \p prim::ListConstruct, the output is a intList.
  /// Store the shape of \p outputShapesOrValues into VariableMeta.shape
  /// store the value of \p outputShapesOrValues into VariableMeta.intValue
  /// For \p aten::embedding_bag, since the output is a std::tuple<Tensor,
  /// Tensor, Tensor, Tensor>(ret, offset2bag, bag_size, bag_size), and for now,
  /// only the ret tensor shape needed, the embeddingBag() only generate the ret
  /// shape.
  if (kind == c10::prim::Constant) {
    if (node->output()->type()->isSubtypeOf(at::TensorType::get())) {
      shapeMap_[node->output()].shape = std::move(outputShapesOrValues[0]);
    } else {
      shapeMap_[node->output()].shape = {1};
      shapeMap_[node->output()].intValue = std::move(outputShapesOrValues[0]);
    }
  } else if (kind == c10::prim::ListConstruct) {
    shapeMap_[node->output()].shape = {
        static_cast<long>(outputShapesOrValues[0].size()), 1};
    shapeMap_[node->output()].intValue = std::move(outputShapesOrValues[0]);
  } else if (kind == c10::aten::embedding_bag) {
    shapeMap_[node->output(0)].shape = std::move(outputShapesOrValues[0]);
  } else {
    for (int i = 0; i < node->outputs().size(); i++) {
      shapeMap_[node->output(i)].shape = std::move(outputShapesOrValues[i]);
    }
  }
  return Error::success();
}

Error ShapeInferenceEngine::run() {

  RETURN_ERR_IF_NOT(
      inputs_.size() == graph_.inputs().size(),
      "Number of inputs mismatch between Graph and actual inputs");

  /// Put graph input into shape mapping
  RETURN_IF_ERR(getGraphIntputShape());

  /// Run shape inference for each node
  for (auto *node : graph_.nodes()) {
    RETURN_IF_ERR(shapeOnNode(node));
  }

  /// Extract output from shape mapping
  generateGraphOutputShape();
  return Error::success();
}

void ShapeInferenceEngine::printShapeMap() {
  for (auto elem : shapeMap_) {
    std::cout << elem.first->debugName() << ":[ ";
    for (auto value : elem.second.shape) {
      std::cout << value << " ";
    }
    std::cout << "]" << std::endl;
  }
}

/// If the input is tensor, store the shape info only;
/// Else If the input is bool or int, store the value, and set shape as 1.
/// Else if the input is intlist, store the intlist, and set shape as [sizeof
/// intlist, 1]
/// Else return an error
Error ShapeInferenceEngine::getGraphIntputShape() {
  for (auto i = 0; i < inputs_.size(); i++) {
    auto gInName = graph_.inputs()[i];
    shapeMap_[gInName].shape = {};
    shapeMap_[gInName].intValue = {};

    auto input = inputs_[i];
    if (input.isTensor()) {
      auto ptTensor = input.toTensor();
      for (auto s : ptTensor.sizes()) {
        shapeMap_[gInName].shape.emplace_back(s);
      }
    } else if (input.isBool() || input.isInt()) {
      shapeMap_[gInName].shape = {1};
      shapeMap_[gInName].intValue = {input.toInt()};
    } else if (input.isIntList()) {
      auto ptIntList = input.toIntVector();
      shapeMap_[gInName].shape = {static_cast<long>(ptIntList.size()), 1};
      shapeMap_[gInName].intValue = ptIntList;
    } else {
      return MAKE_ERR("Input type doesn't support yet.");
    }
  }
  return Error::success();
}

void ShapeInferenceEngine::generateGraphOutputShape() {
  for (auto output : graph_.outputs()) {
    auto it = shapeMap_.find(output);
    CHECK(it != shapeMap_.end());
    outputShape_.emplace_back(it->second.shape);
  }
}

/// The \p prim::Constant may have multiple types of output, eg.
/// int = prim::Constant[value=0]()
/// Float(1:1) = prim::Constant[value={0}]()
/// bool = prim::Constant[value=0]()
/// None = prim::Constant()
/// Tensor = prim::Constant[value= <Tensor>]()
/// If the output is a tensor, return shape info;
/// Else, return the value.
Expected<std::vector<int64_t>>
ShapeInferenceEngine::primConstant(const torch::jit::Node *node) {

  std::vector<int64_t> shapeOrValue;
  at::TypePtr type = node->output()->type();

  if (type->isSubtypeOf(at::FloatType::get())) {
    /// The float type will not affect the shape
    /// Set value as 1
    shapeOrValue = {1};
  } else if (type->isSubtypeOf(at::IntType::get())) {
    shapeOrValue = {node->i(at::attr::value)};
  } else if (type->isSubtypeOf(at::BoolType::get())) {
    shapeOrValue = {node->i(at::attr::value)};
  } else if (type->isSubtypeOf(at::NoneType::get())) {
    shapeOrValue = {};
  } else if (type->isSubtypeOf(at::TensorType::get())) {
    at::Tensor t = node->t(at::attr::value);
    for (auto s : t.sizes()) {
      shapeOrValue.emplace_back(s);
    }
  }
  return shapeOrValue;
}

/**
 * aten::add(Tensor self, Tensor or Scalar other, Scalar alpha=1) -> Tensor
 * aten::pow(Tensor self, Tensor or Scalar other, Scalar alpha=1) -> Tensor
 * aten::mul(Tensor self, Tensor or Scalar other, Scalar alpha=1) -> Tensor
 * variableMetas: 0: self, 1: other
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::binaryOp(const MetaStack &variableMetas) {

  if (variableMetas.size() != 2 && variableMetas.size() != 3) {
    return MAKE_ERR("Expected two or three inputs shapes of this operation.");
  }

  const std::vector<int64_t> &t0 = variableMetas[0].shape;
  const std::vector<int64_t> &t1 = variableMetas[1].shape;

  auto d0 = t0.size();
  auto d1 = t1.size();

  /// One input is Scalar
  if (d1 == 1) {
    return t0;
  }

  size_t dim = std::max(d0, d1);
  std::vector<int64_t> shape(dim);

  for (auto i = 0; i < dim; i++) {
    auto j = -1 - i;
    if (i >= d0 || t0[d0 + j] == 1) {
      shape[dim + j] = t1[d1 + j];
    } else if (i >= d1 || t1[d1 + j] == 1) {
      shape[dim + j] = t0[d0 + j];
    } else {
      if (t1[d1 + j] != t0[d0 + j]) {
        return MAKE_ERR(
            strFormat("The size of tensor a (%zu) must match the size of "
                      "tensor b (%zu)at non-singleton dimension 1.",
                      t0[d0 + j], t1[d1 + j]));
      }

      shape[dim + j] = t1[d1 + j];
    }
  }
  return shape;
}

/**
 * aten::mm(Tensor self, Tensor mat2) -> Tensor
 * variableMetas: 0: self, 1: mat2
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::mm(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(variableMetas.size() == 2,
                    "Expected two inputs shapes of this operation.");

  const std::vector<int64_t> &t0 = variableMetas[0].shape;
  const std::vector<int64_t> &t1 = variableMetas[1].shape;

  if (!(t1.size() == 2 && t0.size() == 2)) {
    return MAKE_ERR("Expected 2-dimensional tensor.");
  }

  if (t0[1] != t1[0]) {
    return MAKE_ERR(
        strFormat("The size of tensor a (%zu) at dimension 1 must match the "
                  "size of tensor b (%zu) at dimension 0.",
                  t0[1], t1[0]));
  }

  std::vector<int64_t> shape = {t0[0], t1[1]};
  return shape;
}

/**
 * aten::bmm(Tensor self, Tensor mat2) -> Tensor
 * variableMetas: 0: self, 1: mat2
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::bmm(const MetaStack &variableMetas) {

  if (variableMetas.size() != 2) {
    return MAKE_ERR("Expected two inputs shapes of this operation.");
  }

  const std::vector<int64_t> &t0 = variableMetas[0].shape;
  const std::vector<int64_t> &t1 = variableMetas[1].shape;

  if (!(t0.size() == 3 && t1.size() == 3)) {
    return MAKE_ERR("Expected 3-dimensional tensor.");
  }

  if (t0[0] != t1[0]) {
    return MAKE_ERR("Expected tensors to have same size at dimension 0");
  }

  if (t0[2] != t1[1]) {
    return MAKE_ERR(strFormat("The size of tensor a (%zu) at dimension 2 must"
                              "match the size of tensor b (%zu) at dimension 1",
                              t0[2], t1[1]));
  }
  std::vector<int64_t> shape = {t0[0], t0[1], t1[2]};
  return shape;
}

/**
 * aten::addmm(Tensor self, Tensor mat1, Tensor mat2, *, Scalar beta=1, Scalar
   alpha=1) -> Tensor
 * variableMetas: 0: self, 1: mat1, 2: mat2
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::addmm(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(variableMetas.size() >= 3,
                    strFormat("Expected at least three inputs shapes, got %zu.",
                              variableMetas.size()));

  const VariableMeta &t0 = variableMetas[0];
  const VariableMeta &t1 = variableMetas[1];
  const VariableMeta &t2 = variableMetas[2];
  VariableMeta t;

  // For Scalar type, the shape.size() is 1
  if (variableMetas[2].shape.size() == 1) {
    t = variableMetas[1];
  } else {
    const MetaStack &mm_shape = {t1, t2};
    ASSIGN_VALUE_OR_RETURN_ERR(t.shape, mm(mm_shape));
  }

  return binaryOp({t0, std::move(t)});
}

/**
 * prim::ConstantChunk[int chunks, int dim](Tensor self) -> Tensors
 * variableMetas: 0: self
 */
Expected<std::vector<std::vector<int64_t>>>
ShapeInferenceEngine::constantChunk(const MetaStack &variableMetas,
                                    int64_t chunks, int64_t dim) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 1,
      strFormat("Expected one input, got %zu.", variableMetas.size()));

  /// Convert dim into positive
  if (dim < 0) {
    dim += variableMetas[0].shape.size();
  }
  RETURN_ERR_IF_NOT(dim < variableMetas[0].shape.size() && dim >= 0,
                    "Dim value is out of range.");

  /// For constant chunk, the size of the last chunk one may smaller than the
  /// others
  int64_t c = (variableMetas[0].shape[dim] + chunks - 1) / chunks;
  int64_t r = variableMetas[0].shape[dim] - c * (chunks - 1);

  std::vector<std::vector<int64_t>> outShapes;
  for (int i = 0; i < chunks; i++) {
    std::vector<int64_t> shape = variableMetas[0].shape;
    shape[dim] = (i == chunks - 1) ? r : c;
    outShapes.emplace_back(shape);
  }

  return outShapes;
}

/**
 * prim::FusedConcat[int dim](Tensor self, Tensor mat1, Tensor mat2, ...) ->
 * Tensor variableMetas: 0: self, 1: mat1, 2: mat2, ...
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::fusedConcat(const MetaStack &variableMetas, int64_t dim) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() >= 1,
      strFormat("Expected at least 1 inputs, got %zu.", variableMetas.size()));

  if (variableMetas.size() == 1) {
    return variableMetas[0].shape;
  }

  std::vector<int64_t> shape = variableMetas[0].shape;
  /// Convert negtive dimension to positive, then check the dim range.
  int64_t inDims = variableMetas[0].shape.size();
  if (dim < 0) {
    dim += inDims;
  }
  RETURN_ERR_IF_NOT(dim < inDims && dim >= 0, "Dim value is out of range.");

  /// Handle multiple inputs cases
  for (int i = 1; i < variableMetas.size(); ++i) {
    RETURN_ERR_IF_NOT(inDims == variableMetas[i].shape.size(),
                      "All inputs must have the same number of dimensions.");
    for (int j = 0; j < inDims; j++) {
      if (j == dim) {
        shape[dim] += variableMetas[i].shape[dim];
      } else {
        RETURN_ERR_IF_NOT(
            shape[j] == variableMetas[i].shape[j],
            strFormat("Sizes of tensors must match except in dimension %zu.",
                      dim));
      }
    }
  }
  return shape;
}

/**
 * aten::slice(Tensor self, int dim, int start, int end, int step)
 * variableMetas: 0: self, 1: dim, 2: start, 3: end, 4: step.
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::slice(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 5,
      strFormat("Expected 5 inputs, got %zu.", variableMetas.size()));

  for (int i = 1; i < 5; i++) {
    RETURN_ERR_IF_NOT(variableMetas[i].shape.size() == 1,
                      "Expected int in Slice.");
  }

  int64_t dim = variableMetas[1].intValue[0];
  int64_t start = variableMetas[2].intValue[0];
  int64_t end = variableMetas[3].intValue[0];
  int64_t step = variableMetas[4].intValue[0];
  int64_t inDims = variableMetas[0].shape[dim];

  std::vector<int64_t> shape = variableMetas[0].shape;

  /// Check if the start or end dim out of the input dimension
  if (start >= inDims || end <= -inDims) {
    shape[dim] = 0;
    return shape;
  }

  /// Convert start dim into positive
  if (start <= -inDims) {
    start = 0;
  } else if (start > -inDims && start < 0) {
    start += inDims;
  }

  /// Convert end dim into positive
  if (end > inDims) {
    end = inDims;
  } else if (end > -inDims && end < 0) {
    end += inDims;
  }

  if (start >= end) {
    shape[dim] = 0;
    return shape;
  }

  shape[dim] = (end - start) / step;
  if ((end - start) % step) {
    shape[dim] += 1;
  }
  return shape;
}

/**
 * aten::reshape(Tensor self, int[] shape) -> Tensor
 * variableMetas: 0: self, 1: shape
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::reshape(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 2,
      strFormat("Expected two inputs shapes, got %zu.", variableMetas.size()));

  int64_t s0 = 1;
  int64_t s1 = 1;

  /// Flag for multiple negative index
  int64_t negIndex = -1;
  for (auto i : variableMetas[0].shape) {
    s0 *= i;
  }

  for (int i = 0; i < variableMetas[1].intValue.size(); i++) {
    s1 *= variableMetas[1].intValue[i];
    if (variableMetas[1].intValue[i] == -1) {
      if (negIndex == -1) {
        negIndex = i;
      } else {
        return MAKE_ERR("Unable to infer undetermined dimension");
      }
    }
  }

  RETURN_ERR_IF_NOT(s0 % s1 == 0, "Reshape size is invalid for input size.");

  std::vector<int64_t> shape = variableMetas[1].intValue;

  if (negIndex != -1) {
    shape[negIndex] = -s0 / s1;
  }
  return shape;
}

/**
 * aten::permute(Tensor self, int[] shape) -> Tensor
 * variableMetas: 0: self, 1: shape
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::permute(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 2,
      strFormat("Expected two inputs shapes, got %zu.", variableMetas.size()));

  int64_t inDims = variableMetas[0].shape.size();

  RETURN_ERR_IF_NOT(inDims == variableMetas[1].intValue.size(),
                    "Shuffle for permute must has the same number of "
                    "dimensions as the input tensor.");

  std::vector<int64_t> shape;

  for (int64_t dim : variableMetas[1].intValue) {
    RETURN_ERR_IF_NOT(dim >= 0,
                      "Negative shuffle dimensions not supported by Glow yet.");
    RETURN_ERR_IF_NOT(
        dim < inDims,
        "All shuffle dimensions must be less than the rank of the input.");
    shape.emplace_back(variableMetas[0].shape[dim]);
  }
  return shape;
}

/**
 * prim::ListContruct(Scalar or Bool self, Scalar or Bool v1, Scalar or Bool v2,
 * ...) -> Scalar[] or Bool[]
 * variableMetas: 0: self, 1: v1, 2: v2, ...
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::listConstruct(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() >= 1,
      strFormat("Expected at least 1 inputs, got %zu.", variableMetas.size()));

  std::vector<int64_t> intValueList;
  for (auto ele : variableMetas) {
    RETURN_ERR_IF_NOT(ele.shape.size() == 1,
                      "Expected int type input in listConstruct.");
    intValueList.emplace_back(ele.intValue[0]);
  }
  return intValueList;
}

/**
 * glow::fused_stack[dim=1](Tensor self, Tensor mat1, Tensor mat2, ...)
 * variableMetas: 0: self, 1: mat1, 2: mat2, ...
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::fusedStack(const MetaStack &variableMetas, int64_t dim) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() >= 1,
      strFormat("Expected at least 1 inputs, got %zu.", variableMetas.size()));

  if (variableMetas.size() == 1) {
    return variableMetas[0].shape;
  }
  int64_t inDims = variableMetas[0].shape.size();
  /// glow::fused_stack will add one more dim
  if (dim < 0) {
    dim += inDims + 1;
  }
  RETURN_ERR_IF_NOT(
      dim < inDims + 1 && dim >= 0,
      "Dim must be less than the rank of inputs plus the added dimension");

  std::vector<int64_t> shape = variableMetas[0].shape;

  for (auto eleShape : variableMetas) {
    RETURN_ERR_IF_NOT(eleShape.shape == shape,
                      "All inputs must have same shape");
  }

  shape.insert(shape.begin() + dim, variableMetas.size());
  return shape;
}

/**
 * aten::_embedding_bag(Tensor weight,
 *                      Tensor indices,
 *                      Tensor offsets,
 *                      bool scale_grad_by_freq=False,
 *                      int mode=0,
 *                      bool sparse=False,
 *                      Tensor? per_sample_weights=None,
 *                      bool include_last_offset=False)
 *                      -> (Tensor, Tensor, Tensor, Tensor)
 */
/// Since the first output tensor is the result, and we only need the shape of
/// result Return the shape of the first tensor only
/// In glow, the include_last_offset is always True.
Expected<std::vector<int64_t>>
ShapeInferenceEngine::embeddingBag(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 8,
      strFormat("Expected 8 inputs, got %zu.", variableMetas.size()));

  std::vector<int64_t> shape;

  if (variableMetas[1].shape.size() == 1) {
    RETURN_ERR_IF_NOT(variableMetas[2].shape.size() == 1,
                      strFormat("Expected 1D offset, got %zu.",
                                variableMetas[2].shape.size()));
    shape = {variableMetas[2].shape[0] - static_cast<int>(hasEndOffset_),
             variableMetas[0].shape[1]};
  } else if (variableMetas[1].shape.size() == 2) {
    shape = {variableMetas[1].shape[0], variableMetas[0].shape[1]};
  } else {
    return MAKE_ERR("Only support 1D and 2D Input in Embedding bag.");
  }
  return shape;
}

/**
 * fb::embedding_bag_byte_rowwise_offsets(Tensor weight,
 *                                        Tensor indices,
 *                                        Tensor offsets,
 *                                        bool scale_grad_by_freq=False,
 *                                        int mode=0,
 *                                        bool sparse=False,
 *                                        Tensor? per_sample_weights=None,
 *                                        bool include_last_offset=True)
 *                                        -> Tensor;
 */
/// In glow, the include_last_offset is always True.
Expected<std::vector<int64_t>>
ShapeInferenceEngine::embeddingBagByteRowwiseOffsets(
    const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 8,
      strFormat("Expected 8 inputs, got %zu.", variableMetas.size()));

  /// variableMetas[0].shape[1] - 8 is to account for scale and bias
  /// 4-byte scale, 4-byte zero_offset
  std::vector<int64_t> shape = {variableMetas[2].shape[0] -
                                    static_cast<int>(hasEndOffset_),
                                variableMetas[0].shape[1] - 8};
  return shape;
}
} // namespace glow