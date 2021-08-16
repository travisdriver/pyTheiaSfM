#include "theia/sfm/bundle_adjustment/bundle_adjustment_wrapper.h"


namespace theia {

std::tuple<BundleAdjustmentSummary, TwoViewInfo> BundleAdjustTwoViewsAngularWrapper(
    const BundleAdjustmentOptions& options,
    const std::vector<FeatureCorrespondence>& correspondences,
    TwoViewInfo& two_view_info_prior){
    BundleAdjustmentSummary ba_summary = BundleAdjustTwoViewsAngular(options, correspondences, &two_view_info_prior);
    return std::make_tuple(ba_summary, two_view_info_prior);
}

std::tuple<BundleAdjustmentSummary, Reconstruction> BundleAdjustViewWrapper(
    Reconstruction& reconstruction, const BundleAdjustmentOptions& options, const ViewId view_id){
    BundleAdjustmentSummary ba_summary = BundleAdjustView(options, view_id, &reconstruction);
    return std::make_tuple(ba_summary, reconstruction);
}


std::tuple<BundleAdjustmentSummary, Reconstruction> BundleAdjustTrackWrapper(
    Reconstruction& reconstruction, const BundleAdjustmentOptions& options,
        const TrackId track_id) {
    BundleAdjustmentSummary ba_summary = BundleAdjustTrack(options, track_id, &reconstruction);
    return std::make_tuple(ba_summary, reconstruction);
}

std::tuple<BundleAdjustmentSummary, Reconstruction, Matrix6d, double> 
          BundleAdjustViewWithCovWrapper(Reconstruction& reconstruction, const BundleAdjustmentOptions& options, const ViewId view_id) {
    Matrix6d cov_mat;
    double emp_variance_factor;
    BundleAdjustmentSummary ba_summary = BundleAdjustView(
        options, view_id, &reconstruction, &cov_mat, &emp_variance_factor);
    return std::make_tuple(ba_summary, reconstruction, cov_mat, emp_variance_factor);
}

std::tuple<BundleAdjustmentSummary, Reconstruction, std::map<ViewId, Matrix6d>, double> 
          BundleAdjustViewsWithCovWrapper(Reconstruction& reconstruction, const BundleAdjustmentOptions& options, const std::vector<ViewId>& view_ids) {
    std::map<ViewId, Matrix6d> cov_mats;
    double emp_variance_factor;
    BundleAdjustmentSummary ba_summary = BundleAdjustViews(
        options, view_ids, &reconstruction, &cov_mats, &emp_variance_factor);
    return std::make_tuple(ba_summary, reconstruction, cov_mats, emp_variance_factor);
}


std::tuple<BundleAdjustmentSummary, Reconstruction, Matrix3d, double> BundleAdjustTrackWithCovWrapper(
    Reconstruction& reconstruction, const BundleAdjustmentOptions& options, const TrackId track_id) {
    Matrix3d cov_mat;
    double emp_variance_factor;
    BundleAdjustmentSummary ba_summary = BundleAdjustTrack(
        options, track_id, &reconstruction, &cov_mat, &emp_variance_factor);
    return std::make_tuple(ba_summary, reconstruction, cov_mat, emp_variance_factor);
}

std::tuple<BundleAdjustmentSummary, Reconstruction, std::map<TrackId, Matrix3d>, double> BundleAdjustTracksWithCovWrapper(
    Reconstruction& reconstruction, const BundleAdjustmentOptions& options, const std::vector<TrackId>& track_ids) {
    std::map<TrackId, Matrix3d> cov_mats;
    double emp_variance_factor;
    BundleAdjustmentSummary ba_summary = BundleAdjustTracks(
        options, track_ids, &reconstruction, &cov_mats, &emp_variance_factor);
    return std::make_tuple(ba_summary, reconstruction, cov_mats, emp_variance_factor);
}

// std::tuple<BundleAdjustmentSummary, Reconstruction> BundleAdjustReconstructionWrapper(
//         const BundleAdjustmentOptions& options){
//     Reconstruction reconstruction;
//     BundleAdjustmentSummary ba_summary = BundleAdjustReconstruction(options, &reconstruction);
//     return std::make_tuple(ba_summary, reconstruction);
// }

// std::tuple<BundleAdjustmentSummary, Reconstruction> BundleAdjustPartialReconstructionWrapper(
//     const BundleAdjustmentOptions& options,
//     const std::unordered_set<ViewId>& views_to_optimize,
//         const std::unordered_set<TrackId>& tracks_to_optimize){
//     Reconstruction reconstruction;
//     BundleAdjustmentSummary ba_summary = BundleAdjustPartialReconstruction(options, views_to_optimize, tracks_to_optimize, &reconstruction);
//     return std::make_tuple(ba_summary, reconstruction);
// }

// std::tuple<BundleAdjustmentSummary, Camera, Camera, std::vector<Eigen::Vector4d>> BundleAdjustTwoViewsWrapper(
//     const TwoViewBundleAdjustmentOptions& options,
//     const theia::TwoViewInfo& two_view_info,
//     const std::vector<FeatureCorrespondence>& correspondences){
//     Camera camera1;
//     Camera camera2;
//     std::vector<Eigen::Vector4d> points_3d;
//     // first triangulate the points


//     BundleAdjustmentSummary ba_summary = BundleAdjustTwoViews(options, correspondences, &camera1, &camera2, &points_3d);
//     return std::make_tuple(ba_summary, camera1, camera2, points_3d);
// }

// std::tuple<bool, Eigen::Vector3d> OptimizeRelativePositionWithKnownRotationWrapper(
//     const std::vector<FeatureCorrespondence>& correspondences,
//     const Eigen::Vector3d& rotation1,
//     const Eigen::Vector3d& rotation2){
//     Eigen::Vector3d relative_position;
//     const bool success = OptimizeRelativePositionWithKnownRotation(correspondences, rotation1, rotation2, &relative_position);
//     return std::make_tuple(success, relative_position);
// }

}  // namespace theia
