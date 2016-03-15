//odom_tf_fncs.cpp:
//wsn, March 2016
//implementation of member functions of OdomTf class

#include "odom_tf.h"
using namespace std;

//constructor: don't need nodehandle here, but could be handy if want to add a subscriber
OdomTf::OdomTf(ros::NodeHandle* nodehandle):nh_(*nodehandle)
{ // constructor
    ROS_INFO("in class constructor of DemoTfListener");
    tfListener_ = new tf::TransformListener;  //create a transform listener
    
    // wait to start receiving valid tf transforms between odom and link2:
    bool tferr=true;
    ROS_INFO("waiting for tf between base_link and odom...");
    while (tferr) {
        tferr=false;
        try {
                //try to lookup transform from target frame "odom" to source frame "link2"
            //The direction of the transform returned will be from the target_frame to the source_frame. 
             //Which if applied to data, will transform data in the source_frame into the target_frame. 
            //See tf/CoordinateFrameConventions#Transform_Direction
                tfListener_->lookupTransform("odom", "base_link", ros::Time(0), stfBaseLinkWrtOdom_);
            } catch(tf::TransformException &exception) {
                ROS_WARN("%s; retrying...", exception.what());
                tferr=true;
                ros::Duration(0.5).sleep(); // sleep for half a second
                ros::spinOnce();                
            }   
    }
    ROS_INFO("odom to base_link tf is good");
    
    //now check for xform base_link w/rt map; would need amcl, or equivalent running
    tferr=true;
    ROS_INFO("waiting for tf between odom and map...");
    while (tferr) {
        tferr=false;
        try {
                //try to lookup transform from target frame "odom" to source frame "link2"
            //The direction of the transform returned will be from the target_frame to the source_frame. 
             //Which if applied to data, will transform data in the source_frame into the target_frame. 
            //See tf/CoordinateFrameConventions#Transform_Direction
                tfListener_->lookupTransform("map", "odom", ros::Time(0), stfOdomWrtMap_);
            } catch(tf::TransformException &exception) {
                ROS_WARN("%s; retrying...", exception.what());
                tferr=true;
                ros::Duration(0.5).sleep(); // sleep for half a second
                ros::spinOnce();                
            }   
    }
    ROS_INFO("map to odom tf is good");    
    // from now on, tfListener will keep track of transforms
    
    initializeSubscribers(); // package up the messy work of creating subscribers; do this overhead in constructor
    //initializePublishers();
    
    odom_phi_ = 1000.0; // put in impossible value for heading; test this value to make sure we have received a viable odom message
    ROS_INFO("waiting for valid odom message...");
    while (odom_phi_ > 500.0) {
        ros::Duration(0.5).sleep(); // sleep for half a second
        std::cout << ".";
        ros::spinOnce();
    }
    ROS_INFO("constructor: got an odom message");    

}



void OdomTf::initializeSubscribers() {
    ROS_INFO("Initializing Subscribers");
    odom_subscriber_ = nh_.subscribe("/drifty_odom", 1, &OdomTf::odomCallback, this); //subscribe to odom messages
    // add more subscribers here, as needed
}


//member helper function to set up publishers;
/*
void OdomTf::initializePublishers()
{
    ROS_INFO("Initializing Publishers"); //nav_msgs::Odometry
    hybrid_pose_publisher_ = nh_.advertise<nav_msgs::Odometry>("hybrid_robot_state", 1, true);
    hybrid_yaw_publisher_ = nh_.advertise<std_msgs::Float64>("hybrid_robot_yaw", 1, true);
}
*/

//some conversion utilities:
double OdomTf::convertPlanarQuat2Phi(geometry_msgs::Quaternion quaternion) {
    double quat_z = quaternion.z;
    double quat_w = quaternion.w;
    double phi = 2.0 * atan2(quat_z, quat_w); // cheap conversion from quaternion to heading for planar motion
    return phi;
}

