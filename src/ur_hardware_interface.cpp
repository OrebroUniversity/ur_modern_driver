/*
 * ur_hardware_control_loop.cpp
 *
 * Copyright 2015 Thomas Timm Andersen
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Based on original source from University of Colorado, Boulder. License copied below. */

/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2015, University of Colorado, Boulder
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
 *   * Neither the name of the Univ of CO, Boulder nor the names of its
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
 *********************************************************************

 Author: Dave Coleman
 */

#include <ur_modern_driver/ur_hardware_interface.h>

#include <iostream>

#include <sensor_msgs/JointState.h>

namespace ros_control_ur {

UrHardwareInterface::UrHardwareInterface(ros::NodeHandle& nh, UrDriver* robot) :
		nh_(nh), robot_(robot) {

	ros::NodeHandle home("~");
	max_vel_change_ = 0.12; // equivalent of an acceleration of 15 rad/sec^2
	
	home.param<double>("vel_limit_alpha",vel_limit_alpha,0.95);
	home.param<double>("vel_alpha",vel_alpha,0.1);
	home.param<double>("pos_alpha",pos_alpha,0.1);
	home.param<double>("eff_alpha",eff_alpha,0.1);
	home.param<double>("frc_alpha",frc_alpha,0.1);
	home.param<double>("trq_alpha",trq_alpha,0.1);

	home.param<bool>("low_pass_filter",low_pass_filter, false);
	home.param<bool>("publish_debug_js",publish_debug_js, false);

	ROS_INFO_NAMED("ur_hardware_interface", "Loaded ur_hardware_interface.");

	if(low_pass_filter) {
	    ROS_WARN("Velocities will be filtered, alpha is %lf",vel_alpha);
	}
	
	if(publish_debug_js) {
	    ROS_INFO("Publishing debug joint states");
	    jnt_state_publisher_ = nh.advertise<sensor_msgs::JointState>("measured_joint_states", 1);
	    jnt_state_publisher_2_ = nh.advertise<sensor_msgs::JointState>("ur10_commands", 1);
	}
	
	init(); // this implementation loads from rosparam
}

void UrHardwareInterface::init() {
	ROS_INFO_STREAM_NAMED("ur_hardware_interface",
			"Reading rosparams from namespace: " << nh_.getNamespace());

	// Get joint names
	nh_.getParam("hardware_interface/joints", joint_names_);
	if (joint_names_.size() == 0) {
		ROS_FATAL_STREAM_NAMED("ur_hardware_interface",
				"No joints found on parameter server for controller, did you load the proper yaml file?" << " Namespace: " << nh_.getNamespace());
		exit(-1);
	}
	num_joints_ = joint_names_.size();

	// Resize vectors
	joint_position_.resize(num_joints_);
	joint_velocity_.resize(num_joints_);
	joint_effort_.resize(num_joints_);
	joint_velocity_limits_.resize(num_joints_);
	joint_position_command_.resize(num_joints_);
	joint_velocity_command_.resize(num_joints_);
	prev_joint_velocity_command_.resize(num_joints_);

	// Initialize controller
	for (std::size_t i = 0; i < num_joints_; ++i) {
		ROS_DEBUG_STREAM_NAMED("ur_hardware_interface",
				"Loading joint name: " << joint_names_[i]);

		// Create joint state interface
		joint_state_interface_.registerHandle(
				hardware_interface::JointStateHandle(joint_names_[i],
						&joint_position_[i], &joint_velocity_[i],
						&joint_effort_[i]));

		// Create position joint interface
		position_joint_interface_.registerHandle(
				hardware_interface::JointHandle(
						joint_state_interface_.getHandle(joint_names_[i]),
						&joint_position_command_[i]));

		// Create velocity joint interface
		velocity_joint_interface_.registerHandle(
				hardware_interface::JointHandle(
						joint_state_interface_.getHandle(joint_names_[i]),
						&joint_velocity_command_[i]));
		prev_joint_velocity_command_[i] = 0.;

		joint_velocity_limits_[i] = 2.0;//0.5*191*M_PI/180;
	}

	joint_velocity_limits_[0] = vel_limit_alpha*131*M_PI/180;
	joint_velocity_limits_[1] = vel_limit_alpha*131*M_PI/180;
	joint_velocity_limits_[2] = vel_limit_alpha*191*M_PI/180;
	joint_velocity_limits_[3] = vel_limit_alpha*191*M_PI/180;
	joint_velocity_limits_[4] = vel_limit_alpha*191*M_PI/180;
	joint_velocity_limits_[5] = vel_limit_alpha*191*M_PI/180;

	// Create force torque interface
	force_torque_interface_.registerHandle(
			hardware_interface::ForceTorqueSensorHandle("wrench", "",
					robot_force_, robot_torque_));

	registerInterface(&joint_state_interface_); // From RobotHW base class.
	registerInterface(&position_joint_interface_); // From RobotHW base class.
	registerInterface(&velocity_joint_interface_); // From RobotHW base class.
	registerInterface(&force_torque_interface_); // From RobotHW base class.
	velocity_interface_running_ = false;
	position_interface_running_ = false;

#ifdef USE_ROBOTIQ_FT
	ft_device_name_ = "";
	nh_.getParam("hardware_interface/ft_sensor_device", ft_device_name_);
	max_retries_ = 100;

	//If we can't initialize, we return an error
	ret = rq_sensor_state(max_retries_, ft_device_name_);
	if(ret == -1)
	{
	    ROS_ERROR("could not connect to FT sensor!");
	}

	//Reads basic info on the sensor
	ret = rq_sensor_state(max_retries_, ft_device_name_);
	if(ret == -1)
	{
	    ROS_ERROR("could not connect to FT sensor!");
	}

	//Starts the stream
	ret = rq_sensor_state(max_retries_, ft_device_name_);
	if(ret == -1)
	{
	    ROS_ERROR("could not connect to FT sensor!");
	}
#endif

}

void UrHardwareInterface::read() {
	std::vector<double> pos, vel, current, tcp;
	pos = robot_->rt_interface_->robot_state_->getQActual();
	vel = robot_->rt_interface_->robot_state_->getQdActual();
	current = robot_->rt_interface_->robot_state_->getIActual();
	tcp = robot_->rt_interface_->robot_state_->getTcpForce();


	for (std::size_t i = 0; i < num_joints_; ++i) {
	    if(low_pass_filter) {		
		joint_position_[i] = (1-pos_alpha)*pos[i] + pos_alpha*joint_position_[i];
		joint_velocity_[i] = (1-vel_alpha)*vel[i] + vel_alpha*joint_velocity_[i];
		joint_effort_[i] = (1-eff_alpha)*current[i] + eff_alpha*joint_effort_[i];
	    } else {
		joint_position_[i] = pos[i];
		joint_velocity_[i] = vel[i];
		joint_effort_[i] = current[i];
	    }
	}

#ifdef USE_ROBOTIQ_FT
	//TODO: This should probably run in a separate thread an not block here!
	//FIXME: If you see red in the command line, it is NOT safe to run the robot!
	max_retries_ = 1;
	ret = rq_sensor_state(max_retries_, ft_device_name_);
	if(ret == -1)
	{
	    ROS_ERROR("Could not read data from FT sensor, defaulting to inbuilt!");
	    for (std::size_t i = 0; i < 3; ++i) {
		robot_force_[i] = (1-frc_alpha)*tcp[i] + frc_alpha*robot_force_[i];
		robot_torque_[i] = (1-trq_alpha)*tcp[i + 3] + trq_alpha*robot_torque_[i];
	    }
	} 
	else {
	    if(rq_sensor_get_current_state() == RQ_STATE_RUN)
	    {
		strcpy(bufStream,"");
		msgStream = get_data();

		if(rq_state_got_new_message())
		{
		    robot_force_[0] = msgStream.Fx;
		    robot_force_[1] = msgStream.Fy;
		    robot_force_[2] = msgStream.Fz;
		    robot_torque_[0] = msgStream.Mx;
		    robot_torque_[1] = msgStream.My;
		    robot_torque_[2] = msgStream.Mz;
		}
	    } else {
		ROS_ERROR("Could not receive data from FT sensor, defaulting to inbuilt!");
		for (std::size_t i = 0; i < 3; ++i) {
		    robot_force_[i] = (1-frc_alpha)*tcp[i] + frc_alpha*robot_force_[i];
		    robot_torque_[i] = (1-trq_alpha)*tcp[i + 3] + trq_alpha*robot_torque_[i];
		}
	    }
	}

#else
	for (std::size_t i = 0; i < 3; ++i) {
	    if(low_pass_filter) {		
		robot_force_[i] = (1-frc_alpha)*tcp[i] + frc_alpha*robot_force_[i];
		robot_torque_[i] = (1-trq_alpha)*tcp[i + 3] + trq_alpha*robot_torque_[i];
	    } else {
		robot_force_[i] =  tcp[i];
		robot_torque_[i] = tcp[i + 3];
	    }
	}
#endif

	if(publish_debug_js) {
	    // publish unfiltered joint state data
	    sensor_msgs::JointState msg;
	    msg.header.stamp = ros::Time::now();
	    for (std::size_t i=0; i<num_joints_; ++i) {
		msg.position.push_back(pos[i]);
		msg.velocity.push_back(vel[i]);
		msg.effort.push_back(current[i]);
	    }
	    jnt_state_publisher_.publish(msg);
	}
#if 0
#endif

}

void UrHardwareInterface::setMaxVelChange(double inp) {
	max_vel_change_ = inp;
	ROS_WARN("Setting max joint acceleration to %lf",inp);
}

void UrHardwareInterface::write() {
	if (velocity_interface_running_) {
		std::vector<double> cmd;
		//do some rate limiting
		//max_vel_change_ = 0.01;
		cmd.resize(joint_velocity_command_.size());
		for (unsigned int i = 0; i < joint_velocity_command_.size(); i++) {
			cmd[i] = joint_velocity_command_[i];
			if (cmd[i] > prev_joint_velocity_command_[i] + max_vel_change_) {
				cmd[i] = prev_joint_velocity_command_[i] + max_vel_change_;
			} else if (cmd[i]
					< prev_joint_velocity_command_[i] - max_vel_change_) {
				cmd[i] = prev_joint_velocity_command_[i] - max_vel_change_;
			}

			if (cmd[i] > joint_velocity_limits_[i]) {
				cmd[i] = joint_velocity_limits_[i];
			} else if (cmd[i] < -joint_velocity_limits_[i]) {
				cmd[i] = -joint_velocity_limits_[i];
			}
			prev_joint_velocity_command_[i] = cmd[i];
		}

		if(publish_debug_js) {
		    sensor_msgs::JointState msg;
		    msg.header.stamp = ros::Time::now();
		    for (std::size_t i=0; i<joint_velocity_command_.size(); ++i) {
			msg.velocity.push_back(cmd[i]);
			//msg.velocity.push_back(joint_velocity_command_[i]);
		    }
		    jnt_state_publisher_2_.publish(msg);
		}

		robot_->setSpeed(cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5],  max_vel_change_*125);

	} else if (position_interface_running_) {
		robot_->servoj(joint_position_command_);
	}
}

