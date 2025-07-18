// Copyright 2022 Chen Jun
// Licensed under the MIT License.

#include <cv_bridge/cv_bridge.h>
#include <rmw/qos_profiles.h>
#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2/convert.h>

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <image_transport/image_transport.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <rclcpp/duration.hpp>
#include <rclcpp/qos.hpp>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>

// STD
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "armor_detector/armor.hpp"
#include "armor_detector/detector_node.hpp"

namespace rm_auto_aim
{
ArmorDetectorNode::ArmorDetectorNode(const rclcpp::NodeOptions & options)
: Node("armor_detector", options)
{
  RCLCPP_INFO(this->get_logger(), "Starting DetectorNode!");

  // Detector
  detector_ = initDetector();

  // Armors Publisher
  armors_pub_ = this->create_publisher<auto_aim_interfaces::msg::Armors>(
    "/detector/armors", rclcpp::SensorDataQoS());

  // Visualization Marker Publisher
  // See http://wiki.ros.org/rviz/DisplayTypes/Marker
  armor_marker_.ns = "armors";
  armor_marker_.action = visualization_msgs::msg::Marker::ADD;
  armor_marker_.type = visualization_msgs::msg::Marker::CUBE;
  armor_marker_.scale.x = 0.05;
  armor_marker_.scale.z = 0.125;
  armor_marker_.color.a = 1.0;
  armor_marker_.color.g = 0.5;
  armor_marker_.color.b = 1.0;
  armor_marker_.lifetime = rclcpp::Duration::from_seconds(0.1);

  text_marker_.ns = "classification";
  text_marker_.action = visualization_msgs::msg::Marker::ADD;
  text_marker_.type = visualization_msgs::msg::Marker::TEXT_VIEW_FACING;
  text_marker_.scale.z = 0.1;
  text_marker_.color.a = 1.0;
  text_marker_.color.r = 1.0;
  text_marker_.color.g = 1.0;
  text_marker_.color.b = 1.0;
  text_marker_.lifetime = rclcpp::Duration::from_seconds(0.1);

  // 新增参数初始化
  camera_params_client_ = std::make_shared<rclcpp::AsyncParametersClient>(this, "camera_node");
  target_brightness_global_ = this->declare_parameter("target_brightness_global", 10.0);
  target_brightness_armor_  = this->declare_parameter("target_brightness_armor",  30.0);
  Kp_ = this->declare_parameter("Kp", 0.1);
  Ki_ = this->declare_parameter("Ki", 0.0);
  Kd_ = this->declare_parameter("Kd", 0.1);
  Use_AAG_ = this->declare_parameter("Use_AAG", true);
  current_exposure_ = this->declare_parameter("exposure", 2500.0);
  current_gain_ = this->declare_parameter("gain", 8.0);
  integral_ = 0.0;
  last_error_ = 0.0;

  marker_pub_ =
    this->create_publisher<visualization_msgs::msg::MarkerArray>("/detector/marker", 10);

  // Debug Publishers
  debug_ = this->declare_parameter("debug", false);
  if (debug_) {
    createDebugPublishers();
  }

  // Debug param change moniter
  debug_param_sub_ = std::make_shared<rclcpp::ParameterEventHandler>(this);
  debug_cb_handle_ =
    debug_param_sub_->add_parameter_callback("debug", [this](const rclcpp::Parameter & p) {
      debug_ = p.as_bool();
      debug_ ? createDebugPublishers() : destroyDebugPublishers();
    });

  cam_info_sub_ = this->create_subscription<sensor_msgs::msg::CameraInfo>(
    "/camera_info", rclcpp::SensorDataQoS(),
    [this](sensor_msgs::msg::CameraInfo::ConstSharedPtr camera_info) {
      cam_center_ = cv::Point2f(camera_info->k[2], camera_info->k[5]);
      cam_info_ = std::make_shared<sensor_msgs::msg::CameraInfo>(*camera_info);
      pnp_solver_ = std::make_unique<PnPSolver>(camera_info->k, camera_info->d);
      cam_info_sub_.reset();
    });

  img_sub_ = this->create_subscription<sensor_msgs::msg::Image>(
    "/image_raw", rclcpp::SensorDataQoS(),
    std::bind(&ArmorDetectorNode::imageCallback, this, std::placeholders::_1));
}

float ArmorDetectorNode::getArmorROI(const Armor& armor, const cv::Mat& image)
{
  float left_bright  = getLightMeanBrightness(armor.left_light , image);
  float right_bright = getLightMeanBrightness(armor.right_light, image);
  float mean_bright = (left_bright+right_bright)/2.0;
  // std::vector<cv::Point2f> points = {
  //     armor.left_light.top, armor.left_light.bottom,
  //     armor.right_light.top, armor.right_light.bottom
  // };
  // cv::Rect roi = cv::boundingRect(points);
  // roi &= cv::Rect(0, 0, image.cols, image.rows);
  RCLCPP_INFO(this->get_logger(), "brightness:%f", mean_bright);
  return mean_bright;
}

float ArmorDetectorNode::getLightMeanBrightness(const Light& light, const cv::Mat& gray_img)
{
  // constexpr float MAX_BRIGHTNESS = 255;
  constexpr float SCALE = 0.07;

  // Scale the bounding box
  cv::Rect light_box = light;
  light_box.x -= light_box.width * SCALE;
  light_box.y -= light_box.height * SCALE;
  light_box.width += light_box.width * SCALE * 2;
  light_box.height += light_box.height * SCALE * 2;

  // Check boundary
  light_box.x = std::max(light_box.x, 0);
  light_box.x = std::min(light_box.x, gray_img.cols - 1);
  light_box.y = std::max(light_box.y, 0);
  light_box.y = std::min(light_box.y, gray_img.rows - 1);
  light_box.width = std::min(light_box.width, gray_img.cols - light_box.x);
  light_box.height = std::min(light_box.height, gray_img.rows - light_box.y);
  // RCLCPP_INFO(this->get_logger(), "brightness set");
  float mean_brightness = static_cast<float>(cv::mean(gray_img(light_box))[0]);
  // RCLCPP_INFO(this->get_logger(), "brightness2:%f", mean_brightness);
  return mean_brightness;
}

void ArmorDetectorNode::imageCallback(const sensor_msgs::msg::Image::ConstSharedPtr img_msg)
{
  auto armors = detectArmors(img_msg);
  if (Use_AAG_) {
    auto img = cv_bridge::toCvShare(img_msg, "rgb8")->image;  

    // 计算当前亮度（全图或ROI）
    cv::Mat gray_img;
    cv::cvtColor(img, gray_img, cv::COLOR_RGB2GRAY);
    float current_brightness;
    float target_brightness_;
    if (!armors.empty()) {
      current_brightness = getArmorROI(armors[0],gray_img);
      target_brightness_ = target_brightness_armor_;
    } else {
      target_brightness_ = target_brightness_global_;
      int patch_width = gray_img.cols / 3;
      int patch_height = gray_img.rows / 3;  
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          cv::Rect roi(j * patch_width, i * patch_height, patch_width, patch_height);
          if (i==0 && j==0){
            current_brightness = 0.05*cv::mean(gray_img(roi))[0];
          } else if(i==1 && j==1) {
            current_brightness += 0.4*cv::mean(gray_img(roi))[0];  
          } else if(i==1 || j==1) {
            current_brightness += 0.1*cv::mean(gray_img(roi))[0];
          } else {
            current_brightness += 0.05*cv::mean(gray_img(roi))[0];
          }
        }
      }
    }

