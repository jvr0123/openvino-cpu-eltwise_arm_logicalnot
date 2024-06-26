// Copyright (C) 2018-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "transformations/common_optimizations/move_eltwise_up_data_movement.hpp"

#include <memory>
#include <numeric>
#include <openvino/opsets/opset8.hpp>

#include "itt.hpp"
#include "openvino/core/rt_info.hpp"
#include "openvino/core/type.hpp"
#include "openvino/pass/pattern/op/wrap_type.hpp"
#include "transformations/utils/utils.hpp"

namespace {
bool is_data_movement_operation(const std::shared_ptr<ov::Node>& node,
                                const std::vector<ov::DiscreteTypeInfo>& allowed_data_movement_ops) {
    for (auto& allowed_type : allowed_data_movement_ops) {
        if (node->get_type_info().is_castable(allowed_type))
            return true;
    }

    return false;
}

bool is_scalar_like(const std::shared_ptr<ov::Node>& node) {
    auto constant_op = std::dynamic_pointer_cast<ov::opset8::Constant>(node);
    return constant_op != nullptr && shape_size(constant_op->get_shape()) == 1;
}
}  // namespace

std::vector<ov::DiscreteTypeInfo> ov::pass::MoveEltwiseUpThroughDataMov::get_default_allowed_ops() {
    return {
        ov::op::v0::Squeeze::get_type_info_static(),
        ov::op::v0::Unsqueeze::get_type_info_static(),
        ov::op::v1::Reshape::get_type_info_static(),
        ov::op::v1::Transpose::get_type_info_static(),
        ov::op::v0::ShuffleChannels::get_type_info_static(),
        ov::op::v7::Roll::get_type_info_static(),
        ov::op::v0::ReverseSequence::get_type_info_static(),
        ov::op::v0::DepthToSpace::get_type_info_static(),
        ov::op::v1::BatchToSpace::get_type_info_static(),
        ov::op::v1::Broadcast::get_type_info_static(),
        ov::op::v3::Broadcast::get_type_info_static(),
        ov::op::v1::Gather::get_type_info_static(),
        ov::op::v7::Gather::get_type_info_static(),
        ov::op::v8::Gather::get_type_info_static(),
    };
}

ov::pass::MoveEltwiseUpThroughDataMov::MoveEltwiseUpThroughDataMov(
    std::vector<DiscreteTypeInfo> allowed_data_movement_ops) {
    MATCHER_SCOPE(MoveEltwiseUpThroughDataMov);
    auto eltwise_pattern = ov::pass::pattern::wrap_type<ov::op::util::UnaryElementwiseArithmetic,
                                                        ov::op::util::BinaryElementwiseArithmetic,
                                                        ov::op::v0::FakeQuantize>(ov::pass::pattern::has_static_rank());

    ov::matcher_pass_callback callback = [OV_CAPTURE_CPY_AND_THIS](ov::pass::pattern::Matcher& m) {
        const auto& pattern_map = m.get_pattern_value_map();

        auto eltwise = pattern_map.at(eltwise_pattern).get_node_shared_ptr();
        if (transformation_callback(eltwise)) {
            return false;
        }

        if (eltwise->get_output_target_inputs(0).size() != 1) {
            return false;
        }

        for (size_t i = 1; i < eltwise->get_input_size(); ++i) {
            if (!is_scalar_like(eltwise->get_input_node_shared_ptr(i))) {
                return false;
            }
        }

        auto current = eltwise->get_input_node_shared_ptr(0);
        auto child = eltwise;

        while (is_data_movement_operation(current, allowed_data_movement_ops)) {
            if (current->get_output_size() != 1 || current->get_output_target_inputs(0).size() != 1 ||
                current->get_output_element_type(0) != current->get_input_element_type(0)) {
                return false;
            }

            child = current;
            current = current->get_input_node_shared_ptr(0);
        }

        // now current is the first not data movement op
        if (child == eltwise) {
            return false;
        }

        // eltwise constant shape should match new input shape
        for (size_t i = 1; i < eltwise->get_input_size(); i++) {
            if (current->get_output_partial_shape(0).size() != eltwise->get_input_partial_shape(i).size()) {
                auto old_eltwise_const = ov::as_type_ptr<ov::opset8::Constant>(eltwise->get_input_node_shared_ptr(i));
                if (old_eltwise_const->get_shape().size() != 0) {
                    auto new_constant = std::make_shared<ov::opset8::Constant>(*old_eltwise_const.get(), ov::Shape{});
                    ov::replace_node_update_name(old_eltwise_const, new_constant);
                }
            }
        }
        ov::replace_output_update_name(eltwise->output(0), eltwise->input_value(0));

        ov::OutputVector eltwise_inputs = eltwise->input_values();
        eltwise_inputs[0] = child->input_value(0);
        auto new_eltwise = eltwise->clone_with_new_inputs(eltwise_inputs);
        // WA: it's necessary to set empty friendly name here
        // to avoid name duplication in TypeRelaxed cases
        new_eltwise->set_friendly_name("");
        ov::copy_runtime_info(eltwise, new_eltwise);

        ov::OutputVector child_inputs = child->input_values();
        child_inputs[0] = new_eltwise;
        auto new_child = child->clone_with_new_inputs(child_inputs);
        ov::copy_runtime_info(child, new_child);
        new_child->set_friendly_name(child->get_friendly_name());

        ov::replace_node(child, new_child);
        return true;
    };

    auto m = std::make_shared<ov::pass::pattern::Matcher>(eltwise_pattern, matcher_name);
    register_matcher(m, callback);
}
