#include <advanced_robotics_franka_controllers/collision_detection_controller.h>
#include <moveit/move_group_interface/move_group_interface.h>
#include <moveit/planning_scene_interface/planning_scene_interface.h>
#include <moveit_msgs/DisplayRobotState.h>
#include <moveit_msgs/DisplayTrajectory.h>
#include <moveit_msgs/CollisionObject.h>
#include <moveit_visual_tools/moveit_visual_tools.h>
#include <ftd2xx.h>
#include <random>
#include <fstream>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <unistd.h>
#include <controller_interface/controller_base.h>
#include <pluginlib/class_list_macros.h>
#include <ros/ros.h>
#include <franka/robot_state.h>
#include "math_type_define.h"


namespace advanced_robotics_franka_controllers
{
    enum MODE {WAIT, EXEC, REST};

    bool CollisionDetectionController::init(hardware_interface::RobotHW* robot_hw, ros::NodeHandle& node_handle)
    {
        std::vector<std::string> joint_names;
        std::string arm_id;
        ROS_WARN(
            "ForceExampleController: Make sure your robot's endeffector is in contact "
            "with a horizontal surface before starting the controller!");
        if (!node_handle.getParam("arm_id", arm_id))
        {
            ROS_ERROR("ForceExampleController: Could not read parameter arm_id");
            return false;
        }
        if ((!node_handle.getParam("joint_names", joint_names)) || (joint_names.size() != 7))
        {
            ROS_ERROR(
                "ForceExampleController: Invalid or no joint_names parameters provided, aborting "
                "controller init!");
            return false;
        }

        auto* model_interface = robot_hw->get<franka_hw::FrankaModelInterface>();
        if (model_interface == nullptr)
        {
            ROS_ERROR_STREAM("ForceExampleController: Error getting model interface from hardware");
            return false;
        }
        try
        {
            model_handle_ = std::make_unique<franka_hw::FrankaModelHandle>(
                model_interface->getHandle(arm_id + "_model"));
        }
        catch (hardware_interface::HardwareInterfaceException& ex)
        {
            ROS_ERROR_STREAM(
                "ForceExampleController: Exception getting model handle from interface: " << ex.what());
            return false;
        }

        auto* state_interface = robot_hw->get<franka_hw::FrankaStateInterface>();
        if (state_interface == nullptr)
        {
            ROS_ERROR_STREAM("ForceExampleController: Error getting state interface from hardware");
            return false;
        }
        try
        {
            state_handle_ = std::make_unique<franka_hw::FrankaStateHandle>(
                state_interface->getHandle(arm_id + "_robot"));
        }
        catch (hardware_interface::HardwareInterfaceException& ex)
        {
            ROS_ERROR_STREAM(
                "ForceExampleController: Exception getting state handle from interface: " << ex.what());
            return false;
        }

        auto* effort_joint_interface = robot_hw->get<hardware_interface::EffortJointInterface>();
        if (effort_joint_interface == nullptr)
        {
            ROS_ERROR_STREAM("ForceExampleController: Error getting effort joint interface from hardware");
            return false;
        }
        for (size_t i = 0; i < 7; i++)
        {
            try
            {
                joint_handles_.push_back(effort_joint_interface->getHandle(joint_names[i]));
            }
            catch (const hardware_interface::HardwareInterfaceException& ex)
            {
                ROS_ERROR_STREAM("ForceExampleController: Exception getting joint handles: " << ex.what());
                return false;
            }
        }

        FT_STATUS ft_status;
        DWORD num_devs;
        DWORD num_bytes_to_send = 0;
        DWORD num_bytes_sent = 0;
        DWORD num_bytes_to_read = 0;
        DWORD num_bytes_read = 0;
        BYTE buffer[8];
        try
        {
            if ((ft_status = FT_CreateDeviceInfoList(&num_devs)) != FT_OK) throw 1;
            if (num_devs < 1) { ROS_ERROR_STREAM("There are no FTDI devices installed"); return false; }
            if ((ft_status = FT_Open(0, &ft_handle)) != FT_OK) throw 2;
            if ((ft_status = FT_ResetDevice(ft_handle)) != FT_OK) throw 3;
            if ((ft_status = FT_GetQueueStatus(ft_handle, &num_bytes_to_read)) != FT_OK) throw 4;
            if (num_bytes_to_read > 0) FT_Read(ft_handle, &buffer, num_bytes_to_read, &num_bytes_read);
            if ((ft_status = FT_SetLatencyTimer(ft_handle, 2)) != FT_OK) throw 5;
            if ((ft_status = FT_SetChars(ft_handle, false, 0, false, 0)) != FT_OK) throw 6;
            if ((ft_status = FT_SetTimeouts(ft_handle, 7, 1000)) != FT_OK) throw 7;
            if ((ft_status = FT_SetFlowControl(ft_handle, FT_FLOW_NONE, 0x0, 0x0)) != FT_OK) throw 8;
            if ((ft_status = FT_SetBitMode(ft_handle, 0x0, FT_BITMODE_RESET)) != FT_OK) throw 9;
            if ((ft_status = FT_SetBitMode(ft_handle, 0x0, FT_BITMODE_MPSSE)) != FT_OK) throw 10;
            usleep(50000);
            // Configure data bits high-byte of MPSSE port
            buffer[0] = 0x82; buffer[1] = 0x00; buffer[2] = 0x00; num_bytes_to_send = 3;
            // Send off the high GPIO config commands
            if ((ft_status = FT_Write(ft_handle, buffer, num_bytes_to_send, &num_bytes_sent)) != FT_OK) throw 11;
            ft232h = true;
        }
        catch (const int step)
        {
            ROS_ERROR_STREAM("MPSSE initialization failed at step " << step);
            if (step > 2) FT_Close(ft_handle);
            ft232h = false;
            return false;
        }

        fs.open("log.txt", (std::ofstream::out | std::ofstream::trunc));
        if (!fs.is_open())
        {
            ROS_ERROR_STREAM("Can't open log file");
            FT_Close(ft_handle);
            return false;
        }

        mode = WAIT;
        generate_random_motion = false;
        random_motion_generated = false;
        waiting = false;
        executing = false;
        resting = false;
        start_time = 0.0;
        end_time = 0.0;
        waypoint = 0;
        waypoints = 0;

        return true;
    }

