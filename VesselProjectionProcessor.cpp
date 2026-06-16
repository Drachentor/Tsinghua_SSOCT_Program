#include "VesselProjectionProcessor.h"

#include "FlowSpeedCalculation.h"
#include "vessel_colorMap.h"
#ifdef VESSEL_USE_CUDA
#include "VesselProjectionCuda.h"
#endif

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLibrary>

#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

const int kFftZeroPadding = 1;
const int kAdPlotOffset = 2048;
const int kAdBitShift = 4;
const float kAdScale = static_cast<float>(1 << kAdBitShift);
const float kPreviewDbLow = 30.0f;
const float kPreviewDbHigh = 70.0f;
const double kPreviewLowPercentile = 1.0;
const double kPreviewHighPercentile = 99.5;
const float kPreviewMinDbSpan = 10.0f;
const float kPreviewMagnitudeFloor = 1.0e-6f;
const int kSurfaceZShift = 1;
const double kSurfaceThresholdRatio = 0.85;
const int kSurfaceIgnoreTopRows = 5;
const double kSurfaceGaussianSigmaX = 15.0;
const double kSurfaceGaussianSigmaZ = 4.0;
const int kSurfaceMedianRadiusX = 50;
const int kSurfaceMedianRadiusY = 12;
const int kSurfaceSupportGapRows = 8;
const int kSurfaceSupportAboveRows = 14;
const int kSurfaceSupportBelowRows = 34;
const float kSurfaceJumpThresholdPixels = 30.0f;
const float kSurfaceMedianDeviationPixels = 35.0f;
const float kSurfaceMaxStepPerColumn = 5.0f;
const float kSurfaceYMedianDeviationPixels = 35.0f;
const float kSurfaceFlatRowRangePixels = 12.0f;
const float kSurfaceFlatNeighborRangePixels = 25.0f;
const float kSurfaceMaxStepPerBscan = 10.0f;
const float kSurfaceRunThresholdRatio = 0.32f;
const int kSurfaceMinBrightRunRows = 24;
const int kSurfaceRunStartOffsetRows = 8;
const float kSurfaceMinSupportRatio = 0.30f;
const float kSurfaceMinEdgeRatio = 0.035f;
const int kSurfaceEdgeGuardColumns = 96;
const int kSurfaceEdgeMinGuardColumns = 32;
const int kSurfaceEdgeReferenceColumns = 48;
const int kSurfaceEdgeSampleColumns = 12;
const int kSurfaceEdgeStableColumns = 4;
const int kSurfaceEdgeMinRepairColumns = 6;
const float kSurfaceEdgeDeviationPixels = 18.0f;
const float kSurfaceEdgeSettlePixels = 9.0f;
const double kBgSubtractionWeight = 0.7;
const int kProjectionMaxAverage = 3;
const int kProjectionRowArtifactNeighborRadiusY = 10;
const int kProjectionRowArtifactNeighborGapY = 2;
const int kProjectionRowArtifactMinNeighborRowsPerSide = 3;
const int kProjectionRowArtifactMinRunColumns = 28;
const int kProjectionRowArtifactProtectRadiusX = 5;
const float kProjectionRowArtifactMinCoverageRatio = 0.06f;
const float kProjectionRowArtifactMaxProtectedRatio = 0.30f;
const float kProjectionRowArtifactMinExcess = 0.020f;
const float kProjectionRowArtifactMeanExcess = 0.026f;
const float kProjectionRowArtifactBlend = 0.85f;
const double kProjectionRowArtifactStrongKeepPercentile = 92.0;
const int kRegistrationMaxShiftPixels = 8;
const int kProjectionTiffDpi = 600;

using Complex = std::complex<float>;
using SteadyClock = std::chrono::steady_clock;

double elapsedSeconds(const SteadyClock::time_point &start)
{
    return std::chrono::duration<double>(SteadyClock::now() - start).count();
}

void logElapsed(const std::function<void(const QString&)> &log,
                const QString &stage,
                double seconds)
{
    log(QStringLiteral("%1 耗时：%2 秒。").arg(stage).arg(seconds, 0, 'f', 2));
}

int parallelWorkerCount(size_t itemCount)
{
    if (itemCount == 0) {
        return 0;
    }
    const unsigned int hardwareThreads = std::thread::hardware_concurrency();
    const int hardware = hardwareThreads == 0 ? 2 : static_cast<int>(hardwareThreads);
    const int maxWorkers = 32;
    return std::max(1, std::min<int>(static_cast<int>(itemCount),
                                     std::min(hardware, maxWorkers)));
}

template <typename Func>
void parallelForRange(size_t itemCount,
                      Func func,
                      size_t grainSize = 0,
                      bool forceParallel = false)
{
    if (itemCount == 0) {
        return;
    }

    const int workerCount = parallelWorkerCount(itemCount);
    if (workerCount <= 1 || (!forceParallel && itemCount < 16384)) {
        func(0, itemCount);
        return;
    }

    if (grainSize == 0) {
        grainSize = std::max<size_t>(1, itemCount / (static_cast<size_t>(workerCount) * 4));
    }

    std::atomic<size_t> next(0);
    std::atomic<bool> failed(false);
    std::exception_ptr firstError;
    std::mutex errorMutex;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(workerCount));

    for (int worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&]() {
            try {
                while (!failed.load()) {
                    const size_t begin = next.fetch_add(grainSize);
                    if (begin >= itemCount) {
                        break;
                    }
                    const size_t end = std::min(itemCount, begin + grainSize);
                    func(begin, end);
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!firstError) {
                        firstError = std::current_exception();
                    }
                }
                failed.store(true);
            }
        });
    }

    for (std::thread &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    if (firstError) {
        std::rethrow_exception(firstError);
    }
}

struct VolumeGeometry
{
    int ascanLen;
    int bscanLen;
    int angioRep;
    int adFileOffsetFrames;
    int previewDepth;
    int cropStart0;
    int cropEnd0;
    int cropDepth;
    int projectionDepth;
    int cscanLen;
    int cscanLenEff;

    size_t voxelCount() const
    {
        return static_cast<size_t>(cropDepth) * bscanLen * cscanLenEff;
    }

    size_t index(int z, int x, int y) const
    {
        return static_cast<size_t>(z) + static_cast<size_t>(cropDepth) *
            (static_cast<size_t>(x) + static_cast<size_t>(bscanLen) * y);
    }
};

struct SurfaceRepairStats
{
    SurfaceRepairStats()
        : flatRows(0)
        , repairedPoints(0)
    {
    }

    int flatRows;
    int repairedPoints;
};

struct ProjectionArtifactSuppressionStats
{
    ProjectionArtifactSuppressionStats()
        : repairedRows(0)
        , repairedPixels(0)
    {
    }

    int repairedRows;
    int repairedPixels;
};

qint64 samplesPerBscanFrame(const VolumeGeometry &geometry)
{
    return static_cast<qint64>(geometry.ascanLen) * geometry.bscanLen;
}

int skippedCscanBlocks(const VolumeGeometry &geometry)
{
    return (geometry.adFileOffsetFrames + geometry.angioRep - 1) / geometry.angioRep;
}

int effectiveCscanLen(int cscanLen, const VolumeGeometry &geometry)
{
    return cscanLen - skippedCscanBlocks(geometry);
}

qint64 requiredSamplesForCscanLen(int cscanLen, const VolumeGeometry &geometry)
{
    return samplesPerBscanFrame(geometry) *
        (static_cast<qint64>(geometry.adFileOffsetFrames) +
         static_cast<qint64>(effectiveCscanLen(cscanLen, geometry)) * geometry.angioRep);
}

std::string openClSource(const VolumeGeometry &geometry)
{
    std::ostringstream source;
    source << "#define BSCAN_LEN " << geometry.bscanLen << "\n"
           << "#define ASCAN_LEN " << geometry.ascanLen << "\n"
           << "#define ANGIO_REP " << geometry.angioRep << "\n";
    source << R"CLC(
inline float2 cadd(float2 a, float2 b) { return (float2)(a.x + b.x, a.y + b.y); }
inline float2 csub(float2 a, float2 b) { return (float2)(a.x - b.x, a.y - b.y); }
inline float2 cscale(float2 a, float s) { return (float2)(a.x * s, a.y * s); }
inline float2 cconj(float2 a) { return (float2)(a.x, -a.y); }
inline float2 cmul(float2 a, float2 b)
{
    return (float2)(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}
inline float2 conjmul(float2 a, float2 b)
{
    return (float2)(a.x * b.x + a.y * b.y, a.x * b.y - a.y * b.x);
}
inline float cabsval(float2 a)
{
    return sqrt(a.x * a.x + a.y * a.y);
}
inline float cnorm2(float2 a)
{
    return a.x * a.x + a.y * a.y;
}

inline float2 readFftAbsZ(__global const uchar *fft_data,
                          int fft_step,
                          int fft_offset,
                          int repeat,
                          int x,
                          int zAbs)
{
    const int row = repeat * BSCAN_LEN + x;
    __global const float2 *rowPtr = (__global const float2 *)(fft_data + fft_offset + row * fft_step);
    return rowPtr[zAbs];
}

inline float2 sampleShifted(__global const uchar *fft_data,
                            int fft_step,
                            int fft_offset,
                            int repeat,
                            int x,
                            int cropStart,
                            int cropDepth,
                            float sourceZ)
{
    if (sourceZ < 0.0f || sourceZ > (float)(cropDepth - 1)) {
        return (float2)(0.0f, 0.0f);
    }
    const int z0 = (int)floor(sourceZ);
    const int z1 = min(z0 + 1, cropDepth - 1);
    const float t = sourceZ - (float)z0;
    const float2 a = readFftAbsZ(fft_data, fft_step, fft_offset, repeat, x, cropStart + z0);
    const float2 b = readFftAbsZ(fft_data, fft_step, fft_offset, repeat, x, cropStart + z1);
    return cadd(cscale(a, 1.0f - t), cscale(b, t));
}

inline void normalizeVector(float2 v[ANGIO_REP])
{
    float normSq = 0.0f;
    for (int i = 0; i < ANGIO_REP; ++i) {
        normSq += cnorm2(v[i]);
    }
    if (normSq < 1e-20f) {
        for (int i = 0; i < ANGIO_REP; ++i) {
            v[i] = (float2)(0.0f, 0.0f);
        }
        v[0] = (float2)(1.0f, 0.0f);
        return;
    }
    const float invNorm = rsqrt(normSq);
    for (int i = 0; i < ANGIO_REP; ++i) {
        v[i] = cscale(v[i], invNorm);
    }
}

inline void multiplyMatrixVector(float2 matrix[ANGIO_REP * ANGIO_REP],
                                 float2 vector[ANGIO_REP],
                                 float2 result[ANGIO_REP])
{
    for (int row = 0; row < ANGIO_REP; ++row) {
        float2 value = (float2)(0.0f, 0.0f);
        for (int col = 0; col < ANGIO_REP; ++col) {
            value = cadd(value, cmul(matrix[row * ANGIO_REP + col], vector[col]));
        }
        result[row] = value;
    }
}

inline float rayleighQuotient(float2 matrix[ANGIO_REP * ANGIO_REP],
                              float2 vector[ANGIO_REP])
{
    float2 mv[ANGIO_REP];
    multiplyMatrixVector(matrix, vector, mv);
    float2 value = (float2)(0.0f, 0.0f);
    for (int i = 0; i < ANGIO_REP; ++i) {
        value = cadd(value, conjmul(vector[i], mv[i]));
    }
    return value.x;
}

inline void dominantEigenvector(float2 matrix[ANGIO_REP * ANGIO_REP],
                                int seed,
                                float2 vector[ANGIO_REP])
{
    for (int i = 0; i < ANGIO_REP; ++i) {
        vector[i] = (float2)(1.0f + 0.13f * (float)seed * (float)(i + 1),
                             0.07f * (float)(seed + 1) * (float)i);
    }
    normalizeVector(vector);

    for (int iter = 0; iter < 32; ++iter) {
        float2 next[ANGIO_REP];
        multiplyMatrixVector(matrix, vector, next);
        for (int i = 0; i < ANGIO_REP; ++i) {
            vector[i] = next[i];
        }
        normalizeVector(vector);
    }
}

__kernel void rawToSpectra(__global const uchar *raw_data,
                           int raw_step,
                           int raw_offset,
                           __global const float *window,
                           __global uchar *spectra_data,
                           int spectra_step,
                           int spectra_offset,
                           int adOffset,
                           float adScale)
{
    const int z = get_global_id(0);
    const int line = get_global_id(1);
    if (z >= ASCAN_LEN || line >= BSCAN_LEN * ANGIO_REP) {
        return;
    }

    __global const ushort *rawRow = (__global const ushort *)(raw_data + raw_offset + line * raw_step);
    __global float *dstRow = (__global float *)(spectra_data + spectra_offset + line * spectra_step);
    dstRow[z] = ((float)rawRow[z] / adScale - (float)adOffset) * window[z];
}

__kernel void extractMagnitude(__global const uchar *fft_data,
                               int fft_step,
                               int fft_offset,
                               __global uchar *mag_data,
                               int mag_step,
                               int mag_offset,
                               int repeat,
                               int cropStart,
                               int cropDepth)
{
    const int x = get_global_id(0);
    const int z = get_global_id(1);
    if (x >= BSCAN_LEN || z >= cropDepth) {
        return;
    }
    const float2 value = readFftAbsZ(fft_data, fft_step, fft_offset, repeat, x, cropStart + z);
    __global float *dstRow = (__global float *)(mag_data + mag_offset + z * mag_step);
    dstRow[x] = cabsval(value);
}

__kernel void shiftMagnitude(__global const uchar *fft_data,
                             int fft_step,
                             int fft_offset,
                             __global uchar *mag_data,
                             int mag_step,
                             int mag_offset,
                             int repeat,
                             float shift,
                             int cropStart,
                             int cropDepth)
{
    const int x = get_global_id(0);
    const int z = get_global_id(1);
    if (x >= BSCAN_LEN || z >= cropDepth) {
        return;
    }
    const float2 value = sampleShifted(fft_data, fft_step, fft_offset, repeat, x,
                                       cropStart, cropDepth, (float)z + shift);
    __global float *dstRow = (__global float *)(mag_data + mag_offset + z * mag_step);
    dstRow[x] = cabsval(value);
}

__kernel void estimateAxialShiftMetrics(__global const uchar *reference_data,
                                        int reference_step,
                                        int reference_offset,
                                        __global const uchar *moving_data,
                                        int moving_step,
                                        int moving_offset,
                                        __global uchar *metrics_data,
                                        int metrics_step,
                                        int metrics_offset,
                                        int maxShift,
                                        int cropDepth)
{
    const int x = get_global_id(0);
    const int shiftIndex = get_global_id(1);
    const int shiftCount = maxShift * 2 + 1;
    if (x >= BSCAN_LEN || shiftIndex >= shiftCount) {
        return;
    }

    const int shift = shiftIndex - maxShift;
    float numerator = 0.0f;
    float referenceEnergy = 0.0f;
    float movingEnergy = 0.0f;

    for (int z = 0; z < cropDepth; ++z) {
        __global const float *referenceRow =
            (__global const float *)(reference_data + reference_offset + z * reference_step);
        const float referenceValue = referenceRow[x];

        float movingValue = 0.0f;
        const int movingZ = z + shift;
        if (movingZ >= 0 && movingZ < cropDepth) {
            __global const float *movingRow =
                (__global const float *)(moving_data + moving_offset + movingZ * moving_step);
            movingValue = movingRow[x];
        }

        numerator += referenceValue * movingValue;
        referenceEnergy += referenceValue * referenceValue;
        movingEnergy += movingValue * movingValue;
    }

    __global float *numeratorRow =
        (__global float *)(metrics_data + metrics_offset + shiftIndex * metrics_step);
    __global float *referenceEnergyRow =
        (__global float *)(metrics_data + metrics_offset + (shiftCount + shiftIndex) * metrics_step);
    __global float *movingEnergyRow =
        (__global float *)(metrics_data + metrics_offset + (shiftCount * 2 + shiftIndex) * metrics_step);
    numeratorRow[x] = numerator;
    referenceEnergyRow[x] = referenceEnergy;
    movingEnergyRow[x] = movingEnergy;
}

__kernel void computeStructuralFlow(__global const uchar *fft_data,
                                    int fft_step,
                                    int fft_offset,
                                    __global const float *shifts,
                                    __global uchar *structural_data,
                                    int structural_step,
                                    int structural_offset,
                                    __global uchar *flow_data,
                                    int flow_step,
                                    int flow_offset,
                                    int cropStart,
                                    int cropDepth)
{
    const int x = get_global_id(0);
    if (x >= BSCAN_LEN) {
        return;
    }

    float2 covariance[ANGIO_REP * ANGIO_REP];
    for (int i = 0; i < ANGIO_REP * ANGIO_REP; ++i) {
        covariance[i] = (float2)(0.0f, 0.0f);
    }

    for (int z = 0; z < cropDepth; ++z) {
        float2 d[ANGIO_REP];
        for (int repeat = 0; repeat < ANGIO_REP; ++repeat) {
            d[repeat] = sampleShifted(fft_data, fft_step, fft_offset, repeat, x,
                                      cropStart, cropDepth, (float)z + shifts[repeat]);
        }
        for (int row = 0; row < ANGIO_REP; ++row) {
            for (int col = 0; col < ANGIO_REP; ++col) {
                const int idx = row * ANGIO_REP + col;
                covariance[idx] = cadd(covariance[idx], conjmul(d[row], d[col]));
            }
        }
    }
    for (int i = 0; i < ANGIO_REP * ANGIO_REP; ++i) {
        covariance[i] = cscale(covariance[i], 1.0f / (float)cropDepth);
    }

    float2 tissueVectors[2][ANGIO_REP];
    float2 deflated[ANGIO_REP * ANGIO_REP];
    for (int i = 0; i < ANGIO_REP * ANGIO_REP; ++i) {
        deflated[i] = covariance[i];
    }
    for (int eig = 0; eig < 2; ++eig) {
        dominantEigenvector(deflated, eig, tissueVectors[eig]);
        const float lambda = fmax(0.0f, rayleighQuotient(deflated, tissueVectors[eig]));
        for (int row = 0; row < ANGIO_REP; ++row) {
            for (int col = 0; col < ANGIO_REP; ++col) {
                const int idx = row * ANGIO_REP + col;
                deflated[idx] = csub(deflated[idx],
                                     cscale(cmul(tissueVectors[eig][row],
                                                 cconj(tissueVectors[eig][col])),
                                            lambda));
            }
        }
    }

    for (int z = 0; z < cropDepth; ++z) {
        float2 d[ANGIO_REP];
        float structural = 0.0f;
        for (int repeat = 0; repeat < ANGIO_REP; ++repeat) {
            d[repeat] = sampleShifted(fft_data, fft_step, fft_offset, repeat, x,
                                      cropStart, cropDepth, (float)z + shifts[repeat]);
            structural += cabsval(d[repeat]);
        }

        float2 tissue[ANGIO_REP];
        for (int repeat = 0; repeat < ANGIO_REP; ++repeat) {
            tissue[repeat] = (float2)(0.0f, 0.0f);
        }
        for (int eig = 0; eig < 2; ++eig) {
            float2 coefficient = (float2)(0.0f, 0.0f);
            for (int repeat = 0; repeat < ANGIO_REP; ++repeat) {
                coefficient = cadd(coefficient, cmul(d[repeat], tissueVectors[eig][repeat]));
            }
            for (int repeat = 0; repeat < ANGIO_REP; ++repeat) {
                tissue[repeat] = cadd(tissue[repeat],
                                      cmul(cconj(tissueVectors[eig][repeat]), coefficient));
            }
        }

        float flow = 0.0f;
        for (int repeat = 0; repeat < ANGIO_REP; ++repeat) {
            flow += cabsval(csub(d[repeat], tissue[repeat]));
        }

        __global float *structuralRow = (__global float *)(structural_data + structural_offset + z * structural_step);
        __global float *flowRow = (__global float *)(flow_data + flow_offset + z * flow_step);
        structuralRow[x] = structural / (float)ANGIO_REP;
        flowRow[x] = flow / (float)ANGIO_REP;
    }
}
)CLC";
    return source.str();
}

