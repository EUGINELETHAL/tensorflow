/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/jit/compilability_check_util.h"

#include "absl/memory/memory.h"
#include "tensorflow/cc/framework/scope.h"
#include "tensorflow/cc/ops/function_ops.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/compiler/tf2xla/xla_op_kernel.h"
#include "tensorflow/compiler/tf2xla/xla_op_registry.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/graph_to_functiondef.h"
#include "tensorflow/core/graph/graph_def_builder.h"
#include "tensorflow/core/graph/graph_def_builder_util.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace {

constexpr char kFunctionalWhileNodeName[] = "While";
constexpr char kCompilableFunctionName[] = "CompilableFn";
constexpr char kCompilableFunctionNodeName[] = "n_c";
constexpr char kUncompilableFunctionName[] = "UncompilableFn";
constexpr char kUncompilableFunctionNodeName[] = "n_c_uncompilable";

// A dummy OpKernel for testing.
class DummyCompilableOp : public XlaOpKernel {
 public:
  explicit DummyCompilableOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {}
  void Compile(XlaOpKernelContext* ctx) override {
    ctx->SetOutput(0, ctx->Input(0));
  }
};

// Register the DummyCompilableOp kernel for CPU.
REGISTER_OP("InputFloatOp").Output("o: float");
REGISTER_OP("CompilableOp").Input("i: float").Output("o: float");
REGISTER_XLA_OP(Name("CompilableOp").Device(DEVICE_CPU_XLA_JIT),
                DummyCompilableOp);

// Dummy op that is uncompilable in CPU.
REGISTER_OP("MissingKernel").Input("i: float").Output("o: float");

class CompilabilityCheckUtilTest : public ::testing::Test {
 protected:
  void SetUp() override {
    XlaOpRegistry::RegisterCompilationKernels();

    op_filter_.allow_resource_ops_in_called_functions = false;
    op_filter_.allow_stack_ops = false;
    op_filter_.allow_tensor_array_ops = false;
    op_filter_.allow_stateful_rng_ops = false;
    op_filter_.allow_control_trigger = false;
    op_filter_.allow_eliding_assert_and_checknumerics_ops = false;
    op_filter_.allow_ops_producing_or_consuming_variant = false;
    op_filter_.allow_inaccurate_ops = false;
    op_filter_.allow_slow_ops = false;

    checker_ = absl::make_unique<RecursiveCompilabilityChecker>(&op_filter_,
                                                                &device_type_);
  }

  FunctionLibraryRuntime* GetFunctionLibraryRuntime() {
    OptimizerOptions opts;
    pflr_ = absl::make_unique<ProcessFunctionLibraryRuntime>(
        nullptr, Env::Default(), TF_GRAPH_DEF_VERSION, flib_def_.get(), opts);

    return pflr_->GetFLR(ProcessFunctionLibraryRuntime::kDefaultFLRDevice);
  }

  RecursiveCompilabilityChecker::OperationFilter op_filter_;
  DeviceType device_type_ = DeviceType(DEVICE_CPU_XLA_JIT);
  std::unique_ptr<FunctionDefLibrary> func_library_ =
      absl::make_unique<FunctionDefLibrary>();
  std::unique_ptr<FunctionLibraryDefinition> flib_def_ =
      absl::make_unique<FunctionLibraryDefinition>(OpRegistry::Global(),
                                                   *func_library_);
  std::unique_ptr<RecursiveCompilabilityChecker> checker_;
  std::unique_ptr<ProcessFunctionLibraryRuntime> pflr_;
};

TEST_F(CompilabilityCheckUtilTest, CheckNonFunctionalNodes) {
  GraphDefBuilder builder(GraphDefBuilder::kFailImmediately);
  auto opts = builder.opts();
  Node* const0 = ops::SourceOp("InputFloatOp", opts);
  Node* compilable_op = ops::UnaryOp("CompilableOp", const0, opts);
  Node* uncompilable_op = ops::UnaryOp("MissingKernel", compilable_op, opts);
  GraphDef graph_def;
  TF_EXPECT_OK(builder.ToGraphDef(&graph_def));

  auto* flib_runtime = GetFunctionLibraryRuntime();
  // Source node is not compilable.
  EXPECT_FALSE(checker_->IsCompilableNode(*const0, flib_runtime));

  EXPECT_TRUE(checker_->IsCompilableNode(*compilable_op, flib_runtime));

  // Uncompilable as we are only checking compilability in CPU device type.
  EXPECT_FALSE(checker_->IsCompilableNode(*uncompilable_op, flib_runtime));

  const auto summary =
      checker_->FindUncompilableNodes(*uncompilable_op, flib_runtime);
  ASSERT_EQ(1, summary.size());
  const auto it = summary.find("");
  ASSERT_NE(summary.end(), it);
  const auto& uncompilable_node = it->second;
  ASSERT_EQ(1, uncompilable_node.size());
  const auto& node_info = uncompilable_node.at(0);
  EXPECT_EQ("unsupported op", node_info.uncompilable_reason);
}