    void CollisionDetectionController::starting(const ros::Time& time)
    {
        generator.seed(rn());
        double range = 0.0;
        for (size_t i = 0; i < 7; i++)
        {
            range = ((joint_q_max[i] - joint_q_min[i]) * 0.1);
            angles[i] = std::uniform_real_distribution<double>((joint_q_min[i] + range), (joint_q_max[i] - range));
        }

        std::thread thread1(generate);
        std::thread thread2(publish);

        global_start_time = time.toSec();
    }

    void CollisionDetectionController::generate(void)
    {
        static const std::string PLANNING_GROUP = "arm";
        moveit::planning_interface::MoveGroupInterface move_group(PLANNING_GROUP);
        moveit::planning_interface::PlanningSceneInterface planning_scene_interface;
        const robot_state::JointModelGroup* joint_model_group = 
            move_group.getCurrentState()->getJointModelGroup(PLANNING_GROUP);
        namespace rvt = rviz_visual_tools;
        moveit_visual_tools::MoveItVisualTools visual_tools("panda_link0");
        visual_tools.deleteAllMarkers();
        visual_tools.loadRemoteControl();

        // Create collision object
        moveit_msgs::CollisionObject collision_object;
        collision_object.header.frame_id = move_group.getPlanningFrame();
        collision_object.id = "plane";
        shape_msgs::Plane plane;
        plane.coef[0] = 0.0;
        plane.coef[1] = 0.0;
        plane.coef[2] = 1.0;
        plane.coef[3] = 0.0;
        geometry_msgs::Pose pose;
        collision_object.planes.push_back(plane);
        collision_object.plane_poses.push_back(pose);
        collision_object.operation = collision_object.ADD;
        std::vector<moveit_msgs::CollisionObject> collision_objects;
        collision_objects.push_back(collision_object);
        planning_scene_interface.addCollisionObjects(collision_objects);
        visual_tools.trigger();

        std::vector<double> joint_group_positions;
        joint_group_positions.resize(7);
        std::vector<double> q; q.resize(7);
        std::vector<double> dq; dq.resize(7);
        moveit::core::RobotState start_state = *(move_group.getCurrentState());
        while (1)
        {
            if (!generate_random_motion) continue;
            generate_random_motion = false;

            const franka::RobotState& robot_state = state_handle_->getRobotState();

            for (int i = 0; i < 7; i++)
            {
                q[i] = robot_state.q[i];
                dq[i] = robot_state.dq[i];
            }
            start_state.setJointGroupPositions(PLANNING_GROUP, q);
            start_state.setJointGroupVelocities(PLANNING_GROUP, dq);
            move_group.setStartState(start_state);
            
            do
            {
                for (int i = 0; i < 7; i++) joint_group_positions[i] = ((angles[i])(generator));
                move_group.setJointValueTarget(joint_group_positions);
            } while (move_group.plan(random_plan) != moveit::planning_interface::MoveItErrorCode::SUCCESS);

            random_motion_generated = true;
        }
    }