std::vector<float> makeTukeyWindow(int length, double alpha)
{
    std::vector<float> window(length, 1.0f);
    if (alpha <= 0.0) {
        return window;
    }
    if (alpha >= 1.0) {
        for (int i = 0; i < length; ++i) {
            window[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * CV_PI * i / (length - 1))));
        }
        return window;
    }

    const double edge = alpha * (length - 1) / 2.0;
    for (int i = 0; i < length; ++i) {
        if (i < edge) {
            window[i] = static_cast<float>(0.5 * (1.0 + std::cos(CV_PI * (2.0 * i / (alpha * (length - 1)) - 1.0))));
        } else if (i > (length - 1) * (1.0 - alpha / 2.0)) {
            window[i] = static_cast<float>(0.5 * (1.0 + std::cos(CV_PI * (2.0 * i / (alpha * (length - 1)) - 2.0 / alpha + 1.0))));
        }
    }
    return window;
}

bool openRawVolumeFile(QFile &file, const QString &filePath)
{
    file.setFileName(filePath);
    return file.open(QIODevice::ReadOnly);
}

bool writeImageFile(const QString &filePath,
                    const cv::Mat &image,
                    const std::vector<int> &params = std::vector<int>())
{
    QString extension = QFileInfo(filePath).suffix().toLower();
    if (extension.isEmpty()) {
        extension = QStringLiteral("png");
    }
    extension.prepend('.');

    std::vector<uchar> encodedImage;
    if (!cv::imencode(extension.toStdString(), image, encodedImage, params)) {
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    const qint64 bytesToWrite = static_cast<qint64>(encodedImage.size());
    return file.write(reinterpret_cast<const char*>(encodedImage.data()), bytesToWrite) == bytesToWrite;
}

std::vector<int> projectionTiffEncodeParams()
{
    return {
        cv::IMWRITE_TIFF_RESUNIT, 2,
        cv::IMWRITE_TIFF_XDPI, kProjectionTiffDpi,
        cv::IMWRITE_TIFF_YDPI, kProjectionTiffDpi
    };
}

QString outputImageName(const QString &filePath)
{
    return QFileInfo(filePath).fileName();
}

bool readRawYBlock(QFile &file,
                   int yIndex0,
                   const VolumeGeometry &geometry,
                   std::vector<uint16_t> &rawBlock)
{
    const qint64 fileHeaderOffsetBytes =
        static_cast<qint64>(geometry.ascanLen) * geometry.bscanLen * sizeof(uint16_t) *
        geometry.adFileOffsetFrames;
    const qint64 blockBytes =
        static_cast<qint64>(geometry.ascanLen) * geometry.bscanLen * geometry.angioRep *
        sizeof(uint16_t);
    const qint64 offset = fileHeaderOffsetBytes + blockBytes * yIndex0;

    if (!file.seek(offset)) {
        return false;
    }

    return file.read(reinterpret_cast<char*>(rawBlock.data()), blockBytes) == blockBytes;
}

cv::Mat computeFftRows(const std::vector<uint16_t> &rawBlock,
                       const std::vector<float> &spectralWindow,
                       const VolumeGeometry &geometry)
{
    const int lineCount = geometry.bscanLen * geometry.angioRep;
    cv::Mat spectra(lineCount, geometry.ascanLen, CV_32F);
    for (int line = 0; line < lineCount; ++line) {
        float *dst = spectra.ptr<float>(line);
        const size_t lineOffset = static_cast<size_t>(line) * geometry.ascanLen;
        for (int z = 0; z < geometry.ascanLen; ++z) {
            dst[z] = (static_cast<float>(rawBlock[lineOffset + z]) / kAdScale - kAdPlotOffset) *
                spectralWindow[z];
        }
    }

    cv::Mat complexFft;
    cv::dft(spectra, complexFft, cv::DFT_ROWS | cv::DFT_COMPLEX_OUTPUT);
    return complexFft;
}

float percentile(std::vector<float> values, double percent)
{
    values.erase(std::remove_if(values.begin(), values.end(),
                                [](float value) { return !std::isfinite(value); }),
                 values.end());
    if (values.empty()) {
        return 0.0f;
    }

    std::sort(values.begin(), values.end());
    const double position = (percent / 100.0) * static_cast<double>(values.size() - 1);
    const size_t low = static_cast<size_t>(std::floor(position));
    const size_t high = static_cast<size_t>(std::ceil(position));
    if (low == high) {
        return values[low];
    }

    const double fraction = position - static_cast<double>(low);
    return static_cast<float>(values[low] * (1.0 - fraction) + values[high] * fraction);
}

float clampFloat(float value, float low, float high)
{
    return std::max(low, std::min(value, high));
}

uint8_t toByte(float value)
{
    return static_cast<uint8_t>(std::round(clampFloat(value, 0.0f, 1.0f) * 255.0f));
}

void savePreviewImage(QFile &file,
                      const QString &outputPath,
                      const std::vector<float> &spectralWindow,
                      const VolumeGeometry &geometry,
                      const std::function<void(const QString&)> &log,
                      const std::function<void(const QString&)> &fileLog)
{
    const int previewRows = std::min(geometry.previewDepth, geometry.ascanLen);
    const int previewSourceCols = geometry.bscanLen * geometry.angioRep;
    const int previewOutputCols = geometry.bscanLen;
    cv::Mat previewAccum = cv::Mat::zeros(previewRows, previewSourceCols, CV_32F);
    std::vector<uint16_t> rawBlock(static_cast<size_t>(geometry.ascanLen) * previewSourceCols);

    int count = 0;
    for (int yIndex1 = 10; yIndex1 <= geometry.cscanLenEff; yIndex1 += 10) {
        if (!readRawYBlock(file, yIndex1 - 1, geometry, rawBlock)) {
            throw std::runtime_error("Failed to read raw preview block.");
        }
        const cv::Mat complexFft = computeFftRows(rawBlock, spectralWindow, geometry);
        for (int line = 0; line < previewSourceCols; ++line) {
            const cv::Vec2f *src = complexFft.ptr<cv::Vec2f>(line);
            for (int z = 0; z < previewRows; ++z) {
                const float re = src[z][0];
                const float im = src[z][1];
                previewAccum.at<float>(z, line) += std::sqrt(re * re + im * im);
            }
        }
        ++count;
        QApplication::processEvents();
    }

    if (count == 0) {
        return;
    }

    previewAccum /= static_cast<float>(count);
    cv::Mat previewDb(previewRows, previewOutputCols, CV_32F);
    std::vector<float> previewSamples;
    previewSamples.reserve(static_cast<size_t>(previewRows) * previewOutputCols / 8 + 1);
    for (int z = 0; z < previewRows; ++z) {
        const float *src = previewAccum.ptr<float>(z);
        float *dst = previewDb.ptr<float>(z);
        for (int x = 0; x < previewOutputCols; ++x) {
            const float dbValue = 20.0f * std::log10(std::max(src[x], kPreviewMagnitudeFloor));
            dst[x] = dbValue;
            if (((z * previewOutputCols + x) % 8) == 0) {
                previewSamples.push_back(dbValue);
            }
        }
    }

    float displayLow = percentile(previewSamples, kPreviewLowPercentile);
    float displayHigh = percentile(previewSamples, kPreviewHighPercentile);
    if (!std::isfinite(displayLow) || !std::isfinite(displayHigh) ||
        displayHigh - displayLow < kPreviewMinDbSpan) {
        displayLow = kPreviewDbLow;
        displayHigh = kPreviewDbHigh;
    }

    cv::Mat preview8(previewRows, previewOutputCols, CV_8U);
    for (int z = 0; z < previewRows; ++z) {
        const float *src = previewDb.ptr<float>(z);
        uint8_t *dst = preview8.ptr<uint8_t>(z);
        for (int x = 0; x < previewOutputCols; ++x) {
            dst[x] = toByte((src[x] - displayLow) / (displayHigh - displayLow));
        }
    }

    if (!writeImageFile(outputPath, preview8)) {
        throw std::runtime_error("Failed to save preview image.");
    }
    log(QStringLiteral("预览图已保存：%1").arg(outputImageName(outputPath)));
    if (fileLog) {
        fileLog(QStringLiteral("Saved output full path: %1").arg(outputPath));
    }
}

Complex sampleShifted(const std::vector<Complex> &frame, int depth, int x, float sourceZ)
{
    if (sourceZ < 0.0f || sourceZ > static_cast<float>(depth - 1)) {
        return Complex(0.0f, 0.0f);
    }

    const int z0 = static_cast<int>(std::floor(sourceZ));
    const int z1 = std::min(z0 + 1, depth - 1);
    const float t = sourceZ - z0;
    const Complex a = frame[static_cast<size_t>(z0) + static_cast<size_t>(depth) * x];
    const Complex b = frame[static_cast<size_t>(z1) + static_cast<size_t>(depth) * x];
    return a * (1.0f - t) + b * t;
}

using FrameSet = std::vector<std::vector<Complex>>;
using ComplexVector = std::vector<Complex>;
using HermitianMatrix = std::vector<Complex>;

void registerRepeatedFrames(FrameSet &frames,
                            const VolumeGeometry &geometry)
{
    cv::Mat reference(geometry.cropDepth, geometry.bscanLen, CV_32F);
    for (int x = 0; x < geometry.bscanLen; ++x) {
        for (int z = 0; z < geometry.cropDepth; ++z) {
            reference.at<float>(z, x) =
                std::abs(frames[0][static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) * x]);
        }
    }

    for (int repeat = 1; repeat < geometry.angioRep; ++repeat) {
        cv::Mat moving(geometry.cropDepth, geometry.bscanLen, CV_32F);
        for (int x = 0; x < geometry.bscanLen; ++x) {
            for (int z = 0; z < geometry.cropDepth; ++z) {
                moving.at<float>(z, x) =
                    std::abs(frames[repeat][static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) * x]);
            }
        }

        const cv::Point2d shift = cv::phaseCorrelate(reference, moving);
        std::vector<Complex> shifted(frames[repeat].size());
        for (int x = 0; x < geometry.bscanLen; ++x) {
            for (int z = 0; z < geometry.cropDepth; ++z) {
                shifted[static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) * x] =
                    sampleShifted(frames[repeat], geometry.cropDepth, x, static_cast<float>(z + shift.y));
            }
        }
        frames[repeat].swap(shifted);

        for (int x = 0; x < geometry.bscanLen; ++x) {
            for (int z = 0; z < geometry.cropDepth; ++z) {
                reference.at<float>(z, x) =
                    std::abs(frames[repeat][static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) * x]);
            }
        }
    }
}

