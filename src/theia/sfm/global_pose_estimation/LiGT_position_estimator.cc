// Copyright (C) 2015 The Regents of the University of California (Regents).
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
// Author: Chris Sweeney (cmsweeney@cs.ucsb.edu)

// Author: Steffen Urban (steffen.urban@googlemail.com), March 2021

#include "theia/sfm/global_pose_estimation/LiGT_position_estimator.h"

#include <Eigen/Core>
#include <Eigen/SparseCore>
#include <Eigen/SparseLU>
#include <Eigen/Geometry>
#include <Eigen/Dense>
#include <algorithm>
#include <ceres/rotation.h>
#include <glog/logging.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "spectra/include/SymEigsShiftSolver.h"

#include "theia/math/graph/triplet_extractor.h"
#include "theia/math/matrix/spectra_linear_operator.h"
#include "theia/sfm/find_common_tracks_in_views.h"
#include "theia/sfm/global_pose_estimation/compute_triplet_baseline_ratios.h"
#include "theia/sfm/reconstruction.h"
#include "theia/sfm/types.h"
#include "theia/sfm/view_triplet.h"
#include "theia/util/map_util.h"
#include "theia/util/threadpool.h"

namespace theia {

using Eigen::Matrix3d;
using Eigen::Vector3d;

namespace {

Eigen::Matrix3d GetSkew(const Eigen::Vector3d& f) {
  Eigen::Matrix3d skew_mat;
  skew_mat << 0.0, -f(2), f(1), f(2), 0.0, -f(0), -f(1), f(0), 0.0;
  return skew_mat;
}

Eigen::Matrix3d GetRij(const Eigen::Matrix3d& i, const Eigen::Matrix3d& j) {
  return j * i.transpose();
}

double GetThetaSq(const Eigen::Vector3d& feat_i,
                  const Eigen::Vector3d& feat_j,
                  const Eigen::Matrix3d& Rij) {
  return (GetSkew(feat_j) * Rij * feat_i).squaredNorm();
}

Eigen::Vector3d Get_aij(const Eigen::Matrix3d& Rij,
                        const Eigen::Vector3d Xi,
                        const Eigen::Vector3d Xj) {
  return (GetSkew(Rij * Xi) * Xj).transpose() * GetSkew(Xj);
}

// Adds the constraint from the triplet to the symmetric matrix. Our standard
// constraint matrix A is a 3M x 3N matrix with M triplet constraints and N
// cameras. We seek to construct A^t * A directly. For each triplet constraint
// in our matrix A (i.e. a 3-row block), we can compute the corresponding
// entries in A^t * A with the following summation:
//
//   A^t * A += Row(i)^t * Row(i)
//
// for each triplet constraint i.
void AddTripletConstraintToSymmetricMatrix(
    const std::vector<Matrix3d>& constraints,
    const std::vector<int>& view_indices,
    std::unordered_map<std::pair<int, int>, double>* sparse_matrix_entries) {
  // Construct Row(i)^t * Row(i). If we denote the row as a block matrix:
  //
  //   Row(i) = [A | B | C]
  //
  // then we have:
  //
  //   Row(i)^t * Row(i) = [A | B | C]^t * [A | B | C]
  //                     = [ A^t * A  |  A^t * B  |  A^t * C]
  //                       [ B^t * A  |  B^t * B  |  B^t * C]
  //                       [ C^t * A  |  C^t * B  |  C^t * C]
  //
  // Since A^t * A is symmetric, we only store the upper triangular portion.
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      // Skip any block entries that correspond to the lower triangular portion
      // of the matrix.
      if (view_indices[i] > view_indices[j]) {
        continue;
      }

      // Compute the A^t * B, etc. matrix.
      const Eigen::Matrix3d symmetric_constraint =
          constraints[i].transpose() * constraints[j];

      // Add to the 3x3 block corresponding to (i, j)
      for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
          const std::pair<int, int> row_col(view_indices[i] + r,
                                            view_indices[j] + c);
          (*sparse_matrix_entries)[row_col] += symmetric_constraint(r, c);
        }
      }
    }
  }
}

inline Matrix3d AngleAxisToRotationMatrix(const Vector3d angle_axis) {
  const double angle = angle_axis.norm();
  const Eigen::AngleAxisd rotation_aa(angle, angle_axis / angle);
  return rotation_aa.toRotationMatrix();
}