    void CollisionDetectionController::publish(void)
    {
        std::chrono::system_clock::time_point time_1;
        std::chrono::system_clock::time_point time_2;
        std::chrono::microseconds interval;
        DWORD num_bytes;
        BYTE buffer;
        int collision;
        register int i;

        time_1 = std::chrono::system_clock::now(); 
        while (1)
        {
            time_2 = std::chrono::system_clock::now();
            interval = std::chrono::duration_cast<std::chrono::microseconds>(time_2 - time_1);
            if (interval.count() < 10000) continue;
            time_1 = std::chrono::system_clock::now();
            buffer = 0x83;
            FT_Write(ft_handle, &buffer, 1, &num_bytes);
            const franka::RobotState& robot_state = state_handle_->getRobotState();
            fs << robot_state.time.toMSec() << ','; // Strictly monotonically increasing timestamp since robot start(in miliseconds)
            for (i = 0; i < 16; i++) fs << robot_state.O_T_EE[i] << ','; // Measured end effector pose in base frame
            for (i = 0; i < 16; i++) fs << robot_state.O_T_EE_d[i] << ','; // Last desired end effector pose of motion generation in base frame
            for (i = 0; i < 16; i++) fs << robot_state.F_T_EE[i] << ','; // End effector frame pose in flange frame
            for (i = 0; i < 16; i++) fs << robot_state.F_T_NE[i] << ','; // Nominal end effector frame pose in flange frame
            for (i = 0; i < 16; i++) fs << robot_state.NE_T_EE[i] << ','; // End effector frame pose in nominal end effector frame
            for (i = 0; i < 16; i++) fs << robot_state.EE_T_K[i] << ','; // Stiffness frame pose in end effector frame
            for (i = 0; i < 2; i++) fs << robot_state.elbow[i] << ','; // Elbow configuration
            for (i = 0; i < 2; i++) fs << robot_state.elbow_d[i] << ','; // Desired elbow configuration
            for (i = 0; i < 2; i++) fs << robot_state.elbow_c[i] << ','; // Commanded elbow configuration
            for (i = 0; i < 2; i++) fs << robot_state.delbow_c[i] << ','; // Commanded elbow velocity
            for (i = 0; i < 2; i++) fs << robot_state.ddelbow_c[i] << ','; // Commanded elbow acceleration
            for (i = 0; i < 7; i++) fs << robot_state.tau_J[i] << ','; // Measured link-side joint torque sensor signals
            for (i = 0; i < 7; i++) fs << robot_state.tau_J_d[i] << ','; // Desired link-side joint torque sensor signals without gravity
            for (i = 0; i < 7; i++) fs << robot_state.dtau_J[i] << ','; // Derivative of measured link-side joint torque sensor signals
            for (i = 0; i < 7; i++) fs << robot_state.q[i] << ','; // Measured joint position
            for (i = 0; i < 7; i++) fs << robot_state.q_d[i] << ','; // Desired joint position
            for (i = 0; i < 7; i++) fs << robot_state.dq[i] << ','; // Measured joint velocity
            for (i = 0; i < 7; i++) fs << robot_state.dq_d[i] << ','; // Desired joint velocity
            for (i = 0; i < 7; i++) fs << robot_state.ddq_d[i] << ','; // Desired joint acceleration
            for (i = 0; i < 7; i++) fs << robot_state.tau_ext_hat_filtered[i] << ','; // Filtered external torque
            for (i = 0; i < 6; i++) fs << robot_state.O_F_ext_hat_K[i] << ','; // Estimated external wrench (force, torque) acting on stiffness frame, expressed relative to the base frame
            for (i = 0; i < 6; i++) fs << robot_state.K_F_ext_hat_K[i] << ','; //  Estimated external wrench (force, torque) acting on stiffness frame, expressed relative to the stiffness frame
            for (i = 0; i < 6; i++) fs << robot_state.O_dP_EE_d[i] << ','; // Desired end effector twist in base frame
            for (i = 0; i < 16; i++) fs << robot_state.O_T_EE_c[i] << ','; // Last commanded end effector pose of motion generation in base frame
            for (i = 0; i < 6; i++) fs << robot_state.O_dP_EE_c[i] << ','; // Last commanded end effector twist in base frame
            for (i = 0; i < 6; i++) fs << robot_state.O_ddP_EE_c[i] << ','; // Last commanded end effector acceleration in base frame
            for (i = 0; i < 7; i++) fs << robot_state.theta[i] << ','; // Motor position
            for (i = 0; i < 7; i++) fs << robot_state.dtheta[i] << ','; // Motor velocity
            FT_Read(ft_handle, &buffer, 1, &num_bytes);
            collision = ((buffer & 0x4) > 0) ? 0 : 1;
            fs << collision << ',' << (1 - collision) << std::endl;
            fs.flush();
        }
    }