ComplexVector normalizeVector(const ComplexVector &input)
{
    double normSq = 0.0;
    for (const Complex &value : input) {
        normSq += std::norm(value);
    }
    const double norm = std::sqrt(normSq);
    ComplexVector output = input;
    if (norm < 1e-20) {
        std::fill(output.begin(), output.end(), Complex(0.0f, 0.0f));
        if (!output.empty()) {
            output[0] = Complex(1.0f, 0.0f);
        }
        return output;
    }
    for (Complex &value : output) {
        value /= static_cast<float>(norm);
    }
    return output;
}

ComplexVector multiply(const HermitianMatrix &matrix,
                       const ComplexVector &vector,
                       int repCount)
{
    ComplexVector result(static_cast<size_t>(repCount), Complex(0.0f, 0.0f));
    for (int row = 0; row < repCount; ++row) {
        for (int col = 0; col < repCount; ++col) {
            result[row] += matrix[static_cast<size_t>(row) * repCount + col] * vector[col];
        }
    }
    return result;
}

float rayleighQuotient(const HermitianMatrix &matrix,
                       const ComplexVector &vector,
                       int repCount)
{
    const ComplexVector mv = multiply(matrix, vector, repCount);
    Complex value(0.0f, 0.0f);
    for (int i = 0; i < repCount; ++i) {
        value += std::conj(vector[i]) * mv[i];
    }
    return value.real();
}

ComplexVector dominantEigenvector(const HermitianMatrix &matrix, int repCount, int seed)
{
    ComplexVector vector(static_cast<size_t>(repCount));
    for (int i = 0; i < repCount; ++i) {
        vector[i] = Complex(1.0f + 0.13f * seed * (i + 1), 0.07f * (seed + 1) * i);
    }
    vector = normalizeVector(vector);

    for (int iter = 0; iter < 32; ++iter) {
        vector = normalizeVector(multiply(matrix, vector, repCount));
    }
    return vector;
}

void computeStructuralAndFlow(const FrameSet &frames,
                              const VolumeGeometry &geometry,
                              std::vector<float> &structuralVolume,
                              std::vector<float> &flowVolume,
                              int yIndex0)
{
    const int repCount = geometry.angioRep;
    for (int x = 0; x < geometry.bscanLen; ++x) {
        HermitianMatrix covariance(static_cast<size_t>(repCount) * repCount, Complex(0.0f, 0.0f));

        for (int z = 0; z < geometry.cropDepth; ++z) {
            ComplexVector d(static_cast<size_t>(repCount));
            for (int repeat = 0; repeat < repCount; ++repeat) {
                d[repeat] = frames[repeat][static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) * x];
            }
            for (int row = 0; row < repCount; ++row) {
                for (int col = 0; col < repCount; ++col) {
                    covariance[static_cast<size_t>(row) * repCount + col] += std::conj(d[row]) * d[col];
                }
            }
        }
        for (int row = 0; row < repCount; ++row) {
            for (int col = 0; col < repCount; ++col) {
                covariance[static_cast<size_t>(row) * repCount + col] /= static_cast<float>(geometry.cropDepth);
            }
        }

        std::array<ComplexVector, 2> tissueVectors;
        HermitianMatrix deflated = covariance;
        for (int eig = 0; eig < 2; ++eig) {
            tissueVectors[eig] = dominantEigenvector(deflated, repCount, eig);
            const float lambda = std::max(0.0f, rayleighQuotient(deflated, tissueVectors[eig], repCount));
            for (int row = 0; row < repCount; ++row) {
                for (int col = 0; col < repCount; ++col) {
                    deflated[static_cast<size_t>(row) * repCount + col] -=
                        lambda * tissueVectors[eig][row] * std::conj(tissueVectors[eig][col]);
                }
            }
        }

        for (int z = 0; z < geometry.cropDepth; ++z) {
            ComplexVector d(static_cast<size_t>(repCount));
            float structural = 0.0f;
            for (int repeat = 0; repeat < repCount; ++repeat) {
                d[repeat] = frames[repeat][static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) * x];
                structural += std::abs(d[repeat]);
            }

            ComplexVector tissue(static_cast<size_t>(repCount), Complex(0.0f, 0.0f));
            for (const auto &v : tissueVectors) {
                Complex coefficient(0.0f, 0.0f);
                for (int repeat = 0; repeat < repCount; ++repeat) {
                    coefficient += d[repeat] * v[repeat];
                }
                for (int repeat = 0; repeat < repCount; ++repeat) {
                    tissue[repeat] += std::conj(v[repeat]) * coefficient;
                }
            }

            float flow = 0.0f;
            for (int repeat = 0; repeat < repCount; ++repeat) {
                flow += std::abs(d[repeat] - tissue[repeat]);
            }

            const size_t volumeIndex = geometry.index(z, x, yIndex0);
            structuralVolume[volumeIndex] = structural / repCount;
            flowVolume[volumeIndex] = flow / repCount;
        }
    }
}

void copyBlockToVolume(const cv::Mat &structuralBlock,
                       const cv::Mat &flowBlock,
                       const VolumeGeometry &geometry,
                       std::vector<float> &structuralVolume,
                       std::vector<float> &flowVolume,
                       int yIndex0)
{
    for (int z = 0; z < geometry.cropDepth; ++z) {
        const float *structuralRow = structuralBlock.ptr<float>(z);
        const float *flowRow = flowBlock.ptr<float>(z);
        for (int x = 0; x < geometry.bscanLen; ++x) {
            const size_t volumeIndex = geometry.index(z, x, yIndex0);
            structuralVolume[volumeIndex] = structuralRow[x];
            flowVolume[volumeIndex] = flowRow[x];
        }
    }
}

void createOpenClKernel(cv::ocl::Kernel &kernel,
                        const char *kernelName,
                        const cv::ocl::ProgramSource &source)
{
    cv::String errors;
    if (!kernel.create(kernelName, source, cv::String(), &errors) || kernel.empty()) {
        std::string message = "Failed to create OpenCL kernel ";
        message += kernelName;
        if (!errors.empty()) {
            message += ": ";
            message += errors;
        }
        throw std::runtime_error(message);
    }
}

bool initializeOpenClGpu(QString *deviceDescription, QString *failureReason)
{
    qputenv("OPENCV_OPENCL_DEVICE", QByteArray(":GPU:"));
    cv::ocl::setUseOpenCL(true);
    if (!cv::ocl::haveOpenCL()) {
        if (failureReason) {
            *failureReason = QStringLiteral("没有可用的 OpenCL 运行时");
        }
        return false;
    }

    cv::ocl::Device device = cv::ocl::Device::getDefault();
    if (!device.available()) {
        if (failureReason) {
            *failureReason = QStringLiteral("OpenCL 默认设备不可用");
        }
        return false;
    }
    if ((device.type() & cv::ocl::Device::TYPE_GPU) == 0) {
        if (failureReason) {
            *failureReason = QStringLiteral("OpenCL 默认设备不是 GPU：%1")
                .arg(QString::fromStdString(device.name()));
        }
        return false;
    }
    if (!device.compilerAvailable()) {
        if (failureReason) {
            *failureReason = QStringLiteral("OpenCL GPU 设备没有可用的 kernel 编译器：%1")
                .arg(QString::fromStdString(device.name()));
        }
        return false;
    }

    try {
        cv::Mat test = cv::Mat::ones(8, 8, CV_32F);
        cv::UMat testGpu;
        cv::UMat testFft;
        test.copyTo(testGpu);
        cv::dft(testGpu, testFft, cv::DFT_ROWS | cv::DFT_COMPLEX_OUTPUT);
        cv::ocl::finish();
    } catch (const cv::Exception &ex) {
        if (failureReason) {
            *failureReason = QStringLiteral("OpenCL FFT 测试失败：%1").arg(QString::fromLocal8Bit(ex.what()));
        }
        return false;
    }

    if (deviceDescription) {
        *deviceDescription = QStringLiteral("%1 (%2)")
            .arg(QString::fromStdString(device.name()),
                 QString::fromStdString(device.vendorName()));
    }
    return true;
}

class GpuBlockProcessor
{
public:
    GpuBlockProcessor(const std::vector<float> &spectralWindow,
                      const VolumeGeometry &geometry)
    {
        cv::Mat windowHost(1, geometry.ascanLen, CV_32F, const_cast<float*>(spectralWindow.data()));
        windowHost.copyTo(m_spectralWindow);

        const std::string sourceText = openClSource(geometry);
        const cv::ocl::ProgramSource source(sourceText.c_str());
        createOpenClKernel(m_rawToSpectra, "rawToSpectra", source);
        createOpenClKernel(m_extractMagnitude, "extractMagnitude", source);
        createOpenClKernel(m_shiftMagnitude, "shiftMagnitude", source);
        createOpenClKernel(m_estimateAxialShiftMetrics, "estimateAxialShiftMetrics", source);
        createOpenClKernel(m_computeStructuralFlow, "computeStructuralFlow", source);
    }

    void processBlock(QFile &file,
                      int y,
                      const VolumeGeometry &geometry,
                      std::vector<float> &structuralVolume,
                      std::vector<float> &flowVolume)
    {
        const int lineCount = geometry.bscanLen * geometry.angioRep;
        std::vector<uint16_t> rawBlock(static_cast<size_t>(geometry.ascanLen) * lineCount);
        if (!readRawYBlock(file, y, geometry, rawBlock)) {
            throw std::runtime_error("Failed to read raw volume block.");
        }

        cv::Mat rawHost(lineCount, geometry.ascanLen, CV_16U, rawBlock.data());
        cv::UMat rawGpu;
        rawHost.copyTo(rawGpu);

        cv::UMat spectra(lineCount, geometry.ascanLen, CV_32F);
        size_t rawGlobalSize[] = {static_cast<size_t>(geometry.ascanLen), static_cast<size_t>(lineCount)};
        m_rawToSpectra.args(cv::ocl::KernelArg::ReadOnlyNoSize(rawGpu),
                            cv::ocl::KernelArg::PtrReadOnly(m_spectralWindow),
                            cv::ocl::KernelArg::WriteOnlyNoSize(spectra),
                            kAdPlotOffset,
                            kAdScale);
        if (!m_rawToSpectra.run_(2, rawGlobalSize, nullptr, true)) {
            throw std::runtime_error("Failed to run OpenCL rawToSpectra kernel.");
        }

        cv::UMat complexFft;
        cv::dft(spectra, complexFft, cv::DFT_ROWS | cv::DFT_COMPLEX_OUTPUT);

        std::vector<float> shifts(static_cast<size_t>(geometry.angioRep), 0.0f);
        cv::UMat referenceMagnitude;
        cv::UMat movingMagnitude;
        extractMagnitude(complexFft, referenceMagnitude, 0, geometry);
        for (int repeat = 1; repeat < geometry.angioRep; ++repeat) {
            extractMagnitude(complexFft, movingMagnitude, repeat, geometry);
            shifts[static_cast<size_t>(repeat)] =
                estimateAxialShift(referenceMagnitude, movingMagnitude, geometry);
            shiftMagnitude(complexFft, referenceMagnitude, repeat, shifts[static_cast<size_t>(repeat)], geometry);
        }

        cv::Mat shiftsHost(1, geometry.angioRep, CV_32F, shifts.data());
        cv::UMat shiftsGpu;
        shiftsHost.copyTo(shiftsGpu);

        cv::UMat structuralBlock(geometry.cropDepth, geometry.bscanLen, CV_32F);
        cv::UMat flowBlock(geometry.cropDepth, geometry.bscanLen, CV_32F);
        size_t flowGlobalSize[] = {static_cast<size_t>(geometry.bscanLen)};
        m_computeStructuralFlow.args(cv::ocl::KernelArg::ReadOnlyNoSize(complexFft),
                                     cv::ocl::KernelArg::PtrReadOnly(shiftsGpu),
                                     cv::ocl::KernelArg::WriteOnlyNoSize(structuralBlock),
                                     cv::ocl::KernelArg::WriteOnlyNoSize(flowBlock),
                                     geometry.cropStart0,
                                     geometry.cropDepth);
        if (!m_computeStructuralFlow.run_(1, flowGlobalSize, nullptr, true)) {
            throw std::runtime_error("Failed to run OpenCL computeStructuralFlow kernel.");
        }

        cv::Mat structuralHost;
        cv::Mat flowHost;
        structuralBlock.copyTo(structuralHost);
        flowBlock.copyTo(flowHost);
        copyBlockToVolume(structuralHost, flowHost, geometry, structuralVolume, flowVolume, y);
    }

    int cpuRegistrationFallbackCount() const
    {
        return m_cpuRegistrationFallbackCount;
    }

private:
    void extractMagnitude(const cv::UMat &complexFft,
                          cv::UMat &magnitude,
                          int repeat,
                          const VolumeGeometry &geometry)
    {
        magnitude.create(geometry.cropDepth, geometry.bscanLen, CV_32F);
        size_t globalSize[] = {static_cast<size_t>(geometry.bscanLen),
                               static_cast<size_t>(geometry.cropDepth)};
        m_extractMagnitude.args(cv::ocl::KernelArg::ReadOnlyNoSize(complexFft),
                                cv::ocl::KernelArg::WriteOnlyNoSize(magnitude),
                                repeat,
                                geometry.cropStart0,
                                geometry.cropDepth);
        if (!m_extractMagnitude.run_(2, globalSize, nullptr, true)) {
            throw std::runtime_error("Failed to run OpenCL extractMagnitude kernel.");
        }
    }

    void shiftMagnitude(const cv::UMat &complexFft,
                        cv::UMat &magnitude,
                        int repeat,
                        float shift,
                        const VolumeGeometry &geometry)
    {
        size_t globalSize[] = {static_cast<size_t>(geometry.bscanLen),
                               static_cast<size_t>(geometry.cropDepth)};
        m_shiftMagnitude.args(cv::ocl::KernelArg::ReadOnlyNoSize(complexFft),
                              cv::ocl::KernelArg::WriteOnlyNoSize(magnitude),
                              repeat,
                              shift,
                              geometry.cropStart0,
                              geometry.cropDepth);
        if (!m_shiftMagnitude.run_(2, globalSize, nullptr, true)) {
            throw std::runtime_error("Failed to run OpenCL shiftMagnitude kernel.");
        }
    }

