#include <minkindr_conversions/kindr_msg.h>
#include <minkindr_conversions/kindr_tf.h>
#include <pcl/conversions.h>
#include <pcl/point_types.h>
#include <pcl_ros/point_cloud.h>
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <tf/transform_listener.h>
#include <visualization_msgs/MarkerArray.h>

#include "voxblox/core/map.h"

namespace voxblox {

// TODO(helenol): split into a ROS wrapper/server and a node that actually
// sets settings, etc.
// Follow open_chisel model.
class VoxbloxNode {
 public:
  VoxbloxNode(const ros::NodeHandle& nh, const ros::NodeHandle& nh_private)
      : nh_(nh), nh_private_(nh_private), world_frame_("world") {
    // Advertise topics.
    sdf_marker_pub_ = nh_private_.advertise<visualization_msgs::MarkerArray>(
        "sdf_markers", 1, true);
    sdf_pointcloud_pub_ =
        nh_private_.advertise<visualization_msgs::MarkerArray>("sdf_pointcloud",
                                                               1, true);

    pointcloud_sub_ = nh_.subscribe("pointcloud", 40,
                                    &VoxbloxNode::insertPointcloudWithTf, this);

    // 16 vps at 0.2 resolution. TODO(helenol): load these from params for
    // faster prototyping...
    tsdf_map_.reset(new TsdfMap(16, 0.2));
  }

  void insertPointcloudWithTf(
      const sensor_msgs::PointCloud2::ConstPtr& pointcloud);

  void publishMarkers();

  bool lookupTransform(const std::string& from_frame,
                       const std::string& to_frame, const ros::Time& timestamp,
                       Transformation* transform);

 private:
  ros::NodeHandle nh_;
  ros::NodeHandle nh_private_;

  // Global/map coordinate frame. Will always look up TF transforms to this
  // frame.
  std::string world_frame_;

  // To be replaced (at least optionally) with odometry + static transform from
  // IMU to visual frame.
  tf::TransformListener tf_listener_;

  // Data subscribers.
  ros::Subscriber pointcloud_sub_;

  // Publish markers for visualization.
  ros::Publisher sdf_marker_pub_;
  ros::Publisher sdf_pointcloud_pub_;

  std::shared_ptr<TsdfMap> tsdf_map_;
};

void VoxbloxNode::insertPointcloudWithTf(
    const sensor_msgs::PointCloud2::ConstPtr& pointcloud) {
  // Look up transform from sensor frame to world frame.
  Transformation sensor_to_world;
  if (lookupTransform(pointcloud->header.frame_id, world_frame_,
                      pointcloud->header.stamp, &sensor_to_world)) {
    // INTEGRATOR CALL BELOW:
    // insertPointcloud(sensor_to_world, pointcloud);
  }
  // ??? Should we transform the pointcloud???? Or not. I think probably best
  // not to.
  // Pass to integrator, which should take minkindr transform and a pointcloud
  // in sensor frame.
}

void VoxbloxNode::publishMarkers() {
  CHECK(tsdf_map_) << "TSDF map not allocated.";

  // Create a pointcloud with distance = intensity.
  pcl::PointCloud<pcl::PointXYZI> pointcloud;

  // Iterate over all voxels to create a pointcloud.
  // TODO(helenol): move this to general IO, replace ply writer with writing
  // this out.
  size_t num_blocks = tsdf_map_->getNumberOfAllocatedBlocks();
  // This function is block-specific:
  size_t num_voxels_per_block = tsdf_map_->getVoxelsPerBlock();

  pointcloud.reserve(num_blocks * num_voxels_per_block);

  BlockIndexList blocks;
  tsdf_map_->getAllAllocatedBlocks(&blocks);

  // Iterate over all blocks.
  const float max_distance = 0.5;
  for (const BlockIndex& index : blocks) {
    // Iterate over all voxels in said blocks.
    const TsdfBlock& block = tsdf_map_->getBlockByIndex(index);
    for (size_t i = 0; i < num_voxels_per_block; ++i) {
      float distance = block.getTsdfVoxelByLinearIndex(i).distance;

      // Get back the original coordinate of this voxel.
      Coordinates coord = block.getCoordinatesOfTsdfVoxelByLinearIndex(i);

      pcl::PointXYZI point;
      point.x = coord.x();
      point.y = coord.y();
      point.z = coord.z();
      point.intensity = distance;
      pointcloud.push_back(point);
    }
  }

  pointcloud.header.frame_id = world_frame_;
  sdf_pointcloud_pub_.publish(pointcloud);
}

// Stolen from octomap_manager
bool VoxbloxNode::lookupTransform(const std::string& from_frame,
                                  const std::string& to_frame,
                                  const ros::Time& timestamp,
                                  Transformation* transform) {
  tf::StampedTransform tf_transform;

  ros::Time time_to_lookup = timestamp;

  // If this transform isn't possible at the time, then try to just look up
  // the latest (this is to work with bag files and static transform publisher,
  // etc).
  if (!tf_listener_.canTransform(to_frame, from_frame, time_to_lookup)) {
    time_to_lookup = ros::Time(0);
    ROS_WARN("Using latest TF transform instead of timestamp match.");
  }

  try {
    tf_listener_.lookupTransform(to_frame, from_frame, time_to_lookup,
                                 tf_transform);
  } catch (tf::TransformException& ex) {
    ROS_ERROR_STREAM(
        "Error getting TF transform from sensor data: " << ex.what());
    return false;
  }

  tf::transformTFToKindr(tf_transform, transform);
  return true;
}

}  // namespace voxblox

int main(int argc, char** argv) {
  ros::init(argc, argv, "voxblox_node");
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, false);
  google::InstallFailureSignalHandler();
  ros::NodeHandle nh;
  ros::NodeHandle nh_private("~");

  voxblox::VoxbloxNode node(nh, nh_private);

  ros::spin();
  return 0;
}