    // PID控制计算曝光和增益
    float error = target_brightness_ - current_brightness;
    integral_ += error;
    float derivative = error - last_error_;
    last_error_ = error;

    current_gain_ += Kp_ * error + Ki_ * integral_ + Kd_ * derivative;
    current_gain_ = std::clamp(current_gain_, 0.0, 25.0);
    // RCLCPP_INFO(this->get_logger(), "CCCG gain: %f",current_gain_);
    // 设置相机参数
    std::vector<rclcpp::Parameter> params;
    params.emplace_back("gain", current_gain_);
    camera_params_client_->set_parameters(params,
      [this](const std::shared_future<std::vector<rcl_interfaces::msg::SetParametersResult>>& future) {
        // 处理结果回调
        auto results = future.get();
      });
  }

  if (pnp_solver_ != nullptr) {
    armors_msg_.header = armor_marker_.header = text_marker_.header = img_msg->header;
    armors_msg_.armors.clear();
    marker_array_.markers.clear();
    armor_marker_.id = 0;
    text_marker_.id = 0;

    auto_aim_interfaces::msg::Armor armor_msg;
    for (const auto & armor : armors) {
      cv::Mat rvec, tvec;
      bool success = pnp_solver_->solvePnP(armor, rvec, tvec);
      if (success) {
        // Fill basic info
        armor_msg.type = ARMOR_TYPE_STR[static_cast<int>(armor.type)];
        armor_msg.number = armor.number;

        // Fill pose
        armor_msg.pose.position.x = tvec.at<double>(0);
        armor_msg.pose.position.y = tvec.at<double>(1);
        armor_msg.pose.position.z = tvec.at<double>(2);
        // rvec to 3x3 rotation matrix
        cv::Mat rotation_matrix;
        cv::Rodrigues(rvec, rotation_matrix);
        // rotation matrix to quaternion
        tf2::Matrix3x3 tf2_rotation_matrix(
          rotation_matrix.at<double>(0, 0), rotation_matrix.at<double>(0, 1),
          rotation_matrix.at<double>(0, 2), rotation_matrix.at<double>(1, 0),
          rotation_matrix.at<double>(1, 1), rotation_matrix.at<double>(1, 2),
          rotation_matrix.at<double>(2, 0), rotation_matrix.at<double>(2, 1),
          rotation_matrix.at<double>(2, 2));
        tf2::Quaternion tf2_q;
        tf2_rotation_matrix.getRotation(tf2_q);
        armor_msg.pose.orientation = tf2::toMsg(tf2_q);

        // Fill the distance to image center
        armor_msg.distance_to_image_center = pnp_solver_->calculateDistanceToCenter(armor.center);

        // Fill the markers
        armor_marker_.id++;
        armor_marker_.scale.y = armor.type == ArmorType::SMALL ? 0.135 : 0.23;
        armor_marker_.pose = armor_msg.pose;
        text_marker_.id++;
        text_marker_.pose.position = armor_msg.pose.position;
        text_marker_.pose.position.y -= 0.1;
        text_marker_.text = armor.classfication_result;
        armors_msg_.armors.emplace_back(armor_msg);
        marker_array_.markers.emplace_back(armor_marker_);
        marker_array_.markers.emplace_back(text_marker_);
      } else {
        RCLCPP_WARN(this->get_logger(), "PnP failed!");
      }
    }

    // Publishing detected armors
    armors_pub_->publish(armors_msg_);

    // Publishing marker
    publishMarkers();
  }
}