    float estimateAxialShift(const cv::UMat &referenceMagnitude,
                             const cv::UMat &movingMagnitude,
                             const VolumeGeometry &geometry)
    {
        const int maxShift = std::min(kRegistrationMaxShiftPixels, geometry.cropDepth - 1);
        if (maxShift <= 0) {
            return 0.0f;
        }

        const int shiftCount = maxShift * 2 + 1;
        cv::UMat metrics(shiftCount * 3, geometry.bscanLen, CV_32F);
        size_t globalSize[] = {static_cast<size_t>(geometry.bscanLen),
                               static_cast<size_t>(shiftCount)};
        m_estimateAxialShiftMetrics.args(cv::ocl::KernelArg::ReadOnlyNoSize(referenceMagnitude),
                                         cv::ocl::KernelArg::ReadOnlyNoSize(movingMagnitude),
                                         cv::ocl::KernelArg::WriteOnlyNoSize(metrics),
                                         maxShift,
                                         geometry.cropDepth);
        if (!m_estimateAxialShiftMetrics.run_(2, globalSize, nullptr, true)) {
            throw std::runtime_error("Failed to run OpenCL estimateAxialShiftMetrics kernel.");
        }

        cv::Mat metricsHost;
        metrics.copyTo(metricsHost);

        std::vector<double> scores(static_cast<size_t>(shiftCount), -std::numeric_limits<double>::infinity());
        int bestIndex = maxShift;
        double bestScore = -std::numeric_limits<double>::infinity();
        for (int shiftIndex = 0; shiftIndex < shiftCount; ++shiftIndex) {
            const float *numeratorRow = metricsHost.ptr<float>(shiftIndex);
            const float *referenceEnergyRow = metricsHost.ptr<float>(shiftCount + shiftIndex);
            const float *movingEnergyRow = metricsHost.ptr<float>(shiftCount * 2 + shiftIndex);
            double numerator = 0.0;
            double referenceEnergy = 0.0;
            double movingEnergy = 0.0;
            for (int x = 0; x < geometry.bscanLen; ++x) {
                numerator += numeratorRow[x];
                referenceEnergy += referenceEnergyRow[x];
                movingEnergy += movingEnergyRow[x];
            }

            if (referenceEnergy > 0.0 && movingEnergy > 0.0) {
                scores[static_cast<size_t>(shiftIndex)] =
                    numerator / std::sqrt(referenceEnergy * movingEnergy);
            }
            if (scores[static_cast<size_t>(shiftIndex)] > bestScore) {
                bestScore = scores[static_cast<size_t>(shiftIndex)];
                bestIndex = shiftIndex;
            }
        }

        if (!std::isfinite(bestScore) || bestIndex == 0 || bestIndex == shiftCount - 1) {
            ++m_cpuRegistrationFallbackCount;
            const cv::Point2d fallbackShift = cv::phaseCorrelate(referenceMagnitude, movingMagnitude);
            return std::isfinite(fallbackShift.y) ? static_cast<float>(fallbackShift.y) : 0.0f;
        }

        float shift = static_cast<float>(bestIndex - maxShift);
        const double left = scores[static_cast<size_t>(bestIndex - 1)];
        const double center = scores[static_cast<size_t>(bestIndex)];
        const double right = scores[static_cast<size_t>(bestIndex + 1)];
        const double denominator = left - 2.0 * center + right;
        if (std::isfinite(left) && std::isfinite(center) && std::isfinite(right) &&
            std::abs(denominator) > 1.0e-12) {
            const double subpixelOffset = 0.5 * (left - right) / denominator;
            if (std::isfinite(subpixelOffset) && std::abs(subpixelOffset) <= 1.0) {
                shift += static_cast<float>(subpixelOffset);
            }
        }

        return shift;
    }

    cv::UMat m_spectralWindow;
    cv::ocl::Kernel m_rawToSpectra;
    cv::ocl::Kernel m_extractMagnitude;
    cv::ocl::Kernel m_shiftMagnitude;
    cv::ocl::Kernel m_estimateAxialShiftMetrics;
    cv::ocl::Kernel m_computeStructuralFlow;
    int m_cpuRegistrationFallbackCount = 0;
};

void normalizeAmplitudeVolumes(std::vector<float> &structuralVolume,
                               std::vector<float> &flowVolume)
{
    std::vector<float> structuralSamples;
    std::vector<float> flowSamples;
    structuralSamples.reserve(structuralVolume.size() / 37 + 1);
    flowSamples.reserve(flowVolume.size() / 37 + 1);

    for (size_t i = 0; i < structuralVolume.size(); i += 37) {
        const float structural = structuralVolume[i] < 1e-5f ? 40.0f : 20.0f * std::log10(structuralVolume[i]);
        const float flow = structuralVolume[i] < 1e-5f ? 40.0f : 20.0f * std::log10(std::max(flowVolume[i], 1e-12f));
        structuralSamples.push_back(structural);
        flowSamples.push_back(flow);
    }

    const float structuralLow = percentile(structuralSamples, 2.5);
    const float structuralHigh = percentile(structuralSamples, 99.99);
    const float flowLow = percentile(flowSamples, 2.5);
    const float flowHigh = percentile(flowSamples, 99.99);
    const float structuralScale = structuralHigh > structuralLow ? 65535.0f / (structuralHigh - structuralLow) : 1.0f;
    const float flowScale = flowHigh > flowLow ? 65535.0f / (flowHigh - flowLow) : 1.0f;

    parallelForRange(structuralVolume.size(), [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const float structuralDb = structuralVolume[i] < 1e-5f ? 40.0f : 20.0f * std::log10(structuralVolume[i]);
            const float flowDb = structuralVolume[i] < 1e-5f ? 40.0f : 20.0f * std::log10(std::max(flowVolume[i], 1e-12f));
            structuralVolume[i] = clampFloat((structuralDb - structuralLow) * structuralScale, 0.0f, 65535.0f);
            flowVolume[i] = clampFloat((flowDb - flowLow) * flowScale, 0.0f, 65535.0f);
        }
    });
}

float medianInWindow(const std::vector<float> &values, int center, int radius)
{
    const int start = std::max(0, center - radius);
    const int end = std::min(static_cast<int>(values.size()) - 1, center + radius);
    std::array<float, 2 * kSurfaceMedianRadiusX + 1> window;
    size_t count = 0;
    for (int i = start; i <= end; ++i) {
        if (std::isfinite(values[static_cast<size_t>(i)])) {
            window[count++] = values[static_cast<size_t>(i)];
        }
    }
    if (count == 0) {
        return values[static_cast<size_t>(center)];
    }

    const size_t middle = count / 2;
    std::nth_element(window.begin(), window.begin() + middle, window.begin() + count);
    float median = window[middle];
    if ((count % 2) == 0) {
        std::nth_element(window.begin(), window.begin() + middle - 1, window.begin() + count);
        median = 0.5f * (median + window[middle - 1]);
    }
    return median;
}

float medianInRange(const std::vector<float> &values, int begin, int end)
{
    if (values.empty()) {
        return std::numeric_limits<float>::quiet_NaN();
    }

    begin = std::max(0, begin);
    end = std::min(static_cast<int>(values.size()) - 1, end);
    if (begin > end) {
        return std::numeric_limits<float>::quiet_NaN();
    }

    std::vector<float> window;
    window.reserve(static_cast<size_t>(end - begin + 1));
    for (int i = begin; i <= end; ++i) {
        const float value = values[static_cast<size_t>(i)];
        if (std::isfinite(value)) {
            window.push_back(value);
        }
    }
    if (window.empty()) {
        return std::numeric_limits<float>::quiet_NaN();
    }

    const size_t middle = window.size() / 2;
    std::nth_element(window.begin(), window.begin() + middle, window.end());
    float median = window[middle];
    if ((window.size() % 2) == 0) {
        std::nth_element(window.begin(), window.begin() + middle - 1, window.end());
        median = 0.5f * (median + window[middle - 1]);
    }
    return median;
}

int surfaceEdgeGuardColumns(int width)
{
    if (width <= 0) {
        return 0;
    }

    int guardColumns = std::min(kSurfaceEdgeGuardColumns,
                                std::max(kSurfaceEdgeMinGuardColumns, width / 8));
    guardColumns = std::min(guardColumns, std::max(0, width / 2 - kSurfaceEdgeReferenceColumns / 2));
    return std::max(0, guardColumns);
}

void markSurfaceEdgeRun(const std::vector<float> &surfaceRow,
                        std::vector<char> &valid,
                        bool leftEdge)
{
    const int width = static_cast<int>(surfaceRow.size());
    const int guardColumns = surfaceEdgeGuardColumns(width);
    if (guardColumns < kSurfaceEdgeMinRepairColumns ||
        width < guardColumns + kSurfaceEdgeSampleColumns + 1) {
        return;
    }

    const int sampleBegin = leftEdge ? 0 : std::max(0, width - kSurfaceEdgeSampleColumns);
    const int sampleEnd = leftEdge ? std::min(width - 1, kSurfaceEdgeSampleColumns - 1) : width - 1;
    const int referenceBegin = leftEdge ?
        guardColumns :
        std::max(0, width - guardColumns - kSurfaceEdgeReferenceColumns);
    const int referenceEnd = leftEdge ?
        std::min(width - 1, guardColumns + kSurfaceEdgeReferenceColumns - 1) :
        std::max(0, width - guardColumns - 1);

    const float edgeMedian = medianInRange(surfaceRow, sampleBegin, sampleEnd);
    const float referenceMedian = medianInRange(surfaceRow, referenceBegin, referenceEnd);
    if (!std::isfinite(edgeMedian) || !std::isfinite(referenceMedian) ||
        std::abs(edgeMedian - referenceMedian) <= kSurfaceEdgeDeviationPixels) {
        return;
    }

    int stableRun = 0;
    int repairColumns = -1;
    for (int offset = 0; offset < guardColumns; ++offset) {
        const int x = leftEdge ? offset : width - 1 - offset;
        if (std::abs(surfaceRow[static_cast<size_t>(x)] - referenceMedian) <=
            kSurfaceEdgeSettlePixels) {
            ++stableRun;
            if (stableRun >= kSurfaceEdgeStableColumns) {
                repairColumns = offset - kSurfaceEdgeStableColumns + 1;
                break;
            }
        } else {
            stableRun = 0;
        }
    }

    if (repairColumns < 0) {
        repairColumns = guardColumns;
    }
    if (repairColumns < kSurfaceEdgeMinRepairColumns) {
        return;
    }

    for (int offset = 0; offset < repairColumns; ++offset) {
        const int x = leftEdge ? offset : width - 1 - offset;
        valid[static_cast<size_t>(x)] = 0;
    }
}

void interpolateInvalidSurfaceRuns(std::vector<float> &surfaceRow,
                                   const std::vector<char> &valid)
{
    const int width = static_cast<int>(surfaceRow.size());
    int x = 0;
    while (x < width) {
        if (valid[static_cast<size_t>(x)]) {
            ++x;
            continue;
        }

        const int runStart = x;
        while (x < width && !valid[static_cast<size_t>(x)]) {
            ++x;
        }
        const int runEnd = x - 1;
        const int left = runStart - 1;
        const int right = x;

        if (left >= 0 && right < width &&
            valid[static_cast<size_t>(left)] && valid[static_cast<size_t>(right)]) {
            const float leftValue = surfaceRow[static_cast<size_t>(left)];
            const float rightValue = surfaceRow[static_cast<size_t>(right)];
            const float span = static_cast<float>(right - left);
            for (int i = runStart; i <= runEnd; ++i) {
                const float t = static_cast<float>(i - left) / span;
                surfaceRow[static_cast<size_t>(i)] = leftValue * (1.0f - t) + rightValue * t;
            }
        } else if (left >= 0 && valid[static_cast<size_t>(left)]) {
            const float fillValue = surfaceRow[static_cast<size_t>(left)];
            for (int i = runStart; i <= runEnd; ++i) {
                surfaceRow[static_cast<size_t>(i)] = fillValue;
            }
        } else if (right < width && valid[static_cast<size_t>(right)]) {
            const float fillValue = surfaceRow[static_cast<size_t>(right)];
            for (int i = runStart; i <= runEnd; ++i) {
                surfaceRow[static_cast<size_t>(i)] = fillValue;
            }
        }
    }
}

void limitSurfaceSteps(std::vector<float> &surfaceRow, float maxStep)
{
    for (size_t i = 1; i < surfaceRow.size(); ++i) {
        surfaceRow[i] = clampFloat(surfaceRow[i],
                                   surfaceRow[i - 1] - maxStep,
                                   surfaceRow[i - 1] + maxStep);
    }
    for (int i = static_cast<int>(surfaceRow.size()) - 2; i >= 0; --i) {
        surfaceRow[static_cast<size_t>(i)] =
            clampFloat(surfaceRow[static_cast<size_t>(i)],
                       surfaceRow[static_cast<size_t>(i + 1)] - maxStep,
                       surfaceRow[static_cast<size_t>(i + 1)] + maxStep);
    }
}

void repairSurfaceRow(std::vector<float> &surfaceRow)
{
    const int width = static_cast<int>(surfaceRow.size());
    if (width <= 2) {
        return;
    }

    std::vector<char> valid(static_cast<size_t>(width), 1);
    int jumpStart = -1;
    int jumpDirection = 0;
    for (int x = 1; x < width; ++x) {
        const float diff = surfaceRow[static_cast<size_t>(x)] -
            surfaceRow[static_cast<size_t>(x - 1)];
        if (std::abs(diff) <= kSurfaceJumpThresholdPixels) {
            continue;
        }

        const int direction = diff > 0.0f ? 1 : -1;
        if (jumpStart < 0) {
            jumpStart = x;
            jumpDirection = direction;
            continue;
        }

        if (direction == -jumpDirection) {
            const int runStart = std::min(jumpStart, x);
            const int runEnd = std::max(jumpStart, x) - 1;
            for (int i = runStart; i <= runEnd; ++i) {
                valid[static_cast<size_t>(i)] = 0;
            }
            jumpStart = -1;
            jumpDirection = 0;
        } else {
            jumpStart = x;
            jumpDirection = direction;
        }
    }

    if (jumpStart >= 0) {
        const int edgeColumns = surfaceEdgeGuardColumns(width);
        if (edgeColumns > 0 && jumpStart <= edgeColumns) {
            for (int i = 0; i < jumpStart; ++i) {
                valid[static_cast<size_t>(i)] = 0;
            }
        } else {
            for (int i = jumpStart; i < width; ++i) {
                valid[static_cast<size_t>(i)] = 0;
            }
        }
    }

    std::vector<float> trend(static_cast<size_t>(width), 0.0f);
    for (int x = 0; x < width; ++x) {
        trend[static_cast<size_t>(x)] =
            medianInWindow(surfaceRow, x, kSurfaceMedianRadiusX);
    }
    for (int x = 0; x < width; ++x) {
        if (std::abs(surfaceRow[static_cast<size_t>(x)] -
                     trend[static_cast<size_t>(x)]) > kSurfaceMedianDeviationPixels) {
            valid[static_cast<size_t>(x)] = 0;
        }
    }

    markSurfaceEdgeRun(surfaceRow, valid, true);
    markSurfaceEdgeRun(surfaceRow, valid, false);
    interpolateInvalidSurfaceRuns(surfaceRow, valid);
    limitSurfaceSteps(surfaceRow, kSurfaceMaxStepPerColumn);
}

