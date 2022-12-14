/******************************************************************************
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Bluewhale Robot
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Author: Xie fusheng, Randoms
 *******************************************************************************/

#include "bw_auto_dock/getDockPosition.h"
#include "bw_auto_dock/DockController.h"
#include <tf2/convert.h>
#include <tf2/utils.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

namespace bw_auto_dock
{
DockController::DockController(double back_distance, double max_linearspeed, double max_rotspeed,double crash_distance, int barDetectFlag,std::string global_frame,
                             StatusPublisher* bw_status, CallbackAsyncSerial* cmd_serial):tf2_(tf2_buffer_),  global_frame_(global_frame)
{
    if(barDetectFlag==1)
    {
      barDetectFlag_ = true;
    }
    else
    {
      barDetectFlag_ = false;
    }
    back_distance_ = back_distance;
    max_linearspeed_ = max_linearspeed;
    max_rotspeed_ = max_rotspeed;
    crash_distance_ = crash_distance;
    bw_status_ = bw_status;
    mcmd_serial_ = cmd_serial;
    mcharge_status_ = CHARGE_STATUS::freed;
    bw_status_->set_charge_status(mcharge_status_);
    mPose3_ = new float[3];
    mPose4_ = new float[3];

    mstationPose3_ = new float[3];

    mcurrentChargeFlag_ = false;
    left2_error1_ = 0.;
    left2_error2_ = 0.;
    left2_error3_ = 0.;
    right2_error1_ = 0.;
    right2_error2_ = 0.;
    right2_error3_ = 0.;
    rot_z_ = 0.;
    usefull_num_ = 0;
    unusefull_num_ = 0;
    kp_ = 0.;
    ki_ = 0.;
    kd_ = 0.;
    current_average_ = 1.0;
    mPose_flag_ = false;

    min_x2_ = 100.0;

    min_x2_4_ = 100.0;//????????????????????????

    mTf_flag_ = false;
    tf2::toMsg(tf2::Transform::getIdentity(), global_pose_.pose);
    tf2::toMsg(tf2::Transform::getIdentity(), robot_pose_.pose);

    power_threshold_ = 41.0;
}

void DockController::setPowerParam(double power_threshold)
{
  power_threshold_ = power_threshold;
}

void DockController::run()
{
    ros::NodeHandle nodeHandler;
    mCmdvelPub_ = nodeHandler.advertise<geometry_msgs::Twist>("/cmd_vel", 1, true);
    mbarDetectPub_ = nodeHandler.advertise<std_msgs::Bool>("/barDetectFlag", 1, true);
    mlimitSpeedPub_ = nodeHandler.advertise<std_msgs::Bool>("/limitSpeedFlag", 1, true);
    ros::Subscriber sub1 =
        nodeHandler.subscribe("/bw_auto_dock/EnableCharge", 1, &DockController::updateChargeFlag, this);
    ros::Subscriber sub2 = nodeHandler.subscribe("/odom", 1, &DockController::updateOdom, this);
    ros::spin();
}

void DockController::updateChargeFlag(const std_msgs::Bool& currentFlag)
{
    boost::mutex::scoped_lock lock(mMutex_charge);
    mcurrentChargeFlag_ = currentFlag.data;
    usefull_num_ = 0;
    unusefull_num_ = 0;
    if (mcurrentChargeFlag_)
    {
        //??????????????????
        std_msgs::Bool pub_data;
        pub_data.data = false;
        if(!barDetectFlag_) mbarDetectPub_.publish(pub_data);
        //????????????????????????
        pub_data.data = true;
        mlimitSpeedPub_.publish(pub_data);
    }
    else
    {
      //????????????????????????
      std_msgs::Bool pub_data;
      pub_data.data = true;
      mlimitSpeedPub_.publish(pub_data);
    }
    //???????????????????????????
    char cmd_str[6] = { (char)0xcd, (char)0xeb, (char)0xd7, (char)0x02, (char)0x4B, (char)0x00 };
    if (NULL != mcmd_serial_)
    {
        mcmd_serial_->write(cmd_str, 6);
    }
}

void DockController::updateOdom(const nav_msgs::Odometry::ConstPtr& msg)
{
    robot_pose_.header.frame_id = msg->header.frame_id;
    robot_pose_.pose = msg->pose.pose;
    robot_pose_.header.stamp = ros::Time();//?????????????????????map??????????????????
    if(mTf_flag_)
    {
      try
      {
        tf2_buffer_.transform(robot_pose_, global_pose_, global_frame_);
      }
      catch (tf2::LookupException& ex)
      {
        boost::mutex::scoped_lock lock(mMutex_pose);
        ROS_ERROR_THROTTLE(1.0, "No Transform available Error looking up robot pose: %s\n", ex.what());
        mTf_flag_ = false;
        mPose_flag_ = false;
        return;
      }
      {
        boost::mutex::scoped_lock lock(mMutex_pose);
        mRobot_pose_ = global_pose_.pose;
        mPose_flag_ = true;
      }
    }
    else
    {
      std::string tf_error;
      if(tf2_buffer_.canTransform(global_frame_, std::string("odom"), ros::Time(), ros::Duration(0.1), &tf_error))
      {
        mTf_flag_ = true;
      }
      else
      {
        mTf_flag_ = false;
        ROS_DEBUG("Timed out waiting for transform from %s to %s to become available before running costmap, tf error: %s",
               "odom", global_frame_.c_str(), tf_error.c_str());
      }
      {
        boost::mutex::scoped_lock lock(mMutex_pose);
        mPose_flag_ = false;
      }
    }
}

void DockController::dealing_status()
{
    boost::mutex::scoped_lock lock1(mMutex_charge);
    boost::mutex::scoped_lock lock2(mMutex_pose);
    UPLOAD_STATUS sensor_status = bw_status_->get_sensor_status();
    geometry_msgs::Twist current_vel;
    if (!mPose_flag_)
    {
      if (mcharge_status_ != CHARGE_STATUS::freed && mcurrentChargeFlag_)
      {
        //????????????
        current_vel.linear.x = 0;
        current_vel.linear.y = 0;
        current_vel.linear.z = 0;
        current_vel.angular.x = 0;
        current_vel.angular.y = 0;
        current_vel.angular.z = 0;
        ROS_ERROR("map to base_link not ready!");
        mCmdvelPub_.publish(current_vel);
      }
      return;  //?????????????????????
    }
    if (mcurrentChargeFlag_)
    {
        if (mcharge_status_ == CHARGE_STATUS::freed)
        {
            if (sensor_status.power > 9.0)
            {
                //??????????????????????????????????????????
                mcharge_status_temp_ = CHARGE_STATUS_TEMP::charging1;
                current_average_ = 1.0;
                mcharge_status_ = CHARGE_STATUS::charging;
                bw_status_->set_charge_status(mcharge_status_);
                usefull_num_ = 0;
                unusefull_num_ = 0;
                //????????????
                current_vel.linear.x = 0;
                current_vel.linear.y = 0;
                current_vel.linear.z = 0;
                current_vel.angular.x = 0;
                current_vel.angular.y = 0;
                current_vel.angular.z = 0;
                mCmdvelPub_.publish(current_vel);
            }
            else
            {
                mcharge_status_ = CHARGE_STATUS::finding;
                mcharge_status_temp_ = CHARGE_STATUS_TEMP::finding0;
                bw_status_->set_charge_status(mcharge_status_);
                usefull_num_ = 0;
                unusefull_num_ = 0;
            }
        }
        DOCK_POSITION dock_position_current = bw_status_->get_dock_position();
        switch (mcharge_status_temp_)
        {
            case CHARGE_STATUS_TEMP::finding0:
                ROS_DEBUG("finding0.0");
                if (usefull_num_ == 0)
                {
                    //?????????????????????,???????????????????????????
                    usefull_num_++;
                    if (mdock_position_caculate_->getDockPosition(mstationPose1_, mstationPose2_))
                    {
                        //?????????????????????????????????station3
                        ROS_DEBUG("finding0.1");
                        this->caculateStation3();
                    }
                    else
                    {
                        // error,???????????????????????????
                        ROS_ERROR("Can not get dock station position!");
                        mcharge_status_ = CHARGE_STATUS::freed;
                        bw_status_->set_charge_status(mcharge_status_);
                        usefull_num_ = 0;
                        unusefull_num_ = 0;
                        mcurrentChargeFlag_ = false;
                        //????????????
                        current_vel.linear.x = 0;
                        current_vel.linear.y = 0;
                        current_vel.linear.z = 0;
                        current_vel.angular.x = 0;
                        current_vel.angular.y = 0;
                        current_vel.angular.z = 0;
                        mCmdvelPub_.publish(current_vel);
                    }
                }
                else
                {
                    if (this->rotate2Station3())
                    {
                        ROS_DEBUG("finding0.2");
                        min_x2_ = 100.0;
                        //??????????????????finding1
                        mcharge_status_ = CHARGE_STATUS::finding;
                        mcharge_status_temp_ = CHARGE_STATUS_TEMP::finding1;
                        bw_status_->set_charge_status(mcharge_status_);
                        usefull_num_ = 0;
                        unusefull_num_ = 0;
                        //????????????
                        current_vel.linear.x = 0;
                        current_vel.linear.y = 0;
                        current_vel.linear.z = 0;
                        current_vel.angular.x = 0;
                        current_vel.angular.y = 0;
                        current_vel.angular.z = 0;
                        mCmdvelPub_.publish(current_vel);
                    }
                }
                break;
            case CHARGE_STATUS_TEMP::finding1:
                ROS_DEBUG("finding1.0");
                //??????????????????????????????left_center??????right_center??????????????????????????????????????????finding2
                if (dock_position_current == DOCK_POSITION::left_center ||
                    dock_position_current == DOCK_POSITION::right_center)
                {
                    mdock__referenss_position_ = dock_position_current;
                    mPose1_ = mRobot_pose_;
                    mcharge_status_temp_ = CHARGE_STATUS_TEMP::finding2;
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                    current_vel.linear.x = 0.1;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                }
                else
                {
                    if (this->goToStation3())
                    {
                        ROS_DEBUG("finding1.1");
                        //????????????
                        mcharge_status_ = CHARGE_STATUS::finding;
                        mcharge_status_temp_ = CHARGE_STATUS_TEMP::finding0;
                        bw_status_->set_charge_status(mcharge_status_);
                        usefull_num_ = 0;
                        unusefull_num_ = 0;
                        //????????????
                        current_vel.linear.x = 0;
                        current_vel.linear.y = 0;
                        current_vel.linear.z = 0;
                        current_vel.angular.x = 0;
                        current_vel.angular.y = 0;
                        current_vel.angular.z = 0;
                        mCmdvelPub_.publish(current_vel);
                    }
                }
                if (dock_position_current == DOCK_POSITION::back_center &&
                    (sensor_status.left_sensor2 == 3 || sensor_status.left_sensor2 == 7) &&
                    (sensor_status.right_sensor2 == 3 || sensor_status.right_sensor2 == 7))
                {
                    ROS_DEBUG("finding1.2");
                    mPose1_ = mRobot_pose_;
                    mPose2_ = mRobot_pose_;
                    this->caculatePose3();
                    mcharge_status_temp_ = CHARGE_STATUS_TEMP::finding4;
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                    current_vel.linear.x = 0;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                }
                break;
            case CHARGE_STATUS_TEMP::finding2:
                ROS_DEBUG("finding2");
                //?????????????????????????????????left_center??????right_center????????????????????????????????????finding3
                if (dock_position_current != DOCK_POSITION::left_center &&
                    dock_position_current != DOCK_POSITION::right_center)
                {
                    mPose2_ = mRobot_pose_;
                    this->caculatePose3();
                    mcharge_status_temp_ = CHARGE_STATUS_TEMP::finding3;
                    //???????????????
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                    current_vel.linear.x = 0;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                }
                else
                {
                    current_vel.linear.x = 0.1;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                }
                if (dock_position_current == DOCK_POSITION::back_center)
                {
                    mPose1_ = mRobot_pose_;
                    mPose2_ = mRobot_pose_;
                    this->caculatePose3();
                    mcharge_status_temp_ = CHARGE_STATUS_TEMP::finding4;
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                    current_vel.linear.x = 0;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                }
                break;
            case CHARGE_STATUS_TEMP::finding3:
                ROS_DEBUG("finding3");
                //???????????????????????????pose3????????????finding4
                if (this->backToPose3())
                {
                    mcharge_status_temp_ = CHARGE_STATUS_TEMP::finding4;
                    //???????????????
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                    current_vel.linear.x = 0;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                }
                break;
            case CHARGE_STATUS_TEMP::finding4:
            {
                ROS_DEBUG("finding4");
                //???????????????????????????DOCK_POSITION::back_center?????????docking1
                static float target_theta = 0;
                float theta;
                geometry_msgs::Pose current_pose = mRobot_pose_;
                tf::Quaternion q1(current_pose.orientation.x, current_pose.orientation.y, current_pose.orientation.z,
                                  current_pose.orientation.w);
                tf::Matrix3x3 m1(q1);
                double roll, pitch, yaw;
                m1.getRPY(roll, pitch, yaw);
                theta = yaw;
                if (usefull_num_ == 0)
                {
                    usefull_num_++;
                    //??????????????????
                    target_theta = theta;
                }
                else
                {
                    float theta_error = theta - target_theta;
                    if (theta_error <= -PI_temp)
                        theta_error += 2 * PI_temp;
                    if (theta_error > PI_temp)
                        theta_error -= 2 * PI_temp;
                    if (fabs(theta_error) >= (0.9 * PI_temp))
                    {
                        //????????????162?????????????????????????????????finding0
                        mcharge_status_temp_ = CHARGE_STATUS_TEMP::finding0;
                        usefull_num_ = 0;
                        unusefull_num_ = 0;
                        //????????????
                        current_vel.linear.x = 0;
                        current_vel.linear.y = 0;
                        current_vel.linear.z = 0;
                        current_vel.angular.x = 0;
                        current_vel.angular.y = 0;
                        current_vel.angular.z = 0;
                        mCmdvelPub_.publish(current_vel);
                    }
                }
                // if(dock_position_current == DOCK_POSITION::back_center &&
                // (((bw_status_->sensor_status.left_sensor2==3||bw_status_->sensor_status.left_sensor2==7) &&
                // mdock__referenss_position_ == DOCK_POSITION::right_center
                // )||((bw_status_->sensor_status.right_sensor2==3 ||bw_status_->sensor_status.right_sensor2==7)  &&
                // mdock__referenss_position_ == DOCK_POSITION::left_center)) )
                // if(((bw_status_->sensor_status.left_sensor2==3||bw_status_->sensor_status.left_sensor2==7) &&
                // mdock__referenss_position_ == DOCK_POSITION::right_center
                // )||((bw_status_->sensor_status.right_sensor2==3 ||bw_status_->sensor_status.right_sensor2==7)  &&
                // mdock__referenss_position_ == DOCK_POSITION::left_center) )
                if (dock_position_current == DOCK_POSITION::back_center)
                {
                    mcharge_status_temp_ = CHARGE_STATUS_TEMP::docking1;
                    mcharge_status_ = CHARGE_STATUS::docking;
                    bw_status_->set_charge_status(mcharge_status_);
                    //????????????
                    current_vel.linear.x = 0;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                    //????????????
                    left2_error1_ = 0.;
                    left2_error2_ = 0.;
                    left2_error3_ = 0.;
                    right2_error1_ = 0.;
                    right2_error2_ = 0.;
                    right2_error3_ = 0.;
                    rot_z_ = 0.;
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                    //????????????????????????
                    std_msgs::Bool pub_data;
                    pub_data.data = false;
                    mlimitSpeedPub_.publish(pub_data);
                }
                else
                {
                    if (mdock__referenss_position_ == DOCK_POSITION::left_center)
                    {
                        //??????
                        current_vel.angular.z = -0.3;
                    }
                    else
                    {
                        //??????
                        current_vel.angular.z = 0.3;
                    }
                    current_vel.linear.x = 0;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    mCmdvelPub_.publish(current_vel);
                }
                break;
            }
            case CHARGE_STATUS_TEMP::docking1:
                ROS_DEBUG("docking1.1");
                // pid???????????????????????????????????????????????????????????????????????????????????????????????????
                if (this->backToDock())
                {
                    mcharge_status_temp_ = CHARGE_STATUS_TEMP::docking2;
                    //????????????
                    current_vel.linear.x = 0;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                    //????????????????????????
                    std_msgs::Bool pub_data;
                    pub_data.data = true;
                    mlimitSpeedPub_.publish(pub_data);
                }
                break;
            case CHARGE_STATUS_TEMP::docking2:
                if (sensor_status.power > 9.0)
                {
                    //??????????????????????????????????????????
                    ROS_DEBUG("docking2.1 %f", sensor_status.power);
                    mcharge_status_temp_ = CHARGE_STATUS_TEMP::charging1;
                    current_average_ = 1.0;
                    mcharge_status_ = CHARGE_STATUS::charging;
                    bw_status_->set_charge_status(mcharge_status_);
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                    //????????????
                    current_vel.linear.x = 0;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                }
                else
                {
                    //????????????????????????
                    if (sensor_status.distance1 <= this->crash_distance_ && sensor_status.distance1>0.1)
                    {
                        //??????docking3
                        ROS_DEBUG("docking2.2");
                        mcharge_status_temp_ = CHARGE_STATUS_TEMP::docking3;
                        usefull_num_ = 0;
                        unusefull_num_ = 0;
                        //????????????
                        current_vel.linear.x = 0;
                        current_vel.linear.y = 0;
                        current_vel.linear.z = 0;
                        current_vel.angular.x = 0;
                        current_vel.angular.y = 0;
                        current_vel.angular.z = 0;
                        mCmdvelPub_.publish(current_vel);
                    }
                    else
                    {
                        //????????????????????????????????????20????????????????????????????????????temp1
                        ROS_DEBUG("docking2.3");
                        usefull_num_++;
                        // if(this->rotateOrigin())
                        if (usefull_num_ >= 10)
                        {
                            ROS_DEBUG("docking2.4");
                            this->caculatePose4();
                            min_x2_4_ = 100.0;
                            mcharge_status_temp_ = CHARGE_STATUS_TEMP::temp1;
                            usefull_num_ = 0;
                            unusefull_num_ = 0;
                            //????????????
                            current_vel.linear.x = 0;
                            current_vel.linear.y = 0;
                            current_vel.linear.z = 0;
                            current_vel.angular.x = 0;
                            current_vel.angular.y = 0;
                            current_vel.angular.z = 0;
                            mCmdvelPub_.publish(current_vel);
                        }
                    }
                }
                break;
            case CHARGE_STATUS_TEMP::docking3:
                //???????????????????????????????????????
                if ((sensor_status.distance1 > this->crash_distance_ && sensor_status.distance1>0.1)||sensor_status.power > 9.0)
                {
                    //??????docking2
                    mcharge_status_temp_ = CHARGE_STATUS_TEMP::docking2;
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                    //????????????
                    geometry_msgs::Twist current_vel;
                    current_vel.linear.x = 0;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                    ROS_DEBUG("docking3.1 %d", usefull_num_);
                }
                else
                {
                    usefull_num_++;
                    geometry_msgs::Twist current_vel;
                    current_vel.linear.x = 0.1;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                    ROS_DEBUG("docking3.2 %d", usefull_num_);
                }
                break;
            case CHARGE_STATUS_TEMP::temp1:
                //??????????????????4
                ROS_DEBUG("temp1.1 ");
                if (this->goToPose4())
                {
                    ROS_DEBUG("temp1.2 ");
                    mcharge_status_temp_ = CHARGE_STATUS_TEMP::temp2;
                    //???????????????
                    current_vel.linear.x = 0;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                }
                break;
            case CHARGE_STATUS_TEMP::temp2:
            {
                //????????????finding0,temp1???????????????????????????????????????????????????????????????
                mcharge_status_ = CHARGE_STATUS::finding;
                mcharge_status_temp_ = CHARGE_STATUS_TEMP::finding0;
                bw_status_->set_charge_status(mcharge_status_);
                usefull_num_ = 0;
                unusefull_num_ = 0;
                current_vel.linear.x = 0;
                current_vel.linear.y = 0;
                current_vel.linear.z = 0;
                current_vel.angular.x = 0;
                current_vel.angular.y = 0;
                current_vel.angular.z = 0;
                mCmdvelPub_.publish(current_vel);
                break;
                //????????????????????????4
                geometry_msgs::Pose current_pose = mRobot_pose_;

                float x, y, theta;
                x = current_pose.position.x;
                y = current_pose.position.y;
                tf::Quaternion q1(current_pose.orientation.x, current_pose.orientation.y, current_pose.orientation.z,
                                  current_pose.orientation.w);
                tf::Matrix3x3 m1(q1);

                double roll, pitch, yaw;
                m1.getRPY(roll, pitch, yaw);
                theta = yaw;

                if (usefull_num_ == 0)
                {
                    mPose4_[2] = atan2(mPose3_[1] - mPose4_[1], mPose3_[0] - mPose4_[0]);
                    usefull_num_++;
                }

                if (fabs(yaw - mPose4_[2]) < 0.02)
                {
                    ROS_DEBUG("temp2.2 ");
                    mcharge_status_ = CHARGE_STATUS::finding;
                    mcharge_status_temp_ = CHARGE_STATUS_TEMP::finding0;
                    bw_status_->set_charge_status(mcharge_status_);
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                    current_vel.linear.x = 0;
                    current_vel.linear.y = 0;
                    current_vel.linear.z = 0;
                    current_vel.angular.x = 0;
                    current_vel.angular.y = 0;
                    current_vel.angular.z = 0;
                    mCmdvelPub_.publish(current_vel);
                }
                else
                {
                    ROS_DEBUG("temp2.1");
                    if (mPose4_[2] > 0)
                    {
                        //??????
                        current_vel.linear.x = 0;
                        current_vel.linear.y = 0;
                        current_vel.linear.z = 0;
                        current_vel.angular.x = 0;
                        current_vel.angular.y = 0;
                        current_vel.angular.z = 0.2;
                        mCmdvelPub_.publish(current_vel);
                    }
                    else
                    {
                        //??????
                        current_vel.linear.x = 0;
                        current_vel.linear.y = 0;
                        current_vel.linear.z = 0;
                        current_vel.angular.x = 0;
                        current_vel.angular.y = 0;
                        current_vel.angular.z = -0.2;
                        mCmdvelPub_.publish(current_vel);
                    }
                }
            }
            break;
            case CHARGE_STATUS_TEMP::charging1:
                // //????????????????????????
                // if (bw_status_->sensor_status.distance1 <= this->crash_distance_ && bw_status_->sensor_status.distance1>0.1)
                // {
                //     //??????docking3
                //     ROS_DEBUG("docking2.2");
                //     mcharge_status_temp_ = CHARGE_STATUS_TEMP::docking3;
                //     usefull_num_ = 0;
                //     unusefull_num_ = 0;
                //     //????????????
                //     current_vel.linear.x = 0;
                //     current_vel.linear.y = 0;
                //     current_vel.linear.z = 0;
                //     current_vel.angular.x = 0;
                //     current_vel.angular.y = 0;
                //     current_vel.angular.z = 0;
                //     mCmdvelPub_.publish(current_vel);
                // }
                if (sensor_status.power < 9.0)
                {
                    //??????????????????????????????temp1
                    usefull_num_++;
                    if (usefull_num_ > 10)
                    {
                        //????????????????????????
                        char cmd_str[6] = { (char)0xcd, (char)0xeb, (char)0xd7, (char)0x02, (char)0x4B, (char)0x00 };
                        if (NULL != mcmd_serial_)
                        {
                            mcmd_serial_->write(cmd_str, 6);
                        }
                        this->caculatePose4();
                        min_x2_4_ = 100.0;
                        mcharge_status_temp_ = CHARGE_STATUS_TEMP::temp1;
                        usefull_num_ = 0;
                        unusefull_num_ = 0;
                        //????????????
                        current_vel.linear.x = 0;
                        current_vel.linear.y = 0;
                        current_vel.linear.z = 0;
                        current_vel.angular.x = 0;
                        current_vel.angular.y = 0;
                        current_vel.angular.z = 0;
                        mCmdvelPub_.publish(current_vel);
                    }
                }
                else
                {
                    unusefull_num_++;
                    usefull_num_ = 0;
                    static int trig_num =0 ;
                    if (unusefull_num_ > 20)
                    {
                        //??????????????????????????????,??????????????????,??????
                        unusefull_num_ = 21;
                        char cmd_str[6] = { (char)0xcd, (char)0xeb, (char)0xd7, (char)0x02, (char)0x4B, (char)0x01 };
                        if (NULL != mcmd_serial_)
                        {
                            mcmd_serial_->write(cmd_str, 6);
                        }
                        //?????????????????????????????????????????????
                        if(sensor_status.current>-0.1 && sensor_status.current<10.0) current_average_ = current_average_ * 0.99 + sensor_status.current * 0.01;
                        if ((current_average_) < 0.1 || bw_status_->get_battery_power() > power_threshold_)
                        {
                          trig_num ++;
                          if(trig_num>50)
                          {
                            //??????????????????
                            //?????????????????????????????????????????????
                            char cmd_str[6] = {
                                (char)0xcd, (char)0xeb, (char)0xd7, (char)0x02, (char)0x4B, (char)0x02
                            };
                            if (NULL != mcmd_serial_)
                            {
                                mcmd_serial_->write(cmd_str, 6);
                            }
                            mcharge_status_temp_ = CHARGE_STATUS_TEMP::charged1;
                            mcharge_status_ = CHARGE_STATUS::charged;
                            bw_status_->set_charge_status(mcharge_status_);
                            usefull_num_ = 0;
                            unusefull_num_ = 0;
                            //??????????????????
                            std_msgs::Bool pub_data;
                            pub_data.data = true;
                            mbarDetectPub_.publish(pub_data);
                          }
                        }
                        else
                        {
                          trig_num = 0;
                        }
                    }
                }
                //????????????
                current_vel.linear.x = 0;
                current_vel.linear.y = 0;
                current_vel.linear.z = 0;
                current_vel.angular.x = 0;
                current_vel.angular.y = 0;
                current_vel.angular.z = 0;
                mCmdvelPub_.publish(current_vel);
                break;
            case CHARGE_STATUS_TEMP::charged1:
                if (usefull_num_ > 18000 || bw_status_->get_battery_power() > power_threshold_)
                {
                    // 10???????????????freed
                    // //??????free??????????????????????????????????????????????????????
                    // char cmd_str[6] = { (char)0xcd, (char)0xeb, (char)0xd7, (char)0x02, (char)0x4B, (char)0x00 };
                    // if (NULL != mcmd_serial_)
                    // {
                    //     mcmd_serial_->write(cmd_str, 6);
                    // }
                    // mcharge_status_temp_ = CHARGE_STATUS_TEMP::freed;
                    // mcharge_status_ = CHARGE_STATUS::freed;
                    // bw_status_->set_charge_status(mcharge_status_);
                    usefull_num_ = 0;
                    unusefull_num_ = 0;
                    mcurrentChargeFlag_ = false;
                }
                else
                {
                    usefull_num_++;
                }
                //????????????
                current_vel.linear.x = 0;
                current_vel.linear.y = 0;
                current_vel.linear.z = 0;
                current_vel.angular.x = 0;
                current_vel.angular.y = 0;
                current_vel.angular.z = 0;
                mCmdvelPub_.publish(current_vel);
                break;
        }
    }
    else
    {
        if(mcharge_status_ == CHARGE_STATUS::charging || mcharge_status_ == CHARGE_STATUS::charged)
        {
          //?????????temp3,?????????pose4,?????????free
          this->caculatePose4();
          min_x2_4_ = 100.0;
          mcharge_status_temp_ = CHARGE_STATUS_TEMP::temp3;
        }

        if(mcharge_status_temp_ == CHARGE_STATUS_TEMP::temp3)
        {
          //??????????????????4
          ROS_DEBUG("temp3.1 ");
          if (this->goToPose4())
          {
              //??????free
              ROS_DEBUG("temp3.2 ");
              mcharge_status_temp_ = CHARGE_STATUS_TEMP::freed;
              //???????????????
              current_vel.linear.x = 0;
              current_vel.linear.y = 0;
              current_vel.linear.z = 0;
              current_vel.angular.x = 0;
              current_vel.angular.y = 0;
              current_vel.angular.z = 0;
              mCmdvelPub_.publish(current_vel);
          }
          else
          {
              return;
          }
        }
        if (mcharge_status_ == CHARGE_STATUS::docking || mcharge_status_ == CHARGE_STATUS::finding)
        {
            //????????????
            current_vel.linear.x = 0;
            current_vel.linear.y = 0;
            current_vel.linear.z = 0;
            current_vel.angular.x = 0;
            current_vel.angular.y = 0;
            current_vel.angular.z = 0;
            mCmdvelPub_.publish(current_vel);
        }
        if (sensor_status.power > 9.0)
        {
          //???????????????????????????
          char cmd_str[6] = { (char)0xcd, (char)0xeb, (char)0xd7, (char)0x02, (char)0x4B, (char)0x00 };
          if (NULL != mcmd_serial_)
          {
              mcmd_serial_->write(cmd_str, 6);
          }
        }
        mcharge_status_ = CHARGE_STATUS::freed;
        mcharge_status_temp_ = CHARGE_STATUS_TEMP::freed;
        bw_status_->set_charge_status(mcharge_status_);
    }
}

void DockController::caculatePose3()
{
    float theta1, theta2, x, y, theta;

    tf::Quaternion q1(mPose1_.orientation.x, mPose1_.orientation.y, mPose1_.orientation.z, mPose1_.orientation.w);
    tf::Matrix3x3 m1(q1);
    double roll, pitch, yaw;
    m1.getRPY(roll, pitch, yaw);
    theta1 = yaw;

    tf::Quaternion q2(mPose1_.orientation.x, mPose1_.orientation.y, mPose1_.orientation.z, mPose1_.orientation.w);
    tf::Matrix3x3 m2(q2);
    m2.getRPY(roll, pitch, yaw);
    theta2 = yaw;

    x = (mPose1_.position.x + mPose2_.position.x) / 2.0f;
    y = (mPose1_.position.y + mPose2_.position.y) / 2.0f;
    theta = (theta1 + theta2) / 2.0f;

    mPose3_[0] = x - back_distance_ * cos(theta);
    mPose3_[1] = y - back_distance_ * sin(theta);
    mPose3_[2] = theta;
    //ROS_DEBUG("theta12  %f  %f x1 y1 %f %f x2 y2 %f %f ", theta1, theta2, mPose1_.position.x, mPose1_.position.y,
    //            mPose2_.position.x, mPose2_.position.y);
}

bool DockController::backToPose3()
{
    geometry_msgs::Pose current_pose = mRobot_pose_;
    float x, y, theta, x2, y2;
    x = current_pose.position.x;
    y = current_pose.position.y;
    tf::Quaternion q1(current_pose.orientation.x, current_pose.orientation.y, current_pose.orientation.z,
                      current_pose.orientation.w);
    tf::Matrix3x3 m1(q1);
    double roll, pitch, yaw;
    m1.getRPY(roll, pitch, yaw);
    theta = yaw;
    //??????pose3???????????????
    x2 = cos(mPose3_[2]) * (x - mPose3_[0]) + sin(mPose3_[2]) * (y - mPose3_[1]);
    y2 = -sin(mPose3_[2]) * (x - mPose3_[0]) + cos(mPose3_[2]) * (y - mPose3_[1]);

    //ROS_DEBUG("theta3  %f  %f x3 y3 %f %f x y %f %f x2 y2 %f %f ", mPose3_[2], theta, mPose3_[0], mPose3_[1], x, y, x2,
    //          y2);

    if (fabs(x2) <= 0.03)
        return true;
    geometry_msgs::Twist current_vel;
    current_vel.linear.x = -0.1;
    current_vel.linear.y = 0;
    current_vel.linear.z = 0;
    current_vel.angular.x = 0;
    current_vel.angular.y = 0;
    current_vel.angular.z = 0;
    mCmdvelPub_.publish(current_vel);
    return false;
}

bool DockController::backToDock()
{
    UPLOAD_STATUS sensor_status = bw_status_->get_sensor_status();
    geometry_msgs::Pose current_pose = mRobot_pose_;
    float x, y, theta;
    x = current_pose.position.x;
    y = current_pose.position.y;
    tf::Quaternion q1(current_pose.orientation.x, current_pose.orientation.y, current_pose.orientation.z,
                      current_pose.orientation.w);
    tf::Matrix3x3 m1(q1);
    double roll, pitch, yaw;
    m1.getRPY(roll, pitch, yaw);
    theta = yaw;
    //??????????????????????????????
    if (sensor_status.power > 9.0)
        return true;

    //???????????????????????????????????????
    if (sensor_status.distance1 <= this->crash_distance_ && sensor_status.distance1>0.1)
        return true;

    if ((sensor_status.left_sensor2 == 0) && (sensor_status.right_sensor2 == 0))
    {
        usefull_num_++;
        unusefull_num_ = 0;
        if (usefull_num_ > 8)
        {
            return true;
        }
    }
    else
    {
        unusefull_num_++;
        usefull_num_ = 0;
    }

    geometry_msgs::Twist current_vel;
    current_vel.linear.x = -0.1;
    current_vel.linear.y = 0;
    current_vel.linear.z = 0;
    current_vel.angular.x = 0;
    current_vel.angular.y = 0;
    current_vel.angular.z = 0;
    // PID??????
    const float Ts = 1.0f / 30.0f;
    float kp = kp_, ki = kp_ * Ts / ki_, kd = kd_ * kp_ / Ts;

    left2_error1_ = this->computeDockError();

    float rot_error_temp1, rot_error_temp2, rot_delta;
    rot_error_temp1 = left2_error1_ - left2_error2_;
    rot_error_temp2 = left2_error1_ - 2 * left2_error2_ + left2_error3_;
    rot_delta = kp * rot_error_temp1 + ki * left2_error1_ + kd * rot_error_temp2;
    //ROS_DEBUG("ousp1 delta  %f  error %f %f %f sonsor %d ", rot_delta, left2_error1_, rot_error_temp1, rot_error_temp2,
    //          bw_status_->sensor_status.left_sensor2);
    // rot_error_temp1 = right2_error1_ - right2_error2_;
    // rot_error_temp2 = right2_error1_ - 2*right2_error2_ + right2_error3_;
    // rot_delta += kp*rot_error_temp1 + ki*right2_error1_ + kd*rot_error_temp2;
    // ROS_DEBUG("ousp2 delta  %f  error %f %f %f sonsor %d
    // ",rot_delta,right2_error1_,rot_error_temp1,rot_error_temp2,bw_status_->sensor_status.right_sensor2);
    if (rot_delta > max_rotspeed_)
        rot_delta = max_rotspeed_;
    if (rot_delta < -max_rotspeed_)
        rot_delta = -max_rotspeed_;
    rot_z_ = rot_delta;
    //ROS_DEBUG("ousp3 rot  %f", rot_z_);
    left2_error3_ = left2_error2_;
    left2_error2_ = left2_error1_;
    // right2_error3_ = right2_error2_;
    // right2_error2_ = right2_error1_;
    current_vel.angular.z = rot_z_;
    mCmdvelPub_.publish(current_vel);
    return false;
}

float DockController::computeDockError()
{
    UPLOAD_STATUS sensor_status = bw_status_->get_sensor_status();
    int l2 = sensor_status.left_sensor2;
    int r2 = sensor_status.right_sensor2;
    float return_value = 0.0;
    if (l2 == 0)
    {
        if (r2 == 0)
            return return_value;
        return_value = 1.0;
    }
    else if (r2 == 0)
    {
        if (l2 == 0)
            return return_value;
        return_value = -1.0;
    }
    else
    {
        switch (r2)
        {
            case 1:
            case 5:
                if (l2 == 2  || l2 == 6 ) //|| l2 == 3 || l2 == 7
                    return return_value;
                return_value = 1.0;
                break;
            case 2:
            case 6:
                if (l2 == 1 || l2 ==5)
                  return return_value;
                if (l2 == 2 || l2 == 6 || l2 == 3 || l2 == 7)
                  return_value = -1;
                if (l2 ==4)
                  return_value = 1;
            case 3:
            case 7:
                if (l2 == 3  || l2 == 7)
                    return return_value;
                if (l2 == 2 || l2 == 6)
                    return_value = -1.0;
                if (l2 == 4 || l2 == 1 || l2 == 5)
                    return_value = 1.0;
                break;
            case 4:
                if (l2 == 4)
                    return return_value;
                return_value = -1.0;
                break;
        }
    }
    return return_value;
}

void DockController::setDockPid(double kp, double ki, double kd)
{
    kp_ = kp;
    ki_ = ki;
    kd_ = kd;
}

bool DockController::rotateOrigin()
{
    geometry_msgs::Pose current_pose = mRobot_pose_;
    float x, y, theta;
    x = current_pose.position.x;
    y = current_pose.position.y;
    tf::Quaternion q1(current_pose.orientation.x, current_pose.orientation.y, current_pose.orientation.z,
                      current_pose.orientation.w);
    tf::Matrix3x3 m1(q1);
    double roll, pitch, yaw;
    m1.getRPY(roll, pitch, yaw);
    theta = yaw;
    static float target_theta = 0;
    if (usefull_num_ == 0)
    {
        //?????????20???
        target_theta = theta + 10.f / 180.f * PI_temp;
        if (target_theta <= -PI_temp)
            target_theta += 2 * PI_temp;
        if (target_theta > PI_temp)
            target_theta -= 2 * PI_temp;
        usefull_num_++;
    }
    else if (usefull_num_ == 2)
    {
        //?????????40???
        target_theta = theta - 20.f / 180.f * PI_temp;
        if (target_theta <= -PI_temp)
            target_theta += 2 * PI_temp;
        if (target_theta > PI_temp)
            target_theta -= 2 * PI_temp;
        usefull_num_++;
    }
    else if (usefull_num_ == 4)
    {
        //????????????20???
        target_theta = theta + 10.f / 180.f * PI_temp;
        if (target_theta <= -PI_temp)
            target_theta += 2 * PI_temp;
        if (target_theta > PI_temp)
            target_theta -= 2 * PI_temp;
        usefull_num_++;
    }
    if (usefull_num_ == 1 || usefull_num_ == 5)
    {
        //??????
        geometry_msgs::Twist current_vel;
        current_vel.linear.x = 0;
        current_vel.linear.y = 0;
        current_vel.linear.z = 0;
        current_vel.angular.x = 0;
        current_vel.angular.y = 0;
        current_vel.angular.z = 0.2;
        mCmdvelPub_.publish(current_vel);
    }
    else if (usefull_num_ == 3)
    {
        //??????
        geometry_msgs::Twist current_vel;
        current_vel.linear.x = 0;
        current_vel.linear.y = 0;
        current_vel.linear.z = 0;
        current_vel.angular.x = 0;
        current_vel.angular.y = 0;
        current_vel.angular.z = -0.2;
        mCmdvelPub_.publish(current_vel);
    }
    float theta_error = theta - target_theta;
    if (theta_error <= -PI_temp)
        theta_error += 2 * PI_temp;
    if (theta_error > PI_temp)
        theta_error -= 2 * PI_temp;
    if (fabs(theta_error) < 0.02)
    {
        //???????????????????????????????????????
        geometry_msgs::Twist current_vel;
        current_vel.linear.x = 0;
        current_vel.linear.y = 0;
        current_vel.linear.z = 0;
        current_vel.angular.x = 0;
        current_vel.angular.y = 0;
        current_vel.angular.z = 0;
        mCmdvelPub_.publish(current_vel);
        usefull_num_++;
    }
    if (usefull_num_ == 6)
    {
        geometry_msgs::Twist current_vel;
        current_vel.linear.x = 0;
        current_vel.linear.y = 0;
        current_vel.linear.z = 0;
        current_vel.angular.x = 0;
        current_vel.angular.y = 0;
        current_vel.angular.z = 0;
        mCmdvelPub_.publish(current_vel);
        return true;
    }
    return false;
}

void DockController::caculatePose4()
{
    //????????????????????????????????????????????????pose3???????????????x?????????????????????????????????
    geometry_msgs::Pose current_pose = mRobot_pose_;
    float x, y, theta, x2;
    x = current_pose.position.x;
    y = current_pose.position.y;
    tf::Quaternion q1(current_pose.orientation.x, current_pose.orientation.y, current_pose.orientation.z,
                      current_pose.orientation.w);
    tf::Matrix3x3 m1(q1);
    double roll, pitch, yaw;
    m1.getRPY(roll, pitch, yaw);
    theta = yaw;

    mPose4_[2] = theta;
    //??????pose3???????????????,??????x??????
    x2 = cos(mPose3_[2]) * (x - mPose3_[0]) + sin(mPose3_[2]) * (y - mPose3_[1]);
    //?????????????????????
    mPose4_[0] = mPose3_[0] + x2 * cos(mPose3_[2]);
    mPose4_[1] = mPose3_[1] + x2 * sin(mPose3_[2]);
}

bool DockController::goToPose4()
{
    static float last_x2 = 0;
    geometry_msgs::Pose current_pose = mRobot_pose_;
    float x, y, theta, x2, y2;
    x = current_pose.position.x;
    y = current_pose.position.y;
    tf::Quaternion q1(current_pose.orientation.x, current_pose.orientation.y, current_pose.orientation.z,
                      current_pose.orientation.w);
    tf::Matrix3x3 m1(q1);
    double roll, pitch, yaw;
    m1.getRPY(roll, pitch, yaw);
    theta = yaw;
    //??????pose4???????????????
    x2 = cos(mPose4_[2]) * (x - mPose4_[0]) + sin(mPose4_[2]) * (y - mPose4_[1]);
    y2 = -sin(mPose4_[2]) * (x - mPose4_[0]) + cos(mPose4_[2]) * (y - mPose4_[1]);

    if(min_x2_4_>99) last_x2 = x2;
    if(fabs(x2)<min_x2_4_) min_x2_4_ = fabs(x2);

    //ROS_ERROR("temp error3 %f %f %f ; %f %f",x,y,yaw, x2,y2);
    //?????????????????????????????????
    if (fabs(x2) <= 0.03)
        return true;
    if((x2*last_x2) < 0.0001) return true; //????????????
    if(fabs(min_x2_4_ - fabs(x2)) > 0.2) return true; //??????

    last_x2 = x2;

    geometry_msgs::Twist current_vel;
    current_vel.linear.x = 0.2;
    current_vel.linear.y = 0;
    current_vel.linear.z = 0;
    current_vel.angular.x = 0;
    current_vel.angular.y = 0;
    current_vel.angular.z = 0;
    mCmdvelPub_.publish(current_vel);
    return false;
}

geometry_msgs::Pose DockController::getRobotPose()
{
    boost::mutex::scoped_lock lock(mMutex_pose);
    return mRobot_pose_;
}
bool DockController::getRobotPose(float (&robot_pose)[3])
{
    boost::mutex::scoped_lock lock(mMutex_pose);
    if (!mPose_flag_)
        return false;
    geometry_msgs::Pose current_pose = mRobot_pose_;
    robot_pose[0] = current_pose.position.x;
    robot_pose[1] = current_pose.position.y;
    tf::Quaternion q1(current_pose.orientation.x, current_pose.orientation.y, current_pose.orientation.z,
                      current_pose.orientation.w);
    tf::Matrix3x3 m1(q1);
    double roll, pitch, yaw;
    m1.getRPY(roll, pitch, yaw);
    robot_pose[2] = yaw;
    return true;
}

bool DockController::getIRPose(float (&robot_pose)[3])
{
    boost::mutex::scoped_lock lock(mMutex_pose);
    if (!mPose_flag_)
        return false;
    geometry_msgs::Pose current_pose = mRobot_pose_;

    tf::Quaternion q1(current_pose.orientation.x, current_pose.orientation.y, current_pose.orientation.z,
                      current_pose.orientation.w);
    tf::Matrix3x3 m1(q1);
    double roll, pitch, yaw;
    m1.getRPY(roll, pitch, yaw);
    robot_pose[2] = yaw;
    robot_pose[0] = current_pose.position.x - back_distance_ * cos(yaw);
    robot_pose[1] = current_pose.position.y - back_distance_ * sin(yaw);
    return true;
}

void DockController::setDockPositionCaculate(CaculateDockPosition* dock_position_caculate)
{
    mdock_position_caculate_ = dock_position_caculate;
}

void DockController::caculateStation3()
{
    //?????????????????????
    geometry_msgs::Pose current_pose = mRobot_pose_;
    float x, y, theta;
    x = current_pose.position.x;
    y = current_pose.position.y;

    float distance1, distance2;
    distance1 = (x - mstationPose1_[0]) * (x - mstationPose1_[0]) + (y - mstationPose1_[1]) * (y - mstationPose1_[1]);
    distance2 = (x - mstationPose2_[0]) * (x - mstationPose2_[0]) + (y - mstationPose2_[1]) * (y - mstationPose2_[1]);
    if (distance1 > distance2  )
    {
        if(distance2>(0.1*0.1))
        {//???2
           mstationPose3_[0] = mstationPose2_[0];
           mstationPose3_[1] = mstationPose2_[1];
        }
        else
        {
	   //???1
           mstationPose3_[0] = mstationPose1_[0];
            mstationPose3_[1] = mstationPose1_[1];
        }
    }
    else
    {
        if(distance1<(0.1*0.1))
        {//???2
           mstationPose3_[0] = mstationPose2_[0];
           mstationPose3_[1] = mstationPose2_[1];
        }
        else
        {
	   //???1
           mstationPose3_[0] = mstationPose1_[0];
            mstationPose3_[1] = mstationPose1_[1];
        }
    }
    theta = atan2(mstationPose3_[1] - y, mstationPose3_[0] - x);

    mstationPose3_[2] = theta;
    //  ROS_INFO("station3 %f %f %f %f %f %f %f %f
    //  %f",mstationPose1_[0],mstationPose1_[1],mstationPose2_[0],mstationPose2_[1],mstationPose3_[0],mstationPose3_[1],mstationPose3_[2],x,y);
    //ROS_ERROR("temp error1 %f %f; %f %f %f",x,y, mstationPose3_[0],mstationPose3_[1],mstationPose3_[2]);
}

bool DockController::rotate2Station3()
{
    //????????????????????????3
    geometry_msgs::Pose current_pose = mRobot_pose_;

    float x, y, theta;
    x = current_pose.position.x;
    y = current_pose.position.y;
    tf::Quaternion q1(current_pose.orientation.x, current_pose.orientation.y, current_pose.orientation.z,
                      current_pose.orientation.w);
    tf::Matrix3x3 m1(q1);

    double roll, pitch, yaw;
    m1.getRPY(roll, pitch, yaw);
    theta = yaw;
    //ROS_ERROR("temp error %f %f %f ; %f %f %f",roll,pitch,yaw, mstationPose3_[0],mstationPose3_[1],mstationPose3_[2]);
    if (fabs(theta - mstationPose3_[2]) < 0.08)
    {
        //ROS_ERROR("temp error2 %f %f %f ; %f %f %f",x,y,yaw, mstationPose3_[0],mstationPose3_[1],mstationPose3_[2]);
        return true;
    }
    else
    {
        geometry_msgs::Twist current_vel;
        current_vel.linear.x = 0;
        current_vel.linear.y = 0;
        current_vel.linear.z = 0;
        current_vel.angular.x = 0;
        current_vel.angular.y = 0;
        current_vel.angular.z = 0;
        //mCmdvelPub_.publish(current_vel);

        float delta_theta = theta - mstationPose3_[2];

        if (delta_theta > 0.001)
        {
            if (delta_theta < (PI_temp + 0.001))
            {
                //??????
                current_vel.angular.z = -0.3;
            }
            else
            {
                //??????
                current_vel.angular.z = 0.3;
            }
        }
        else
        {
            if (delta_theta < (-PI_temp - 0.001))
            {
                //??????
                current_vel.angular.z = -0.3;
            }
            else
            {
                //??????
                current_vel.angular.z = 0.3;
            }
        }
        mCmdvelPub_.publish(current_vel);
    }
    return false;
}

bool DockController::goToStation3()
{
    static float last_x2 = 0;
    geometry_msgs::Pose current_pose = mRobot_pose_;
    float x, y, theta, x2, y2;
    x = current_pose.position.x;
    y = current_pose.position.y;
    tf::Quaternion q1(current_pose.orientation.x, current_pose.orientation.y, current_pose.orientation.z,
                      current_pose.orientation.w);
    tf::Matrix3x3 m1(q1);
    double roll, pitch, yaw;
    m1.getRPY(roll, pitch, yaw);
    theta = yaw;
    //??????pose4???????????????
    x2 = cos(mstationPose3_[2]) * (x - mstationPose3_[0]) + sin(mstationPose3_[2]) * (y - mstationPose3_[1]);
    y2 = -sin(mstationPose3_[2]) * (x - mstationPose3_[0]) + cos(mstationPose3_[2]) * (y - mstationPose3_[1]);

    if(min_x2_>99) last_x2 = x2;
    if(fabs(x2)<min_x2_) min_x2_ = fabs(x2);

    //ROS_ERROR("temp error3 %f %f %f ; %f %f",x,y,yaw, x2,y2);
    //?????????????????????????????????
    if (fabs(x2) <= 0.06)
        return true;
    if((x2*last_x2) < 0.0001) return true; //????????????
    if(fabs(min_x2_ - fabs(x2)) > 0.2) return true; //??????

    last_x2 = x2;

    geometry_msgs::Twist current_vel;
    current_vel.linear.x = 0.2;
    current_vel.linear.y = 0;
    current_vel.linear.z = 0;
    current_vel.angular.x = 0;
    current_vel.angular.y = 0;
    current_vel.angular.z = 0;
    mCmdvelPub_.publish(current_vel);
    return false;
}

}  // namespace bw_auto_dock