std::unique_ptr<Detector> ArmorDetectorNode::initDetector()
{
  rcl_interfaces::msg::ParameterDescriptor param_desc;
  param_desc.integer_range.resize(1);
  param_desc.integer_range[0].step = 1;
  param_desc.integer_range[0].from_value = 0;
  param_desc.integer_range[0].to_value = 255;
  int binary_thres = declare_parameter("binary_thres", 160, param_desc);

  param_desc.description = "0-RED, 1-BLUE";
  param_desc.integer_range[0].from_value = 0;
  param_desc.integer_range[0].to_value = 1;
  auto detect_color = declare_parameter("detect_color", RED, param_desc);

  Detector::LightParams l_params = {
    .min_ratio = declare_parameter("light.min_ratio", 0.1),
    .max_ratio = declare_parameter("light.max_ratio", 0.4),
    .min_fill_ratio = declare_parameter("min_fill_ratio", 0.7),
    .min_length = declare_parameter("light.min_length", 10),
    .max_angle = declare_parameter("light.max_angle", 20.0)};

  Detector::ArmorParams a_params = {
    .min_light_ratio = declare_parameter("armor.min_light_ratio", 0.7),
    .min_small_center_distance = declare_parameter("armor.min_small_center_distance", 0.8),
    .max_small_center_distance = declare_parameter("armor.max_small_center_distance", 3.2),
    .min_large_center_distance = declare_parameter("armor.min_large_center_distance", 3.2),
    .max_large_center_distance = declare_parameter("armor.max_large_center_distance", 5.0),
    .max_angle = declare_parameter("armor.max_angle", 30.0)};

  auto detector = std::make_unique<Detector>(binary_thres, detect_color, l_params, a_params);

  // Init classifier
  auto pkg_path = ament_index_cpp::get_package_share_directory("armor_detector");
  auto model_path = pkg_path + "/model/mlp.onnx";
  auto label_path = pkg_path + "/model/label.txt";
  double threshold = this->declare_parameter("classifier_threshold", 0.7);
  std::vector<std::string> ignore_classes =
    this->declare_parameter("ignore_classes", std::vector<std::string>{"negative"});
  detector->classifier =
    std::make_unique<NumberClassifier>(model_path, label_path, threshold, ignore_classes);

  return detector;
}