// Returns true if the vector R1 * (c2 - c1) is in the same direction as t_12.
bool VectorsAreSameDirection(const Vector3d& position1,
                             const Vector3d& position2,
                             const Vector3d& rotation1,
                             const Vector3d& relative_position12) {
  const Vector3d global_relative_position =
      (position2 - position1).normalized();
  Vector3d rotated_relative_position;
  ceres::AngleAxisRotatePoint(rotation1.data(),
                              global_relative_position.data(),
                              rotated_relative_position.data());
  return rotated_relative_position.dot(relative_position12) > 0;
}

// Returns the features as a unit-norm pixel ray after camera intrinsics
// (i.e. focal length an principal point) have been removed.
Feature GetNormalizedFeature(const View& view, const TrackId track_id) {
  Feature feature = *view.GetFeature(track_id);
  const Camera& camera = view.Camera();
  Eigen::Vector3d ray = camera.PixelToNormalizedCoordinates(feature.point_);
  Feature normalized_Feature(ray.hnormalized());
  // todo normalized covariance?
  return normalized_Feature;
}

std::pair<ViewId, ViewId> GetBestBaseViews(
    const Reconstruction& reconstruction,
    const TrackId& track_id) {

    const Track* track = reconstruction.Track(track_id);
    std::vector<ViewId> view_ids(track->ViewIds().begin(), track->ViewIds().end());
    double theta_max = 0.0;
    std::pair<ViewId, ViewId> base_views;
    for (size_t i = 0; i < view_ids.size(); ++i) {
      for (size_t j = i+1; j < view_ids.size(); ++j) {
        ViewId id1 = view_ids[i];
        ViewId id2 = view_ids[j];
        const View* view1 = reconstruction.View(id1);
        const View* view2 = reconstruction.View(id2);
        const Vector3d feature1 =
            GetNormalizedFeature(*view1, track_id).point_.homogeneous();
        const Vector3d feature2 =
            GetNormalizedFeature(*view2, track_id).point_.homogeneous();

        const Eigen::Matrix3d R1 = view1->Camera().GetOrientationAsRotationMatrix();
        const Eigen::Matrix3d R2 = view2->Camera().GetOrientationAsRotationMatrix();
        const Matrix3d R12 = GetRij(R1, R2);
        const double theta = GetThetaSq(feature1, feature2, R12);
        if (theta > theta_max) {
            base_views = std::make_pair(id1, id2);
            theta_max = theta;
        }
      }
    }
    return base_views;
}

}  // namespace

LiGTPositionEstimator::LiGTPositionEstimator(
    const Options& options, const Reconstruction& reconstruction)
    : options_(options), reconstruction_(reconstruction) {
  CHECK_GT(options.num_threads, 0);
}

bool LiGTPositionEstimator::EstimatePositions(
    const std::unordered_map<ViewIdPair, TwoViewInfo>& view_pairs,
    const std::unordered_map<ViewId, Vector3d>& orientations,
    std::unordered_map<ViewId, Vector3d>* positions) {
  CHECK_NOTNULL(positions)->clear();

  num_triplets_for_view_.clear();
  linear_system_index_.clear();
  BCDs_.clear();
  triplets_for_tracks_.clear();

  view_pairs_ = &view_pairs;
  orientations_ = &orientations;

  VLOG(2) << "Extracting triplets from tracks and calculating BCDs for tracks.";
  FindTripletsForTracks();

//  VLOG(2) << "Calculating BCD for tracks.";
//  for (const auto& t : triplets_for_tracks_) {
//    auto t_id = t.first;
//    auto view_ids = t.second;
//    for (const auto& vids : view_ids) {
//      const auto view1 = reconstruction_.View(std::get<0>(vids));
//      const auto view2 = reconstruction_.View(std::get<1>(vids));
//      const auto view3 = reconstruction_.View(std::get<2>(vids));
//      std::tuple<Matrix3d, Matrix3d, Matrix3d> BCD;
//      CalculateBCDForTrack(view1, view2, view3, t_id, BCD);

////      Eigen::Vector3d shouldnull = std::get<1>(BCD)*view1->Camera().GetPosition() +
////              std::get<0>(BCD)*view2->Camera().GetPosition() +
////              std::get<2>(BCD)*view3->Camera().GetPosition();
////      std::cout<<"shouldnull: "<<shouldnull<<"\n";

//      BCDs_[t_id].push_back(BCD);
//    }
//  }

  VLOG(2) << "Building the constraint matrix...";
  // Create the linear system based on triplet constraints.
  Eigen::SparseMatrix<double> constraint_matrix;
  CreateLinearSystem(&constraint_matrix);

  // Solve for positions by examining the smallest eigenvalues. Since we have
  // set one position constant at the origin, we only need to solve for the
  // eigenvector corresponding to the smallest eigenvalue. This can be done
  // efficiently with inverse power iterations.
  VLOG(2) << "Solving for positions from the sparse eigenvalue problem...";
  SparseSymShiftSolveLLT op(constraint_matrix);
  Spectra::SymEigsShiftSolver<double, Spectra::LARGEST_MAGN,
                              SparseSymShiftSolveLLT>
  eigs(&op, 1, 6, 0.0);
  eigs.init();
  eigs.compute();

  // Compute with power iterations.
  const Eigen::VectorXd solution = eigs.eigenvectors().col(0);
  // Add the solutions to the output. Set the position with an index of -1 to
  // be at the origin.
  for (const auto &view_index : linear_system_index_) {
    if (view_index.second < 0) {
      (*positions)[view_index.first].setZero();
    } else {
      (*positions)[view_index.first] =
          solution.segment<3>(view_index.second * 3);
    }
  }

  return true;
}