TEST_F(CompilabilityCheckUtilTest, CheckSimpleFunctionNode) {
  FunctionDefLibrary flib;
  *flib.add_function() = FunctionDefHelper::Define(
      /*Function*/ kUncompilableFunctionName,
      /*Inputs*/ {"n_a:float"},
      /*Outputs*/ {"n_c_uncompilable:float"},
      /*Attributes*/ {},
      // Node info
      {{{kUncompilableFunctionNodeName}, "MissingKernel", {"n_a"}}});
  flib_def_.reset(new FunctionLibraryDefinition(OpRegistry::Global(), flib));

  GraphDefBuilder builder(GraphDefBuilder::kFailImmediately, flib_def_.get());
  std::unique_ptr<Graph> graph(new Graph(flib_def_.get()));
  Node* const0 = ops::SourceOp("InputFloatOp", builder.opts());
  Node* functional_node = ops::UnaryOp(kUncompilableFunctionName, const0,
                                       builder.opts().WithName("D"));
  TF_EXPECT_OK(GraphDefBuilderToGraph(builder, graph.get()));

  auto* flib_runtime = GetFunctionLibraryRuntime();
  EXPECT_FALSE(checker_->IsCompilableNode(*functional_node, flib_runtime));
  const auto summary =
      checker_->FindUncompilableNodes(*functional_node, flib_runtime);

  EXPECT_EQ(1, summary.size());
  const auto it = summary.find(kUncompilableFunctionName);
  ASSERT_NE(summary.end(), it);
  const auto& uncompilable_nodes = it->second;
  ASSERT_EQ(1, uncompilable_nodes.size());

  const auto& node_info = uncompilable_nodes.at(0);
  const auto& node_stack = node_info.stack_trace;
  ASSERT_EQ(2, node_stack.size());
  EXPECT_EQ("D", node_stack.at(0));
  EXPECT_EQ(kUncompilableFunctionNodeName, node_stack.at(1));

  EXPECT_EQ(kUncompilableFunctionNodeName, node_info.name);
  EXPECT_EQ("unsupported op", node_info.uncompilable_reason);
}

TEST_F(CompilabilityCheckUtilTest, CheckFunctionalWhileNode) {
  FunctionDefLibrary flib;
  *flib.add_function() = FunctionDefHelper::Define(
      /*Function*/ kCompilableFunctionName,
      /*Inputs*/ {"n_a:float", "n_b:float"},
      /*Outputs*/ {"n_c:float"},
      /*Attribute*/ {},
      // Node info
      {{{kCompilableFunctionNodeName},
        "Add",
        {"n_a", "n_b"},
        {{"T", DT_FLOAT}}}});
  *flib.add_function() = FunctionDefHelper::Define(
      /*Function*/ kUncompilableFunctionName,
      /*Inputs*/ {"n_a:float"},
      /*Outputs*/ {"n_c_uncompilable:float"},
      /*Attributes*/ {},
      // Node info
      {{{kUncompilableFunctionNodeName}, "MissingKernel", {"n_a"}}});

  flib_def_.reset(new FunctionLibraryDefinition(OpRegistry::Global(), flib));
  GraphDefBuilder builder(GraphDefBuilder::kFailImmediately, flib_def_.get());

  Node* const0 = ops::SourceOp("InputFloatOp", builder.opts());
  Node* input_node = ops::UnaryOp("CompilableOp", const0, builder.opts());

  NameAttrList compilable;
  compilable.set_name(kCompilableFunctionName);
  NameAttrList uncompilable;
  uncompilable.set_name(kUncompilableFunctionName);

  NodeBuilder while_builder(kFunctionalWhileNodeName, "While",
                            builder.opts().op_registry());
  while_builder.Input({input_node, input_node})
      .Attr("cond", compilable)
      .Attr("body", uncompilable);
  builder.opts().FinalizeBuilder(&while_builder);

  GraphDef graph_def;
  TF_EXPECT_OK(builder.ToGraphDef(&graph_def));
  std::unique_ptr<Graph> graph(new Graph(flib_def_.get()));
  TF_CHECK_OK(GraphDefBuilderToGraph(builder, graph.get()));

  auto while_node_it = std::find_if(
      graph->nodes().begin(), graph->nodes().end(),
      [&](const Node* n) { return n->name() == kFunctionalWhileNodeName; });
  EXPECT_NE(while_node_it, graph->nodes().end());

  auto* flib_runtime = GetFunctionLibraryRuntime();

  EXPECT_FALSE(checker_->IsCompilableNode(**while_node_it, flib_runtime));
  const auto summary =
      checker_->FindUncompilableNodes(**while_node_it, flib_runtime);
  ASSERT_FALSE(summary.empty());
  const auto it = summary.find(kUncompilableFunctionName);

  ASSERT_NE(summary.end(), it);
  const auto& uncompilable_nodes = it->second;
  ASSERT_EQ(1, uncompilable_nodes.size());

  const auto& node_info = uncompilable_nodes.at(0);
  const auto& node_stack = node_info.stack_trace;
  ASSERT_EQ(2, node_stack.size());
  EXPECT_EQ(kFunctionalWhileNodeName, node_stack.at(0));
  EXPECT_EQ(kUncompilableFunctionNodeName, node_stack.at(1));

  EXPECT_EQ(kUncompilableFunctionNodeName, node_info.name);
  EXPECT_EQ("unsupported op", node_info.uncompilable_reason);
}

}  // namespace
}  // namespace tensorflow
