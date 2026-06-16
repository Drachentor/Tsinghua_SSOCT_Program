#ifndef VESSELSEGMENTER_H
#define VESSELSEGMENTER_H

#include <opencv2/core.hpp>

namespace VesselSegmenter {

struct Params
{
    int spurPruneIterations = 2;
    int minSkeletonSegmentPixels = 6;
    int minAssignedPixels = 8;
};

struct Result
{
    cv::Mat segmentLabels; // CV_32S, 0 means non-vessel / unassigned.
    cv::Mat skeleton;      // CV_8U skeleton used for graph segmentation.
    int segmentCount = 0;
    int skeletonSegmentCount = 0;
    int assignedPixelCount = 0;
};

Result segmentVesselMask(const cv::Mat &vesselMask, const Params &params = Params());

} // namespace VesselSegmenter

#endif // VESSELSEGMENTER_H