void LiGTPositionEstimator::CalculateBCDForTrack(
    const theia::View* view1,
    const theia::View* view2,
    const theia::View* view3,
    const TrackId& track_id,
    std::tuple<Eigen::Matrix3d, Eigen::Matrix3d, Eigen::Matrix3d>& BCD) {
  const Vector3d feature1 =
      GetNormalizedFeature(*view1, track_id).point_.homogeneous();
  const Vector3d feature2 =
      GetNormalizedFeature(*view2, track_id).point_.homogeneous();
  const Vector3d feature3 =
      GetNormalizedFeature(*view3, track_id).point_.homogeneous();

  const Eigen::Matrix3d R1 = view1->Camera().GetOrientationAsRotationMatrix();
  const Eigen::Matrix3d R2 = view2->Camera().GetOrientationAsRotationMatrix();
  const Eigen::Matrix3d R3 = view3->Camera().GetOrientationAsRotationMatrix();

  const Matrix3d R31 = GetRij(R3, R1);
  const Matrix3d R32 = GetRij(R3, R2);

  const Vector3d a32 = Get_aij(R32, feature3, feature2);

  const Matrix3d skew_feat1 = GetSkew(feature1);
  // equation 18
  std::get<0>(BCD) = skew_feat1 * R31 * feature3 * a32.transpose() * R2;

  const double theta = GetThetaSq(feature3, feature2, R32);
  std::get<1>(BCD) = theta * skew_feat1 * R1;

  std::get<2>(BCD) = -(std::get<0>(BCD) + std::get<1>(BCD));
}

void LiGTPositionEstimator::FindTripletsForTracks() {
  auto track_ids = reconstruction_.TrackIds();
  uint32_t total_nr_triplets = 0;
  for (size_t t = 0; t < track_ids.size(); ++t) {
    auto t_id = track_ids[t];
    //std::cout<<"searching triplet for "<<t_id<<"\n";

    auto view_ids_for_track = reconstruction_.Track(t_id)->ViewIds();
    //std::cout<<"view_ids_for_track size: "<<view_ids_for_track.size()<<"\n";
    if (view_ids_for_track.size() < 3) {
      continue;
    }
    // implements equation 29 from paper. Get base views for point
    std::pair<ViewId, ViewId> base_views = GetBestBaseViews(reconstruction_, t_id);
    // now iterate all other observations beside the base views
    for (size_t v = 0; v < view_ids_for_track.size(); ++v) {
      ViewId cur_id = *std::next(view_ids_for_track.begin(), v);
      // check if the current id is one of the base views
      if (cur_id == base_views.first || cur_id == base_views.second) {
          continue;
      }
      //std::cout<<"Track: "<<t_id<<" triplet: (base l, central, base r) ("<<base_views.first<<", "<<cur_id<<", "<<base_views.second<<")\n";

      ViewIdTriplet triplet = std::make_tuple(base_views.first, cur_id, base_views.second);
      AddTripletConstraint(triplet);

      const auto view1 = reconstruction_.View(base_views.first);
      const auto view2 = reconstruction_.View(cur_id);
      const auto view3 = reconstruction_.View(base_views.second);
      std::tuple<Matrix3d, Matrix3d, Matrix3d> BCD;
      CalculateBCDForTrack(view1, view2, view3, t_id, BCD);

//      Eigen::Vector3d shouldbezero = std::get<1>(BCD)*view1->Camera().GetPosition() +
//              std::get<0>(BCD)*view2->Camera().GetPosition() +
//              std::get<2>(BCD)*view3->Camera().GetPosition();
//      std::cout<<"shouldbezero: "<<shouldbezero<<"\n";

      triplets_for_tracks_[t_id].push_back(triplet);
      BCDs_[t_id].push_back(BCD);
      total_nr_triplets++;
    }
  }

  std::cout<<"Total number of triplets: "<<total_nr_triplets<<" for "<< track_ids.size()<<" tracks and "<<reconstruction_.ViewIds().size()<<" views.\n";
}