void clampSurfaceValues(std::vector<float> &values, int cropDepth)
{
    const float low = 1.0f;
    const float high = static_cast<float>(std::max(1, cropDepth));
    for (float &value : values) {
        value = clampFloat(value, low, high);
    }
}

std::vector<char> detectFlatSurfaceRows(const std::vector<float> &surface,
                                        const VolumeGeometry &geometry,
                                        SurfaceRepairStats *repairStats)
{
    const int width = geometry.bscanLen;
    const int height = geometry.cscanLenEff;
    std::vector<float> rowRange(static_cast<size_t>(height), 0.0f);
    std::vector<float> rowValues(static_cast<size_t>(width));

    for (int y = 0; y < height; ++y) {
        const size_t rowOffset = static_cast<size_t>(width) * y;
        for (int x = 0; x < width; ++x) {
            rowValues[static_cast<size_t>(x)] = surface[rowOffset + x];
        }
        const float low = percentile(rowValues, 10.0);
        const float high = percentile(rowValues, 90.0);
        rowRange[static_cast<size_t>(y)] = high - low;
    }

    std::vector<char> flatRow(static_cast<size_t>(height), 0);
    const float globalRange = percentile(rowRange, 50.0);
    for (int y = 0; y < height; ++y) {
        const int begin = std::max(0, y - kSurfaceMedianRadiusY);
        const int end = std::min(height - 1, y + kSurfaceMedianRadiusY);
        std::vector<float> neighborRanges;
        neighborRanges.reserve(static_cast<size_t>(end - begin + 1));
        for (int yy = begin; yy <= end; ++yy) {
            neighborRanges.push_back(rowRange[static_cast<size_t>(yy)]);
        }

        const float localRange = percentile(neighborRanges, 50.0);
        const float referenceRange = std::max(localRange, globalRange);
        if (rowRange[static_cast<size_t>(y)] <= kSurfaceFlatRowRangePixels &&
            referenceRange >= kSurfaceFlatNeighborRangePixels) {
            flatRow[static_cast<size_t>(y)] = 1;
            if (repairStats) {
                ++repairStats->flatRows;
            }
        }
    }
    return flatRow;
}

void repairSurfaceMapAcrossY(std::vector<float> &surface,
                             const VolumeGeometry &geometry,
                             SurfaceRepairStats *repairStats)
{
    const int width = geometry.bscanLen;
    const int height = geometry.cscanLenEff;
    if (width <= 0 || height <= 2) {
        return;
    }

    const std::vector<char> flatRow = detectFlatSurfaceRows(surface, geometry, repairStats);
    std::vector<float> column(static_cast<size_t>(height), 0.0f);
    std::vector<float> trendInput(static_cast<size_t>(height), 0.0f);
    std::vector<char> valid(static_cast<size_t>(height), 1);
    std::vector<char> rowNeedsRepair(static_cast<size_t>(height), 0);

    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) {
            const float value = surface[static_cast<size_t>(x) + static_cast<size_t>(width) * y];
            column[static_cast<size_t>(y)] = value;
            trendInput[static_cast<size_t>(y)] =
                flatRow[static_cast<size_t>(y)] ? std::numeric_limits<float>::quiet_NaN() : value;
            valid[static_cast<size_t>(y)] = flatRow[static_cast<size_t>(y)] ? 0 : 1;
        }

        for (int y = 0; y < height; ++y) {
            const float trend = medianInWindow(trendInput, y, kSurfaceMedianRadiusY);
            if (std::isfinite(trend) &&
                std::abs(column[static_cast<size_t>(y)] - trend) > kSurfaceYMedianDeviationPixels) {
                valid[static_cast<size_t>(y)] = 0;
            }
        }

        for (int y = 0; y < height; ++y) {
            if (!valid[static_cast<size_t>(y)]) {
                rowNeedsRepair[static_cast<size_t>(y)] = 1;
                if (repairStats) {
                    ++repairStats->repairedPoints;
                }
            }
        }

        interpolateInvalidSurfaceRuns(column, valid);
        limitSurfaceSteps(column, kSurfaceMaxStepPerBscan);
        clampSurfaceValues(column, geometry.cropDepth);

        for (int y = 0; y < height; ++y) {
            surface[static_cast<size_t>(x) + static_cast<size_t>(width) * y] =
                column[static_cast<size_t>(y)];
        }
    }

    std::vector<char> rowsToRepair = rowNeedsRepair;
    for (int y = 0; y < height; ++y) {
        if (!rowNeedsRepair[static_cast<size_t>(y)]) {
            continue;
        }
        const int begin = std::max(0, y - 1);
        const int end = std::min(height - 1, y + 1);
        for (int yy = begin; yy <= end; ++yy) {
            rowsToRepair[static_cast<size_t>(yy)] = 1;
        }
    }

    std::vector<float> surfaceRow(static_cast<size_t>(width), 0.0f);
    for (int y = 0; y < height; ++y) {
        if (!rowsToRepair[static_cast<size_t>(y)]) {
            continue;
        }
        const size_t rowOffset = static_cast<size_t>(width) * y;
        for (int x = 0; x < width; ++x) {
            surfaceRow[static_cast<size_t>(x)] = surface[rowOffset + x];
        }
        repairSurfaceRow(surfaceRow);
        clampSurfaceValues(surfaceRow, geometry.cropDepth);
        for (int x = 0; x < width; ++x) {
            surface[rowOffset + x] = surfaceRow[static_cast<size_t>(x)];
        }
    }
}

float legacySurfacePosition(const cv::Mat &smoothed, int x, int cropDepth, float columnMax)
{
    const float threshold = columnMax * static_cast<float>(kSurfaceThresholdRatio);
    for (int z = 0; z < cropDepth; ++z) {
        if (smoothed.at<float>(z, x) > threshold) {
            return static_cast<float>(z + 1 - 5);
        }
    }
    return 1.0f;
}

float supportedSurfacePosition(const cv::Mat &smoothed,
                               int x,
                               int cropDepth,
                               std::vector<float> &prefix)
{
    prefix[0] = 0.0f;
    float columnMax = 0.0f;
    for (int z = 0; z < cropDepth; ++z) {
        const float value = smoothed.at<float>(z, x);
        prefix[static_cast<size_t>(z + 1)] = prefix[static_cast<size_t>(z)] + value;
        columnMax = std::max(columnMax, value);
    }

    if (columnMax <= 0.0f) {
        return 1.0f;
    }

    const int searchStart = std::min(kSurfaceIgnoreTopRows, cropDepth - 1);
    const float runThreshold = columnMax * kSurfaceRunThresholdRatio;
    float bestRunScore = -std::numeric_limits<float>::max();
    int bestRunStart = -1;
    int zRun = searchStart;
    while (zRun < cropDepth) {
        while (zRun < cropDepth && smoothed.at<float>(zRun, x) < runThreshold) {
            ++zRun;
        }
        const int runStart = zRun;
        while (zRun < cropDepth && smoothed.at<float>(zRun, x) >= runThreshold) {
            ++zRun;
        }
        const int runEnd = zRun;
        const int runLength = runEnd - runStart;
        if (runLength >= kSurfaceMinBrightRunRows) {
            const float runMean =
                (prefix[static_cast<size_t>(runEnd)] - prefix[static_cast<size_t>(runStart)]) /
                static_cast<float>(runLength);
            const float score = static_cast<float>(runLength) *
                (0.5f + runMean / std::max(columnMax, 1.0f));
            if (score > bestRunScore) {
                bestRunScore = score;
                bestRunStart = runStart;
            }
        }
    }

    if (bestRunStart >= 0) {
        const int shiftedStart = std::min(cropDepth - 1, bestRunStart + kSurfaceRunStartOffsetRows);
        return static_cast<float>(shiftedStart + 1);
    }

    const float minSupport = columnMax * kSurfaceMinSupportRatio;
    const float minEdge = columnMax * kSurfaceMinEdgeRatio;
    float bestScore = -std::numeric_limits<float>::max();
    int bestZ = -1;

    const int searchEnd = std::max(searchStart, cropDepth - 2);
    for (int z = searchStart; z <= searchEnd; ++z) {
        const int aboveStart = std::max(0, z - kSurfaceSupportAboveRows);
        const int belowStart = std::min(cropDepth - 1, z + kSurfaceSupportGapRows);
        const int belowEnd = std::min(cropDepth, belowStart + kSurfaceSupportBelowRows);
        const int aboveCount = std::max(1, z - aboveStart);
        const int belowCount = std::max(1, belowEnd - belowStart);
        const float aboveMean =
            (prefix[static_cast<size_t>(z)] - prefix[static_cast<size_t>(aboveStart)]) /
            static_cast<float>(aboveCount);
        const float belowMean =
            (prefix[static_cast<size_t>(belowEnd)] - prefix[static_cast<size_t>(belowStart)]) /
            static_cast<float>(belowCount);
        const float edge = belowMean - aboveMean;

        if (belowMean < minSupport || edge < minEdge) {
            continue;
        }

        const float shallowPenalty = 0.02f * columnMax *
            (static_cast<float>(z) / static_cast<float>(std::max(1, cropDepth - 1)));
        const float score = edge + 0.20f * belowMean - shallowPenalty;
        if (score > bestScore) {
            bestScore = score;
            bestZ = z;
        }
    }

    if (bestZ >= 0) {
        return static_cast<float>(bestZ + 1);
    }

    return legacySurfacePosition(smoothed, x, cropDepth, columnMax);
}

std::vector<float> estimateSurface(const std::vector<float> &structuralImage,
                                   const VolumeGeometry &geometry,
                                   SurfaceRepairStats *repairStats)
{
    std::vector<float> surface(static_cast<size_t>(geometry.bscanLen) * geometry.cscanLenEff, 1.0f);
    parallelForRange(static_cast<size_t>(geometry.cscanLenEff), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            cv::Mat slice(geometry.cropDepth, geometry.bscanLen, CV_32F);
            for (int z = 0; z < geometry.cropDepth; ++z) {
                float *dst = slice.ptr<float>(z);
                for (int x = 0; x < geometry.bscanLen; ++x) {
                    dst[x] = structuralImage[geometry.index(z, x, y)];
                }
            }
            for (int z = 0; z < std::min(kSurfaceIgnoreTopRows, geometry.cropDepth); ++z) {
                slice.row(z).setTo(0.0f);
            }

            cv::Mat smoothed;
            cv::GaussianBlur(slice, smoothed, cv::Size(0, 0),
                             kSurfaceGaussianSigmaX, kSurfaceGaussianSigmaZ,
                             cv::BORDER_REFLECT_101);
            std::vector<float> surfaceRow(static_cast<size_t>(geometry.bscanLen), 1.0f);
            std::vector<float> prefix(static_cast<size_t>(geometry.cropDepth) + 1, 0.0f);
            for (int x = 0; x < geometry.bscanLen; ++x) {
                surfaceRow[static_cast<size_t>(x)] =
                    supportedSurfacePosition(smoothed, x, geometry.cropDepth, prefix);
            }

            repairSurfaceRow(surfaceRow);
            for (int x = 0; x < geometry.bscanLen; ++x) {
                surface[static_cast<size_t>(x) + static_cast<size_t>(geometry.bscanLen) * y] =
                    surfaceRow[static_cast<size_t>(x)];
            }
        }
    }, 1, true);
    repairSurfaceMapAcrossY(surface, geometry, repairStats);
    QApplication::processEvents();
    return surface;
}

void saveSurfaceChecks(const std::vector<float> &structuralImage,
                       const std::vector<float> &surface,
                       const VolumeGeometry &geometry,
                       const QString &basePath,
                       const std::function<void(const QString&)> &log,
                       const std::function<void(const QString&)> &fileLog)
{
    const QFileInfo baseInfo(basePath);
    QDir outputDir(baseInfo.absolutePath());
    const QString surfacePattern = baseInfo.fileName() + QStringLiteral("_surface_*.png");
    const QFileInfoList oldSurfaceFiles =
        outputDir.entryInfoList(QStringList(surfacePattern), QDir::Files);
    int removedCount = 0;
    for (const QFileInfo &oldSurfaceFile : oldSurfaceFiles) {
        if (outputDir.remove(oldSurfaceFile.fileName())) {
            ++removedCount;
        }
    }
    if (removedCount > 0) {
        log(QStringLiteral("已删除旧的表面检测检查图：%1 张。").arg(removedCount));
    }

    for (int marker = 1; marker <= 7; ++marker) {
        const int yIndex1 = static_cast<int>(std::round(geometry.cscanLenEff * marker / 8.0));
        const int y = std::max(0, std::min(yIndex1 - 1, geometry.cscanLenEff - 1));
        cv::Mat gray(geometry.cropDepth, geometry.bscanLen, CV_8U);
        for (int x = 0; x < geometry.bscanLen; ++x) {
            for (int z = 0; z < geometry.cropDepth; ++z) {
                gray.at<uint8_t>(z, x) = toByte(structuralImage[geometry.index(z, x, y)] / 65535.0f);
            }
        }

        cv::Mat color;
        cv::cvtColor(gray, color, cv::COLOR_GRAY2BGR);
        std::vector<cv::Point> points;
        points.reserve(geometry.bscanLen);
        for (int x = 0; x < geometry.bscanLen; ++x) {
            int z = static_cast<int>(std::round(surface[static_cast<size_t>(x) + static_cast<size_t>(geometry.bscanLen) * y])) - 1;
            z = std::max(0, std::min(z, geometry.cropDepth - 1));
            points.emplace_back(x, z);
        }
        cv::polylines(color, points, false, cv::Scalar(0, 0, 255), 1, cv::LINE_AA);

        const QString outputPath = QStringLiteral("%1_surface_%2.png").arg(basePath).arg(yIndex1);
        if (!writeImageFile(outputPath, color)) {
            throw std::runtime_error("Failed to save surface check image.");
        }
    }
    log(QStringLiteral("表面检测检查图已保存：%1_surface_*.png").arg(outputImageName(basePath)));
    if (fileLog) {
        fileLog(QStringLiteral("Saved output full path: %1_surface_*.png").arg(basePath));
    }
}

