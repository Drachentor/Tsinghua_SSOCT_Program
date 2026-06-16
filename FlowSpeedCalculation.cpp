#include "FlowSpeedCalculation.h"
#include "FlowSpeedMaskDialog.h"
#include "VesselFlowShared.h"
#include "VesselSegmenter.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>

#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace FlowSpeedCalculation {
namespace {

const int kAdPlotOffset = 2048;
const int kAdBitShift = 4;
const float kAdScale = static_cast<float>(1 << kAdBitShift);
const int kProjectionTiffDpi = 600;
const int kRegistrationMaxShiftPixels = 8;
const float kFlowSpeedFitMinAlpha = 0.02f;
const float kFlowSpeedFitMaxAlpha = 12.0f;
const int kFlowSpeedFitGridCount = 64;
const float kFlowSpeedMaxNormalizedOcta = 0.995f;
const float kFlowSpeedMinProjectionWeight = 1.0e-3f;
const double kFlowSpeedDisplayLowPercentile = 5.0;
const double kFlowSpeedDisplayHighPercentile = 95.0;
const double kSegmentWiseEnhancedLowPercentile = 2.0;
const double kSegmentWiseEnhancedHighPercentile = 90.0;
const float kSegmentWiseEnhancedDisplayGamma = 0.65f;
const float kSegmentWiseMinFitCorrelationForDisplay = 0.30f;
const double kFlowSpeedMaskConfidenceLowPercentile = 55.0;
const double kFlowSpeedMaskConfidenceHighPercentile = 99.7;
const double kFlowSpeedBrightnessLowPercentile = 1.0;
const double kFlowSpeedBrightnessHighPercentile = 99.5;
const int kFlowSpeedMaskMinThreshold8 = 32;
const int kFlowSpeedMaskMinAreaPixels = 12;
const int kSkeletonDenoiseSpurPruneIterations = 4;
const int kSkeletonDenoiseMinSkeletonSegmentPixels = 12;
const int kSkeletonDenoiseMinAssignedPixels = 16;
const int kSkeletonDenoiseDilateRadius = 5;
const int kSkeletonDenoiseMinPixels = 32;
const int kFlowSpeedVesselProjectionTopAverage = 3;
const double kFlowSpeedVesselProjectionLowPercentile = 40.0;
const double kFlowSpeedVesselProjectionHighPercentile = 99.9;
const uint8_t kFlowSpeedMinVisibleBrightness = 32;

using Complex = std::complex<float>;
using FrameSet = std::vector<std::vector<Complex>>;
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

size_t volumeIndex(const Geometry &geometry, int z, int x, int y)
{
    return static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) *
        (static_cast<size_t>(x) + static_cast<size_t>(geometry.bscanLen) * y);
}

