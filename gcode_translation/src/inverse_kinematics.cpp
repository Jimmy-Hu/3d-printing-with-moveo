#include <moveit/move_group_interface/move_group_interface.h>
#include <moveit/planning_scene_interface/planning_scene_interface.h>

#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit/kinematic_constraints/utils.h>

#include <moveit_msgs/AttachedCollisionObject.h>
#include <moveit_msgs/CollisionObject.h>

#include <trac_ik/trac_ik.hpp>
#include <ros/ros.h>

#include <sstream>
#include <fstream>

#include <time.h>
#include <math.h>

#include <omp.h>

#include <stdio.h>
#include <unistd.h>
int decimal_point(double &A){
  std::string change = std::to_string(A);
  bool point = 0;
  for(int i = 0;i<change.size();i++){
    if(change[i] == '.'){
      point = 1;
    }
    if(point == 1){
      if(change[i+3] == '0'){
        if(change[i+2] == '0'){
          if(change[i+1] == '0'){
            return 0;
          }
          return 1;
        }
        return 2;
      }
      return 3;
    }
  }
}
bool check_trac_ik_valid(TRAC_IK::TRAC_IK &tracik_solver,KDL::Chain &chain, KDL::JntArray &ll, KDL::JntArray &ul){
  bool valid = tracik_solver.getKDLChain(chain);
  //ROS_INFO_NAMED("moveo", "TRAC-IK setup");
  if (!valid)
  {
    ROS_ERROR("There was no valid KDL chain found");
    return false;
  }
  valid = tracik_solver.getKDLLimits(ll, ul);
  if (!valid)
  {
    ROS_ERROR("There were no valid KDL joint limits found");
    return false;
  }
  return true;
}
//1028.57143*4*4.523809*0.95/5.18*100
void motor_setep_convert(Eigen::VectorXd &data){
                                        // steps * micro_steps * belt * error
  static const double joint_division[6] = {200          * 32  * 10.533   * 0.95,  //J
                                           200          * 128 * 5.71428  * 0.95,  //A 
                                           19810.111813 * 4   * 4.523809 * 0.95,  //B
                                           5370.24793   * 32  * 1        * 1,     //C
                                           1036.36364   * 16  * 4.666    * 1      //D
                                           };
  for(int i = 0; i < 5; i++){
    data(i) = data(i)/(M_PI)*180 * joint_division[i]/360;
  }
}

