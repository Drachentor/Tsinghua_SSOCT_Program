#ifndef FLOWSPEEDCALCULATION_H
#define FLOWSPEEDCALCULATION_H

#include <QString>

#include <functional>
#include <vector>

namespace FlowSpeedCalculation {

struct Geometry
{
    int ascanLen = 0;
    int bscanLen = 0;
    int cscanLenEff = 0;
    int angioRep = 0;
    int adFileOffsetFrames = 0;
    int cropStart0 = 0;
    int cropDepth = 0;
    int projectionDepth = 0;
    int surfaceZShift = 0;
    float backgroundSubtractionWeight = 0.0f;
};

struct OutputOptions
{
    bool generatePixelWiseImage = false;
    bool generateAveragedImage = false;
    bool generateSegmentWiseImage = false;
    bool generateFitCorrelationImage = false;
    bool useSkeletonDenoise = false;
    bool useManualMask = false;
    int cropTop = 0;
    int cropBottom = 0;
    int cropLeft = 0;
    int cropRight = 0;
};

void saveProjection(const QString &filePath,
                    const QString &basePath,
                    const Geometry &geometry,
                    const OutputOptions &options,
                    const std::vector<float> &spectralWindow,
                    const std::vector<float> &structuralVolume,
                    const std::vector<float> &flowVolume,
                    const std::vector<float> &surface,
                    const std::function<void(const QString&)> &log,
                    const std::function<void(const QString&)> &fileLog);

} // namespace FlowSpeedCalculation

#endif // FLOWSPEEDCALCULATION_H