bool openRawVolumeFile(QFile &file, const QString &filePath)
{
    file.setFileName(filePath);
    return file.open(QIODevice::ReadOnly);
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

bool readRawYBlock(QFile &file,
                   int yIndex0,
                   const Geometry &geometry,
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

std::string openClFloatLiteral(float value)
{
    std::ostringstream literal;
    literal.imbue(std::locale::classic());
    literal << std::setprecision(9) << value;

    std::string text = literal.str();
    if (text.find_first_of(".eE") == std::string::npos) {
        text += ".0";
    }
    text += "f";
    return text;
}

std::string openClSource(const Geometry &geometry)
{
    std::ostringstream source;
    source << "#define BSCAN_LEN " << geometry.bscanLen << "\n"
           << "#define ASCAN_LEN " << geometry.ascanLen << "\n"
           << "#define ANGIO_REP " << geometry.angioRep << "\n"
           << "#define FLOW_SPEED_FIT_MIN_ALPHA " << openClFloatLiteral(kFlowSpeedFitMinAlpha) << "\n"
           << "#define FLOW_SPEED_FIT_MAX_ALPHA " << openClFloatLiteral(kFlowSpeedFitMaxAlpha) << "\n"
           << "#define FLOW_SPEED_FIT_GRID_COUNT " << kFlowSpeedFitGridCount << "\n"
           << "#define FLOW_SPEED_MAX_NORMALIZED_OCTA " << openClFloatLiteral(kFlowSpeedMaxNormalizedOcta) << "\n"
           << "#define FLOW_SPEED_MIN_PROJECTION_WEIGHT " << openClFloatLiteral(kFlowSpeedMinProjectionWeight) << "\n";
    source << R"CLC(
inline float2 cadd(float2 a, float2 b) { return (float2)(a.x + b.x, a.y + b.y); }
inline float2 cscale(float2 a, float s) { return (float2)(a.x * s, a.y * s); }
inline float cabsval(float2 a)
{
    return sqrt(a.x * a.x + a.y * a.y);
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

__kernel void packRegisteredFrames(__global const uchar *fft_data,
                                   int fft_step,
                                   int fft_offset,
                                   __global const float *shifts,
                                   __global uchar *registered_data,
                                   int registered_step,
                                   int registered_offset,
                                   int cropStart,
                                   int cropDepth)
{
    const int x = get_global_id(0);
    const int z = get_global_id(1);
    const int repeat = get_global_id(2);
    if (x >= BSCAN_LEN || z >= cropDepth || repeat >= ANGIO_REP) {
        return;
    }

    const int row = repeat * BSCAN_LEN + x;
    const float2 value = sampleShifted(fft_data, fft_step, fft_offset, repeat, x,
                                       cropStart, cropDepth, (float)z + shifts[repeat]);
    __global float2 *dstRow =
        (__global float2 *)(registered_data + registered_offset + row * registered_step);
    dstRow[z] = value;
}

inline float fitTemporalAutocorrelationAlphaGpu(float normalizedOcta[ANGIO_REP - 1],
                                                float lagWeights[ANGIO_REP - 1])
{
    float totalWeight = 0.0f;
    float maxOcta = 0.0f;
    for (int i = 0; i < ANGIO_REP - 1; ++i) {
        if (lagWeights[i] <= 0.0f || !isfinite(normalizedOcta[i])) {
            continue;
        }
        totalWeight += lagWeights[i];
        maxOcta = fmax(maxOcta, normalizedOcta[i]);
    }
    if (totalWeight <= 0.0f || maxOcta <= 1.0e-6f) {
        return 0.0f;
    }

    if (ANGIO_REP == 2) {
        const float decorrelation = clamp(normalizedOcta[0], 0.0f, FLOW_SPEED_MAX_NORMALIZED_OCTA);
        return fmin(FLOW_SPEED_FIT_MAX_ALPHA,
                    -log(fmax(1.0e-6f, 1.0f - decorrelation)));
    }

    const float logMin = log(FLOW_SPEED_FIT_MIN_ALPHA);
    const float logMax = log(FLOW_SPEED_FIT_MAX_ALPHA);
    float bestAlpha = 0.0f;
    float bestError = 3.402823466e+38f;

    for (int gridIndex = 0; gridIndex < FLOW_SPEED_FIT_GRID_COUNT; ++gridIndex) {
        const float t = (float)gridIndex / (float)(FLOW_SPEED_FIT_GRID_COUNT - 1);
        const float alpha = exp(logMin + t * (logMax - logMin));
        float numerator = 0.0f;
        float denominator = 0.0f;

        for (int lagIndex = 0; lagIndex < ANGIO_REP - 1; ++lagIndex) {
            const float weight = lagWeights[lagIndex];
            if (weight <= 0.0f) {
                continue;
            }
            const float tau = (float)(lagIndex + 1);
            const float basis = 1.0f - exp(-alpha * tau);
            numerator += weight * normalizedOcta[lagIndex] * basis;
            denominator += weight * basis * basis;
        }
        if (denominator <= 0.0f) {
            continue;
        }

        float beta = clamp(numerator / denominator, 0.0f, 1.25f);
        float error = 0.0f;
        for (int lagIndex = 0; lagIndex < ANGIO_REP - 1; ++lagIndex) {
            const float weight = lagWeights[lagIndex];
            if (weight <= 0.0f) {
                continue;
            }
            const float tau = (float)(lagIndex + 1);
            const float basis = 1.0f - exp(-alpha * tau);
            const float residual = normalizedOcta[lagIndex] - beta * basis;
            error += weight * residual * residual;
        }

        if (error < bestError) {
            bestError = error;
            bestAlpha = alpha;
        }
    }

    return bestAlpha;
}

inline void estimateVoxelFlowSpeedOctaGpu(__global const uchar *fft_data,
                                          int fft_step,
                                          int fft_offset,
                                          __global const float *shifts,
                                          int cropStart,
                                          int cropDepth,
                                          int z,
                                          int x,
                                          __private float *normalizedOcta,
                                          __private float *lagWeights)
{
    for (int i = 0; i < ANGIO_REP - 1; ++i) {
        normalizedOcta[i] = 0.0f;
        lagWeights[i] = 0.0f;
    }

    for (int lag = 1; lag < ANGIO_REP; ++lag) {
        float weightedOcta = 0.0f;
        float totalWeight = 0.0f;

        for (int repeat = 0; repeat + lag < ANGIO_REP; ++repeat) {
            const float2 a = sampleShifted(fft_data, fft_step, fft_offset, repeat, x,
                                           cropStart, cropDepth, (float)z + shifts[repeat]);
            const float2 b = sampleShifted(fft_data, fft_step, fft_offset, repeat + lag, x,
                                           cropStart, cropDepth, (float)z + shifts[repeat + lag]);
            const float energyA = a.x * a.x + a.y * a.y;
            const float energyB = b.x * b.x + b.y * b.y;
            const float denominator = energyA + energyB + 1.0e-20f;
            if (denominator <= 1.0e-20f) {
                continue;
            }

            const float correlation = 2.0f * (a.x * b.x + a.y * b.y) / denominator;
            const float decorrelation = 1.0f - clamp(correlation, -1.0f, 1.0f);
            const float pairWeight = sqrt(fmax(0.0f, energyA * energyB));
            if (pairWeight <= 0.0f || !isfinite(pairWeight)) {
                continue;
            }

            weightedOcta += pairWeight * clamp(decorrelation, 0.0f, FLOW_SPEED_MAX_NORMALIZED_OCTA);
            totalWeight += pairWeight;
        }

        if (totalWeight > 0.0f) {
            normalizedOcta[lag - 1] = weightedOcta / totalWeight;
            lagWeights[lag - 1] = totalWeight;
        }
    }
}

inline float estimateVoxelFlowSpeedAlphaGpu(__global const uchar *fft_data,
                                            int fft_step,
                                            int fft_offset,
                                            __global const float *shifts,
                                            int cropStart,
                                            int cropDepth,
                                            int z,
                                            int x)
{
    float normalizedOcta[ANGIO_REP - 1];
    float lagWeights[ANGIO_REP - 1];
    estimateVoxelFlowSpeedOctaGpu(fft_data,
                                  fft_step,
                                  fft_offset,
                                  shifts,
                                  cropStart,
                                  cropDepth,
                                  z,
                                  x,
                                  normalizedOcta,
                                  lagWeights);

    return fitTemporalAutocorrelationAlphaGpu(normalizedOcta, lagWeights);
}

__kernel void computeProjectedFlowSpeed(__global const uchar *fft_data,
                                        int fft_step,
                                        int fft_offset,
                                        __global const float *shifts,
                                        __global const uchar *structural_data,
                                        int structural_step,
                                        int structural_offset,
                                        __global const uchar *flow_data,
                                        int flow_step,
                                        int flow_offset,
                                        __global const float *surface,
                                        __global uchar *speed_data,
                                        int speed_step,
                                        int speed_offset,
                                        __global uchar *confidence_data,
                                        int confidence_step,
                                        int confidence_offset,
                                        __global uchar *lag_octa_data,
                                        int lag_octa_step,
                                        int lag_octa_offset,
                                        __global uchar *lag_weight_data,
                                        int lag_weight_step,
                                        int lag_weight_offset,
                                        int cropStart,
                                        int cropDepth,
                                        int projectionDepth,
                                        int surfaceZShift,
                                        float backgroundSubtractionWeight)
{
    const int x = get_global_id(0);
    if (x >= BSCAN_LEN) {
        return;
    }

    const int surfaceShifted = (int)floor(surface[x] + (float)surfaceZShift + 0.5f);
    float weightedAlpha = 0.0f;
    float weightSum = 0.0f;
    float projectedOcta[ANGIO_REP - 1];
    float projectedLagWeight[ANGIO_REP - 1];
    for (int lagIndex = 0; lagIndex < ANGIO_REP - 1; ++lagIndex) {
        projectedOcta[lagIndex] = 0.0f;
        projectedLagWeight[lagIndex] = 0.0f;
    }

    for (int d = 0; d < projectionDepth; ++d) {
        const int sourceZ = clamp(d + surfaceShifted, 0, cropDepth - 1);
        __global const float *structuralRow =
            (__global const float *)(structural_data + structural_offset + sourceZ * structural_step);
        __global const float *flowRow =
            (__global const float *)(flow_data + flow_offset + sourceZ * flow_step);
        const float weight = flowRow[x] - backgroundSubtractionWeight * structuralRow[x];
        if (!isfinite(weight) || weight <= FLOW_SPEED_MIN_PROJECTION_WEIGHT) {
            continue;
        }

        float normalizedOcta[ANGIO_REP - 1];
        float lagWeights[ANGIO_REP - 1];
        estimateVoxelFlowSpeedOctaGpu(fft_data,
                                      fft_step,
                                      fft_offset,
                                      shifts,
                                      cropStart,
                                      cropDepth,
                                      sourceZ,
                                      x,
                                      normalizedOcta,
                                      lagWeights);
        const float alpha = fitTemporalAutocorrelationAlphaGpu(normalizedOcta, lagWeights);
        if (!isfinite(alpha) || alpha <= 0.0f) {
            continue;
        }

        weightedAlpha += weight * alpha;
        weightSum += weight;
        for (int lagIndex = 0; lagIndex < ANGIO_REP - 1; ++lagIndex) {
            if (lagWeights[lagIndex] <= 0.0f || !isfinite(normalizedOcta[lagIndex])) {
                continue;
            }
            const float lagWeight = weight * lagWeights[lagIndex];
            projectedOcta[lagIndex] += lagWeight * normalizedOcta[lagIndex];
            projectedLagWeight[lagIndex] += lagWeight;
        }
    }

    __global float *speedRow =
        (__global float *)(speed_data + speed_offset);
    __global float *confidenceRow =
        (__global float *)(confidence_data + confidence_offset);
    speedRow[x] = weightSum > 0.0f ? weightedAlpha / weightSum : 0.0f;
    confidenceRow[x] = weightSum > 0.0f ?
        weightSum / (float)max(1, projectionDepth) : 0.0f;
    for (int lagIndex = 0; lagIndex < ANGIO_REP - 1; ++lagIndex) {
        __global float *octaRow =
            (__global float *)(lag_octa_data + lag_octa_offset + lagIndex * lag_octa_step);
        __global float *weightRow =
            (__global float *)(lag_weight_data + lag_weight_offset + lagIndex * lag_weight_step);
        octaRow[x] = projectedLagWeight[lagIndex] > 0.0f ?
            projectedOcta[lagIndex] / projectedLagWeight[lagIndex] : 0.0f;
        weightRow[x] = projectedLagWeight[lagIndex] > 0.0f ?
            projectedLagWeight[lagIndex] / (float)max(1, projectionDepth) : 0.0f;
    }
}
)CLC";
    return source.str();
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

class GpuRegistrationProcessor
{
public:
    GpuRegistrationProcessor(const std::vector<float> &spectralWindow,
                             const Geometry &geometry)
    {
        cv::Mat windowHost(1, geometry.ascanLen, CV_32F, const_cast<float*>(spectralWindow.data()));
        windowHost.copyTo(m_spectralWindow);

        const std::string sourceText = openClSource(geometry);
        const cv::ocl::ProgramSource source(sourceText.c_str());
        createOpenClKernel(m_rawToSpectra, "rawToSpectra", source);
        createOpenClKernel(m_extractMagnitude, "extractMagnitude", source);
        createOpenClKernel(m_shiftMagnitude, "shiftMagnitude", source);
        createOpenClKernel(m_estimateAxialShiftMetrics, "estimateAxialShiftMetrics", source);
        createOpenClKernel(m_packRegisteredFrames, "packRegisteredFrames", source);
        createOpenClKernel(m_computeProjectedFlowSpeed, "computeProjectedFlowSpeed", source);
    }

    void computeProjectedSpeedBlock(const std::vector<uint16_t> &rawBlock,
                                    const Geometry &geometry,
                                    const cv::Mat &structuralBlock,
                                    const cv::Mat &flowBlock,
                                    const cv::Mat &surfaceRow,
                                    cv::Mat &speedRow,
                                    cv::Mat &confidenceRow,
                                    cv::Mat &lagOctaRows,
                                    cv::Mat &lagWeightRows)
    {
        const int lineCount = geometry.bscanLen * geometry.angioRep;
        cv::Mat rawHost(lineCount, geometry.ascanLen, CV_16U, const_cast<uint16_t*>(rawBlock.data()));
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
            throw std::runtime_error("Failed to run OpenCL rawToSpectra kernel for flow speed.");
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

        cv::UMat structuralGpu;
        cv::UMat flowGpu;
        cv::UMat surfaceGpu;
        structuralBlock.copyTo(structuralGpu);
        flowBlock.copyTo(flowGpu);
        surfaceRow.copyTo(surfaceGpu);

        cv::UMat speedGpu(1, geometry.bscanLen, CV_32F);
        cv::UMat confidenceGpu(1, geometry.bscanLen, CV_32F);
        cv::UMat lagOctaGpu(geometry.angioRep - 1, geometry.bscanLen, CV_32F);
        cv::UMat lagWeightGpu(geometry.angioRep - 1, geometry.bscanLen, CV_32F);
        size_t speedGlobalSize[] = {static_cast<size_t>(geometry.bscanLen)};
        m_computeProjectedFlowSpeed.args(cv::ocl::KernelArg::ReadOnlyNoSize(complexFft),
                                         cv::ocl::KernelArg::PtrReadOnly(shiftsGpu),
                                         cv::ocl::KernelArg::ReadOnlyNoSize(structuralGpu),
                                         cv::ocl::KernelArg::ReadOnlyNoSize(flowGpu),
                                         cv::ocl::KernelArg::PtrReadOnly(surfaceGpu),
                                         cv::ocl::KernelArg::WriteOnlyNoSize(speedGpu),
                                         cv::ocl::KernelArg::WriteOnlyNoSize(confidenceGpu),
                                         cv::ocl::KernelArg::WriteOnlyNoSize(lagOctaGpu),
                                         cv::ocl::KernelArg::WriteOnlyNoSize(lagWeightGpu),
                                         geometry.cropStart0,
                                         geometry.cropDepth,
                                         geometry.projectionDepth,
                                         geometry.surfaceZShift,
                                         geometry.backgroundSubtractionWeight);
        if (!m_computeProjectedFlowSpeed.run_(1, speedGlobalSize, nullptr, true)) {
            throw std::runtime_error("Failed to run OpenCL computeProjectedFlowSpeed kernel.");
        }

        speedGpu.copyTo(speedRow);
        confidenceGpu.copyTo(confidenceRow);
        lagOctaGpu.copyTo(lagOctaRows);
        lagWeightGpu.copyTo(lagWeightRows);
    }

    FrameSet registeredFrames(const std::vector<uint16_t> &rawBlock,
                              const Geometry &geometry)
    {
        const int lineCount = geometry.bscanLen * geometry.angioRep;
        cv::Mat rawHost(lineCount, geometry.ascanLen, CV_16U, const_cast<uint16_t*>(rawBlock.data()));
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
            throw std::runtime_error("Failed to run OpenCL rawToSpectra kernel for flow speed.");
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

        cv::UMat registeredRows(lineCount, geometry.cropDepth, CV_32FC2);
        size_t packGlobalSize[] = {
            static_cast<size_t>(geometry.bscanLen),
            static_cast<size_t>(geometry.cropDepth),
            static_cast<size_t>(geometry.angioRep)
        };
        m_packRegisteredFrames.args(cv::ocl::KernelArg::ReadOnlyNoSize(complexFft),
                                    cv::ocl::KernelArg::PtrReadOnly(shiftsGpu),
                                    cv::ocl::KernelArg::WriteOnlyNoSize(registeredRows),
                                    geometry.cropStart0,
                                    geometry.cropDepth);
        if (!m_packRegisteredFrames.run_(3, packGlobalSize, nullptr, true)) {
            throw std::runtime_error("Failed to run OpenCL packRegisteredFrames kernel.");
        }

        cv::Mat registeredHost;
        registeredRows.copyTo(registeredHost);
        return framesFromRegisteredRows(registeredHost, geometry);
    }

    int cpuRegistrationFallbackCount() const
    {
        return m_cpuRegistrationFallbackCount;
    }

private:
    static FrameSet framesFromRegisteredRows(const cv::Mat &registeredRows,
                                            const Geometry &geometry)
    {
        const size_t frameSize = static_cast<size_t>(geometry.cropDepth) * geometry.bscanLen;
        FrameSet frames(static_cast<size_t>(geometry.angioRep));
        for (int repeat = 0; repeat < geometry.angioRep; ++repeat) {
            frames[static_cast<size_t>(repeat)].assign(frameSize, Complex(0.0f, 0.0f));
            for (int x = 0; x < geometry.bscanLen; ++x) {
                const cv::Vec2f *src = registeredRows.ptr<cv::Vec2f>(repeat * geometry.bscanLen + x);
                for (int z = 0; z < geometry.cropDepth; ++z) {
                    frames[static_cast<size_t>(repeat)]
                        [static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) * x] =
                        Complex(src[z][0], src[z][1]);
                }
            }
        }
        return frames;
    }

    void extractMagnitude(const cv::UMat &complexFft,
                          cv::UMat &magnitude,
                          int repeat,
                          const Geometry &geometry)
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
            throw std::runtime_error("Failed to run OpenCL extractMagnitude kernel for flow speed.");
        }
    }

    void shiftMagnitude(const cv::UMat &complexFft,
                        cv::UMat &magnitude,
                        int repeat,
                        float shift,
                        const Geometry &geometry)
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
            throw std::runtime_error("Failed to run OpenCL shiftMagnitude kernel for flow speed.");
        }
    }

    float estimateAxialShift(const cv::UMat &referenceMagnitude,
                             const cv::UMat &movingMagnitude,
                             const Geometry &geometry)
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
            throw std::runtime_error("Failed to run OpenCL estimateAxialShiftMetrics kernel for flow speed.");
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
    cv::ocl::Kernel m_packRegisteredFrames;
    cv::ocl::Kernel m_computeProjectedFlowSpeed;
    int m_cpuRegistrationFallbackCount = 0;
};