    void CollisionDetectionController::generateNewMotion(void)
    {
        generate_random_motion = true;
    }

    bool CollisionDetectionController::checkForNewMotion(void)
    {
        bool result = random_motion_generated;
        if (result) random_motion_generated = false;
        return result;
    }

    void CollisionDetectionController::update(const ros::Time& time, const ros::Duration& period)
    {
        const franka::RobotState& robot_state = state_handle_->getRobotState();
        const std::array<double, 42>& jacobian_array = model_handle_->getZeroJacobian(franka::Frame::kEndEffector);
        const std::array<double, 7>& gravity_array = model_handle_->getGravity();
        const std::array<double, 49>& massmatrix_array = model_handle_->getMass();
        const std::array<double, 7>& coriolis_array = model_handle_->getCoriolis();

        Eigen::Map<const Eigen::Matrix<double, 7, 1>> gravity(gravity_array.data());
        Eigen::Map<const Eigen::Matrix<double, 7, 7>> mass_matrix(massmatrix_array.data());
        Eigen::Map<const Eigen::Matrix<double, 7, 1>> coriolis(coriolis_array.data());
        Eigen::Map<const Eigen::Matrix<double, 7, 1>> q(robot_state.q.data());
        Eigen::Map<const Eigen::Matrix<double, 7, 1>> qd(robot_state.dq.data());

        Eigen::Matrix<double, 7, 1> q_desired;
        Eigen::Matrix<double, 7, 1> qd_desired;
        Eigen::Matrix<double, 7, 1> tau_cmd;

        double current_time = time.toSec();
        double simulation_time = current_time - global_start_time;

        switch (mode)
        {
        case WAIT: wait(q_desired, qd_desired, q); break;

        case EXEC: exec(current_time, q_desired, qd_desired); break;

        case REST: rest(current_time, q_desired, qd_desired); break;
        }
        
        double kp, kv;
        kp = 1500;
        kv = 10;
        tau_cmd = mass_matrix * (kp * (q_desired - q) + kv * (qd_desired - qd)) + coriolis;

        if (print_rate_trigger_())
        {
            ROS_INFO("--------------------------------------------------");
            ROS_INFO_STREAM("tau: " << tau_cmd.transpose());
            ROS_INFO_STREAM("time :" << simulation_time);
            ROS_INFO_STREAM("q_curent: " << q.transpose());
            ROS_INFO_STREAM("q_desired: " << q_desired.transpose());
        }

        for (int i = 0; i < 7; i++) joint_handles_[i].setCommand(tau_cmd(i));
    }

    void CollisionDetectionController::wait(Eigen::Matrix<double, 7, 1>& q_desired, 
        Eigen::Matrix<double, 7, 1>& qd_desired, Eigen::Map<const Eigen::Matrix<double, 7, 1>>& q)
    {
        if (!waiting) { generateNewMotion(); waiting = true; }
        if (checkForNewMotion()) { waiting = false; mode = EXEC; }
        for (int i = 0; i < 7; i++)
        {
            q_desired(i) = q(i);
            qd_desired(i) = 0.0;
        }
    }

    void CollisionDetectionController::exec(double current_time, Eigen::Matrix<double, 7, 1>& q_desired, 
        Eigen::Matrix<double, 7, 1>& qd_desired)
    {
        double x_0, x_dot_0, x_ddot_0, x_f, x_dot_f, x_ddot_f;
        std::vector<Eigen::Vector3d> cmd;

        if (!executing)
        {
            start_time = current_time;
            waypoints = random_plan.trajectory_.joint_trajectory.points.size();
            waypoint = 0;
            end_time = random_plan.trajectory_.joint_trajectory.points[waypoints - 1].time_from_start.toSec();
            end_time += start_time;
            executing = true;
        }
        if (current_time > end_time)
        {
            executing = false;
            mode = REST;
            rest(current_time, q_desired, qd_desired);
            return;
        }
        while (waypoint < waypoints)
        {
            waypoint++;
            if (current_time < (random_plan.trajectory_.joint_trajectory.points[waypoint].time_from_start.toSec() + 
                start_time)) break; 
        }
        waypoint--;
        double interval_start_time = (random_plan.trajectory_.joint_trajectory.points[waypoint].time_from_start.toSec() + 
            start_time);
        double interval_end_time = (random_plan.trajectory_.joint_trajectory.points[waypoint + 1].time_from_start.toSec() + 
            start_time);          
        for (int i = 0; i < 7; i++)
        {
            x_0 = random_plan.trajectory_.joint_trajectory.points[waypoint].positions[i];
            x_dot_0 = random_plan.trajectory_.joint_trajectory.points[waypoint].velocities[i];
            x_ddot_0 = random_plan.trajectory_.joint_trajectory.points[waypoint].accelerations[i];
            x_0 = random_plan.trajectory_.joint_trajectory.points[waypoint + 1].positions[i];
            x_dot_0 = random_plan.trajectory_.joint_trajectory.points[waypoint + 1].velocities[i];
            x_ddot_0 = random_plan.trajectory_.joint_trajectory.points[waypoint + 1].accelerations[i];
            cmd[i] = quintic_spline(current_time, interval_start_time, interval_end_time, 
                x_0, x_dot_0, x_ddot_0, x_f, x_dot_f, x_ddot_f);
        }
        for (int i = 0; i < 7; i++)
        {
            q_desired(i) = cmd[i](0);
            qd_desired(i) = cmd[i](1);
        }
    }