ros::WallTime start_, end_;
int main(int argc, char **argv){
  start_ = ros::WallTime::now();
  ros::init(argc, argv, "gcode_translation");
  ros::NodeHandle node_handle("~");
  ros::AsyncSpinner spinner(1);
  spinner.start();
  //----------------------------
  //Setup
  //----------------------------
  std::string chain_start, chain_end, urdf_param;
  double timeout,timeout_second,timeout_third,eps;
  int num_threads;
  node_handle.param("chain_start", chain_start, std::string(""));
  node_handle.param("chain_end", chain_end, std::string(""));
  if (chain_start == "" || chain_end == ""){
    ROS_FATAL("Missing chain info in launch file");
    exit(-1);
  }
  node_handle.param("timeout", timeout, 0.05);
  node_handle.param("timeout_second", timeout_second, 0.1);
  node_handle.param("urdf_param", urdf_param, std::string("/robot_description"));
  node_handle.param("eps", eps, 1e-5);
  node_handle.param("num_threads", num_threads, omp_get_num_procs()*2);
  double X_offset, Y_offset, Z_offset;
  node_handle.param("X_offset", X_offset, 0.0);
  node_handle.param("Y_offset", Y_offset, 0.3);
  node_handle.param("Z_offset", Z_offset,-0.085);

  KDL::Chain chain;
  KDL::JntArray ll, ul; //lower joint limits, upper joint limits

  TRAC_IK::TRAC_IK** tracik_solver = new TRAC_IK::TRAC_IK*[num_threads];
  for(int i = 0; i < num_threads; i++){
    tracik_solver[i] = new TRAC_IK::TRAC_IK(chain_start, chain_end, urdf_param, timeout, eps, TRAC_IK::Speed);
    if(!check_trac_ik_valid(*tracik_solver[i], chain, ll, ul)) return -1;
  }
  TRAC_IK::TRAC_IK tracik_solver_onecore(chain_start, chain_end, urdf_param, timeout_second, eps, TRAC_IK::Speed);
  if(!check_trac_ik_valid(tracik_solver_onecore, chain, ll, ul)) return -1;

  TRAC_IK::TRAC_IK tracik_solver_p_n(chain_start, chain_end, urdf_param, timeout_second, eps, TRAC_IK::Speed);
  if(!check_trac_ik_valid(tracik_solver_p_n, chain, ll, ul)) return -1;

  assert(chain.getNrOfJoints() == ll.data.size());
  assert(chain.getNrOfJoints() == ul.data.size());
  // Create Nominal chain configuration midway between all joint limits
  KDL::JntArray nominal(chain.getNrOfJoints());
  for (uint j = 0; j < nominal.data.size(); j++){
    nominal(j) = (ll(j) + ul(j)) / 2.0;
  }
  robot_model_loader::RobotModelLoader robot_model_loader("robot_description");
  robot_model::RobotModelPtr kinematic_model = robot_model_loader.getModel();
  planning_scene::PlanningScene planning_scene(kinematic_model);
  collision_detection::CollisionRequest collision_request;
  collision_detection::CollisionResult collision_result;
  planning_scene.checkSelfCollision(collision_request, collision_result);
  ROS_INFO_STREAM("Test 1: Current state is " << (collision_result.collision ? "in" : "not in") << " self collision");
  robot_state::RobotState& current_state = planning_scene.getCurrentStateNonConst();
  collision_request.group_name = "arm";
  
  std::string g_n_judge_Y;
  node_handle.param("g_n_judge_Y", g_n_judge_Y, std::string("/g_n_judge_Y"));
  std::ifstream input_file_first(g_n_judge_Y);
  if(!input_file_first.is_open())ROS_ERROR_STREAM("Can't open " <<g_n_judge_Y);
  std::vector<double> judge_Y;
  std::string line;
  int all_line = 0;
  while(input_file_first){
    std::getline(input_file_first, line);
    judge_Y.push_back(stod(line.substr(0)));
  }
  input_file_first.close();
  ROS_INFO_STREAM("Read judge_Y file success");

  std::string gcode_in, gcode_out;
  node_handle.param("gcode_in", gcode_in, std::string("/gcode_in"));
  std::ifstream input_file(gcode_in);
  if(!input_file.is_open())ROS_ERROR_STREAM("Can't open " <<gcode_in);

  node_handle.param("gcode_out", gcode_out, std::string("/gcode_out"));
  std::ofstream output_file(gcode_out);
  if(!output_file.is_open())ROS_ERROR_STREAM("Can't open " <<gcode_out);

  bool check = 0;
  bool first_point = 0;
  int error_num = 0;
  int second_execution = 0;
  all_line = 0;

  std::vector<KDL::Vector> find_end_effector_target_vol;
  find_end_effector_target_vol.reserve(1000);
  KDL::Vector previous_end_effector_target_vol;
  std::vector<int> save_place;
  save_place.reserve(1000);
  std::vector<std::string> save;
  save.reserve(1000);
  std::vector<int> save_p_or_n;
  save_p_or_n.reserve(1000);
  int previous_save_p_or_n;
  KDL::Vector end_effector_target_vol;
  while(ros::ok()){
    while(input_file){
      std::getline(input_file, line);
      save.push_back(line);
      all_line++;
      if(all_line % 1000 == 0 || !input_file){
        for(int i = 0;i < all_line ;i++){
          line = save[i];
          if(!line.compare(0,8,";LAYER:0")){
            check = 1;
          }
          if(check == 1){
            if(!line.compare(0,2,"G0") || !line.compare(0,2,"G1")){
              size_t colon_pos_X = line.find('X');
              if(colon_pos_X < 100){
                end_effector_target_vol.data[0] = (stod(line.substr(colon_pos_X+1))*1e-3)+X_offset;
              }
              size_t colon_pos_Y = line.find('Y');
              if(colon_pos_Y < 100){
                end_effector_target_vol.data[1] = (stod(line.substr(colon_pos_Y+1))*1e-3)+Y_offset;
              }
              size_t colon_pos_Z = line.find('Z');
              if(colon_pos_Z < 100){
                end_effector_target_vol.data[2] = (stod(line.substr(colon_pos_Z+1))*1e-3)+Z_offset;
              }
              find_end_effector_target_vol.push_back(end_effector_target_vol);
              save_place.push_back(i);
              if(end_effector_target_vol.data[1]*100 <= 25.2){
                if(end_effector_target_vol.data[1]*100 <= sqrt(pow(judge_Y[int((end_effector_target_vol.data[2]+0.085)*1000000)],2)-pow(end_effector_target_vol.data[0]*100,2))){
                  save_p_or_n.push_back(0);
                }
                else{
                  save_p_or_n.push_back(1);
                }
              }
              else{
                save_p_or_n.push_back(1);
              }
            }
          }
        }
        std::vector<KDL::JntArray> save_result(1000);
        std::vector<int> save_use_onecore(1000);
        omp_set_num_threads(num_threads);
        #pragma omp parallel for
        for(int j = 0;j < save_place.size();j++){
          int rc = -1;
          int thread_num = omp_get_thread_num();
          KDL::JntArray result;
          KDL::Rotation end_effector_target_rot;
          KDL::Vector target_bounds_rot(0, 0, M_PI*45/180);
          KDL::Vector target_bounds_vel(0,0,0);
          if(save_p_or_n[j] == 0){
            end_effector_target_rot = KDL::Rotation::Quaternion(0,0,1,0);
          }
          else{
            end_effector_target_rot = KDL::Rotation::Quaternion(0,0,0,1);
          }
          const KDL::Twist target_bounds(target_bounds_vel, target_bounds_rot);
          KDL::Frame end_effector_pose(end_effector_target_rot, find_end_effector_target_vol[j]);
          rc = tracik_solver[thread_num]->CartToJnt(nominal, end_effector_pose, result, target_bounds);
          if(rc < 0){
            save_use_onecore.at(j) = 1;
          }
          else{
            save_result.at(j) = result;
            save_use_onecore.at(j) = 0;
          }
        }
        std::vector<int> use_onecore;
        use_onecore.reserve(1000);
        for(int k = 0;k < 1000;k++){
          if(save_use_onecore[k] == 1){
            use_onecore.push_back(k);
          }
        }
        save_use_onecore.clear();
        for(int l = 0;l < use_onecore.size();l++){
          int rc = -1;
          second_execution++;
          KDL::JntArray result;
          KDL::Rotation end_effector_target_rot;
          KDL::Vector target_bounds_rot(0, 0, M_PI*45/180);
          KDL::Vector target_bounds_vel(0,0,0);
          if(save_p_or_n[use_onecore[l]] == 0){
            end_effector_target_rot = KDL::Rotation::Quaternion(0,0,1,0);
          }
          else{
            end_effector_target_rot = KDL::Rotation::Quaternion(0,0,0,1);
          }
          const KDL::Twist target_bounds(target_bounds_vel, target_bounds_rot);
          KDL::Frame end_effector_pose(end_effector_target_rot, find_end_effector_target_vol[use_onecore[l]]);
          rc = tracik_solver_onecore.CartToJnt(nominal, end_effector_pose, result, target_bounds);
          if(rc < 0){
            output_file << std::endl << "ERROR_TRACIK" << std::endl;
            output_file << "X:" << find_end_effector_target_vol[use_onecore[l]].data[0];
            output_file << "Y:" << find_end_effector_target_vol[use_onecore[l]].data[1];
            output_file << "Z:" << find_end_effector_target_vol[use_onecore[l]].data[2];
            output_file << std::endl << "END" << std::endl;
            ROS_ERROR_STREAM("TRACIK_ERROR");
            ROS_ERROR_STREAM("save_p_or_n:" << save_p_or_n[use_onecore[l]]);
            error_num++;
            save_result.at(use_onecore[l]) = result;
          }
          else{
            save_result.at(use_onecore[l]) = result;
          }
        }
        use_onecore.clear();
        int pop_out = 0;
        for(int m = 0;m < all_line ;m++){
          line = save[m];
          if((save_place[pop_out] == m) && (pop_out < save_place.size())){
            output_file << line[0] << line[1];
            std::vector<double> joint_values(chain.getNrOfJoints());
            for(int i = 0;i < chain.getNrOfJoints(); i++){
              joint_values.at(i) = save_result.at(pop_out).data(i);
            }  
            const robot_model::JointModelGroup* joint_model_group = current_state.getJointModelGroup("arm");
            current_state.setJointGroupPositions(joint_model_group, joint_values);
            collision_request.contacts = true;
            collision_request.max_contacts = 1000;
            collision_result.clear();
            planning_scene.checkSelfCollision(collision_request, collision_result);
            if(collision_result.collision == 0){
              motor_setep_convert(save_result.at(pop_out).data);
              bool need_change = 0;
              if(pop_out > 0){
                previous_save_p_or_n = save_p_or_n[pop_out-1];
                previous_end_effector_target_vol = find_end_effector_target_vol[pop_out-1];
              }
              if(first_point == 1 && (previous_save_p_or_n != save_p_or_n[pop_out])){
                int rc = -1;
                KDL::JntArray result;
                KDL::Rotation end_effector_target_rot;
                KDL::Vector target_bounds_rot(0, 0, M_PI*45/180);
                KDL::Vector target_bounds_vel(0,0,0);
                end_effector_target_rot = KDL::Rotation::Quaternion(0,0,1,0);
                const KDL::Twist target_bounds(target_bounds_vel, target_bounds_rot);
                KDL::Vector output_end_effector_target_vol;
                if(save_p_or_n[pop_out] == 0){
                  output_end_effector_target_vol = previous_end_effector_target_vol;
                }
                else{
                  output_end_effector_target_vol = find_end_effector_target_vol[pop_out];
                }
                KDL::Frame end_effector_pose(end_effector_target_rot, output_end_effector_target_vol);
                rc = tracik_solver_p_n.CartToJnt(nominal, end_effector_pose, result, target_bounds);
                if(rc < 0){
                  output_file << std::endl << "ERROR_TRACIK_NOW" << std::endl;
                  output_file << "X:" << output_end_effector_target_vol.data[0];
                  output_file << "Y:" << output_end_effector_target_vol.data[1];
                  output_file << "Z:" << output_end_effector_target_vol.data[2];
                  output_file << std::endl << "END" << std::endl;
                  ROS_ERROR_STREAM("TRACIK_ERROR_NOW");
                  ROS_ERROR_STREAM("save_p_or_n:" << save_p_or_n[pop_out]);
                  error_num++;
                }
                else{
                  motor_setep_convert(result.data);
                  for(int i = 0;i < chain.getNrOfJoints(); i++){
                    static const char joint_code[6] = {'J', 'A', 'B', 'C', 'D'};
                    output_file << " " << joint_code[i] << int(result.data(i));
                  }
                  if(save_p_or_n[pop_out] == 0){
                    double decimal_x = (output_end_effector_target_vol.data[0]-X_offset)*1e3;
                    double decimal_y = (output_end_effector_target_vol.data[1]-Y_offset)*1e3;
                    output_file << std::fixed << std::setprecision(decimal_point(decimal_x)) << " X" << (output_end_effector_target_vol.data[0]-X_offset)*1e3 << std::defaultfloat;
                    output_file << std::fixed << std::setprecision(decimal_point(decimal_y)) << " Y" << (output_end_effector_target_vol.data[1]-Y_offset)*1e3 << std::defaultfloat;
                  }
                  else{
                    for(int j = 2;j < line.length(); j++){
                      output_file << line[j];
                    }
                  }
                  output_file << std::endl;
                  output_file << line[0] << line[1];
                  for(int i = 0;i < chain.getNrOfJoints(); i++){
                    static const char joint_code[6] = {'J', 'A', 'B', 'C', 'D'};
                    output_file << " " << joint_code[i] << int(save_result.at(pop_out).data(i));
                  }
                  if(save_p_or_n[pop_out] == 0){
                    for(int j = 2;j < line.length(); j++){
                      output_file << line[j];
                    }
                  }
                  else{
                    double decimal_x = (output_end_effector_target_vol.data[0]-X_offset)*1e3;
                    double decimal_y = (output_end_effector_target_vol.data[1]-Y_offset)*1e3;
                    output_file << std::fixed << std::setprecision(decimal_point(decimal_x)) << " X" << (output_end_effector_target_vol.data[0]-X_offset)*1e3 << std::defaultfloat;
                    output_file << std::fixed << std::setprecision(decimal_point(decimal_y)) << " Y" << (output_end_effector_target_vol.data[1]-Y_offset)*1e3 << std::defaultfloat;
                  }
                  output_file << std::endl;
                }
              }
              else{
                for(int i = 0;i < chain.getNrOfJoints(); i++){
                  static const char joint_code[6] = {'J', 'A', 'B', 'C', 'D'};
                  output_file << " " << joint_code[i] << int(save_result.at(pop_out).data(i));
                }
                for(int j = 2;j < line.length(); j++){
                  output_file << line[j];
                }
                output_file << std::endl;
              }
              need_change = 0;
              pop_out++;
              first_point = 1;
            }
            else{
              collision_detection::CollisionResult::ContactMap::const_iterator it;
              for (it = collision_result.contacts.begin(); it != collision_result.contacts.end(); ++it){
                ROS_ERROR("Contact between: %s and %s", it->first.first.c_str(), it->first.second.c_str());
              }
              motor_setep_convert(save_result.at(pop_out).data);
              output_file << std::endl << "ERROR_COLLISION" << std::endl;
              output_file << "J:" << (save_result.at(pop_out).data(0))<< std::endl;
              output_file << "A:" << (save_result.at(pop_out).data(1))<< std::endl;
              output_file << "B:" << (save_result.at(pop_out).data(2))<< std::endl;
              output_file << "C:" << (save_result.at(pop_out).data(3))<< std::endl;
              output_file << "D:" << (save_result.at(pop_out).data(4))<< std::endl;
              output_file << std::endl << "END" << std::endl;
              error_num++;
            }
          }
          else{
            output_file << line << std::endl;
          }
        }
        previous_save_p_or_n = save_p_or_n[pop_out-1];
        previous_end_effector_target_vol = find_end_effector_target_vol[pop_out-1];
        find_end_effector_target_vol.clear();
        save_p_or_n.clear();
        save.clear();
        save_place.clear();
        save_result.clear();
        all_line = 0;
      }
    }
    for(int i = 0; i < num_threads; i++){
      delete tracik_solver[i];
    }
    delete tracik_solver;
    end_ = ros::WallTime::now();
    double execution_time = (end_ - start_).toNSec() * 1e-9;
    ROS_INFO_STREAM("second_execution: " << second_execution);
    ROS_INFO_STREAM("can't exectution:" << error_num);
    ROS_INFO_STREAM("Exectution time (s): " << execution_time);
    input_file.close();
    output_file.close();
    ros::shutdown();
  }
  return 0;
}
