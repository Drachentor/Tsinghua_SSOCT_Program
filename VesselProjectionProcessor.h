#ifndef VESSELPROJECTIONPROCESSOR_H
#define VESSELPROJECTIONPROCESSOR_H

#include <QString>
#include <QtGlobal>
#include <functional>

struct VesselProjectionParams
{
    int ascanLen;
    int bscanLen;
    int cscanLen;
    int angioRep;
    int adFileOffsetFrames;
    int previewDepth;
    int cropZStart;
    int cropZEnd;
    int projectionDepth;
    bool generatePreviewImage;
    bool generateGrayscaleImage;
    bool generatePixelWiseFlowSpeedImage;
    bool generateAveragedFlowSpeedImage;
    bool generateSegmentWiseFlowSpeedImage;
    bool generateFlowSpeedFitCorrelationImage;
    bool useFlowSpeedSkeletonDenoise;
    bool useFlowSpeedManualMask;
    int flowSpeedCropTop;
    int flowSpeedCropBottom;
    int flowSpeedCropLeft;
    int flowSpeedCropRight;
};

struct VesselProjectionFileSizeInfo
{
    qint64 fileSamples;
    qint64 requiredSamples;
    qint64 inferredRequiredSamples;
    qint64 unusedSamples;
    int programCscanLen;
    int inferredCscanLen;
    bool hasPartialFrame;
};

bool convertAngio3dToColorProjection(const QString &filePath,
                                     const VesselProjectionParams &params,
                                     const std::function<void(const QString&)> &log,
                                     const std::function<void(const QString&)> &fileLog,
                                     const std::function<bool(const VesselProjectionFileSizeInfo&)> &confirmSizeMismatch,
                                     QString *errorMessage);

#endif // VESSELPROJECTIONPROCESSOR_H
