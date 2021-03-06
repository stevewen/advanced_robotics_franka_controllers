
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <controller_interface/multi_interface_controller.h>
#include <dynamic_reconfigure/server.h>
#include <franka_hw/franka_model_interface.h>
#include <franka_hw/franka_state_interface.h>
#include <hardware_interface/joint_command_interface.h>
#include <hardware_interface/robot_hw.h>
#include <franka_hw/trigger_rate.h>
#include <ros/node_handle.h>
#include <ros/time.h>
#include <realtime_tools/realtime_publisher.h>
#include <geometry_msgs/Twist.h>
#include <Eigen/Dense>

#include "math_type_define.h"

namespace advanced_robotics_franka_controllers {

class PositionJointSpaceController : public controller_interface::MultiInterfaceController<
								   franka_hw::FrankaModelInterface,
//								   hardware_interface::EffortJointInterface,
                   hardware_interface::PositionJointInterface,
								   franka_hw::FrankaStateInterface> {
                     
  bool init(hardware_interface::RobotHW* robot_hw, ros::NodeHandle& node_handle) override;
  void starting(const ros::Time& time) override;
  void update(const ros::Time& time, const ros::Duration& period) override;

 private: 
  std::unique_ptr<franka_hw::FrankaModelHandle> model_handle_;
  std::unique_ptr<franka_hw::FrankaStateHandle> state_handle_;
  std::vector<hardware_interface::JointHandle> joint_handles_;

  ros::Time start_time_;
  ros::Duration elapsed_time_;
  franka_hw::TriggerRate print_rate_trigger_{10}; 
									   
  Eigen::Matrix<double, 7, 1> q_init_;
  Eigen::Matrix<double, 7, 1> q_goal_;
  Eigen::Affine3d transform_init_;

  DyrosMath::SaveData joint_data;
  DyrosMath::SaveData save_data1;

  //FILE *hqp_joint_vel;
  //FILE *hqp_joint_pos;
  //FILE *hqp_joint_tor;

  FILE *joint0_data;

};

}  // namespace advanced_robotics_franka_controllers
