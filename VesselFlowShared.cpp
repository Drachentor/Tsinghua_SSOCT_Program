#include "VesselFlowShared.h"

#include <algorithm>

namespace {

size_t volumeIndex(const VesselFlowSharedGeometry &geometry, int z, int x, int y)
{
    return static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) *
        (static_cast<size_t>(x) + static_cast<size_t>(geometry.bscanLen) * y);
}

} // namespace

void copyVesselFlowProjectionBlock(const std::vector<float> &structuralVolume,
                                   const std::vector<float> &flowVolume,
                                   const std::vector<float> &surface,
                                   const VesselFlowSharedGeometry &geometry,
                                   int y,
                                   cv::Mat &structuralBlock,
                                   cv::Mat &flowBlock,
                                   cv::Mat &surfaceRow)
{
    structuralBlock.create(geometry.cropDepth, geometry.bscanLen, CV_32F);
    flowBlock.create(geometry.cropDepth, geometry.bscanLen, CV_32F);
    surfaceRow.create(1, geometry.bscanLen, CV_32F);

    if (geometry.cropDepth <= 0 || geometry.bscanLen <= 0 ||
        y < 0 || y >= geometry.cscanLenEff) {
        structuralBlock.setTo(0.0f);
        flowBlock.setTo(0.0f);
        surfaceRow.setTo(0.0f);
        return;
    }

    const size_t rowOffset = static_cast<size_t>(geometry.bscanLen) * y;
    float *surfaceDst = surfaceRow.ptr<float>(0);
    for (int x = 0; x < geometry.bscanLen; ++x) {
        const size_t surfaceIndex = rowOffset + static_cast<size_t>(x);
        surfaceDst[x] = surfaceIndex < surface.size() ? surface[surfaceIndex] : 0.0f;
    }

    for (int z = 0; z < geometry.cropDepth; ++z) {
        float *structuralDst = structuralBlock.ptr<float>(z);
        float *flowDst = flowBlock.ptr<float>(z);
        for (int x = 0; x < geometry.bscanLen; ++x) {
            const size_t index = volumeIndex(geometry, z, x, y);
            structuralDst[x] = index < structuralVolume.size() ? structuralVolume[index] : 0.0f;
            flowDst[x] = index < flowVolume.size() ? flowVolume[index] : 0.0f;
        }
    }
}