struct TemporalAutocorrelationFitResult
{
    float alpha = 0.0f;
    float beta = 0.0f;
    float correlation = 0.0f;
};

double weightedFitCorrelation(const std::vector<float> &normalizedOcta,
                              const std::vector<float> &lagWeights,
                              double alpha,
                              double beta)
{
    double totalWeight = 0.0;
    double observedMean = 0.0;
    double fittedMean = 0.0;
    int validCount = 0;
    for (size_t lagIndex = 0; lagIndex < normalizedOcta.size(); ++lagIndex) {
        const double weight = lagWeights[lagIndex];
        const double observed = normalizedOcta[lagIndex];
        if (weight <= 0.0 || !std::isfinite(observed)) {
            continue;
        }
        const double tau = static_cast<double>(lagIndex + 1);
        const double fitted = beta * (1.0 - std::exp(-alpha * tau));
        if (!std::isfinite(fitted)) {
            continue;
        }
        totalWeight += weight;
        observedMean += weight * observed;
        fittedMean += weight * fitted;
        ++validCount;
    }
    if (validCount < 2 || totalWeight <= 0.0) {
        return 0.0;
    }

    observedMean /= totalWeight;
    fittedMean /= totalWeight;
    double covariance = 0.0;
    double observedVariance = 0.0;
    double fittedVariance = 0.0;
    for (size_t lagIndex = 0; lagIndex < normalizedOcta.size(); ++lagIndex) {
        const double weight = lagWeights[lagIndex];
        const double observed = normalizedOcta[lagIndex];
        if (weight <= 0.0 || !std::isfinite(observed)) {
            continue;
        }
        const double tau = static_cast<double>(lagIndex + 1);
        const double fitted = beta * (1.0 - std::exp(-alpha * tau));
        if (!std::isfinite(fitted)) {
            continue;
        }
        const double observedDelta = observed - observedMean;
        const double fittedDelta = fitted - fittedMean;
        covariance += weight * observedDelta * fittedDelta;
        observedVariance += weight * observedDelta * observedDelta;
        fittedVariance += weight * fittedDelta * fittedDelta;
    }
    if (observedVariance <= 0.0 || fittedVariance <= 0.0) {
        return 0.0;
    }

    const double correlation = covariance / std::sqrt(observedVariance * fittedVariance);
    if (!std::isfinite(correlation)) {
        return 0.0;
    }
    return std::max(-1.0, std::min(1.0, correlation));
}

TemporalAutocorrelationFitResult fitTemporalAutocorrelationModel(
    const std::vector<float> &normalizedOcta,
    const std::vector<float> &lagWeights)
{
    TemporalAutocorrelationFitResult result;
    double totalWeight = 0.0;
    float maxOcta = 0.0f;
    for (size_t i = 0; i < normalizedOcta.size(); ++i) {
        if (lagWeights[i] <= 0.0f || !std::isfinite(normalizedOcta[i])) {
            continue;
        }
        totalWeight += lagWeights[i];
        maxOcta = std::max(maxOcta, normalizedOcta[i]);
    }
    if (totalWeight <= 0.0 || maxOcta <= 1.0e-6f) {
        return result;
    }

    if (normalizedOcta.size() == 1) {
        const float decorrelation = clampFloat(normalizedOcta[0], 0.0f, kFlowSpeedMaxNormalizedOcta);
        result.alpha = static_cast<float>(std::min<double>(
            kFlowSpeedFitMaxAlpha,
            -std::log(std::max(1.0e-6f, 1.0f - decorrelation))));
        result.beta = 1.0f;
        return result;
    }

    const double logMin = std::log(static_cast<double>(kFlowSpeedFitMinAlpha));
    const double logMax = std::log(static_cast<double>(kFlowSpeedFitMaxAlpha));
    double bestAlpha = 0.0;
    double bestBeta = 0.0;
    double bestError = std::numeric_limits<double>::max();

    for (int i = 0; i < kFlowSpeedFitGridCount; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kFlowSpeedFitGridCount - 1);
        const double alpha = std::exp(logMin + t * (logMax - logMin));
        double numerator = 0.0;
        double denominator = 0.0;

        for (size_t lagIndex = 0; lagIndex < normalizedOcta.size(); ++lagIndex) {
            const double weight = lagWeights[lagIndex];
            if (weight <= 0.0) {
                continue;
            }
            const double tau = static_cast<double>(lagIndex + 1);
            const double basis = 1.0 - std::exp(-alpha * tau);
            numerator += weight * normalizedOcta[lagIndex] * basis;
            denominator += weight * basis * basis;
        }
        if (denominator <= 0.0) {
            continue;
        }

        double beta = numerator / denominator;
        beta = std::max(0.0, std::min(beta, 1.25));

        double error = 0.0;
        for (size_t lagIndex = 0; lagIndex < normalizedOcta.size(); ++lagIndex) {
            const double weight = lagWeights[lagIndex];
            if (weight <= 0.0) {
                continue;
            }
            const double tau = static_cast<double>(lagIndex + 1);
            const double basis = 1.0 - std::exp(-alpha * tau);
            const double residual = static_cast<double>(normalizedOcta[lagIndex]) - beta * basis;
            error += weight * residual * residual;
        }

        if (error < bestError) {
            bestError = error;
            bestAlpha = alpha;
            bestBeta = beta;
        }
    }

    if (bestAlpha <= 0.0 || !std::isfinite(bestAlpha)) {
        return result;
    }
    result.alpha = static_cast<float>(bestAlpha);
    result.beta = static_cast<float>(bestBeta);
    result.correlation = static_cast<float>(std::max(
        0.0,
        weightedFitCorrelation(normalizedOcta, lagWeights, bestAlpha, bestBeta)));
    return result;
}

float fitTemporalAutocorrelationAlpha(const std::vector<float> &normalizedOcta,
                                      const std::vector<float> &lagWeights)
{
    return fitTemporalAutocorrelationModel(normalizedOcta, lagWeights).alpha;
}