// An alternative interface is to instead add triplets one by one to linear
// estimator. This allows for adding redundant observations of triplets, which
// may be useful if there are multiple estimates of the data.
void LiGTPositionEstimator::AddTripletConstraint(
    const ViewIdTriplet& view_triplet) {
  num_triplets_for_view_[std::get<0>(view_triplet)] += 1;
  num_triplets_for_view_[std::get<1>(view_triplet)] += 1;
  num_triplets_for_view_[std::get<2>(view_triplet)] += 1;

  // Determine the order of the views in the linear system. We subtract 1 from
  // the linear system index so that the first position added to the system
  // will be set constant (index of -1 is intentionally not evaluated later).
  InsertIfNotPresent(&linear_system_index_,
                     std::get<0>(view_triplet),
                     linear_system_index_.size() - 1);
  InsertIfNotPresent(&linear_system_index_,
                     std::get<1>(view_triplet),
                     linear_system_index_.size() - 1);
  InsertIfNotPresent(&linear_system_index_,
                     std::get<2>(view_triplet),
                     linear_system_index_.size() - 1);
}

// Sets up the linear system with the constraints that each triplet adds.
void LiGTPositionEstimator::CreateLinearSystem(
    Eigen::SparseMatrix<double>* constraint_matrix) {
  const int num_views = num_triplets_for_view_.size();

    std::unordered_map<std::pair<int, int>, double> sparse_matrix_entries;
    sparse_matrix_entries.reserve(27 * num_triplets_for_view_.size());
    for (const auto& triplet_vector : triplets_for_tracks_) {
        const TrackId t_id = triplet_vector.first;
        const std::vector<ViewIdTriplet> triplet_v = triplet_vector.second;
        for (size_t i = 0; i < triplet_v.size(); ++i) {
            const ViewId &view_id1 = std::get<0>(triplet_v[i]);
            const ViewId &view_id2 = std::get<1>(triplet_v[i]);
            const ViewId &view_id3 = std::get<2>(triplet_v[i]);
            AddTripletConstraintToSparseMatrix(view_id1, view_id2, view_id3,
                                               BCDs_[t_id][i],
                                               &sparse_matrix_entries);

        }
    }

    // Set the sparse matrix from the container of the accumulated entries.
    std::vector<Eigen::Triplet<double>> triplet_list;
    triplet_list.reserve(sparse_matrix_entries.size());
    for (const auto &sparse_matrix_entry : sparse_matrix_entries) {
      // Skip this entry if the indices are invalid. This only occurs when we
      // encounter a constraint with the constant camera (which has a view index
      // of -1).
      if (sparse_matrix_entry.first.first < 0 ||
          sparse_matrix_entry.first.second < 0) {
        continue;
      }
      triplet_list.emplace_back(sparse_matrix_entry.first.first,
                                sparse_matrix_entry.first.second,
                                sparse_matrix_entry.second);
    }

    // We construct the constraint matrix A^t * A directly, which is an
    // N - 1 x N - 1 matrix where N is the number of cameras (and 3 entries per
    // camera, corresponding to the camera position entries).

    constraint_matrix->resize((num_views - 1) * 3, (num_views - 1) * 3);
    constraint_matrix->setFromTriplets(triplet_list.begin(),
    triplet_list.end());
}