std::vector<Armor> ArmorDetectorNode::detectArmors(
  const sensor_msgs::msg::Image::ConstSharedPtr & img_msg)
{
  // Convert ROS img to cv::Mat
  auto img = cv_bridge::toCvShare(img_msg, "rgb8")->image;

  // Update params
  detector_->binary_thres = get_parameter("binary_thres").as_int();
  detector_->detect_color = get_parameter("detect_color").as_int();
  detector_->classifier->threshold = get_parameter("classifier_threshold").as_double();

  auto armors = detector_->detect(img);

  auto final_time = this->now();
  auto latency = (final_time - img_msg->header.stamp).seconds() * 1000;
  RCLCPP_DEBUG_STREAM(this->get_logger(), "Latency: " << latency << "ms");

  // Publish debug info
  if (debug_) {
    binary_img_pub_.publish(
      cv_bridge::CvImage(img_msg->header, "mono8", detector_->binary_img).toImageMsg());

    // Sort lights and armors data by x coordinate
    std::sort(
      detector_->debug_lights.data.begin(), detector_->debug_lights.data.end(),
      [](const auto & l1, const auto & l2) { return l1.center_x < l2.center_x; });
    std::sort(
      detector_->debug_armors.data.begin(), detector_->debug_armors.data.end(),
      [](const auto & a1, const auto & a2) { return a1.center_x < a2.center_x; });

    lights_data_pub_->publish(detector_->debug_lights);
    armors_data_pub_->publish(detector_->debug_armors);

    if (!armors.empty()) {
      auto all_num_img = detector_->getAllNumbersImage();
      number_img_pub_.publish(
        *cv_bridge::CvImage(img_msg->header, "mono8", all_num_img).toImageMsg());
    }

    detector_->drawResults(img);
    // Draw camera center
    cv::circle(img, cam_center_, 5, cv::Scalar(255, 0, 0), 2);
    // Draw latency
    std::stringstream latency_ss;
    latency_ss << "Latency: " << std::fixed << std::setprecision(2) << latency << "ms";
    auto latency_s = latency_ss.str();
    cv::putText(
      img, latency_s, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);
    result_img_pub_.publish(cv_bridge::CvImage(img_msg->header, "rgb8", img).toImageMsg());
  }

  return armors;
}

void ArmorDetectorNode::createDebugPublishers()
{
  lights_data_pub_ =
    this->create_publisher<auto_aim_interfaces::msg::DebugLights>("/detector/debug_lights", 10);
  armors_data_pub_ =
    this->create_publisher<auto_aim_interfaces::msg::DebugArmors>("/detector/debug_armors", 10);

  binary_img_pub_ = image_transport::create_publisher(this, "/detector/binary_img");
  number_img_pub_ = image_transport::create_publisher(this, "/detector/number_img");
  result_img_pub_ = image_transport::create_publisher(this, "/detector/result_img");
}

void ArmorDetectorNode::destroyDebugPublishers()
{
  lights_data_pub_.reset();
  armors_data_pub_.reset();

  binary_img_pub_.shutdown();
  number_img_pub_.shutdown();
  result_img_pub_.shutdown();
}

void ArmorDetectorNode::publishMarkers()
{
  using Marker = visualization_msgs::msg::Marker;
  armor_marker_.action = armors_msg_.armors.empty() ? Marker::DELETE : Marker::ADD;
  marker_array_.markers.emplace_back(armor_marker_);
  marker_pub_->publish(marker_array_);
}

}  // namespace rm_auto_aim

#include "rclcpp_components/register_node_macro.hpp"

// Register the component with class_loader.
// This acts as a sort of entry point, allowing the component to be discoverable when its library
// is being loaded into a running process.
RCLCPP_COMPONENTS_REGISTER_NODE(rm_auto_aim::ArmorDetectorNode)