bool UrHardwareInterface::canSwitch(
		const std::list<hardware_interface::ControllerInfo> &start_list,
		const std::list<hardware_interface::ControllerInfo> &stop_list) const {
	for (std::list<hardware_interface::ControllerInfo>::const_iterator controller_it =
			start_list.begin(); controller_it != start_list.end();
			++controller_it) {
		if (controller_it->claimed_resources.at(0).hardware_interface
				== "hardware_interface::VelocityJointInterface") {
			if (velocity_interface_running_) {
				ROS_ERROR(
						"%s: An interface of that type (%s) is already running",
						controller_it->name.c_str(),
						controller_it->claimed_resources.at(0).hardware_interface.c_str());
				return false;
			}
			if (position_interface_running_) {
				bool error = true;
				for (std::list<hardware_interface::ControllerInfo>::const_iterator stop_controller_it =
						stop_list.begin();
						stop_controller_it != stop_list.end();
						++stop_controller_it) {
					if (stop_controller_it->claimed_resources.at(0).hardware_interface
							== "hardware_interface::PositionJointInterface") {
						error = false;
						break;
					}
				}
				if (error) {
					ROS_ERROR(
							"%s (type %s) can not be run simultaneously with a PositionJointInterface",
							controller_it->name.c_str(),
							controller_it->claimed_resources.at(0).hardware_interface.c_str());
					return false;
				}
			}
		} else if (controller_it->claimed_resources.at(0).hardware_interface
				== "hardware_interface::PositionJointInterface") {
			if (position_interface_running_) {
				ROS_ERROR(
						"%s: An interface of that type (%s) is already running",
						controller_it->name.c_str(),
						controller_it->claimed_resources.at(0).hardware_interface.c_str());
				return false;
			}
			if (velocity_interface_running_) {
				bool error = true;
				for (std::list<hardware_interface::ControllerInfo>::const_iterator stop_controller_it =
						stop_list.begin();
						stop_controller_it != stop_list.end();
						++stop_controller_it) {
					if (stop_controller_it->claimed_resources.at(0).hardware_interface
							== "hardware_interface::VelocityJointInterface") {
						error = false;
						break;
					}
				}
				if (error) {
					ROS_ERROR(
							"%s (type %s) can not be run simultaneously with a VelocityJointInterface",
							controller_it->name.c_str(),
							controller_it->claimed_resources.at(0).hardware_interface.c_str());
					return false;
				}
			}
		}
	}

// we can always stop a controller
	return true;
}