void LiGTPositionEstimator::ComputeRotatedRelativeTranslationRotations(
    const ViewId view_id0,
    const ViewId view_id1,
    const ViewId view_id2,
    Eigen::Matrix3d* r012,
    Eigen::Matrix3d* r201,
    Eigen::Matrix3d* r120) {
  // Relative camera positions.
  const Eigen::Vector3d& orientation0_aa =
      FindOrDieNoPrint(*orientations_, view_id0);
  const Eigen::Vector3d& orientation1_aa =
      FindOrDieNoPrint(*orientations_, view_id1);
  const Matrix3d orientation0 = AngleAxisToRotationMatrix(orientation0_aa);
  const Matrix3d orientation1 = AngleAxisToRotationMatrix(orientation1_aa);
  const Vector3d t01 =
      -orientation0.transpose() *
      FindOrDieNoPrint(*view_pairs_, ViewIdPair(view_id0, view_id1)).position_2;
  const Vector3d t02 =
      -orientation0.transpose() *
      FindOrDieNoPrint(*view_pairs_, ViewIdPair(view_id0, view_id2)).position_2;
  const Vector3d t12 =
      -orientation1.transpose() *
      FindOrDieNoPrint(*view_pairs_, ViewIdPair(view_id1, view_id2)).position_2;

  // Rotations between the translation vectors.
  *r012 = Eigen::Quaterniond::FromTwoVectors(t12, -t01).toRotationMatrix();
  *r201 = Eigen::Quaterniond::FromTwoVectors(t01, t02).toRotationMatrix();
  *r120 = Eigen::Quaterniond::FromTwoVectors(-t02, -t12).toRotationMatrix();
}

// Adds a triplet constraint to the linear system. The weight of the constraint
// (w), the global orientations, baseline (ratios), and view triplet information
// are needed to form the constraint.
void LiGTPositionEstimator::AddTripletConstraintToSparseMatrix(
    const ViewId view_id0,
    const ViewId view_id1,
    const ViewId view_id2,
    const std::tuple<Matrix3d, Matrix3d, Matrix3d>& BCD,
    std::unordered_map<std::pair<int, int>, double>* sparse_matrix_entries) {
//  // Weight each term by the inverse of the # of triplet that the nodes
//  // participate in.
//  const double w =
//      1.0 / std::sqrt(std::min({num_triplets_for_view_[view_id0],
//                                num_triplets_for_view_[view_id1],
//                                num_triplets_for_view_[view_id2]}));

  // Get the index of each camera in the sparse matrix.
  const std::vector<int> view_indices = {
      static_cast<int>(3 * FindOrDie(linear_system_index_, view_id0)),
      static_cast<int>(3 * FindOrDie(linear_system_index_, view_id1)),
      static_cast<int>(3 * FindOrDie(linear_system_index_, view_id2))};

  // important to use index 1 0 2 here.
  // 0 ist the central camera and equation 17 is using it that way
  std::vector<Matrix3d> constraints = {std::get<1>(BCD),std::get<0>(BCD),std::get<2>(BCD)};
  AddTripletConstraintToSymmetricMatrix(
      constraints, view_indices, sparse_matrix_entries);
}

void LiGTPositionEstimator::FlipSignOfPositionsIfNecessary(
    std::unordered_map<ViewId, Vector3d>* positions) {
  // If this value is below zero, then we should flip the sign.
  int correct_sign_votes = 0;
  for (const auto& view_pair : *view_pairs_) {
    // Only count the votes for edges where both positions were successfully
    // estimated.
    const Vector3d* position1 = FindOrNull(*positions, view_pair.first.first);
    const Vector3d* position2 = FindOrNull(*positions, view_pair.first.second);
    if (position1 == nullptr || position2 == nullptr) {
      continue;
    }

    // Check the relative translation of views 1 and 2 in the triplet.
    if (VectorsAreSameDirection(
            *position1,
            *position2,
            FindOrDieNoPrint(*orientations_, view_pair.first.first),
            view_pair.second.position_2)) {
      correct_sign_votes += 1;
    } else {
      correct_sign_votes -= 1;
    }
  }

  // If the sign of the votes is below zero, we must flip the sign of all
  // position estimates.
  if (correct_sign_votes < 0) {
    const int num_correct_votes =
        (view_pairs_->size() + correct_sign_votes) / 2;
    VLOG(2) << "Sign of the positions was incorrect: " << num_correct_votes
            << " of " << view_pairs_->size()
            << " relative translations had the correct sign. "
               "Flipping the sign of the camera positions.";
    for (auto& position : *positions) {
      position.second *= -1.0;
    }
  }
}

std::unordered_map<ViewId, Eigen::Vector3d>
LiGTPositionEstimator::EstimatePositionsWrapper(
    const std::unordered_map<ViewIdPair, TwoViewInfo>& view_pairs,
    const std::unordered_map<ViewId, Eigen::Vector3d>& orientation) {
  std::unordered_map<ViewId, Eigen::Vector3d> positions;
  EstimatePositions(view_pairs, orientation, &positions);
  return positions;
}

}  // namespace theia