float estimateVoxelFlowSpeedAlpha(const FrameSet &frames,
                                  const Geometry &geometry,
                                  int z,
                                  int x)
{
    const int repCount = geometry.angioRep;
    if (repCount < 2) {
        return 0.0f;
    }

    std::vector<float> normalizedOcta(static_cast<size_t>(repCount - 1), 0.0f);
    std::vector<float> lagWeights(static_cast<size_t>(repCount - 1), 0.0f);
    const size_t frameIndex = static_cast<size_t>(z) + static_cast<size_t>(geometry.cropDepth) * x;

    for (int lag = 1; lag < repCount; ++lag) {
        double weightedOcta = 0.0;
        double totalWeight = 0.0;

        for (int repeat = 0; repeat + lag < repCount; ++repeat) {
            const Complex a = frames[static_cast<size_t>(repeat)][frameIndex];
            const Complex b = frames[static_cast<size_t>(repeat + lag)][frameIndex];
            const double energyA = static_cast<double>(std::norm(a));
            const double energyB = static_cast<double>(std::norm(b));
            const double denominator = energyA + energyB + 1.0e-20;
            if (denominator <= 1.0e-20) {
                continue;
            }

            const double correlation =
                2.0 * static_cast<double>((std::conj(a) * b).real()) / denominator;
            const double decorrelation = 1.0 - std::max(-1.0, std::min(correlation, 1.0));
            const double pairWeight = std::sqrt(std::max(0.0, energyA * energyB));
            if (pairWeight <= 0.0 || !std::isfinite(pairWeight)) {
                continue;
            }

            weightedOcta += pairWeight *
                std::max(0.0, std::min(decorrelation, static_cast<double>(kFlowSpeedMaxNormalizedOcta)));
            totalWeight += pairWeight;
        }

        if (totalWeight > 0.0) {
            normalizedOcta[static_cast<size_t>(lag - 1)] =
                static_cast<float>(weightedOcta / totalWeight);
            lagWeights[static_cast<size_t>(lag - 1)] = static_cast<float>(totalWeight);
        }
    }

    return fitTemporalAutocorrelationAlpha(normalizedOcta, lagWeights);
}

void processProjectionBlock(QFile &file,
                            GpuRegistrationProcessor &processor,
                            int y,
                            const Geometry &geometry,
                            const std::vector<float> &structuralVolume,
                            const std::vector<float> &flowVolume,
                            const std::vector<float> &surface,
                            cv::Mat &speedAlphaMap,
                            cv::Mat &confidenceMap,
                            std::vector<cv::Mat> &lagOctaMaps,
                            std::vector<cv::Mat> &lagWeightMaps)
{
    const int lineCount = geometry.bscanLen * geometry.angioRep;
    std::vector<uint16_t> rawBlock(static_cast<size_t>(geometry.ascanLen) * lineCount);

    if (!readRawYBlock(file, y, geometry, rawBlock)) {
        throw std::runtime_error("Failed to read raw volume block for flow-speed projection.");
    }

    VesselFlowSharedGeometry sharedGeometry;
    sharedGeometry.cropDepth = geometry.cropDepth;
    sharedGeometry.bscanLen = geometry.bscanLen;
    sharedGeometry.cscanLenEff = geometry.cscanLenEff;

    cv::Mat structuralBlock;
    cv::Mat flowBlock;
    cv::Mat surfaceRow;
    copyVesselFlowProjectionBlock(structuralVolume,
                                  flowVolume,
                                  surface,
                                  sharedGeometry,
                                  y,
                                  structuralBlock,
                                  flowBlock,
                                  surfaceRow);

    cv::Mat speedRowGpu;
    cv::Mat confidenceRowGpu;
    cv::Mat lagOctaRowsGpu;
    cv::Mat lagWeightRowsGpu;
    processor.computeProjectedSpeedBlock(rawBlock,
                                         geometry,
                                         structuralBlock,
                                         flowBlock,
                                         surfaceRow,
                                         speedRowGpu,
                                         confidenceRowGpu,
                                         lagOctaRowsGpu,
                                         lagWeightRowsGpu);

    const float *speedSrc = speedRowGpu.ptr<float>(0);
    const float *confidenceSrc = confidenceRowGpu.ptr<float>(0);
    float *speedDst = speedAlphaMap.ptr<float>(y);
    float *confidenceDst = confidenceMap.ptr<float>(y);
    for (int x = 0; x < geometry.bscanLen; ++x) {
        speedDst[x] = speedSrc[x];
        confidenceDst[x] = confidenceSrc[x];
    }
    for (int lagIndex = 0; lagIndex < geometry.angioRep - 1; ++lagIndex) {
        const float *octaSrc = lagOctaRowsGpu.ptr<float>(lagIndex);
        const float *weightSrc = lagWeightRowsGpu.ptr<float>(lagIndex);
        float *octaDst = lagOctaMaps[static_cast<size_t>(lagIndex)].ptr<float>(y);
        float *weightDst = lagWeightMaps[static_cast<size_t>(lagIndex)].ptr<float>(y);
        for (int x = 0; x < geometry.bscanLen; ++x) {
            octaDst[x] = octaSrc[x];
            weightDst[x] = weightSrc[x];
        }
    }
}

void computeProjectionGpu(const QString &filePath,
                          const Geometry &geometry,
                          const std::vector<float> &spectralWindow,
                          const std::vector<float> &structuralVolume,
                          const std::vector<float> &flowVolume,
                          const std::vector<float> &surface,
                          cv::Mat &speedAlphaMap,
                          cv::Mat &confidenceMap,
                          std::vector<cv::Mat> &lagOctaMaps,
                          std::vector<cv::Mat> &lagWeightMaps,
                          const std::function<void(const QString&)> &log)
{
    QString deviceDescription;
    QString failureReason;
    if (!initializeOpenClGpu(&deviceDescription, &failureReason)) {
        throw std::runtime_error(QStringLiteral("OpenCL GPU 配准不可用：%1")
                                 .arg(failureReason)
                                 .toLocal8Bit()
                                 .constData());
    }

    speedAlphaMap = cv::Mat::zeros(geometry.cscanLenEff, geometry.bscanLen, CV_32F);
    confidenceMap = cv::Mat::zeros(geometry.cscanLenEff, geometry.bscanLen, CV_32F);
    const int lagCount = geometry.angioRep - 1;
    lagOctaMaps.assign(static_cast<size_t>(lagCount), cv::Mat());
    lagWeightMaps.assign(static_cast<size_t>(lagCount), cv::Mat());
    for (int lagIndex = 0; lagIndex < lagCount; ++lagIndex) {
        lagOctaMaps[static_cast<size_t>(lagIndex)] =
            cv::Mat::zeros(geometry.cscanLenEff, geometry.bscanLen, CV_32F);
        lagWeightMaps[static_cast<size_t>(lagIndex)] =
            cv::Mat::zeros(geometry.cscanLenEff, geometry.bscanLen, CV_32F);
    }
    log(QStringLiteral("使用 GPU/OpenCL 配准并计算 TAC 血流速度图：%1。").arg(deviceDescription));

    QFile file;
    if (!openRawVolumeFile(file, filePath)) {
        throw std::runtime_error("Failed to open selected .3d file.");
    }

    GpuRegistrationProcessor processor(spectralWindow, geometry);
    int nextProgress = 100;
    for (int y = 0; y < geometry.cscanLenEff; ++y) {
        processProjectionBlock(file,
                               processor,
                               y,
                               geometry,
                               structuralVolume,
                               flowVolume,
                               surface,
                               speedAlphaMap,
                               confidenceMap,
                               lagOctaMaps,
                               lagWeightMaps);
        const int done = y + 1;
        while (nextProgress <= geometry.cscanLenEff && done >= nextProgress) {
            log(QStringLiteral("已计算血流速度 %1 / %2 个 Bscan 板块。")
                .arg(nextProgress)
                .arg(geometry.cscanLenEff));
            nextProgress += 100;
        }
        QCoreApplication::processEvents();
    }

    if (processor.cpuRegistrationFallbackCount() > 0) {
        log(QStringLiteral("GPU/OpenCL 速度图配准有 %1 次触发 phaseCorrelate 兜底。")
            .arg(processor.cpuRegistrationFallbackCount()));
    }
    log(QStringLiteral("已完成所有 Bscan 板块的 TAC 血流速度计算。"));
}

cv::Mat normalizeFloatImageTo8U(const cv::Mat &image,
                                double lowPercentile,
                                double highPercentile,
                                float *displayLow,
                                float *displayHigh,
                                const cv::Mat *sampleMask = nullptr)
{
    const bool useMask = sampleMask && !sampleMask->empty() &&
        sampleMask->rows == image.rows && sampleMask->cols == image.cols &&
        sampleMask->type() == CV_8U;
    std::vector<float> samples;
    samples.reserve(static_cast<size_t>(image.rows) * image.cols / 4 + 1);
    for (int y = 0; y < image.rows; ++y) {
        const float *row = image.ptr<float>(y);
        const uint8_t *maskRow = useMask ? sampleMask->ptr<uint8_t>(y) : nullptr;
        for (int x = 0; x < image.cols; ++x) {
            if (maskRow && maskRow[x] == 0) {
                continue;
            }
            const float value = row[x];
            if (std::isfinite(value) && value > 0.0f && ((x + y * image.cols) % 4 == 0)) {
                samples.push_back(value);
            }
        }
    }

    float low = percentile(samples, lowPercentile);
    float high = percentile(samples, highPercentile);
    if (!std::isfinite(low) || !std::isfinite(high) || high <= low) {
        low = 0.0f;
        high = 1.0f;
    }

    if (displayLow) {
        *displayLow = low;
    }
    if (displayHigh) {
        *displayHigh = high;
    }

    cv::Mat output(image.rows, image.cols, CV_8U);
    const float scale = high > low ? 1.0f / (high - low) : 1.0f;
    parallelForRange(static_cast<size_t>(image.rows), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            const float *src = image.ptr<float>(y);
            const uint8_t *maskRow = useMask ? sampleMask->ptr<uint8_t>(y) : nullptr;
            uint8_t *dst = output.ptr<uint8_t>(y);
            for (int x = 0; x < image.cols; ++x) {
                if (maskRow && maskRow[x] == 0) {
                    dst[x] = 0;
                    continue;
                }
                if (!std::isfinite(src[x]) || src[x] <= 0.0f) {
                    dst[x] = 0;
                    continue;
                }
                dst[x] = toByte((src[x] - low) * scale);
            }
        }
    }, 1, true);
    return output;
}

