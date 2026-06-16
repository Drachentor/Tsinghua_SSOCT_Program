#ifndef VESSELFLOWSHARED_H
#define VESSELFLOWSHARED_H

#include <opencv2/core.hpp>

#include <vector>

struct VesselFlowSharedGeometry
{
    int cropDepth = 0;
    int bscanLen = 0;
    int cscanLenEff = 0;
};

void copyVesselFlowProjectionBlock(const std::vector<float> &structuralVolume,
                                   const std::vector<float> &flowVolume,
                                   const std::vector<float> &surface,
                                   const VesselFlowSharedGeometry &geometry,
                                   int y,
                                   cv::Mat &structuralBlock,
                                   cv::Mat &flowBlock,
                                   cv::Mat &surfaceRow);

#endif // VESSELFLOWSHARED_H