std::vector<float> flattenFlowVolume(const std::vector<float> &flowImage,
                                     const std::vector<float> &structuralImage,
                                     const std::vector<float> &surface,
                                     const VolumeGeometry &geometry)
{
    std::vector<float> bgRemovedSamples;
    bgRemovedSamples.reserve(flowImage.size() / 36 + 1);

    for (size_t i = 0; i < flowImage.size(); i += 36) {
        bgRemovedSamples.push_back(flowImage[i] - kBgSubtractionWeight * structuralImage[i]);
    }
    const float bgLow = percentile(bgRemovedSamples, 40.0);
    const float bgHigh = percentile(bgRemovedSamples, 99.99);
    const float bgScale = bgHigh > bgLow ? 1.0f / (bgHigh - bgLow) : 1.0f;

    std::vector<float> flattened(static_cast<size_t>(geometry.projectionDepth) * geometry.bscanLen * geometry.cscanLenEff, 0.0f);
    parallelForRange(static_cast<size_t>(geometry.cscanLenEff), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            for (int x = 0; x < geometry.bscanLen; ++x) {
                const size_t columnIndex = static_cast<size_t>(x) + static_cast<size_t>(geometry.bscanLen) * y;
                const int surfaceShifted = static_cast<int>(std::round(surface[columnIndex] + kSurfaceZShift));
                for (int d = 0; d < geometry.projectionDepth; ++d) {
                    int sourceZ = d + surfaceShifted;
                    sourceZ = std::max(0, std::min(sourceZ, geometry.cropDepth - 1));
                    const size_t sourceIndex = geometry.index(sourceZ, x, y);
                    const float bgRemoved = flowImage[sourceIndex] -
                        kBgSubtractionWeight * structuralImage[sourceIndex];
                    flattened[static_cast<size_t>(d) + static_cast<size_t>(geometry.projectionDepth) *
                        (static_cast<size_t>(x) + static_cast<size_t>(geometry.bscanLen) * y)] =
                        (bgRemoved - bgLow) * bgScale;
                }
            }
        }
    }, 1, true);
    QApplication::processEvents();
    return flattened;
}

void normalizeFlattened(std::vector<float> &flattened)
{
    std::vector<float> samples;
    samples.reserve(flattened.size() / 40 + 1);
    for (size_t i = 0; i < flattened.size(); i += 40) {
        samples.push_back(flattened[i]);
    }
    const float low = percentile(samples, 40.0);
    const float high = percentile(samples, 99.9);
    const float scale = high > low ? 1.0f / (high - low) : 1.0f;
    parallelForRange(flattened.size(), [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            flattened[i] = clampFloat((flattened[i] - low) * scale, 0.0f, 1.0f);
        }
    });
}

std::vector<float> smooth3x3x3(const std::vector<float> &input, const VolumeGeometry &geometry)
{
    const int depth = geometry.projectionDepth;
    std::vector<float> temp(input.size(), 0.0f);
    std::vector<float> output(input.size(), 0.0f);
    const float kernel[3] = {0.25f, 0.5f, 0.25f};

    auto flatIndex = [depth, &geometry](int z, int x, int y) {
        return static_cast<size_t>(z) + static_cast<size_t>(depth) *
            (static_cast<size_t>(x) + static_cast<size_t>(geometry.bscanLen) * y);
    };

    parallelForRange(static_cast<size_t>(geometry.cscanLenEff), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            for (int x = 0; x < geometry.bscanLen; ++x) {
                for (int z = 0; z < depth; ++z) {
                    float sum = 0.0f;
                    for (int dz = -1; dz <= 1; ++dz) {
                        const int zz = std::max(0, std::min(z + dz, depth - 1));
                        sum += kernel[dz + 1] * input[flatIndex(zz, x, y)];
                    }
                    temp[flatIndex(z, x, y)] = sum;
                }
            }
        }
    }, 1, true);

    std::vector<float> temp2(input.size(), 0.0f);
    parallelForRange(static_cast<size_t>(geometry.cscanLenEff), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            for (int x = 0; x < geometry.bscanLen; ++x) {
                const int xm = std::max(0, x - 1);
                const int xp = std::min(geometry.bscanLen - 1, x + 1);
                for (int z = 0; z < depth; ++z) {
                    temp2[flatIndex(z, x, y)] =
                        kernel[0] * temp[flatIndex(z, xm, y)] +
                        kernel[1] * temp[flatIndex(z, x, y)] +
                        kernel[2] * temp[flatIndex(z, xp, y)];
                }
            }
        }
    }, 1, true);

    parallelForRange(static_cast<size_t>(geometry.cscanLenEff), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            const int ym = std::max(0, y - 1);
            const int yp = std::min(geometry.cscanLenEff - 1, y + 1);
            for (int x = 0; x < geometry.bscanLen; ++x) {
                for (int z = 0; z < depth; ++z) {
                    output[flatIndex(z, x, y)] =
                        kernel[0] * temp2[flatIndex(z, x, ym)] +
                        kernel[1] * temp2[flatIndex(z, x, y)] +
                        kernel[2] * temp2[flatIndex(z, x, yp)];
                }
            }
        }
    }, 1, true);
    QApplication::processEvents();
    return output;
}

bool projectionNeighborReference(const cv::Mat &gray,
                                 const cv::Mat &depthIndex,
                                 int x,
                                 int y,
                                 float *grayReference,
                                 int *depthReference)
{
    std::array<float, 2 * kProjectionRowArtifactNeighborRadiusY> grayValues;
    std::array<int, 2 * kProjectionRowArtifactNeighborRadiusY> depthValues;
    int count = 0;
    int aboveCount = 0;
    int belowCount = 0;

    for (int dy = -kProjectionRowArtifactNeighborRadiusY;
         dy <= kProjectionRowArtifactNeighborRadiusY;
         ++dy) {
        if (std::abs(dy) < kProjectionRowArtifactNeighborGapY) {
            continue;
        }

        const int yy = y + dy;
        if (yy < 0 || yy >= gray.rows) {
            continue;
        }

        grayValues[static_cast<size_t>(count)] = gray.at<float>(yy, x);
        depthValues[static_cast<size_t>(count)] = depthIndex.at<int>(yy, x);
        ++count;
        if (dy < 0) {
            ++aboveCount;
        } else {
            ++belowCount;
        }
    }

    if (aboveCount < kProjectionRowArtifactMinNeighborRowsPerSide ||
        belowCount < kProjectionRowArtifactMinNeighborRowsPerSide ||
        count <= 0) {
        return false;
    }

    const int middle = count / 2;
    std::nth_element(grayValues.begin(),
                     grayValues.begin() + middle,
                     grayValues.begin() + count);
    float grayMedian = grayValues[static_cast<size_t>(middle)];
    if ((count % 2) == 0) {
        std::nth_element(grayValues.begin(),
                         grayValues.begin() + middle - 1,
                         grayValues.begin() + count);
        grayMedian = 0.5f * (grayMedian + grayValues[static_cast<size_t>(middle - 1)]);
    }

    std::nth_element(depthValues.begin(),
                     depthValues.begin() + middle,
                     depthValues.begin() + count);
    int depthMedian = depthValues[static_cast<size_t>(middle)];
    if ((count % 2) == 0) {
        std::nth_element(depthValues.begin(),
                         depthValues.begin() + middle - 1,
                         depthValues.begin() + count);
        depthMedian = (depthMedian + depthValues[static_cast<size_t>(middle - 1)]) / 2;
    }

    *grayReference = grayMedian;
    *depthReference = depthMedian;
    return true;
}

ProjectionArtifactSuppressionStats suppressProjectionRowArtifacts(cv::Mat &gray,
                                                                  cv::Mat &depthIndex)
{
    ProjectionArtifactSuppressionStats stats;
    if (gray.empty() || depthIndex.empty() ||
        gray.rows != depthIndex.rows || gray.cols != depthIndex.cols ||
        gray.rows <= 2 * kProjectionRowArtifactNeighborRadiusY) {
        return stats;
    }

    std::vector<float> samples;
    samples.reserve(static_cast<size_t>(gray.rows) * gray.cols / 16 + 1);
    for (int y = 0; y < gray.rows; y += 2) {
        const float *row = gray.ptr<float>(y);
        for (int x = 0; x < gray.cols; x += 8) {
            samples.push_back(row[x]);
        }
    }
    const float strongKeepThreshold =
        percentile(samples, kProjectionRowArtifactStrongKeepPercentile);

    const cv::Mat originalGray = gray.clone();
    const cv::Mat originalDepth = depthIndex.clone();
    const int minCoveragePixels = std::max(
        kProjectionRowArtifactMinRunColumns,
        static_cast<int>(std::round(gray.cols * kProjectionRowArtifactMinCoverageRatio)));

    std::vector<float> reference(static_cast<size_t>(gray.cols), 0.0f);
    std::vector<int> referenceDepth(static_cast<size_t>(gray.cols), 0);
    std::vector<float> excess(static_cast<size_t>(gray.cols), 0.0f);
    std::vector<char> excessMask(static_cast<size_t>(gray.cols), 0);
    std::vector<char> strongMask(static_cast<size_t>(gray.cols), 0);
    std::vector<char> protectMask(static_cast<size_t>(gray.cols), 0);
    std::vector<char> runMask(static_cast<size_t>(gray.cols), 0);
    std::vector<char> repairMask(static_cast<size_t>(gray.cols), 0);

    for (int y = 0; y < gray.rows; ++y) {
        int candidatePixels = 0;
        float candidateExcessSum = 0.0f;
        std::fill(excessMask.begin(), excessMask.end(), 0);
        std::fill(strongMask.begin(), strongMask.end(), 0);
        std::fill(protectMask.begin(), protectMask.end(), 0);
        std::fill(runMask.begin(), runMask.end(), 0);
        std::fill(repairMask.begin(), repairMask.end(), 0);

        for (int x = 0; x < gray.cols; ++x) {
            float grayReference = 0.0f;
            int depthReference = 0;
            if (!projectionNeighborReference(originalGray,
                                             originalDepth,
                                             x,
                                             y,
                                             &grayReference,
                                             &depthReference)) {
                continue;
            }

            const float current = originalGray.at<float>(y, x);
            const float diff = current - grayReference;
            reference[static_cast<size_t>(x)] = grayReference;
            referenceDepth[static_cast<size_t>(x)] = depthReference;
            excess[static_cast<size_t>(x)] = diff;
            if (diff > kProjectionRowArtifactMinExcess) {
                excessMask[static_cast<size_t>(x)] = 1;
            }
            if (current >= strongKeepThreshold) {
                strongMask[static_cast<size_t>(x)] = 1;
            }
        }

        for (int xStrong = 0; xStrong < gray.cols; ++xStrong) {
            if (!strongMask[static_cast<size_t>(xStrong)]) {
                continue;
            }
            const int begin = std::max(0, xStrong - kProjectionRowArtifactProtectRadiusX);
            const int end = std::min(gray.cols - 1, xStrong + kProjectionRowArtifactProtectRadiusX);
            for (int xx = begin; xx <= end; ++xx) {
                protectMask[static_cast<size_t>(xx)] = 1;
            }
        }

        for (int xCandidate = 0; xCandidate < gray.cols; ++xCandidate) {
            if (excessMask[static_cast<size_t>(xCandidate)]) {
                runMask[static_cast<size_t>(xCandidate)] = 1;
            }
        }

        int x = 0;
        while (x < gray.cols) {
            if (!runMask[static_cast<size_t>(x)]) {
                ++x;
                continue;
            }

            const int runStart = x;
            float runExcessSum = 0.0f;
            int protectedPixels = 0;
            while (x < gray.cols && runMask[static_cast<size_t>(x)]) {
                runExcessSum += excess[static_cast<size_t>(x)];
                if (protectMask[static_cast<size_t>(x)]) {
                    ++protectedPixels;
                }
                ++x;
            }
            const int runEnd = x - 1;
            const int runLength = runEnd - runStart + 1;
            if (runLength < kProjectionRowArtifactMinRunColumns) {
                continue;
            }

            const float runMeanExcess = runExcessSum / static_cast<float>(runLength);
            const float protectedRatio =
                static_cast<float>(protectedPixels) / static_cast<float>(runLength);
            if (runMeanExcess < kProjectionRowArtifactMeanExcess ||
                protectedRatio > kProjectionRowArtifactMaxProtectedRatio) {
                continue;
            }

            for (int xx = runStart; xx <= runEnd; ++xx) {
                if (!protectMask[static_cast<size_t>(xx)]) {
                    repairMask[static_cast<size_t>(xx)] = 1;
                    ++candidatePixels;
                    candidateExcessSum += excess[static_cast<size_t>(xx)];
                }
            }
        }

        if (candidatePixels < minCoveragePixels ||
            candidateExcessSum / static_cast<float>(std::max(1, candidatePixels)) <
                kProjectionRowArtifactMeanExcess) {
            continue;
        }

        ++stats.repairedRows;
        for (int xRepair = 0; xRepair < gray.cols; ++xRepair) {
            if (!repairMask[static_cast<size_t>(xRepair)]) {
                continue;
            }

            const float current = originalGray.at<float>(y, xRepair);
            const float grayReference = reference[static_cast<size_t>(xRepair)];
            const float suppressed =
                grayReference + (current - grayReference) * (1.0f - kProjectionRowArtifactBlend);
            gray.at<float>(y, xRepair) = std::min(current, std::max(grayReference, suppressed));
            depthIndex.at<int>(y, xRepair) = referenceDepth[static_cast<size_t>(xRepair)];
            ++stats.repairedPixels;
        }
    }

    return stats;
}

