// Copyright (c) 2023, Samsung Research America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "pluginlib/class_loader.hpp"
#include "pluginlib/class_list_macros.hpp"
#include "nav2_util/node_utils.hpp"
#include "nav2_util/lifecycle_node.hpp"
#include "nav2_route/types.hpp"
#include "nav2_route/utils.hpp"
#include "nav2_route/interfaces/route_operation.hpp"
#include "nav2_route/operations_manager.hpp"

namespace nav2_route
{

OperationsManager::OperationsManager(nav2_util::LifecycleNode::SharedPtr node)
: plugin_loader_("nav2_route", "nav2_route::RouteOperation")
{
  logger_ = node->get_logger();
  nav2_util::declare_parameter_if_not_declared(
    node, "use_feedback_operations", rclcpp::ParameterValue(true));
  use_feedback_operations_ = node->get_parameter("use_feedback_operations").as_bool();

  // Have some default operations
  const std::vector<std::string> default_plugin_id({"AdjustSpeedLimit"});
  const std::string default_plugin_type = "nav2_route::AdjustSpeedLimit";

  nav2_util::declare_parameter_if_not_declared(
    node, "operations", rclcpp::ParameterValue(default_plugin_id));
  auto operation_ids = node->get_parameter("operations").as_string_array();

  if (operation_ids == default_plugin_id) {
    nav2_util::declare_parameter_if_not_declared(
      node, default_plugin_id[0] + ".plugin", rclcpp::ParameterValue(default_plugin_type));
  }

  // Create plugins and sort them into On Query, Status Change, and Graph-calling Operations
  for (size_t i = 0; i != operation_ids.size(); i++) {
    try {
      std::string type = nav2_util::get_plugin_type_param(node, operation_ids[i]);
      RouteOperation::Ptr operation = plugin_loader_.createSharedInstance(type);
      RCLCPP_INFO(
        node->get_logger(), "Created route operation %s of type %s",
        operation_ids[i].c_str(), type.c_str());
      operation->configure(node, operation_ids[i]);
      RouteOperationType process_type = operation->processType();
      if (process_type == RouteOperationType::ON_QUERY) {
        query_operations_.push_back(std::move(operation));
      } else if (process_type == RouteOperationType::ON_STATUS_CHANGE) {
        change_operations_.push_back(std::move(operation));
      } else {
        graph_operations_[operation->getName()] = operation;
      }
    } catch (const pluginlib::PluginlibException & ex) {
      RCLCPP_FATAL(
        node->get_logger(),
        "Failed to create route operation. Exception: %s", ex.what());
      throw ex;
    }
  }
}

template<typename T>
void OperationsManager::findGraphOperationsToProcess(
  T & obj, const OperationTrigger & trigger,
  OperationPtrs & operations)
{
  if (!obj) {
    return;
  }

  Operations & op_vec = obj->operations;
  for (Operations::iterator it = op_vec.begin(); it != op_vec.end(); ++it) {
    if (it->trigger == trigger) {
      operations.push_back(&(*it));
    }
  }
}

OperationPtrs OperationsManager::findGraphOperations(
  const NodePtr node, const EdgePtr edge_enter, const EdgePtr edge_exit)
{
  OperationPtrs ops;
  findGraphOperationsToProcess(node, OperationTrigger::NODE, ops);
  findGraphOperationsToProcess(edge_enter, OperationTrigger::ON_ENTER, ops);
  findGraphOperationsToProcess(edge_exit, OperationTrigger::ON_EXIT, ops);
  return ops;
}

void OperationsManager::updateResult(
  const std::string & name, const OperationResult & op_result, OperationsResult & result)
{
  result.reroute = result.reroute || op_result.reroute;
  result.blocked_ids.insert(
    result.blocked_ids.end(), op_result.blocked_ids.begin(), op_result.blocked_ids.end());
  result.operations_triggered.push_back(name);
}

void OperationsManager::processOperationsPluginVector(
  const std::vector<RouteOperation::Ptr> & route_operations,
  OperationsResult & result,
  const NodePtr node,
  const EdgePtr edge_entered,
  const EdgePtr edge_exited,
  const Route & route,
  const geometry_msgs::msg::PoseStamped & pose)
{
  for (OperationsIter it = route_operations.begin(); it != route_operations.end(); ++it) {
    const RouteOperation::Ptr & plugin = *it;
    OperationResult op_result = plugin->perform(node, edge_entered, edge_exited, route, pose);
    updateResult(plugin->getName(), op_result, result);
  }
}

OperationsResult OperationsManager::process(
  const bool status_change,
  const RouteTrackingState & state,
  const Route & route,
  const geometry_msgs::msg::PoseStamped & pose,
  const ReroutingState & rerouting_info)
{
  // Get important state information
  OperationsResult result;
  NodePtr node = state.last_node;
  EdgePtr edge_entered = state.current_edge;
  EdgePtr edge_exited =
    state.route_edges_idx > 0 ? route.edges[state.route_edges_idx - 1] : nullptr;

  // If we have rerouting info curr_edge, then after the first node is achieved,
  // the robot is exiting the partial previous edge.
  if (state.route_edges_idx == 0 && rerouting_info.curr_edge) {
    edge_exited = rerouting_info.curr_edge;
  }

  if (status_change) {
    // Process operations defined in the navigation graph at node or edge
    OperationPtrs operations = findGraphOperations(node, edge_entered, edge_exited);
    for (unsigned int i = 0; i != operations.size(); i++) {
      auto op = graph_operations_.find(operations[i]->type);
      if (op != graph_operations_.end()) {
        OperationResult op_result = op->second->perform(
          node, edge_entered, edge_exited, route, pose, &operations[i]->metadata);
        updateResult(op->second->getName(), op_result, result);
      } else if (use_feedback_operations_) {
        RCLCPP_INFO(
          logger_, "Operation '%s' should be called from action feedback!",
          operations[i]->type.c_str());
        result.operations_triggered.push_back(operations[i]->type);
      } else {
        throw nav2_core::OperationFailed(
                "Operation " + operations[i]->type +
                " does not exist in route operations loaded!");
      }
    }

    // Process operations which trigger on any status changes
    processOperationsPluginVector(
      change_operations_, result, node, edge_entered, edge_exited, route, pose);
  }

  // Process operations which trigger regardless of status change or nodes / edges
  processOperationsPluginVector(
    query_operations_, result, node, edge_entered /*edge_curr*/, edge_exited, route, pose);
  return result;
}

}  // namespace nav2_route
