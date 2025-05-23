/*
 *  Copyright (C) 2023 – 2024 Advanced Micro Devices, Inc. All rights reserved.
 *  Licensed under the MIT License.
 */

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#  pragma GCC diagnostic ignored "-Wconversion"
#endif

#include "vaip/dd/coeffs.hpp"
// #include "vaip/pattern_zoo.hpp"
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

#include "vaip/dd/dd_utils.hpp"
#include "vaip/vaip.hpp"
#include "vitis/ai/env_config.hpp"

#include <cmath>
#include <glog/logging.h>
#include <unordered_map>
#include <vector>

DEF_ENV_PARAM(DEBUG_DD_MERGE_QCONCAT, "0")
#define MY_LOG(n) LOG_IF(INFO, ENV_PARAM(DEBUG_DD_MERGE_QCONCAT) >= n)
namespace {
using namespace vaip_core;

static void add_node_attr_qconcat_single_node(NodeBuilder& building_node,
                                              const NodeInput& concat) {
  std::vector<std::string> nodes = {node_arg_get_name(*concat.node_arg)};
  building_node.add("nodes", nodes);
}

// gets all the nodes that are matched with binder and stores in nodes attribute
// of new node

static void add_node_attr_qconcat(onnxruntime::Graph* graph,
                                  NodeBuilder& building_node,
                                  binder_t& binder) {
  std::vector<std::string> nodes = vaip::dd::get_node_names(graph, binder);

  building_node.add("nodes", nodes);
}

static void update_shape(NodeBuilder& building_node, const NodeInput& output) {
  auto shape = *node_arg_get_shape_i64(*output.node_arg);
  building_node.add("orig_output_shape", shape);
}
std::vector<const NodeArg*>
get_matched_input(const std::vector<std::shared_ptr<Pattern>>& dequant,
                  const binder_t& binder) {
  std::vector<const NodeArg*> ret;
  for (auto p : dequant) {
    auto node_input = binder[p->get_id()];
    if (!node_input.node_arg) { // no more input
      break;
    }
    ret.push_back(node_input.node_arg);
  }
  return ret;
}
struct MergeQConcat {
  MergeQConcat(IPass& self) : self_{self} {}
  std::unique_ptr<Rule> create_rule(IPass* self) {
    auto builder = vaip_core::PatternBuilder();
    constexpr int max_input = 20;
    std::vector<std::shared_ptr<Pattern>> input;
    std::vector<bool> optional(max_input, true);
    optional[0] = false; // at least two input for concat
    optional[1] = false;
    input.reserve(max_input);
    std::vector<std::shared_ptr<Pattern>> dq_input_vec_wildcard;
    dq_input_vec_wildcard.reserve(max_input);
    std::vector<std::shared_ptr<Pattern>> dq_input_vec;
    dq_input_vec.reserve(max_input * 2);

    // Pattern in the following for loop matches with atmost 20 dequantizelinear
    // nodes
    for (auto i = 0; i < max_input; i++) {
      dq_input_vec_wildcard.push_back(builder.wildcard());
      dq_input_vec.push_back(builder.constant());
      dq_input_vec.push_back(builder.constant());
      input.push_back(builder.node2(
          "com.microsoft:DequantizeLinear",
          {dq_input_vec_wildcard[i], dq_input_vec[2 * i],
           dq_input_vec[2 * i + 1]})); // change this to com.mocrosoft.deqautn
    }
    auto concat_0 = builder.node3("Concat", input, optional);
    auto constant_0 = builder.constant();
    auto constant_1 = builder.constant();
    auto pattern_ = builder.node2("com.microsoft:QuantizeLinear",
                                  {concat_0, constant_0, constant_1});

    return Rule::create_rule(
        pattern_, [=](onnxruntime::Graph* graph, binder_t& binder) -> bool {
          auto matched_input = get_matched_input(dq_input_vec_wildcard, binder);
          auto ni_output = binder[pattern_->get_id()];
          auto out_scale = binder[constant_0->get_id()];
          auto out_zp = binder[constant_1->get_id()];

          // Excluding the op fusion of 2 concats, as they are causing high
          // l2norms
          bool is_custom = false;
          auto args = self->get_pass_proto().args();
          if (!args.empty()) {
            std::vector<std::string> arg;
            for (const auto& str : args) {
              arg.push_back(str);
            }
            if (arg[0] == "custom_op")
              is_custom = true;
          }
          if (is_custom) {
            auto shape = *node_arg_get_shape_i64(*ni_output.node_arg);
            auto output_scale =
                node_arg_get_const_data_as_float(*graph, *out_scale.node_arg);
            auto output_zp =
                node_arg_get_const_data_as_u16(*graph, *out_zp.node_arg);
            std::vector<std::string> in_dtypes(matched_input.size(), "uint16");
            std::vector<std::string> out_dtypes = {"uint16"};
            auto node_name = node_arg_get_name(*ni_output.node_arg);
            std::vector<std::string> inputs;
            for (int i = 0; i < matched_input.size(); i++) {
              const NodeArg* inps = matched_input[i];
              inputs.push_back(node_arg_get_name(*inps));
            }
            std::vector<std::string> outputs = {node_name};
            auto constant_initializers = std::vector<std::string>{};
            auto [meta_def, err] =
                self->try_fuse(*graph, "concat_" + node_name, inputs, outputs,
                               constant_initializers, "CONCAT");
            if (meta_def == nullptr) {
              LOG(FATAL) << "Cannot fuse DQ:  " << err.comments;
            }
            auto& generic_param = *meta_def->mutable_generic_param();
            generic_param["out_scale"] = std::to_string(output_scale);
            generic_param["out_zero_point"] = std::to_string(output_zp);
            [[maybe_unused]] auto& fused_node =
                self->fuse(*graph, std::move(*meta_def));

            return true;

          } else if (
              node_arg_get_name(*ni_output.node_arg) ==
                  "/up_blocks.2/cats.2/Concat_output_0_QuantizeLinear_Output" ||
              node_arg_get_name(*ni_output.node_arg) ==
                  "/up_blocks.3/cats.0/Concat_output_0_QuantizeLinear_Output") {
            auto matched_input = get_matched_input(input, binder);
            auto ni_output = binder[concat_0->get_id()];
            auto new_node = NodeBuilder(*graph, self_);
            new_node.set_input_node_args(matched_input);
            new_node.set_op_type("QConcat", "com.xilinx");
            update_shape(new_node, ni_output);
            add_node_attr_qconcat_single_node(new_node,
                                              binder[concat_0->get_id()]);
            std::vector<std::string> in_dtypes(matched_input.size(),
                                               "bfloat16");
            std::vector<std::string> out_dtypes = {"bfloat16"};
            new_node.add("in_dtypes", in_dtypes);
            new_node.add("out_dtypes", out_dtypes);
            new_node.add("QDQConcat", "false");
            new_node.set_anchor_point1(*ni_output.node_arg);
            new_node.build();
            MY_LOG(1) << "Modified Concat Pattern";
            return true;
          } else {
            auto new_node = NodeBuilder(*graph, self_);
            new_node.set_input_node_args(matched_input);
            new_node.set_op_type("QConcat", "com.xilinx");
            add_node_attr_qconcat(graph, new_node, binder);
            update_shape(new_node, ni_output);
            std::vector<std::string> in_dtypes(matched_input.size(), "uint16");
            std::vector<std::string> out_dtypes = {"uint16"};
            new_node.add("QDQConcat", "true");
            new_node.add("in_dtypes", in_dtypes);
            new_node.add("out_dtypes", out_dtypes);
            // Get final quant linear node of the pattern's scale and zp and
            // store them as attibutes, so that any op's pass  that is after
            // concat pass can use these attributes if they are parents of
            // concat (eg: EltWiseAdd op)
            auto output_scale = node_arg_get_const_data_as_float(
                *graph, *binder[constant_0->get_id()].node_arg);
            auto output_zp = node_arg_get_const_data_as_u16(
                *graph, *binder[constant_1->get_id()].node_arg);
            new_node.add("output_scale", output_scale);
            new_node.add("output_zp", float(output_zp));

            new_node.set_anchor_point1(*ni_output.node_arg);
            new_node.build();
            MY_LOG(1) << "Modified Concat Pattern";
            return true;
          }
        });
  }
  void process(IPass& self, Graph& graph) { create_rule(&self)->apply(&graph); }

public:
  IPass& self_;
};
} // namespace

DEFINE_VAIP_PASS(MergeQConcat, vaip_pass_dd_merge_qconcat)