cv::Mat projectColor(const std::vector<float> &smoothed,
                     const VolumeGeometry &geometry,
                     cv::Mat *grayscaleProjection,
                     cv::Mat *depthColorProjection,
                     ProjectionArtifactSuppressionStats *artifactStats)
{
    const int depth = geometry.projectionDepth;
    // Saved en face projections use image columns as X/B-scan positions and rows as Y positions.
    cv::Mat gray(geometry.cscanLenEff, geometry.bscanLen, CV_32F, cv::Scalar(0));
    cv::Mat depthIndex(geometry.cscanLenEff, geometry.bscanLen, CV_32S, cv::Scalar(0));

    auto flatIndex = [depth, &geometry](int z, int x, int y) {
        return static_cast<size_t>(z) + static_cast<size_t>(depth) *
            (static_cast<size_t>(x) + static_cast<size_t>(geometry.bscanLen) * y);
    };

    parallelForRange(static_cast<size_t>(geometry.cscanLenEff), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            for (int x = 0; x < geometry.bscanLen; ++x) {
                float top[kProjectionMaxAverage];
                std::fill(top, top + kProjectionMaxAverage, -std::numeric_limits<float>::max());
                float maxValue = -std::numeric_limits<float>::max();
                int maxZ = 0;
                for (int z = 0; z < depth; ++z) {
                    const float value = smoothed[flatIndex(z, x, y)];
                    if (value > maxValue) {
                        maxValue = value;
                        maxZ = z;
                    }
                    for (int i = 0; i < kProjectionMaxAverage; ++i) {
                        if (value > top[i]) {
                            for (int j = kProjectionMaxAverage - 1; j > i; --j) {
                                top[j] = top[j - 1];
                            }
                            top[i] = value;
                            break;
                        }
                    }
                }

                float avg = 0.0f;
                for (float value : top) {
                    avg += value;
                }
                avg /= kProjectionMaxAverage;
                gray.at<float>(y, x) = avg;
                depthIndex.at<int>(y, x) = maxZ;
            }
        }
    }, 1, true);

    double grayMinDouble = 0.0;
    double grayMaxDouble = 0.0;
    cv::minMaxLoc(gray, &grayMinDouble, &grayMaxDouble);
    const float grayMin = static_cast<float>(grayMinDouble);
    const float grayMax = static_cast<float>(grayMaxDouble);

    const ProjectionArtifactSuppressionStats stats =
        suppressProjectionRowArtifacts(gray, depthIndex);
    if (artifactStats) {
        *artifactStats = stats;
    }

    cv::Mat output(geometry.cscanLenEff, geometry.bscanLen, CV_8UC3);
    cv::Mat grayscaleOutput;
    if (grayscaleProjection) {
        grayscaleOutput = cv::Mat(geometry.cscanLenEff, geometry.bscanLen, CV_8UC1);
    }
    cv::Mat depthColorOutput;
    if (depthColorProjection) {
        depthColorOutput = cv::Mat(geometry.cscanLenEff, geometry.bscanLen, CV_8UC3);
    }
    const float grayScale = grayMax > grayMin ? 1.0f / (grayMax - grayMin) : 1.0f;
    parallelForRange(static_cast<size_t>(geometry.cscanLenEff), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            for (int x = 0; x < geometry.bscanLen; ++x) {
                float intensity = (gray.at<float>(y, x) - grayMin) * grayScale;
                intensity = clampFloat((intensity - 0.3f) / 0.6f, 0.0f, 1.0f);
                const uint8_t grayByte = toByte(intensity);

                float depthNorm = static_cast<float>(depthIndex.at<int>(y, x) + 1) / static_cast<float>(depth);
                depthNorm = clampFloat((depthNorm - 0.01f) / 0.98f, 0.0f, 1.0f);
                int cmapIndex = static_cast<int>(std::round(depthNorm * (CMAP_ROWS - 1)));
                cmapIndex = std::max(0, std::min(cmapIndex, CMAP_ROWS - 1));

                cv::Vec3b &pixel = output.at<cv::Vec3b>(y, x);
                pixel[0] = toByte(static_cast<float>(cmap[cmapIndex][2]) * intensity);
                pixel[1] = toByte(static_cast<float>(cmap[cmapIndex][1]) * intensity);
                pixel[2] = toByte(static_cast<float>(cmap[cmapIndex][0]) * intensity);
                if (depthColorProjection) {
                    cv::Vec3b &depthPixel = depthColorOutput.at<cv::Vec3b>(y, x);
                    depthPixel[0] = toByte(static_cast<float>(cmap[cmapIndex][2]));
                    depthPixel[1] = toByte(static_cast<float>(cmap[cmapIndex][1]));
                    depthPixel[2] = toByte(static_cast<float>(cmap[cmapIndex][0]));
                }
                if (grayscaleProjection) {
                    grayscaleOutput.at<uint8_t>(y, x) = grayByte;
                }
            }
        }
    }, 1, true);
    if (grayscaleProjection) {
        *grayscaleProjection = grayscaleOutput;
    }
    if (depthColorProjection) {
        *depthColorProjection = depthColorOutput;
    }
    return output;
}

void processVolumeBlock(QFile &file,
                        int y,
                        const VolumeGeometry &geometry,
                        const std::vector<float> &spectralWindow,
                        std::vector<float> &structuralVolume,
                        std::vector<float> &flowVolume)
{
    const int lineCount = geometry.bscanLen * geometry.angioRep;
    std::vector<uint16_t> rawBlock(static_cast<size_t>(geometry.ascanLen) * lineCount);
    const size_t frameSize = static_cast<size_t>(geometry.cropDepth) * geometry.bscanLen;

    if (!readRawYBlock(file, y, geometry, rawBlock)) {
        throw std::runtime_error("Failed to read raw volume block.");
    }

    const cv::Mat complexFft = computeFftRows(rawBlock, spectralWindow, geometry);
    FrameSet frames(static_cast<size_t>(geometry.angioRep));
    for (int repeat = 0; repeat < geometry.angioRep; ++repeat) {
        frames[repeat].assign(frameSize, Complex(0.0f, 0.0f));
        for (int x = 0; x < geometry.bscanLen; ++x) {
            const cv::Vec2f *src = complexFft.ptr<cv::Vec2f>(repeat * geometry.bscanLen + x);
            for (int z = 0; z < geometry.cropDepth; ++z) {
                const cv::Vec2f value = src[geometry.cropStart0 + z];
                frames[repeat][static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) * x] =
                    Complex(value[0], value[1]);
            }
        }
    }

    registerRepeatedFrames(frames, geometry);
    computeStructuralAndFlow(frames, geometry, structuralVolume, flowVolume, y);
}

void packRegisteredFrames(const FrameSet &frames,
                          const VolumeGeometry &geometry,
                          std::vector<float> &packedFrames)
{
    const size_t frameSize = static_cast<size_t>(geometry.cropDepth) * geometry.bscanLen;
    packedFrames.assign(frameSize * geometry.angioRep * 2, 0.0f);
    for (int repeat = 0; repeat < geometry.angioRep; ++repeat) {
        for (int x = 0; x < geometry.bscanLen; ++x) {
            for (int z = 0; z < geometry.cropDepth; ++z) {
                const size_t frameIndex = static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) * x;
                const size_t packedIndex = (static_cast<size_t>(repeat) * frameSize + frameIndex) * 2;
                packedFrames[packedIndex] = frames[repeat][frameIndex].real();
                packedFrames[packedIndex + 1] = frames[repeat][frameIndex].imag();
            }
        }
    }
}

#ifdef VESSEL_USE_CUDA
bool processVolumeCudaBackend(const QString &filePath,
                              const VolumeGeometry &geometry,
                              const std::vector<float> &spectralWindow,
                              std::vector<float> &structuralVolume,
                              std::vector<float> &flowVolume,
                              const std::function<void(const QString&)> &log,
                              QString *failureReason)
{
    QLibrary cufftRuntime(QStringLiteral("cufft64_12"));
    if (!cufftRuntime.load()) {
        if (failureReason) {
            *failureReason = QStringLiteral("找不到 CUDA cuFFT 运行库 cufft64_12.dll");
        }
        return false;
    }

    std::string cudaDeviceDescription;
    std::string cudaError;
    VesselCudaProcessor *cudaProcessor = nullptr;
    if (!createVesselCudaProcessor(spectralWindow.data(),
                                   geometry.ascanLen,
                                   geometry.bscanLen,
                                   geometry.angioRep,
                                   geometry.cropDepth,
                                   &cudaProcessor,
                                   &cudaDeviceDescription,
                                   &cudaError)) {
        if (failureReason) {
            *failureReason = QString::fromLocal8Bit(cudaError.c_str());
        }
        return false;
    }

    try {
        log(QStringLiteral("使用 CUDA 重建 OCTA 体数据：%1。")
            .arg(QString::fromLocal8Bit(cudaDeviceDescription.c_str())));
        log(QStringLiteral("CUDA 配准在 GPU 内完成：FFT block 不再回传 CPU 做配准。"));

        QFile file;
        if (!openRawVolumeFile(file, filePath)) {
            throw std::runtime_error("Failed to open selected .3d file.");
        }

        const int lineCount = geometry.bscanLen * geometry.angioRep;
        std::vector<uint16_t> rawBlock(static_cast<size_t>(geometry.ascanLen) * lineCount);
        const size_t frameSize = static_cast<size_t>(geometry.cropDepth) * geometry.bscanLen;
        std::vector<float> structuralBlock(frameSize, 0.0f);
        std::vector<float> flowBlock(frameSize, 0.0f);

        int nextProgress = 100;
        for (int y = 0; y < geometry.cscanLenEff; ++y) {
            if (!readRawYBlock(file, y, geometry, rawBlock)) {
                throw std::runtime_error("Failed to read raw volume block.");
            }

            cudaError.clear();
            if (!computeRegisteredStructuralAndFlowCuda(cudaProcessor,
                                                        rawBlock.data(),
                                                        geometry.cropStart0,
                                                        kRegistrationMaxShiftPixels,
                                                        structuralBlock.data(),
                                                        flowBlock.data(),
                                                        &cudaError)) {
                throw std::runtime_error(cudaError);
            }

            cv::Mat structuralMat(geometry.cropDepth, geometry.bscanLen, CV_32F, structuralBlock.data());
            cv::Mat flowMat(geometry.cropDepth, geometry.bscanLen, CV_32F, flowBlock.data());
            copyBlockToVolume(structuralMat, flowMat, geometry, structuralVolume, flowVolume, y);

            const int done = y + 1;
            while (nextProgress <= geometry.cscanLenEff && done >= nextProgress) {
                log(QStringLiteral("已处理 %1 / %2 个 Bscan 板块！").arg(nextProgress).arg(geometry.cscanLenEff));
                nextProgress += 100;
            }
            QApplication::processEvents();
        }

        log(QStringLiteral("已处理所有 Bscan 板块！"));
        destroyVesselCudaProcessor(cudaProcessor);
        return true;
    } catch (const std::exception &ex) {
        destroyVesselCudaProcessor(cudaProcessor);
        if (failureReason) {
            *failureReason = QString::fromLocal8Bit(ex.what());
        }
        return false;
    }
}
#else
bool processVolumeCudaBackend(const QString &,
                              const VolumeGeometry &,
                              const std::vector<float> &,
                              std::vector<float> &,
                              std::vector<float> &,
                              const std::function<void(const QString&)> &,
                              QString *failureReason)
{
    if (failureReason) {
        *failureReason = QStringLiteral("当前构建未启用 CUDA 后端");
    }
    return false;
}
#endif

bool processVolumeGpu(const QString &filePath,
                      const VolumeGeometry &geometry,
                      const std::vector<float> &spectralWindow,
                      std::vector<float> &structuralVolume,
                      std::vector<float> &flowVolume,
                      const std::function<void(const QString&)> &log,
                      QString *failureReason)
{
    QString deviceDescription;
    QString reason;
    if (!initializeOpenClGpu(&deviceDescription, &reason)) {
        if (failureReason) {
            *failureReason = reason;
        }
        return false;
    }

    try {
        log(QStringLiteral("使用 GPU/OpenCL 重建 OCTA 体数据：%1。").arg(deviceDescription));
        log(QStringLiteral("OpenCL 配准使用 GPU 轴向相关性估计；通常只回传少量 shift 分数。"));
        GpuBlockProcessor processor(spectralWindow, geometry);

        QFile file;
        if (!openRawVolumeFile(file, filePath)) {
            throw std::runtime_error("Failed to open selected .3d file.");
        }

        int nextProgress = 100;
        for (int y = 0; y < geometry.cscanLenEff; ++y) {
            processor.processBlock(file, y, geometry, structuralVolume, flowVolume);
            const int done = y + 1;
            while (nextProgress <= geometry.cscanLenEff && done >= nextProgress) {
                log(QStringLiteral("已处理 %1 / %2 个 Bscan 板块！").arg(nextProgress).arg(geometry.cscanLenEff));
                nextProgress += 100;
            }
            QApplication::processEvents();
        }

        if (processor.cpuRegistrationFallbackCount() > 0) {
            log(QStringLiteral("OpenCL 配准有 %1 次触发 CPU phaseCorrelate 兜底。")
                .arg(processor.cpuRegistrationFallbackCount()));
        }
        log(QStringLiteral("已处理所有 Bscan 板块！"));
        return true;
    } catch (const cv::Exception &ex) {
        if (failureReason) {
            *failureReason = QStringLiteral("OpenCL 处理失败：%1").arg(QString::fromLocal8Bit(ex.what()));
        }
    } catch (const std::exception &ex) {
        if (failureReason) {
            *failureReason = QStringLiteral("OpenCL 处理失败：%1").arg(QString::fromLocal8Bit(ex.what()));
        }
    }

    return false;
}

void processVolumeCpu(const QString &filePath,
                      const VolumeGeometry &geometry,
                      const std::vector<float> &spectralWindow,
                      std::vector<float> &structuralVolume,
                      std::vector<float> &flowVolume,
                      const std::function<void(const QString&)> &log)
{
    const unsigned int hardwareThreads = std::thread::hardware_concurrency();
    const int availableThreads = hardwareThreads == 0 ? 2 : static_cast<int>(hardwareThreads);
    const int workerCount = std::max(1, std::min(geometry.cscanLenEff, availableThreads));
    const QString volumeFilePath = filePath;

    log(QStringLiteral("并行重建 OCTA 体数据：%1 个线程。").arg(workerCount));

    std::atomic<int> nextY(0);
    std::atomic<int> completed(0);
    std::atomic<int> activeWorkers(workerCount);
    std::atomic<bool> failed(false);
    std::exception_ptr firstError;
    std::mutex errorMutex;
    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    for (int worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&, volumeFilePath]() {
            try {
                QFile workerFile;
                if (!openRawVolumeFile(workerFile, volumeFilePath)) {
                    throw std::runtime_error("Failed to open selected .3d file.");
                }

                while (!failed.load()) {
                    const int y = nextY.fetch_add(1);
                    if (y >= geometry.cscanLenEff) {
                        break;
                    }

                    processVolumeBlock(workerFile, y, geometry, spectralWindow, structuralVolume, flowVolume);
                    completed.fetch_add(1);
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!firstError) {
                        firstError = std::current_exception();
                    }
                }
                failed.store(true);
            }

            activeWorkers.fetch_sub(1);
        });
    }

    int nextProgress = 100;
    while (activeWorkers.load() > 0) {
        const int done = completed.load();
        while (nextProgress <= geometry.cscanLenEff && done >= nextProgress) {
            log(QStringLiteral("已处理 %1 / %2 个 Bscan 板块！").arg(nextProgress).arg(geometry.cscanLenEff));
            nextProgress += 100;
        }
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    for (std::thread &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    if (firstError) {
        std::rethrow_exception(firstError);
    }

    const int done = completed.load();
    while (nextProgress <= geometry.cscanLenEff && done >= nextProgress) {
        log(QStringLiteral("已处理 %1 / %2 个 Bscan 板块！").arg(nextProgress).arg(geometry.cscanLenEff));
        nextProgress += 100;
    }
    if (done < geometry.cscanLenEff) {
        throw std::runtime_error("Failed to process all raw volume blocks.");
    }
    log(QStringLiteral("已处理所有 Bscan 板块！"));
}

void processVolume(const QString &filePath,
                   const VolumeGeometry &geometry,
                   const std::vector<float> &spectralWindow,
                   std::vector<float> &structuralVolume,
                   std::vector<float> &flowVolume,
                   const std::function<void(const QString&)> &log)
{
    QString gpuFailureReason;
    if (processVolumeGpu(filePath, geometry, spectralWindow, structuralVolume, flowVolume, log, &gpuFailureReason)) {
        return;
    }

    if (!gpuFailureReason.isEmpty()) {
        log(QStringLiteral("GPU/OpenCL 重建不可用，尝试 CUDA：%1").arg(gpuFailureReason));
    }
    std::fill(structuralVolume.begin(), structuralVolume.end(), 0.0f);
    std::fill(flowVolume.begin(), flowVolume.end(), 0.0f);

    QString cudaFailureReason;
    if (processVolumeCudaBackend(filePath, geometry, spectralWindow, structuralVolume, flowVolume, log, &cudaFailureReason)) {
        return;
    }

    if (!cudaFailureReason.isEmpty()) {
        log(QStringLiteral("CUDA 重建不可用，改用 CPU 并行：%1").arg(cudaFailureReason));
    }
    std::fill(structuralVolume.begin(), structuralVolume.end(), 0.0f);
    std::fill(flowVolume.begin(), flowVolume.end(), 0.0f);
    processVolumeCpu(filePath, geometry, spectralWindow, structuralVolume, flowVolume, log);
}

} // namespace