cv::Mat removeSmallMaskComponents(const cv::Mat &mask, int minArea)
{
    cv::Mat binary;
    cv::threshold(mask, binary, 0, 255, cv::THRESH_BINARY);
    if (binary.empty() || minArea <= 1) {
        return binary;
    }

    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int count = cv::connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
    cv::Mat output = cv::Mat::zeros(binary.size(), CV_8U);
    for (int label = 1; label < count; ++label) {
        if (stats.at<int>(label, cv::CC_STAT_AREA) >= minArea) {
            output.setTo(255, labels == label);
        }
    }
    return output;
}

cv::Mat buildFlowSpeedVesselMask(const cv::Mat &confidenceMap, double *threshold8)
{
    cv::Mat confidenceForMask = normalizeFloatImageTo8U(confidenceMap,
                                                        kFlowSpeedMaskConfidenceLowPercentile,
                                                        kFlowSpeedMaskConfidenceHighPercentile,
                                                        nullptr,
                                                        nullptr);
    cv::Mat smoothed;
    cv::GaussianBlur(confidenceForMask, smoothed, cv::Size(0, 0), 1.0, 1.0);

    cv::Mat mask;
    const double otsuThreshold = cv::threshold(smoothed, mask, 0, 255,
                                              cv::THRESH_BINARY | cv::THRESH_OTSU);
    double appliedThreshold = otsuThreshold;
    if (appliedThreshold < kFlowSpeedMaskMinThreshold8) {
        appliedThreshold = kFlowSpeedMaskMinThreshold8;
        cv::threshold(smoothed, mask, appliedThreshold, 255, cv::THRESH_BINARY);
    }
    if (threshold8) {
        *threshold8 = appliedThreshold;
    }

    return removeSmallMaskComponents(mask, kFlowSpeedMaskMinAreaPixels);
}

cv::Mat buildFlowVolumeProjectionVesselMask(const std::vector<float> &structuralVolume,
                                            const std::vector<float> &flowVolume,
                                            const std::vector<float> &surface,
                                            const Geometry &geometry,
                                            const std::function<void(const QString&)> &log)
{
    const size_t voxelCount = static_cast<size_t>(geometry.cropDepth) *
        static_cast<size_t>(geometry.bscanLen) *
        static_cast<size_t>(geometry.cscanLenEff);
    const size_t surfaceCount = static_cast<size_t>(geometry.bscanLen) *
        static_cast<size_t>(geometry.cscanLenEff);
    if (geometry.cropDepth <= 0 || geometry.projectionDepth <= 0 ||
        geometry.bscanLen <= 0 || geometry.cscanLenEff <= 0 ||
        structuralVolume.size() < voxelCount || flowVolume.size() < voxelCount ||
        surface.size() < surfaceCount) {
        log(QStringLiteral("TAC 血管投影掩膜输入数据不完整，自动骨架将退回 confidence 掩膜。"));
        return cv::Mat();
    }

    std::vector<float> bgRemovedSamples;
    bgRemovedSamples.reserve(flowVolume.size() / 36 + 1);
    for (size_t i = 0; i < voxelCount; i += 36) {
        bgRemovedSamples.push_back(flowVolume[i] -
                                   geometry.backgroundSubtractionWeight * structuralVolume[i]);
    }
    const float bgLow = percentile(bgRemovedSamples, 40.0);
    const float bgHigh = percentile(bgRemovedSamples, 99.99);
    const float bgScale = bgHigh > bgLow ? 1.0f / (bgHigh - bgLow) : 1.0f;

    cv::Mat projection(geometry.cscanLenEff, geometry.bscanLen, CV_32F, cv::Scalar(0));
    const int projectionDepth = std::max(1, geometry.projectionDepth);
    parallelForRange(static_cast<size_t>(geometry.cscanLenEff), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            float *projectionRow = projection.ptr<float>(y);
            for (int x = 0; x < geometry.bscanLen; ++x) {
                float top[kFlowSpeedVesselProjectionTopAverage];
                std::fill(top,
                          top + kFlowSpeedVesselProjectionTopAverage,
                          -std::numeric_limits<float>::max());
                const size_t columnIndex = static_cast<size_t>(x) +
                    static_cast<size_t>(geometry.bscanLen) * y;
                const int surfaceShifted =
                    static_cast<int>(std::round(surface[columnIndex] + geometry.surfaceZShift));
                for (int d = 0; d < projectionDepth; ++d) {
                    const int sourceZ = std::max(
                        0,
                        std::min(geometry.cropDepth - 1, d + surfaceShifted));
                    const size_t sourceIndex = volumeIndex(geometry, sourceZ, x, y);
                    const float bgRemoved = flowVolume[sourceIndex] -
                        geometry.backgroundSubtractionWeight * structuralVolume[sourceIndex];
                    const float value = (bgRemoved - bgLow) * bgScale;
                    for (int i = 0; i < kFlowSpeedVesselProjectionTopAverage; ++i) {
                        if (value > top[i]) {
                            for (int j = kFlowSpeedVesselProjectionTopAverage - 1; j > i; --j) {
                                top[j] = top[j - 1];
                            }
                            top[i] = value;
                            break;
                        }
                    }
                }

                float average = 0.0f;
                for (float value : top) {
                    average += std::isfinite(value) ? value : 0.0f;
                }
                projectionRow[x] = average / static_cast<float>(kFlowSpeedVesselProjectionTopAverage);
            }
        }
    }, 1, true);

    cv::Mat projection8 = normalizeFloatImageTo8U(projection,
                                                  kFlowSpeedVesselProjectionLowPercentile,
                                                  kFlowSpeedVesselProjectionHighPercentile,
                                                  nullptr,
                                                  nullptr);
    cv::GaussianBlur(projection8, projection8, cv::Size(0, 0), 1.0, 1.0);

    cv::Mat mask;
    const double otsuThreshold = cv::threshold(projection8,
                                              mask,
                                              0,
                                              255,
                                              cv::THRESH_BINARY | cv::THRESH_OTSU);
    mask = removeSmallMaskComponents(mask, kFlowSpeedMaskMinAreaPixels);
    const int maskPixels = cv::countNonZero(mask);
    if (maskPixels < kSkeletonDenoiseMinPixels) {
        log(QStringLiteral("TAC 血管投影掩膜过少（%1 像素），自动骨架将退回 confidence 掩膜。")
            .arg(maskPixels));
        return cv::Mat();
    }

    log(QStringLiteral("TAC 血管投影掩膜：阈值 %1/255，候选 %2 像素。")
        .arg(otsuThreshold, 0, 'f', 1)
        .arg(maskPixels));
    return mask;
}

struct FlowSpeedRenderContext
{
    cv::Mat vesselMask;
    cv::Mat confidence8;
    double maskThreshold8 = 0.0;
    float confidenceLow = 0.0f;
    float confidenceHigh = 1.0f;
};

struct SegmentAlphaResult
{
    cv::Mat alphaMap;
    cv::Mat fitCorrelationMap;
    int validSegmentCount = 0;
    int validFitCorrelationCount = 0;
};

cv::Mat buildFitCorrelationMask(const cv::Mat &fitCorrelationMap,
                                const cv::Mat &vesselMask,
                                float minCorrelation)
{
    if (fitCorrelationMap.empty() || vesselMask.empty()) {
        return cv::Mat();
    }

    cv::Mat mask(fitCorrelationMap.rows, fitCorrelationMap.cols, CV_8U, cv::Scalar(0));
    parallelForRange(static_cast<size_t>(fitCorrelationMap.rows), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            const float *correlationRow = fitCorrelationMap.ptr<float>(y);
            const uint8_t *vesselRow = vesselMask.ptr<uint8_t>(y);
            uint8_t *maskRow = mask.ptr<uint8_t>(y);
            for (int x = 0; x < fitCorrelationMap.cols; ++x) {
                const float correlation = correlationRow[x];
                if (vesselRow[x] != 0 && std::isfinite(correlation) &&
                    correlation >= minCorrelation) {
                    maskRow[x] = 255;
                }
            }
        }
    }, 1, true);
    return removeSmallMaskComponents(mask, kFlowSpeedMaskMinAreaPixels);
}

bool hasFlowSpeedCrop(const OutputOptions &options)
{
    return options.cropTop > 0 || options.cropBottom > 0 ||
        options.cropLeft > 0 || options.cropRight > 0;
}

cv::Rect flowSpeedCropRect(const cv::Size &size,
                           const OutputOptions &options,
                           bool *valid)
{
    const int left = std::max(0, options.cropLeft);
    const int right = std::max(0, options.cropRight);
    const int top = std::max(0, options.cropTop);
    const int bottom = std::max(0, options.cropBottom);
    const int width = size.width - left - right;
    const int height = size.height - top - bottom;
    if (valid) {
        *valid = width > 0 && height > 0;
    }
    if (width <= 0 || height <= 0) {
        return cv::Rect(0, 0, size.width, size.height);
    }
    return cv::Rect(left, top, width, height);
}

void applyFlowSpeedCrop(cv::Mat &vesselMask,
                        const OutputOptions &options,
                        const std::function<void(const QString&)> &log)
{
    if (vesselMask.empty() || !hasFlowSpeedCrop(options)) {
        return;
    }

    bool valid = false;
    const cv::Rect crop = flowSpeedCropRect(vesselMask.size(), options, &valid);
    if (!valid) {
        log(QStringLiteral("TAC 速度图裁剪参数无效，已忽略裁剪。"));
        return;
    }

    cv::Mat cropMask = cv::Mat::zeros(vesselMask.size(), CV_8U);
    cropMask(crop).setTo(255);
    cv::bitwise_and(vesselMask, cropMask, vesselMask);
    log(QStringLiteral("TAC 速度图裁剪：保留 x=%1..%2, y=%3..%4。")
        .arg(crop.x)
        .arg(crop.x + crop.width - 1)
        .arg(crop.y)
        .arg(crop.y + crop.height - 1));
}