void UrHardwareInterface::doSwitch(
		const std::list<hardware_interface::ControllerInfo>& start_list,
		const std::list<hardware_interface::ControllerInfo>& stop_list) {
	for (std::list<hardware_interface::ControllerInfo>::const_iterator controller_it =
			stop_list.begin(); controller_it != stop_list.end();
			++controller_it) {
		if (controller_it->claimed_resources.at(0).hardware_interface
				== "hardware_interface::VelocityJointInterface") {
			velocity_interface_running_ = false;
			ROS_DEBUG("Stopping velocity interface");
		}
		if (controller_it->claimed_resources.at(0).hardware_interface
				== "hardware_interface::PositionJointInterface") {
			position_interface_running_ = false;
			std::vector<double> tmp;
			robot_->closeServo(tmp);
			ROS_DEBUG("Stopping position interface");
		}
	}
	for (std::list<hardware_interface::ControllerInfo>::const_iterator controller_it =
			start_list.begin(); controller_it != start_list.end();
			++controller_it) {
		if (controller_it->claimed_resources.at(0).hardware_interface
				== "hardware_interface::VelocityJointInterface") {
			velocity_interface_running_ = true;
			ROS_DEBUG("Starting velocity interface");
		}
		if (controller_it->claimed_resources.at(0).hardware_interface
				== "hardware_interface::PositionJointInterface") {
			position_interface_running_ = true;
			robot_->uploadProg();
			ROS_DEBUG("Starting position interface");
		}
	}

}

} // namespace