//some conversion utilities:
//getting a transform from a stamped transform is trickier than expected--there is not "get" fnc for transform
tf::Transform OdomTf::get_tf_from_stamped_tf(tf::StampedTransform sTf) {
   tf::Transform tf(sTf.getBasis(),sTf.getOrigin()); //construct a transform using elements of sTf
   return tf;
}

//a transform describes the position and orientation of a frame w/rt a reference frame
//a Pose is a position and orientation of a frame of interest w/rt a reference frame
// can re-interpret a transform as a named pose using this example function
// the PoseStamped also has a header, which includes naming the reference frame
geometry_msgs::PoseStamped OdomTf::get_pose_from_transform(tf::StampedTransform tf) {
  //clumsy conversions--points, vectors and quaternions are different data types in tf vs geometry_msgs
  geometry_msgs::PoseStamped stPose;
  geometry_msgs::Quaternion quat;  //geometry_msgs object for quaternion
  tf::Quaternion tfQuat; // tf library object for quaternion
  tfQuat = tf.getRotation(); // member fnc to extract the quaternion from a transform
  quat.x = tfQuat.x(); // copy the data from tf-style quaternion to geometry_msgs-style quaternion
  quat.y = tfQuat.y();
  quat.z = tfQuat.z();
  quat.w = tfQuat.w();  
  stPose.pose.orientation = quat; //set the orientation of our PoseStamped object from result
  
  // now do the same for the origin--equivalently, vector from parent to child frame 
  tf::Vector3 tfVec;  //tf-library type
  geometry_msgs::Point pt; //equivalent geometry_msgs type
  tfVec = tf.getOrigin(); // extract the vector from parent to child from transform
  pt.x = tfVec.getX(); //copy the components into geometry_msgs type
  pt.y = tfVec.getY();
  pt.z = tfVec.getZ();  
  stPose.pose.position= pt; //and use this compatible type to set the position of the PoseStamped
  stPose.header.frame_id = tf.frame_id_; //the pose is expressed w/rt this reference frame
  stPose.header.stamp = tf.stamp_; // preserve the time stamp of the original transform
  return stPose;
}

//a function to multiply two stamped transforms;  this function checks to make sure the
// multiplication is logical, e.g.: T_B/A * T_C/B = T_C/A
// returns false if the two frames are inconsistent as sequential transforms
// returns true if consistent A_stf and B_stf transforms, and returns result of multiply in C_stf
// The reference frame and child frame are populated in C_stf accordingly
bool OdomTf::multiply_stamped_tfs(tf::StampedTransform A_stf, 
        tf::StampedTransform B_stf, tf::StampedTransform &C_stf) {
   tf::Transform A,B,C; //simple transforms--not stamped
  std::string str1 (A_stf.child_frame_id_); //want to compare strings to check consistency
  std::string str2 (B_stf.frame_id_);
  if (str1.compare(str2) != 0) { //SHOULD get that child frame of A is parent frame of B
      std::cout<<"can't multiply transforms; mismatched frames"<<endl;
    std::cout << str1 << " is not " << str2 << '\n'; 
    return false;
  }
   //if here, the named frames are logically consistent
   A = get_tf_from_stamped_tf(A_stf); // get the transform from the stamped transform
   B = get_tf_from_stamped_tf(B_stf);
   C = A*B; //multiplication is defined for transforms 
   C_stf.frame_id_ = A_stf.frame_id_; //assign appropriate parent and child frames to result
   C_stf.child_frame_id_ = B_stf.child_frame_id_;
   C_stf.setOrigin(C.getOrigin()); //populate the origin and orientation of the result
   C_stf.setBasis(C.getBasis());
   C_stf.stamp_ = ros::Time::now(); //assign the time stamp to current time; 
     // alternatively, could assign this to the OLDER of A or B transforms
   return true; //if got here, the multiplication is valid
}