cv::Mat buildSkeletonDenoiseVesselMask(const cv::Mat &vesselMask,
                                       const cv::Mat &skeletonSourceMask,
                                       const std::function<void(const QString&)> &log)
{
    if (vesselMask.empty()) {
        return vesselMask;
    }

    const cv::Mat &sourceMask =
        !skeletonSourceMask.empty() &&
        skeletonSourceMask.size() == vesselMask.size() &&
        skeletonSourceMask.type() == CV_8U ?
        skeletonSourceMask : vesselMask;

    VesselSegmenter::Params params;
    params.spurPruneIterations = kSkeletonDenoiseSpurPruneIterations;
    params.minSkeletonSegmentPixels = kSkeletonDenoiseMinSkeletonSegmentPixels;
    params.minAssignedPixels = kSkeletonDenoiseMinAssignedPixels;
    const VesselSegmenter::Result segments = VesselSegmenter::segmentVesselMask(sourceMask, params);
    const int skeletonPixels = cv::countNonZero(segments.skeleton);
    if (skeletonPixels <= 0) {
        log(QStringLiteral("TAC 自动骨架抑噪没有检测到可用骨架，已保留原始速度图掩膜。"));
        return vesselMask;
    }

    cv::Mat skeletonMask;
    cv::threshold(segments.skeleton, skeletonMask, 0, 255, cv::THRESH_BINARY);
    const int radius = std::max(1, kSkeletonDenoiseDilateRadius);
    const cv::Mat element = cv::getStructuringElement(
        cv::MORPH_ELLIPSE,
        cv::Size(radius * 2 + 1, radius * 2 + 1));
    cv::dilate(skeletonMask, skeletonMask, element);

    cv::Mat denoised;
    cv::bitwise_and(vesselMask, skeletonMask, denoised);
    denoised = removeSmallMaskComponents(denoised, kFlowSpeedMaskMinAreaPixels);
    const int denoisedPixels = cv::countNonZero(denoised);
    if (denoisedPixels < kSkeletonDenoiseMinPixels) {
        log(QStringLiteral("TAC 自动骨架抑噪结果过少（%1 像素），已保留原始速度图掩膜。")
            .arg(denoisedPixels));
        return vesselMask;
    }

    const int originalPixels = std::max(1, cv::countNonZero(vesselMask));
    log(QStringLiteral("TAC 自动骨架抑噪：骨架 %1 像素，掩膜 %2 -> %3 像素（%4%）。")
        .arg(skeletonPixels)
        .arg(originalPixels)
        .arg(denoisedPixels)
        .arg(100.0 * static_cast<double>(denoisedPixels) /
             static_cast<double>(originalPixels), 0, 'f', 1));
    return denoised;
}

void updateFlowSpeedConfidenceDisplay(FlowSpeedRenderContext &context,
                                      const cv::Mat &confidenceMap)
{
    cv::Mat confidence8 = normalizeFloatImageTo8U(confidenceMap,
                                                  kFlowSpeedBrightnessLowPercentile,
                                                  kFlowSpeedBrightnessHighPercentile,
                                                  &context.confidenceLow,
                                                  &context.confidenceHigh,
                                                  &context.vesselMask);
    cv::medianBlur(confidence8, confidence8, 3);
    confidence8.setTo(0, context.vesselMask == 0);
    parallelForRange(static_cast<size_t>(confidence8.rows), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            uint8_t *confidenceRow = confidence8.ptr<uint8_t>(y);
            const uint8_t *maskRow = context.vesselMask.ptr<uint8_t>(y);
            for (int x = 0; x < confidence8.cols; ++x) {
                if (maskRow[x] == 0 || confidenceRow[x] == 0) {
                    confidenceRow[x] = 0;
                    continue;
                }
                const float normalized = std::sqrt(confidenceRow[x] / 255.0f);
                confidenceRow[x] = std::max(kFlowSpeedMinVisibleBrightness, toByte(normalized));
            }
        }
    }, 1, true);
    context.confidence8 = confidence8;
}

FlowSpeedRenderContext makeFlowSpeedRenderContext(const cv::Mat &confidenceMap,
                                                  const cv::Mat &projectionVesselMask,
                                                  const OutputOptions &options,
                                                  const std::function<void(const QString&)> &log)
{
    FlowSpeedRenderContext context;
    context.vesselMask = buildFlowSpeedVesselMask(confidenceMap, &context.maskThreshold8);
    applyFlowSpeedCrop(context.vesselMask, options, log);
    if (options.useSkeletonDenoise) {
        context.vesselMask = buildSkeletonDenoiseVesselMask(context.vesselMask,
                                                            projectionVesselMask,
                                                            log);
    }
    updateFlowSpeedConfidenceDisplay(context, confidenceMap);
    return context;
}

void applyManualFlowSpeedMask(FlowSpeedRenderContext &context,
                              const cv::Mat &confidenceMap,
                              const OutputOptions &options,
                              const std::function<void(const QString&)> &log)
{
    if (!options.useManualMask || context.vesselMask.empty()) {
        return;
    }

    cv::Mat dialogBase = normalizeFloatImageTo8U(confidenceMap,
                                                 kFlowSpeedBrightnessLowPercentile,
                                                 kFlowSpeedBrightnessHighPercentile,
                                                 nullptr,
                                                 nullptr);
    cv::medianBlur(dialogBase, dialogBase, 3);

    FlowSpeedMaskDialog dialog(dialogBase, context.vesselMask);
    if (dialog.exec() != QDialog::Accepted) {
        log(QStringLiteral("TAC 手动速度图掩膜窗口已取消，保留自动掩膜。"));
        return;
    }

    cv::Mat manualMask = dialog.mask();
    if (manualMask.empty()) {
        log(QStringLiteral("TAC 手动速度图掩膜为空，保留自动掩膜。"));
        return;
    }
    if (manualMask.type() != CV_8U) {
        manualMask.convertTo(manualMask, CV_8U);
    }
    if (manualMask.size() != context.vesselMask.size()) {
        cv::resize(manualMask,
                   manualMask,
                   context.vesselMask.size(),
                   0.0,
                   0.0,
                   cv::INTER_NEAREST);
    }
    cv::threshold(manualMask, manualMask, 0, 255, cv::THRESH_BINARY);

    const int oldPixels = cv::countNonZero(context.vesselMask);
    const int newPixels = cv::countNonZero(manualMask);
    context.vesselMask = manualMask;
    updateFlowSpeedConfidenceDisplay(context, confidenceMap);
    log(QStringLiteral("TAC 手动速度图掩膜：掩膜 %1 -> %2 像素，并已重新计算亮度和显示范围。")
        .arg(oldPixels)
        .arg(newPixels));
}

cv::Mat renderFlowSpeedMap(const cv::Mat &speedAlphaMap,
                           const FlowSpeedRenderContext &context,
                           const QString &label,
                           const std::function<void(const QString&)> &log,
                           const cv::Mat *displayMask = nullptr,
                           double lowPercentile = kFlowSpeedDisplayLowPercentile,
                           double highPercentile = kFlowSpeedDisplayHighPercentile,
                           float displayGamma = 1.0f)
{
    float speedLow = 0.0f;
    float speedHigh = 1.0f;
    const cv::Mat *sampleMask = displayMask && !displayMask->empty() ?
        displayMask : &context.vesselMask;
    cv::Mat speed8 = normalizeFloatImageTo8U(speedAlphaMap,
                                             lowPercentile,
                                             highPercentile,
                                             &speedLow,
                                             &speedHigh,
                                             sampleMask);

    cv::Mat hsv(speedAlphaMap.rows, speedAlphaMap.cols, CV_8UC3, cv::Scalar(0, 0, 0));
    const bool hasDisplayMask = displayMask && !displayMask->empty() &&
        displayMask->rows == speedAlphaMap.rows && displayMask->cols == speedAlphaMap.cols &&
        displayMask->type() == CV_8U;
    const float gamma = displayGamma > 0.0f && std::isfinite(displayGamma) ?
        displayGamma : 1.0f;
    parallelForRange(static_cast<size_t>(hsv.rows), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            const uint8_t *confidenceRow = context.confidence8.ptr<uint8_t>(y);
            const uint8_t *maskRow = context.vesselMask.ptr<uint8_t>(y);
            const uint8_t *displayMaskRow = hasDisplayMask ? displayMask->ptr<uint8_t>(y) : nullptr;
            const uint8_t *speed8Row = speed8.ptr<uint8_t>(y);
            const float *speedRow = speedAlphaMap.ptr<float>(y);
            cv::Vec3b *hsvRow = hsv.ptr<cv::Vec3b>(y);
            for (int x = 0; x < hsv.cols; ++x) {
                if (maskRow[x] == 0 || (displayMaskRow && displayMaskRow[x] == 0) ||
                    confidenceRow[x] == 0 ||
                    speedRow[x] <= 0.0f || !std::isfinite(speedRow[x])) {
                    hsvRow[x] = cv::Vec3b(0, 0, 0);
                    continue;
                }
                double normalizedSpeed = static_cast<double>(speed8Row[x]) / 255.0;
                if (std::abs(gamma - 1.0f) > 1.0e-6f) {
                    normalizedSpeed = std::pow(normalizedSpeed, static_cast<double>(gamma));
                }
                const uint8_t hue = static_cast<uint8_t>(
                    std::round(120.0 * (1.0 - normalizedSpeed)));
                hsvRow[x] = cv::Vec3b(hue, 255, confidenceRow[x]);
            }
        }
    }, 1, true);
    cv::Mat color;
    cv::cvtColor(hsv, color, cv::COLOR_HSV2BGR);

    const double vesselMaskPercent = speedAlphaMap.empty() ? 0.0 :
        100.0 * static_cast<double>(cv::countNonZero(context.vesselMask)) /
        static_cast<double>(speedAlphaMap.rows * speedAlphaMap.cols);
    const double displayMaskPercent = !hasDisplayMask || speedAlphaMap.empty() ? vesselMaskPercent :
        100.0 * static_cast<double>(cv::countNonZero(*displayMask)) /
        static_cast<double>(speedAlphaMap.rows * speedAlphaMap.cols);
    log(QStringLiteral("TAC %1 显示范围：alpha=%2 ~ %3（只统计显示掩膜内像素，单位：每个重复间隔的相对衰减率）。")
        .arg(label)
        .arg(speedLow, 0, 'f', 3)
        .arg(speedHigh, 0, 'f', 3));
    log(QStringLiteral("TAC %1 血管掩膜覆盖 %2%，显示掩膜覆盖 %3%，阈值 %4/255；亮度权重范围：%5 ~ %6。")
        .arg(label)
        .arg(vesselMaskPercent, 0, 'f', 1)
        .arg(displayMaskPercent, 0, 'f', 1)
        .arg(context.maskThreshold8, 0, 'f', 1)
        .arg(context.confidenceLow, 0, 'f', 1)
        .arg(context.confidenceHigh, 0, 'f', 1));
    return color;
}