    void CollisionDetectionController::rest(double current_time, Eigen::Matrix<double, 7, 1>& q_desired, 
        Eigen::Matrix<double, 7, 1>& qd_desired)
    {
        if (!resting) { resting = true; start_time = current_time; end_time = (start_time + 1.0); }
        if (current_time > end_time) { resting = false; mode = WAIT; }
        for (int i = 0; i < 7; i++)
        {
            q_desired(i) = random_plan.trajectory_.joint_trajectory.points[waypoints - 1].positions[i];
            qd_desired(i) = random_plan.trajectory_.joint_trajectory.points[waypoints - 1].velocities[i];
        }
    }

    Eigen::Vector3d CollisionDetectionController::quintic_spline(
        double time,       // Current time
        double time_0,     // Start time
        double time_f,     // End time
        double x_0,        // Start state
        double x_dot_0,    // Start state dot
        double x_ddot_0,   // Start state ddot
        double x_f,        // End state
        double x_dot_f,    // End state
        double x_ddot_f    // End state ddot
    )
    {
        double a1, a2, a3, a4, a5, a6;
        double time_s;

        Eigen::Vector3d result;

        if (time < time_0)
        {
            result << x_0, x_dot_0, x_ddot_0;
            return result;
        }
        else if (time > time_f)
        {
            result << x_f, x_dot_f, x_ddot_f;
            return result;
        }

        time_s = time_f - time_0;
        a1 = x_0; a2 = x_dot_0; a3 = (x_ddot_0 / 2.0);

        Eigen::Matrix3d Temp;
        Temp << pow(time_s, 3), pow(time_s, 4), pow(time_s, 5),
            (3.0 * pow(time_s, 2)), (4.0 * pow(time_s, 3)), (5.0 * pow(time_s, 4)),
            (6.0 * time_s), (12.0 * pow(time_s, 2)), (20.0 * pow(time_s, 3));

        Eigen::Vector3d R_temp;
        R_temp << (x_f - x_0 - x_dot_0 * time_s - x_ddot_0 * pow(time_s, 2) / 2.0),
            (x_dot_f - x_dot_0 - x_ddot_0 * time_s),
            (x_ddot_f - x_ddot_0);

        Eigen::Vector3d RES;

        RES = (Temp.inverse() * R_temp);
        a4 = RES(0); a5 = RES(1); a6 = RES(2);

        double time_fs = (time - time_0);
        double position = (a1 + a2 * pow(time_fs, 1) + a3 * pow(time_fs, 2) + a4 * pow(time_fs, 3) + a5 * pow(time_fs, 4) + a6 * pow(time_fs, 5));
        double velocity = (a2 + 2.0 * a3 * pow(time_fs, 1) + 3.0 * a4 * pow(time_fs, 2) + 4.0 * a5 * pow(time_fs, 3) + 5.0 * a6 * pow(time_fs, 4));
        double acceleration = (2.0 * a3 + 6.0 * a4 * pow(time_fs, 1) + 12.0 * a5 * pow(time_fs, 2) + 20.0 * a6 * pow(time_fs, 3));
        result << position, velocity, acceleration;

        return result;
    }

    CollisionDetectionController::~CollisionDetectionController()
    {
        if (fs.is_open()) fs.close();
        if (ft232h) FT_Close(ft_handle);
    }
} // namespace advanced_robotics_franka_controllers

PLUGINLIB_EXPORT_CLASS(advanced_robotics_franka_controllers::CollisionDetectionController, controller_interface::ControllerBase)