//fnc to print components of a transform
void OdomTf::printTf(tf::Transform tf) {
    tf::Vector3 tfVec;
    tf::Matrix3x3 tfR;
    tf::Quaternion quat;
    tfVec = tf.getOrigin();
    cout<<"vector from reference frame to child frame: "<<tfVec.getX()<<","<<tfVec.getY()<<","<<tfVec.getZ()<<endl;
    tfR = tf.getBasis();
    cout<<"orientation of child frame w/rt reference frame: "<<endl;
    tfVec = tfR.getRow(0);
    cout<<tfVec.getX()<<","<<tfVec.getY()<<","<<tfVec.getZ()<<endl;
    tfVec = tfR.getRow(1);
    cout<<tfVec.getX()<<","<<tfVec.getY()<<","<<tfVec.getZ()<<endl;    
    tfVec = tfR.getRow(2);
    cout<<tfVec.getX()<<","<<tfVec.getY()<<","<<tfVec.getZ()<<endl; 
    quat = tf.getRotation();
    cout<<"quaternion: " <<quat.x()<<", "<<quat.y()<<", "
            <<quat.z()<<", "<<quat.w()<<endl;   
}

//fnc to print components of a stamped transform
void OdomTf::printStampedTf(tf::StampedTransform sTf){
    tf::Transform tf;
    cout<<"frame_id: "<<sTf.frame_id_<<endl;
    cout<<"child_frame_id: "<<sTf.child_frame_id_<<endl; 
    tf = get_tf_from_stamped_tf(sTf); //extract the tf from the stamped tf  
    printTf(tf); //and print its components      
    }

//fnc to print components of a stamped pose
void OdomTf::printStampedPose(geometry_msgs::PoseStamped stPose){
    cout<<"frame id = "<<stPose.header.frame_id<<endl;
    cout<<"origin: "<<stPose.pose.position.x<<", "<<stPose.pose.position.y<<", "<<stPose.pose.position.z<<endl;
    cout<<"quaternion: "<<stPose.pose.orientation.x<<", "<<stPose.pose.orientation.y<<", "
            <<stPose.pose.orientation.z<<", "<<stPose.pose.orientation.w<<endl;
}


void OdomTf::odomCallback(const nav_msgs::Odometry& odom_rcvd) {
    // copy some of the components of the received message into member vars
    // we care about speed and spin, as well as position estimates x,y and heading
    current_odom_ = odom_rcvd; // save the entire message
    // but also pick apart pieces, for ease of use
    odom_pose_ = odom_rcvd.pose.pose;
    //odom_vel_ = odom_rcvd.twist.twist.linear.x;
    //odom_omega_ = odom_rcvd.twist.twist.angular.z;
    odom_x_ = odom_rcvd.pose.pose.position.x;
    odom_y_ = odom_rcvd.pose.pose.position.y;
    odom_quat_ = odom_rcvd.pose.pose.orientation;
    //odom publishes orientation as a quaternion.  Convert this to a simple heading
    odom_phi_ = convertPlanarQuat2Phi(odom_quat_); // cheap conversion from quaternion to heading for planar motion
    //get the position from odom, converted to a tf type
    tf::Vector3 pos;
    pos.setX(odom_pose_.position.x);
    pos.setY(odom_pose_.position.y);
    pos.setZ(odom_pose_.position.z);
    tfBaseLinkWrtDriftyOdom_.stamp_=ros::Time::now();
    tfBaseLinkWrtDriftyOdom_.setOrigin(pos);
    // also need to get the orientation of odom_pose and use setRotation
    tf::Quaternion q;
    q.setX(odom_quat_.x);
    q.setY(odom_quat_.y);
    q.setZ(odom_quat_.z);   
    q.setW(odom_quat_.w);      
    tfBaseLinkWrtDriftyOdom_.setRotation(q);
    tfBaseLinkWrtDriftyOdom_.frame_id_ = "odom";
    tfBaseLinkWrtDriftyOdom_.child_frame_id_ = "base_link";
}