cv::Mat renderFitCorrelationMap(const cv::Mat &correlationMap,
                                const FlowSpeedRenderContext &context,
                                const std::function<void(const QString&)> &log)
{
    cv::Mat output(correlationMap.rows, correlationMap.cols, CV_8U, cv::Scalar(0));
    parallelForRange(static_cast<size_t>(output.rows), [&](size_t begin, size_t end) {
        for (int y = static_cast<int>(begin); y < static_cast<int>(end); ++y) {
            const float *correlationRow = correlationMap.ptr<float>(y);
            const uint8_t *maskRow = context.vesselMask.ptr<uint8_t>(y);
            uint8_t *outputRow = output.ptr<uint8_t>(y);
            for (int x = 0; x < output.cols; ++x) {
                const float correlation = correlationRow[x];
                if (maskRow[x] == 0 || !std::isfinite(correlation) || correlation <= 0.0f) {
                    outputRow[x] = 0;
                    continue;
                }
                const float clippedCorrelation = clampFloat(correlation, 0.0f, 1.0f);
                const float displayCorrelation = 1.0f -
                    std::sqrt(std::max(0.0f, 1.0f - clippedCorrelation * clippedCorrelation));
                outputRow[x] = toByte(displayCorrelation);
            }
        }
    }, 1, true);

    log(QStringLiteral("TAC segment-wise 拟合相关系数图：R=0~1，灰度使用 1-sqrt(1-R^2) 放缩，越亮表示多 lag 退相关曲线越符合单指数模型。"));
    return output;
}

SegmentAlphaResult buildSegmentAveragedAlphaMap(const cv::Mat &speedAlphaMap,
                                                const cv::Mat &confidenceMap,
                                                const cv::Mat &segmentLabels,
                                                int segmentCount)
{
    SegmentAlphaResult result;
    result.alphaMap = cv::Mat::zeros(speedAlphaMap.size(), CV_32F);
    if (segmentCount <= 0 || segmentLabels.empty()) {
        return result;
    }

    std::vector<double> alphaSums(static_cast<size_t>(segmentCount) + 1, 0.0);
    std::vector<double> weightSums(static_cast<size_t>(segmentCount) + 1, 0.0);
    for (int y = 0; y < speedAlphaMap.rows; ++y) {
        const float *alphaRow = speedAlphaMap.ptr<float>(y);
        const float *confidenceRow = confidenceMap.ptr<float>(y);
        const int *labelRow = segmentLabels.ptr<int>(y);
        for (int x = 0; x < speedAlphaMap.cols; ++x) {
            const int label = labelRow[x];
            const float alpha = alphaRow[x];
            const float weight = confidenceRow[x];
            if (label <= 0 || label > segmentCount || alpha <= 0.0f || weight <= 0.0f ||
                !std::isfinite(alpha) || !std::isfinite(weight)) {
                continue;
            }
            alphaSums[static_cast<size_t>(label)] += static_cast<double>(alpha) * weight;
            weightSums[static_cast<size_t>(label)] += weight;
        }
    }

    std::vector<float> segmentAlpha(static_cast<size_t>(segmentCount) + 1, 0.0f);
    for (int label = 1; label <= segmentCount; ++label) {
        if (weightSums[static_cast<size_t>(label)] > 0.0) {
            segmentAlpha[static_cast<size_t>(label)] =
                static_cast<float>(alphaSums[static_cast<size_t>(label)] /
                                   weightSums[static_cast<size_t>(label)]);
            ++result.validSegmentCount;
        }
    }

    for (int y = 0; y < segmentLabels.rows; ++y) {
        const int *labelRow = segmentLabels.ptr<int>(y);
        float *dst = result.alphaMap.ptr<float>(y);
        for (int x = 0; x < segmentLabels.cols; ++x) {
            const int label = labelRow[x];
            if (label > 0 && label <= segmentCount) {
                dst[x] = segmentAlpha[static_cast<size_t>(label)];
            }
        }
    }
    return result;
}

SegmentAlphaResult buildSegmentFittedAlphaMap(const std::vector<cv::Mat> &lagOctaMaps,
                                              const std::vector<cv::Mat> &lagWeightMaps,
                                              const cv::Mat &segmentLabels,
                                              int segmentCount)
{
    SegmentAlphaResult result;
    if (lagOctaMaps.empty() || lagOctaMaps.size() != lagWeightMaps.size()) {
        return result;
    }
    result.alphaMap = cv::Mat::zeros(lagOctaMaps[0].size(), CV_32F);
    if (segmentCount <= 0 || segmentLabels.empty()) {
        return result;
    }

    const int lagCount = static_cast<int>(lagOctaMaps.size());
    std::vector<double> octaSums((static_cast<size_t>(segmentCount) + 1) * lagCount, 0.0);
    std::vector<double> weightSums((static_cast<size_t>(segmentCount) + 1) * lagCount, 0.0);
    auto statIndex = [lagCount](int label, int lagIndex) {
        return static_cast<size_t>(label) * lagCount + static_cast<size_t>(lagIndex);
    };

    for (int y = 0; y < segmentLabels.rows; ++y) {
        const int *labelRow = segmentLabels.ptr<int>(y);
        for (int x = 0; x < segmentLabels.cols; ++x) {
            const int label = labelRow[x];
            if (label <= 0 || label > segmentCount) {
                continue;
            }
            for (int lagIndex = 0; lagIndex < lagCount; ++lagIndex) {
                const float octa = lagOctaMaps[static_cast<size_t>(lagIndex)].at<float>(y, x);
                const float weight = lagWeightMaps[static_cast<size_t>(lagIndex)].at<float>(y, x);
                if (weight <= 0.0f || !std::isfinite(weight) || !std::isfinite(octa)) {
                    continue;
                }
                const size_t index = statIndex(label, lagIndex);
                octaSums[index] += static_cast<double>(octa) * weight;
                weightSums[index] += weight;
            }
        }
    }

    std::vector<float> segmentAlpha(static_cast<size_t>(segmentCount) + 1, 0.0f);
    std::vector<float> segmentCorrelation(static_cast<size_t>(segmentCount) + 1, 0.0f);
    for (int label = 1; label <= segmentCount; ++label) {
        std::vector<float> normalizedOcta(static_cast<size_t>(lagCount), 0.0f);
        std::vector<float> lagWeights(static_cast<size_t>(lagCount), 0.0f);
        for (int lagIndex = 0; lagIndex < lagCount; ++lagIndex) {
            const size_t index = statIndex(label, lagIndex);
            if (weightSums[index] <= 0.0) {
                continue;
            }
            normalizedOcta[static_cast<size_t>(lagIndex)] =
                static_cast<float>(octaSums[index] / weightSums[index]);
            lagWeights[static_cast<size_t>(lagIndex)] = static_cast<float>(weightSums[index]);
        }
        const TemporalAutocorrelationFitResult fit =
            fitTemporalAutocorrelationModel(normalizedOcta, lagWeights);
        segmentAlpha[static_cast<size_t>(label)] = fit.alpha;
        segmentCorrelation[static_cast<size_t>(label)] = fit.correlation;
        if (segmentAlpha[static_cast<size_t>(label)] > 0.0f) {
            ++result.validSegmentCount;
        }
        if (segmentCorrelation[static_cast<size_t>(label)] > 0.0f) {
            ++result.validFitCorrelationCount;
        }
    }

    result.fitCorrelationMap = cv::Mat::zeros(result.alphaMap.size(), CV_32F);
    for (int y = 0; y < segmentLabels.rows; ++y) {
        const int *labelRow = segmentLabels.ptr<int>(y);
        float *dst = result.alphaMap.ptr<float>(y);
        float *correlationDst = result.fitCorrelationMap.ptr<float>(y);
        for (int x = 0; x < segmentLabels.cols; ++x) {
            const int label = labelRow[x];
            if (label > 0 && label <= segmentCount) {
                dst[x] = segmentAlpha[static_cast<size_t>(label)];
                correlationDst[x] = segmentCorrelation[static_cast<size_t>(label)];
            }
        }
    }
    return result;
}

} // namespace

