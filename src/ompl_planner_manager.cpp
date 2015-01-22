/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2011, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Ioan Sucan */

#include <moveit/ompl/ompl_planner_manager.h>
#include <moveit/robot_state/conversions.h>
#include <moveit/kinematic_constraints/utils.h>
#include <moveit/ompl/detail/constrained_valid_state_sampler.h>
#include <moveit/profiler/profiler.h>
#include <fstream>

namespace moveit_ompl
{

OMPLPlannerManager::OMPLPlannerManager()
  : planning_interface::PlannerManager(),
    nh_("~"),
    simplify_solutions_(true)
{
  std::cout << "loading OMPLPlannerManager " << std::endl;
}

bool OMPLPlannerManager::initialize(const robot_model::RobotModelConstPtr& robot_model, const std::string &ns)
{
  ROS_INFO("Initializing OMPL interface");

  // Save parameters
  robot_model_ = robot_model;
  if (!ns.empty())
    nh_ = ros::NodeHandle(ns);
  std::string ompl_ns = ns.empty() ? "ompl" : ns + "/ompl";

  // Load managers
  constraint_sampler_manager_.reset(new  constraint_samplers::ConstraintSamplerManager());
  constraint_sampler_manager_loader_.reset(new constraint_sampler_manager_loader::ConstraintSamplerManagerLoader(constraint_sampler_manager_));

  context_manager_.reset(new PlanningContextManager(robot_model_, constraint_sampler_manager_));

  // Populate
  loadPlannerConfigurations();

  return true;
}

void OMPLPlannerManager::loadPlannerConfigurations()
{
  const std::vector<std::string> &group_names = robot_model_->getJointModelGroupNames();
  planning_interface::PlannerConfigurationMap pconfig;

  // read the planning configuration for each group
  pconfig.clear();
  for (std::size_t i = 0 ; i < group_names.size() ; ++i)
  {
    // the set of planning parameters that can be specific for the group (inherited by configurations of that group)
    static const std::string KNOWN_GROUP_PARAMS[] = {
      "projection_evaluator", "longest_valid_segment_fraction"
    };

    // get parameters specific for the robot planning group
    std::map<std::string, std::string> specific_group_params;
    for (std::size_t k = 0 ; k < sizeof(KNOWN_GROUP_PARAMS) / sizeof(std::string) ; ++k)
    {
      if (nh_.hasParam(group_names[i] + "/" + KNOWN_GROUP_PARAMS[k]))
      {
        std::string value;
        if (nh_.getParam(group_names[i] + "/" + KNOWN_GROUP_PARAMS[k], value))
        {
          if (!value.empty())
            specific_group_params[KNOWN_GROUP_PARAMS[k]] = value;
        }
        else
        {
          double value_d;
          if (nh_.getParam(group_names[i] + "/" + KNOWN_GROUP_PARAMS[k], value_d))
            specific_group_params[KNOWN_GROUP_PARAMS[k]] = boost::lexical_cast<std::string>(value_d);
          else
          {
            int value_i;
            if (nh_.getParam(group_names[i] + "/" + KNOWN_GROUP_PARAMS[k], value_d))
              specific_group_params[KNOWN_GROUP_PARAMS[k]] = boost::lexical_cast<std::string>(value_i);
            else
            {
              bool value_b;
              if (nh_.getParam(group_names[i] + "/" + KNOWN_GROUP_PARAMS[k], value_b))
                specific_group_params[KNOWN_GROUP_PARAMS[k]] = boost::lexical_cast<std::string>(value_b);
            }
          }
        }
      }
    }

    // set the parameters (if any) for the default group configuration;
    if (!specific_group_params.empty())
    {
      planning_interface::PlannerConfigurationSettings pc;
      pc.name = group_names[i];
      pc.group = group_names[i];
      pc.config = specific_group_params;
      pconfig[pc.name] = pc;
    }

    // get parameters specific to each planner type
    XmlRpc::XmlRpcValue config_names;
    if (nh_.getParam(group_names[i] + "/planner_configs", config_names))
    {
      if (config_names.getType() == XmlRpc::XmlRpcValue::TypeArray)
      {
        for (int32_t j = 0; j < config_names.size() ; ++j)
          if (config_names[j].getType() == XmlRpc::XmlRpcValue::TypeString)
          {
            std::string planner_config = static_cast<std::string>(config_names[j]);
            XmlRpc::XmlRpcValue xml_config;
            if (nh_.getParam("planner_configs/" + planner_config, xml_config))
            {
              if (xml_config.getType() == XmlRpc::XmlRpcValue::TypeStruct)
              {
                planning_interface::PlannerConfigurationSettings pc;
                pc.name = group_names[i] + "[" + planner_config + "]";
                pc.group = group_names[i];
                // inherit parameters from the group (which can be overriden)
                pc.config = specific_group_params;

                // read parameters specific for this configuration
                for (XmlRpc::XmlRpcValue::iterator it = xml_config.begin() ; it != xml_config.end() ; ++it)
                  if (it->second.getType() == XmlRpc::XmlRpcValue::TypeString)
                    pc.config[it->first] = static_cast<std::string>(it->second);
                  else
                    if (it->second.getType() == XmlRpc::XmlRpcValue::TypeDouble)
                      pc.config[it->first] = boost::lexical_cast<std::string>(static_cast<double>(it->second));
                    else
                      if (it->second.getType() == XmlRpc::XmlRpcValue::TypeInt)
                        pc.config[it->first] = boost::lexical_cast<std::string>(static_cast<int>(it->second));
                      else
                        if (it->second.getType() == XmlRpc::XmlRpcValue::TypeBoolean)
                          pc.config[it->first] = boost::lexical_cast<std::string>(static_cast<bool>(it->second));
                pconfig[pc.name] = pc;
              }
              else
                ROS_ERROR("A planning configuration should be of type XmlRpc Struct type (for configuration '%s')", planner_config.c_str());
            }
            else
              ROS_ERROR("Could not find the planner configuration '%s' on the param server", planner_config.c_str());
          }
          else
            ROS_ERROR("Planner configuration names must be of type string (for group '%s')", group_names[i].c_str());
      }
      else
        ROS_ERROR("The planner_configs argument of a group configuration should be an array of strings (for group '%s')", group_names[i].c_str());
    }
  }

  for(planning_interface::PlannerConfigurationMap::iterator it = pconfig.begin();
      it != pconfig.end(); ++it)
  {
    ROS_DEBUG_STREAM_NAMED("parameters","Parameters for configuration '"<< it->first << "'");
    for (std::map<std::string, std::string>::const_iterator config_it = it->second.config.begin() ;
         config_it != it->second.config.end() ; ++config_it)
      ROS_DEBUG_STREAM_NAMED("parameters"," - " << config_it->first << " = " << config_it->second);
  }
  setPlannerConfigurations(pconfig);
}

bool OMPLPlannerManager::canServiceRequest(const moveit_msgs::MotionPlanRequest &req) const
{
  return req.trajectory_constraints.constraints.empty();
}

void OMPLPlannerManager::getPlanningAlgorithms(std::vector<std::string> &algs) const
{
  const planning_interface::PlannerConfigurationMap &pconfig = getPlannerConfigurations();
  algs.clear();
  algs.reserve(pconfig.size());
  for (planning_interface::PlannerConfigurationMap::const_iterator it = pconfig.begin() ; it != pconfig.end() ; ++it)
    algs.push_back(it->first);
}

void OMPLPlannerManager::setPlannerConfigurations(const planning_interface::PlannerConfigurationMap &pconfig)
{
  planning_interface::PlannerConfigurationMap pconfig2 = pconfig;

  // construct default configurations for planning groups that don't have configs already passed in
  const std::vector<const robot_model::JointModelGroup*>& groups = robot_model_->getJointModelGroups();
  for (std::size_t i = 0 ; i < groups.size() ; ++i)
  {
    if (pconfig.find(groups[i]->getName()) == pconfig.end())
    {
      planning_interface::PlannerConfigurationSettings empty;
      empty.name = empty.group = groups[i]->getName();
      pconfig2[empty.name] = empty;
    }
  }

  context_manager_->setPlannerConfigurations(pconfig2);

  PlannerManager::setPlannerConfigurations(getPlannerConfigurations());
}

planning_interface::PlanningContextPtr OMPLPlannerManager::getPlanningContext(const planning_scene::PlanningSceneConstPtr& planning_scene,
                                                                              const planning_interface::MotionPlanRequest &req,
                                                                              moveit_msgs::MoveItErrorCodes &error_code) const
{
  ModelBasedPlanningContextPtr ctx = context_manager_->getPlanningContext(planning_scene, req, error_code);
  if (ctx)
    configureContext(ctx);
  return ctx;
}

/*ModelBasedPlanningContextPtr OMPLPlannerManager::getPlanningContext(const std::string &config, const std::string &factory_type) const
{
  ModelBasedPlanningContextPtr ctx = context_manager_->getPlanningContext(config, factory_type);
  if (ctx)
    configureContext(ctx);
  return ctx;
  }*/

void OMPLPlannerManager::configureContext(const ModelBasedPlanningContextPtr &context) const
{
  context->simplifySolutions(simplify_solutions_);
}

void OMPLPlannerManager::printStatus()
{
  ROS_INFO("OMPL ROS interface is running.");
}

} // namespace