bool convertAngio3dToColorProjection(const QString &filePath,
                                     const VesselProjectionParams &params,
                                     const std::function<void(const QString&)> &log,
                                     const std::function<void(const QString&)> &fileLog,
                                     const std::function<bool(const VesselProjectionFileSizeInfo&)> &confirmSizeMismatch,
                                     QString *errorMessage)
{
    try {
        VolumeGeometry geometry;
        geometry.ascanLen = params.ascanLen;
        geometry.bscanLen = params.bscanLen;
        geometry.angioRep = params.angioRep;
        geometry.adFileOffsetFrames = params.adFileOffsetFrames;
        geometry.previewDepth = params.previewDepth;
        geometry.cropStart0 = params.cropZStart - 1;
        geometry.cropEnd0 = params.cropZEnd - 1;
        geometry.cropDepth = geometry.cropEnd0 - geometry.cropStart0 + 1;
        geometry.projectionDepth = params.projectionDepth;
        geometry.cscanLen = params.cscanLen;
        geometry.cscanLenEff = effectiveCscanLen(geometry.cscanLen, geometry);

        if (geometry.ascanLen <= 0 || geometry.bscanLen <= 0 ||
            geometry.cscanLen <= 0 || geometry.angioRep <= 0 ||
            geometry.adFileOffsetFrames < 0) {
            throw std::runtime_error("Invalid OCTA scan dimensions.");
        }
        if (geometry.previewDepth <= 0) {
            geometry.previewDepth = geometry.ascanLen;
        }
        geometry.previewDepth = std::min(geometry.previewDepth, geometry.ascanLen);

        if (geometry.cropStart0 < 0 || geometry.cropEnd0 >= geometry.ascanLen || geometry.cropDepth <= 0) {
            throw std::runtime_error("Invalid crop depth range.");
        }
        if (geometry.projectionDepth <= 0) {
            throw std::runtime_error("Invalid projection depth.");
        }
        if (geometry.cscanLenEff <= 0) {
            throw std::runtime_error("Invalid Cscan length.");
        }

        const QFileInfo fileInfo(filePath);
        const qint64 requiredSamples = requiredSamplesForCscanLen(geometry.cscanLen, geometry);
        const qint64 requiredBytes = requiredSamples * static_cast<qint64>(sizeof(uint16_t));
        if (fileInfo.size() != requiredBytes) {
            const qint64 fileSamples = fileInfo.size() / static_cast<qint64>(sizeof(uint16_t));
            const qint64 frameSamples = samplesPerBscanFrame(geometry);
            const qint64 wholeFrames = frameSamples > 0 ? fileSamples / frameSamples : 0;
            const qint64 usableFrames = wholeFrames - geometry.adFileOffsetFrames;
            if (usableFrames <= 0) {
                log(QStringLiteral("3D Angio 文件大小和程序要求不匹配。"));
                log(QStringLiteral("本文件大小：%1 个样点。").arg(fileSamples));
                log(QStringLiteral("程序要求：%1 个样点（Cscan 长度 %2）。")
                    .arg(requiredSamples)
                    .arg(geometry.cscanLen));
                throw std::runtime_error("文件大小和程序要求不匹配，并且无法推断有效的 Cscan 长度。");
            }

            const qint64 inferredEff = usableFrames / geometry.angioRep;
            if (inferredEff <= 0 || inferredEff > std::numeric_limits<int>::max() - skippedCscanBlocks(geometry)) {
                log(QStringLiteral("3D Angio 文件大小和程序要求不匹配。"));
                log(QStringLiteral("本文件大小：%1 个样点。").arg(fileSamples));
                log(QStringLiteral("程序要求：%1 个样点（Cscan 长度 %2）。")
                    .arg(requiredSamples)
                    .arg(geometry.cscanLen));
                throw std::runtime_error("文件大小和程序要求不匹配，并且推断出的 Cscan 长度无效。");
            }

            VesselProjectionFileSizeInfo sizeInfo;
            sizeInfo.fileSamples = fileSamples;
            sizeInfo.requiredSamples = requiredSamples;
            sizeInfo.programCscanLen = geometry.cscanLen;
            sizeInfo.inferredCscanLen = static_cast<int>(inferredEff) + skippedCscanBlocks(geometry);
            sizeInfo.inferredRequiredSamples = requiredSamplesForCscanLen(sizeInfo.inferredCscanLen, geometry);
            sizeInfo.unusedSamples = std::max<qint64>(0, fileSamples - sizeInfo.inferredRequiredSamples);
            sizeInfo.hasPartialFrame =
                (fileInfo.size() % static_cast<qint64>(sizeof(uint16_t)) != 0) ||
                (fileSamples % frameSamples != 0) ||
                (usableFrames % geometry.angioRep != 0);

            if (!confirmSizeMismatch || !confirmSizeMismatch(sizeInfo)) {
                throw std::runtime_error("文件大小和程序要求不匹配，用户取消使用推断出的 Cscan 长度。");
            }

            geometry.cscanLen = sizeInfo.inferredCscanLen;
            geometry.cscanLenEff = effectiveCscanLen(geometry.cscanLen, geometry);
            if (geometry.cscanLenEff <= 0) {
                throw std::runtime_error("推断出的 Cscan 长度无效。");
            }
            log(QStringLiteral("使用新的 Cscan 长度进行计算：%1。").arg(geometry.cscanLen));
        }

        const QString basePath = fileInfo.absolutePath() + "/" + fileInfo.completeBaseName();
        const QString previewPath = basePath + "_preview.png";
        const QString colorOutputPath = basePath + ".tiff";
        const QString depthOutputPath = basePath + "_depth.tiff";
        const QString grayscaleOutputPath = basePath + "_grayscale.tiff";
        const std::vector<float> spectralWindow = makeTukeyWindow(geometry.ascanLen, 0.25);

        auto stageStart = SteadyClock::now();
        if (params.generatePreviewImage) {
            QFile file;
            if (!openRawVolumeFile(file, filePath)) {
                throw std::runtime_error("Failed to open selected .3d file.");
            }

            log(QStringLiteral("开始生成预览图..."));
            stageStart = SteadyClock::now();
            savePreviewImage(file, previewPath, spectralWindow, geometry, log, fileLog);
            logElapsed(log, QStringLiteral("生成预览图"), elapsedSeconds(stageStart));
        } else {
            log(QStringLiteral("已跳过多帧平均预览图。"));
        }

        log(QStringLiteral("开始重建 OCTA 体数据..."));
        std::vector<float> structuralVolume(geometry.voxelCount(), 0.0f);
        std::vector<float> flowVolume(geometry.voxelCount(), 0.0f);
        stageStart = SteadyClock::now();
        processVolume(filePath, geometry, spectralWindow, structuralVolume, flowVolume, log);
        logElapsed(log, QStringLiteral("重建与配准"), elapsedSeconds(stageStart));

        log(QStringLiteral("归一化结构/血流体数据..."));
        stageStart = SteadyClock::now();
        normalizeAmplitudeVolumes(structuralVolume, flowVolume);
        logElapsed(log, QStringLiteral("归一化"), elapsedSeconds(stageStart));
        QApplication::processEvents();

        log(QStringLiteral("检测组织表面..."));
        QApplication::processEvents();
        stageStart = SteadyClock::now();
        SurfaceRepairStats surfaceRepairStats;
        const std::vector<float> surface = estimateSurface(structuralVolume, geometry, &surfaceRepairStats);
        if (surfaceRepairStats.flatRows > 0 || surfaceRepairStats.repairedPoints > 0) {
            log(QStringLiteral("表面连续性修补：修补 %1 个可疑 C-scan 行，%2 个表面点。")
                .arg(surfaceRepairStats.flatRows)
                .arg(surfaceRepairStats.repairedPoints));
        }
        logElapsed(log, QStringLiteral("检测表面"), elapsedSeconds(stageStart));
        QApplication::processEvents();

        stageStart = SteadyClock::now();
        saveSurfaceChecks(structuralVolume, surface, geometry, basePath, log, fileLog);
        logElapsed(log, QStringLiteral("保存表面检查图"), elapsedSeconds(stageStart));
        QApplication::processEvents();

        const bool generateAnyFlowSpeedImage =
            params.generatePixelWiseFlowSpeedImage ||
            params.generateAveragedFlowSpeedImage ||
            params.generateSegmentWiseFlowSpeedImage ||
            params.generateFlowSpeedFitCorrelationImage;
        if (generateAnyFlowSpeedImage) {
            log(QStringLiteral("开始生成 TAC 血流速度图（相对速度 alpha）..."));
            FlowSpeedCalculation::Geometry flowSpeedGeometry;
            flowSpeedGeometry.ascanLen = geometry.ascanLen;
            flowSpeedGeometry.bscanLen = geometry.bscanLen;
            flowSpeedGeometry.cscanLenEff = geometry.cscanLenEff;
            flowSpeedGeometry.angioRep = geometry.angioRep;
            flowSpeedGeometry.adFileOffsetFrames = geometry.adFileOffsetFrames;
            flowSpeedGeometry.cropStart0 = geometry.cropStart0;
            flowSpeedGeometry.cropDepth = geometry.cropDepth;
            flowSpeedGeometry.projectionDepth = geometry.projectionDepth;
            flowSpeedGeometry.surfaceZShift = kSurfaceZShift;
            flowSpeedGeometry.backgroundSubtractionWeight =
                static_cast<float>(kBgSubtractionWeight);
            FlowSpeedCalculation::OutputOptions flowSpeedOptions;
            flowSpeedOptions.generatePixelWiseImage = params.generatePixelWiseFlowSpeedImage;
            flowSpeedOptions.generateAveragedImage = params.generateAveragedFlowSpeedImage;
            flowSpeedOptions.generateSegmentWiseImage = params.generateSegmentWiseFlowSpeedImage;
            flowSpeedOptions.generateFitCorrelationImage = params.generateFlowSpeedFitCorrelationImage;
            flowSpeedOptions.useSkeletonDenoise = params.useFlowSpeedSkeletonDenoise;
            flowSpeedOptions.useManualMask = params.useFlowSpeedManualMask;
            flowSpeedOptions.cropTop = params.flowSpeedCropTop;
            flowSpeedOptions.cropBottom = params.flowSpeedCropBottom;
            flowSpeedOptions.cropLeft = params.flowSpeedCropLeft;
            flowSpeedOptions.cropRight = params.flowSpeedCropRight;
            FlowSpeedCalculation::saveProjection(filePath,
                                                 basePath,
                                                 flowSpeedGeometry,
                                                 flowSpeedOptions,
                                                 spectralWindow,
                                                 structuralVolume,
                                                 flowVolume,
                                                 surface,
                                                 log,
                                                 fileLog);
            QApplication::processEvents();
        } else {
            log(QStringLiteral("已跳过 TAC 血流速度图。"));
        }

        log(QStringLiteral("拉平血流体数据..."));
        stageStart = SteadyClock::now();
        std::vector<float> flattened = flattenFlowVolume(flowVolume, structuralVolume, surface, geometry);
        logElapsed(log, QStringLiteral("拉平血流体数据"), elapsedSeconds(stageStart));
        flowVolume.clear();
        flowVolume.shrink_to_fit();
        structuralVolume.clear();
        structuralVolume.shrink_to_fit();
        QApplication::processEvents();

        log(QStringLiteral("生成彩色血管投影图..."));
        stageStart = SteadyClock::now();
        normalizeFlattened(flattened);
        std::vector<float> smoothed = smooth3x3x3(flattened, geometry);
        flattened.clear();
        flattened.shrink_to_fit();
        cv::Mat grayscaleProjection;
        cv::Mat depthColorProjection;
        ProjectionArtifactSuppressionStats projectionArtifactStats;
        cv::Mat colorProjection = projectColor(
            smoothed,
            geometry,
            params.generateGrayscaleImage ? &grayscaleProjection : nullptr,
            &depthColorProjection,
            &projectionArtifactStats);
        if (projectionArtifactStats.repairedRows > 0) {
            log(QStringLiteral("行级伪影抑制：修补 %1 个可疑 Bscan 行，%2 个投影像素。")
                .arg(projectionArtifactStats.repairedRows)
                .arg(projectionArtifactStats.repairedPixels));
        }
        logElapsed(log, QStringLiteral("生成血管图"), elapsedSeconds(stageStart));

        const std::vector<int> tiffParams = projectionTiffEncodeParams();
        if (!writeImageFile(colorOutputPath, colorProjection, tiffParams)) {
            throw std::runtime_error("Failed to save color projection image.");
        }
        log(QStringLiteral("彩色血管投影图已保存：%1").arg(outputImageName(colorOutputPath)));
        if (fileLog) {
            fileLog(QStringLiteral("Saved output full path: %1").arg(colorOutputPath));
        }
        if (!writeImageFile(depthOutputPath, depthColorProjection, tiffParams)) {
            throw std::runtime_error("Failed to save depth projection image.");
        }
        log(QStringLiteral("深度颜色投影图已保存：%1").arg(outputImageName(depthOutputPath)));
        if (fileLog) {
            fileLog(QStringLiteral("Saved output full path: %1").arg(depthOutputPath));
        }
        if (params.generateGrayscaleImage) {
            if (!writeImageFile(grayscaleOutputPath, grayscaleProjection, tiffParams)) {
                throw std::runtime_error("Failed to save grayscale projection image.");
            }
            log(QStringLiteral("血管灰度图已保存：%1").arg(outputImageName(grayscaleOutputPath)));
            if (fileLog) {
                fileLog(QStringLiteral("Saved output full path: %1").arg(grayscaleOutputPath));
            }
        }
        return true;
    } catch (const std::bad_alloc &) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("内存不足，无法完成血管投影转换。");
        }
    } catch (const std::exception &ex) {
        if (errorMessage) {
            *errorMessage = QString::fromLocal8Bit(ex.what());
        }
    }
    return false;
}