void saveProjection(const QString &filePath,
                    const QString &basePath,
                    const Geometry &geometry,
                    const OutputOptions &options,
                    const std::vector<float> &spectralWindow,
                    const std::vector<float> &structuralVolume,
                    const std::vector<float> &flowVolume,
                    const std::vector<float> &surface,
                    const std::function<void(const QString&)> &log,
                    const std::function<void(const QString&)> &fileLog)
{
    if (geometry.angioRep < 2) {
        log(QStringLiteral("Angio 重复数小于 2，无法计算 TAC 血流速度图，已跳过。"));
        return;
    }

    cv::Mat speedAlphaMap;
    cv::Mat confidenceMap;
    std::vector<cv::Mat> lagOctaMaps;
    std::vector<cv::Mat> lagWeightMaps;
    auto stageStart = SteadyClock::now();
    computeProjectionGpu(filePath,
                         geometry,
                         spectralWindow,
                         structuralVolume,
                         flowVolume,
                         surface,
                         speedAlphaMap,
                         confidenceMap,
                         lagOctaMaps,
                         lagWeightMaps,
                         log);
    logElapsed(log, QStringLiteral("计算 TAC 血流速度图"), elapsedSeconds(stageStart));
    QCoreApplication::processEvents();

    stageStart = SteadyClock::now();
    const cv::Mat projectionVesselMask = options.useSkeletonDenoise ?
        buildFlowVolumeProjectionVesselMask(structuralVolume,
                                            flowVolume,
                                            surface,
                                            geometry,
                                            log) :
        cv::Mat();
    FlowSpeedRenderContext renderContext = makeFlowSpeedRenderContext(confidenceMap,
                                                                      projectionVesselMask,
                                                                      options,
                                                                      log);
    applyManualFlowSpeedMask(renderContext, confidenceMap, options, log);
    cv::Mat pixelWiseColor;
    cv::Mat averagedColor;
    cv::Mat segmentWiseColor;
    cv::Mat fitCorrelationImage;
    bool hasAveragedColor = false;
    bool hasSegmentWiseColor = false;
    bool hasFitCorrelationImage = false;

    VesselSegmenter::Result segments;
    const bool needsSegments =
        options.generateAveragedImage ||
        options.generateSegmentWiseImage ||
        options.generateFitCorrelationImage;
    if (needsSegments) {
        VesselSegmenter::Params segmentParams;
        segments = VesselSegmenter::segmentVesselMask(renderContext.vesselMask, segmentParams);
        log(QStringLiteral("TAC 血管分段：骨架段 %1，最终血管段 %2，分配像素 %3。")
            .arg(segments.skeletonSegmentCount)
            .arg(segments.segmentCount)
            .arg(segments.assignedPixelCount));
    }

    if (options.generatePixelWiseImage) {
        pixelWiseColor = renderFlowSpeedMap(speedAlphaMap,
                                            renderContext,
                                            QStringLiteral("pixel-wise 速度图"),
                                            log);
    }
    if (options.generateAveragedImage) {
        const SegmentAlphaResult averaged =
            buildSegmentAveragedAlphaMap(speedAlphaMap,
                                         confidenceMap,
                                         segments.segmentLabels,
                                         segments.segmentCount);
        if (averaged.validSegmentCount > 0) {
            averagedColor = renderFlowSpeedMap(averaged.alphaMap,
                                               renderContext,
                                               QStringLiteral("快速平均速度图"),
                                               log);
            hasAveragedColor = true;
            log(QStringLiteral("TAC 快速平均速度图：有效血管段 %1 / %2。")
                .arg(averaged.validSegmentCount)
                .arg(segments.segmentCount));
        } else {
            log(QStringLiteral("TAC 快速平均速度图没有可用血管段，已跳过保存。"));
        }
    }
    SegmentAlphaResult segmented;
    bool hasSegmentedFit = false;
    if (options.generateSegmentWiseImage || options.generateFitCorrelationImage) {
        segmented =
            buildSegmentFittedAlphaMap(lagOctaMaps,
                                       lagWeightMaps,
                                       segments.segmentLabels,
                                       segments.segmentCount);
        hasSegmentedFit = segmented.validSegmentCount > 0;
    }
    if (options.generateSegmentWiseImage) {
        if (segmented.validSegmentCount > 0) {
            const cv::Mat reliableSegmentMask =
                buildFitCorrelationMask(segmented.fitCorrelationMap,
                                        renderContext.vesselMask,
                                        kSegmentWiseMinFitCorrelationForDisplay);
            segmentWiseColor = renderFlowSpeedMap(segmented.alphaMap,
                                                  renderContext,
                                                  QStringLiteral("segment-wise 速度图（相关系数筛选增强）"),
                                                  log,
                                                  &reliableSegmentMask,
                                                  kSegmentWiseEnhancedLowPercentile,
                                                  kSegmentWiseEnhancedHighPercentile,
                                                  kSegmentWiseEnhancedDisplayGamma);
            hasSegmentWiseColor = true;
            log(QStringLiteral("TAC segment-wise 速度图已用拟合相关系数 R >= %1 进行显示增强，gamma=%2。")
                .arg(kSegmentWiseMinFitCorrelationForDisplay, 0, 'f', 2)
                .arg(kSegmentWiseEnhancedDisplayGamma, 0, 'f', 2));
            log(QStringLiteral("TAC segment-wise 速度图：有效拟合血管段 %1 / %2。")
                .arg(segmented.validSegmentCount)
                .arg(segments.segmentCount));
        } else {
            log(QStringLiteral("TAC segment-wise 速度图没有可用血管段，已跳过保存。"));
        }
    }
    if (options.generateFitCorrelationImage) {
        if (hasSegmentedFit && segmented.validFitCorrelationCount > 0) {
            fitCorrelationImage = renderFitCorrelationMap(segmented.fitCorrelationMap,
                                                          renderContext,
                                                          log);
            hasFitCorrelationImage = true;
            log(QStringLiteral("TAC segment-wise 拟合相关系数图：有效血管段 %1 / %2。")
                .arg(segmented.validFitCorrelationCount)
                .arg(segments.segmentCount));
        } else {
            log(QStringLiteral("TAC segment-wise 拟合相关系数图没有可用血管段，已跳过保存。"));
        }
    }
    logElapsed(log, QStringLiteral("渲染 TAC 血流速度图"), elapsedSeconds(stageStart));

    const QString flowSpeedPath = basePath + "_flow_speed.tiff";
    const QString pixelWisePath = basePath + "_flow_speed_pixelwise.tiff";
    const QString averagedPath = basePath + "_flow_speed_avg.tiff";
    const QString segmentWisePath = basePath + "_flow_speed_segmented.tiff";
    const QString fitCorrelationPath = basePath + "_flow_speed_fit_correlation.tiff";   // 注意这里的 correlation 是对 D(tau) 作拟合的相关系数，而不是程序的相干系数！！！
    const QString confidencePath = basePath + "_flow_speed_confidence.tiff";
    const QString maskPath = basePath + "_flow_speed_mask.tiff";
    const std::vector<int> tiffParams = projectionTiffEncodeParams();
    if (options.generatePixelWiseImage) {
        if (!writeImageFile(flowSpeedPath, pixelWiseColor, tiffParams) ||
            !writeImageFile(pixelWisePath, pixelWiseColor, tiffParams)) {
            throw std::runtime_error("Failed to save pixel-wise flow-speed image.");
        }
        log(QStringLiteral("TAC pixel-wise 血流速度图已保存：%1").arg(outputImageName(flowSpeedPath)));
        if (fileLog) {
            fileLog(QStringLiteral("Saved output full path: %1").arg(flowSpeedPath));
        }
        log(QStringLiteral("TAC pixel-wise 血流速度图副本已保存：%1").arg(outputImageName(pixelWisePath)));
        if (fileLog) {
            fileLog(QStringLiteral("Saved output full path: %1").arg(pixelWisePath));
        }
    }
    if (hasAveragedColor) {
        if (!writeImageFile(averagedPath, averagedColor, tiffParams)) {
            throw std::runtime_error("Failed to save averaged flow-speed image.");
        }
        log(QStringLiteral("TAC 快速平均血流速度图已保存：%1").arg(outputImageName(averagedPath)));
        if (fileLog) {
            fileLog(QStringLiteral("Saved output full path: %1").arg(averagedPath));
        }
    }
    if (hasSegmentWiseColor) {
        if (!writeImageFile(segmentWisePath, segmentWiseColor, tiffParams)) {
            throw std::runtime_error("Failed to save segment-wise flow-speed image.");
        }
        log(QStringLiteral("TAC segment-wise 血流速度图已保存：%1").arg(outputImageName(segmentWisePath)));
        if (fileLog) {
            fileLog(QStringLiteral("Saved output full path: %1").arg(segmentWisePath));
        }
    }
    if (hasFitCorrelationImage) {
        if (!writeImageFile(fitCorrelationPath, fitCorrelationImage, tiffParams)) {
            throw std::runtime_error("Failed to save segment-wise fit correlation image.");
        }
        log(QStringLiteral("TAC segment-wise 拟合相关系数图已保存：%1").arg(outputImageName(fitCorrelationPath)));
        if (fileLog) {
            fileLog(QStringLiteral("Saved output full path: %1").arg(fitCorrelationPath));
        }
    }
    if (!writeImageFile(confidencePath, renderContext.confidence8, tiffParams)) {
        throw std::runtime_error("Failed to save flow-speed confidence image.");
    }

    log(QStringLiteral("TAC 血流速度亮度/置信图已保存：%1").arg(outputImageName(confidencePath)));
    if (fileLog) {
        fileLog(QStringLiteral("Saved output full path: %1").arg(confidencePath));
    }

    if (options.useSkeletonDenoise || options.useManualMask || hasFlowSpeedCrop(options)) {
        if (!writeImageFile(maskPath, renderContext.vesselMask, tiffParams)) {
            throw std::runtime_error("Failed to save flow-speed mask image.");
        }
        log(QStringLiteral("TAC 血流速度最终掩膜已保存：%1").arg(outputImageName(maskPath)));
        if (fileLog) {
            fileLog(QStringLiteral("Saved output full path: %1").arg(maskPath));
        }
    }
}

} // namespace FlowSpeedCalculation
