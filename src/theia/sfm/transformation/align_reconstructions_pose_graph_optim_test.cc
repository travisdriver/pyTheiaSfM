// Copyright (C) 2023 Steffen Urban
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//
//     * Neither the name of The Regents or University of California nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Please contact the author of this library if you have any questions.
// Author: Steffen Urban (urbste@googlemail.com)

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <glog/logging.h>
#include <string>
#include <utility>
#include <vector>
#include <Sophus/sophus/se3.hpp>
#include <Sophus/sophus/sim3.hpp>
#include "gtest/gtest.h"

#include <ceres/local_parameterization.h>
#include <ceres/solver.h>

#include "theia/sfm/reconstruction.h"
#include "theia/sfm/transformation/align_reconstructions.h"
#include "theia/sfm/transformation/transform_reconstruction.h"
#include "theia/sfm/types.h"
#include "theia/util/random.h"
#include "theia/util/stringprintf.h"
#include "theia/io/reconstruction_reader.h"
#include "theia/io/reconstruction_writer.h"
#include "theia/io/write_ply_file.h"
#include "theia/sfm/view_graph/view_graph.h"
#include "theia/sfm/view_graph/view_graph_from_reconstruction.h"
#include "theia/sfm/transformation/align_reconstructions_pose_graph_optim.h"
#include "theia/sfm/camera/create_reprojection_error_cost_function.h"
#include "theia/sfm/transformation/align_reconstructions.h"


namespace theia {


TEST(AlignReconstructionPoseGraphOptim, Test) {
  Reconstruction recon_qry;
  Reconstruction recon_qry_in_ref;
  
  std::string base_path = "/home/steffen/Data/GPStrava/Muehltal/MilowsClaw";
  theia::ReadReconstruction(base_path+"/run2_recs/theia_recon_0.recon", &recon_qry);  
  theia::ReadReconstruction(base_path+"/run2_recs/run2_in_run1.recon", &recon_qry_in_ref);  
  

  // first do a general transformation of traj 2 to 1 
  AlignReconstructionsRobust(1.0, recon_qry_in_ref, &recon_qry);

  // // calculate inverse depth from depth
  // for (const auto& track_id : recon_qry.TrackIds()) {
  //   Track* track = CHECK_NOTNULL(recon_qry.MutableTrack(track_id));
  //   if (track == nullptr || !track->IsEstimated()) {
  //     continue;
  //   }
  //   std::cout<<"Old inv depth: "<<track->InverseDepth()<<std::endl;
  //   const View* ref_view = recon_qry.View(track->ReferenceViewId());
  //   Eigen::Vector2d pt;
  //   const double d = ref_view->Camera().ProjectPoint(track->Point(), &pt);
  //   track->SetInverseDepth(1/d);
  //   std::cout<<"New inv depth: "<<track->InverseDepth()<<std::endl;

  // }

  // ViewGraph view_graph;
  // ViewGraphFromReconstruction(recon_qry, 0, &view_graph);

   ceres::Problem problem;
   ceres::LocalParameterization* sim3_local_parameterization =
       new Sim3Parameterization;

  // get all transformations
  std::map<ViewId, Eigen::Matrix<double, 7, 1>> sim3s;
  for (const auto& view_id : recon_qry.ViewIds()) {
    Eigen::Matrix3d R_c_w = recon_qry.View(view_id)->Camera().GetOrientationAsRotationMatrix();
    Eigen::Vector3d t_c_w = -R_c_w*recon_qry.View(view_id)->Camera().GetPosition();
    Sophus::Sim3d sim3(Sophus::RxSO3d(1.0, R_c_w), t_c_w);
    sim3s[view_id] = sim3.log();
  }

  // // //  create sim3 relative tranformations
  // // for (const auto& edge : view_graph.GetAllEdges()) {
  // //   const ViewId view_id1 = edge.first.first;
  // //   const ViewId view_id2 = edge.first.second;
  // //   std::cout << "view_id1: " << view_id1 << " view_id2: " << view_id2 << std::endl;

  // //   Eigen::Matrix3d R_c_w_i = recon_qry.View(view_id1)->Camera().GetOrientationAsRotationMatrix();
  // //   Eigen::Vector3d t_c_w_i = -R_c_w_i*recon_qry.View(view_id1)->Camera().GetPosition();

  // //   Eigen::Matrix3d R_c_w_j = recon_qry.View(view_id2)->Camera().GetOrientationAsRotationMatrix();
  // //   Eigen::Vector3d t_c_w_j = -R_c_w_j*recon_qry.View(view_id2)->Camera().GetPosition();

  // //   Sophus::SE3d T_i_w(R_c_w_i, t_c_w_i);
  // //   Sophus::SE3d T_j_w(R_c_w_j, t_c_w_j);

  // //   Sophus::Sim3d Siw(Sophus::RxSO3d(1.0, T_i_w.rotationMatrix()), T_i_w.translation());
  // //   Sophus::Sim3d Sjw(Sophus::RxSO3d(1.0, T_j_w.rotationMatrix()), T_j_w.translation());

  // //   // relative transformation from camera 1 to camera 2
  // //   Sophus::SE3d Tji = T_j_w * T_i_w.inverse(); 
  // //   Sophus::Sim3d Sji(Sophus::RxSO3d(1.0, Tji.rotationMatrix()), Tji.translation());

  // //   ceres::CostFunction* cost_function =
  // //         SelfEdgesErrorTerm::Create(Sji, Eigen::Matrix<double, 7, 7>::Identity());
  // //   problem.AddResidualBlock(cost_function, nullptr, 
  // //       sim3s[view_id1].data(),
  // //       sim3s[view_id2].data());
  // // }


    // now Add edges to the reference reconstruction
    for (const auto& view_id : recon_qry_in_ref.ViewIds()) {
      const View* view_qry_in_ref = recon_qry_in_ref.View(view_id);
      const auto& name = view_qry_in_ref->Name();
      const ViewId vid_qry = recon_qry.ViewIdFromName(name);
      if (vid_qry == kInvalidViewId) {
          continue;
      }
      const View* view_in_qry = recon_qry.View(view_id);
      if (view_in_qry == nullptr || view_qry_in_ref == nullptr) {
          continue;
      } 

        // get pose from view_qrys_in_ref
        Eigen::Matrix3d R_c_w_ref = view_qry_in_ref->Camera().GetOrientationAsRotationMatrix();
        Eigen::Vector3d t_c_w_ref = -R_c_w_ref*view_qry_in_ref->Camera().GetPosition();

        Sophus::Sim3d S_r_w(Sophus::RxSO3d(1.0, R_c_w_ref), t_c_w_ref);

        Eigen::Matrix<double, 7, 7> inv_sqrt = Eigen::Matrix<double, 7, 7>::Identity();
        inv_sqrt.block<3,3>(0,0) = Eigen::Matrix3d::Identity()*100.;
        inv_sqrt.block<3,3>(3,3) = Eigen::Matrix3d::Identity()*100.;
        inv_sqrt.block<1,1>(6,6) = Eigen::Matrix<double, 1, 1>::Identity()*0.1;
        problem.AddResidualBlock(
            CrossEdgesErrorTerm::Create(S_r_w, inv_sqrt), 
            nullptr, sim3s[vid_qry].data());
    }

    // add reprojection residuals
    for (const auto& track_id : recon_qry.TrackIds()) {
      Track* track = CHECK_NOTNULL(recon_qry.MutableTrack(track_id));
      if (track == nullptr || !track->IsEstimated()) {
        continue;
      }
      
      const auto ref_view_id = track->ReferenceViewId();
      if (ref_view_id == kInvalidViewId) {
        continue;
      }
      View* ref_view = CHECK_NOTNULL(recon_qry.MutableView(ref_view_id));
      if (ref_view == nullptr) {
        continue;
      }
      Camera* ref_cam = ref_view->MutableCamera();
      const Eigen::Vector3d ref_bearing_vector = track->ReferenceBearingVector();

      const auto& observed_view_ids = track->ViewIds();
      for (const ViewId view_id : observed_view_ids) {
        View* view = CHECK_NOTNULL(recon_qry.MutableView(view_id));

        // Check if view is estimated and can now be optimized
        if (!view->IsEstimated()) {
          continue;
        }

        const Feature* feature = CHECK_NOTNULL(view->GetFeature(track_id));
        Camera* camera = view->MutableCamera();

        if (camera != ref_cam) {
            problem.AddResidualBlock(
            CreateSim3InvReprojectionErrorCostFunction(
                camera->GetCameraIntrinsicsModelType(), 
                *feature, ref_bearing_vector), 
            NULL,
            sim3s[ref_view_id].data(),
            sim3s[view_id].data(),
            camera->mutable_intrinsics(), 
            track->MutableInverseDepth());
        } else {
            problem.AddResidualBlock(
            CreateSim3InvReprojectionPoseErrorCostFunction(
                ref_cam->GetCameraIntrinsicsModelType(), 
                *feature, ref_bearing_vector), 
            NULL,
            sim3s[ref_view_id].data(),
            ref_cam->mutable_intrinsics(), 
            track->MutableInverseDepth());
        }
      }
    }



    ceres::Solver::Options solver_options;
    ceres::Solver::Summary solver_summary;
    solver_options.linear_solver_type = ceres::CGNR;
    solver_options.minimizer_progress_to_stdout = true;
    solver_options.max_num_iterations = 30;
    solver_options.num_threads = 20;
    ceres::Solve(solver_options, &problem, &solver_summary);

    // set sim3s to recon_qry
    for (const auto& view_id : recon_qry.ViewIds()) {
      Sophus::Sim3d sim3 = Sophus::Sim3d::exp(sim3s[view_id]);
      
      recon_qry.MutableView(view_id)->MutableCamera()->SetOrientationFromRotationMatrix(sim3.rotationMatrix());
      const Eigen::Vector3d translation = sim3.translation() / sim3.scale();
      recon_qry.MutableView(view_id)->MutableCamera()->SetPosition(-sim3.rotationMatrix().transpose() * translation);
    }

    theia::WriteReconstruction(recon_qry, base_path+"/run2_recs/run2_in_run1_sim3.recon");
    theia::WritePlyFile(base_path+"/run2_recs/run2_in_run1_sim3.ply",recon_qry, Eigen::Vector3i(255, 255,255), 2);
}

}  // namespace theia
