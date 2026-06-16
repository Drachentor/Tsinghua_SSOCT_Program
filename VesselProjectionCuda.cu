#include "VesselProjectionCuda.h"

#include <cuda_runtime.h>
#include <cufft.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace {

const int kCudaMaxAngioRep = 8;
const int kCudaMaxRegistrationShiftPixels = 8;
const int kAdPlotOffset = 2048;
const int kAdBitShift = 4;
const int kAdScale = 1 << kAdBitShift;

using Complex = cufftComplex;

const char *cudaErrorText(cudaError_t status)
{
    return cudaGetErrorString(status);
}

const char *cufftErrorText(cufftResult status)
{
    switch (status) {
    case CUFFT_SUCCESS: return "CUFFT_SUCCESS";
    case CUFFT_INVALID_PLAN: return "CUFFT_INVALID_PLAN";
    case CUFFT_ALLOC_FAILED: return "CUFFT_ALLOC_FAILED";
    case CUFFT_INVALID_TYPE: return "CUFFT_INVALID_TYPE";
    case CUFFT_INVALID_VALUE: return "CUFFT_INVALID_VALUE";
    case CUFFT_INTERNAL_ERROR: return "CUFFT_INTERNAL_ERROR";
    case CUFFT_EXEC_FAILED: return "CUFFT_EXEC_FAILED";
    case CUFFT_SETUP_FAILED: return "CUFFT_SETUP_FAILED";
    case CUFFT_INVALID_SIZE: return "CUFFT_INVALID_SIZE";
    case CUFFT_UNALIGNED_DATA: return "CUFFT_UNALIGNED_DATA";
    case CUFFT_INVALID_DEVICE: return "CUFFT_INVALID_DEVICE";
    case CUFFT_NO_WORKSPACE: return "CUFFT_NO_WORKSPACE";
    case CUFFT_NOT_IMPLEMENTED: return "CUFFT_NOT_IMPLEMENTED";
    case CUFFT_NOT_SUPPORTED: return "CUFFT_NOT_SUPPORTED";
    default: return "CUFFT_UNKNOWN_ERROR";
    }
}

void setError(std::string *errorMessage, const std::string &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

bool checkCuda(cudaError_t status, std::string *errorMessage, const char *operation)
{
    if (status == cudaSuccess) {
        return true;
    }
    std::ostringstream stream;
    stream << operation << ": " << cudaErrorText(status);
    setError(errorMessage, stream.str());
    return false;
}

bool checkCufft(cufftResult status, std::string *errorMessage, const char *operation)
{
    if (status == CUFFT_SUCCESS) {
        return true;
    }
    std::ostringstream stream;
    stream << operation << ": " << cufftErrorText(status);
    setError(errorMessage, stream.str());
    return false;
}

__device__ __forceinline__ Complex cadd(Complex a, Complex b)
{
    return make_cuFloatComplex(a.x + b.x, a.y + b.y);
}

__device__ __forceinline__ Complex csub(Complex a, Complex b)
{
    return make_cuFloatComplex(a.x - b.x, a.y - b.y);
}

__device__ __forceinline__ Complex cscale(Complex a, float scale)
{
    return make_cuFloatComplex(a.x * scale, a.y * scale);
}

__device__ __forceinline__ Complex cconj(Complex a)
{
    return make_cuFloatComplex(a.x, -a.y);
}

__device__ __forceinline__ Complex cmul(Complex a, Complex b)
{
    return make_cuFloatComplex(a.x * b.x - a.y * b.y,
                               a.x * b.y + a.y * b.x);
}

__device__ __forceinline__ Complex conjmul(Complex a, Complex b)
{
    return make_cuFloatComplex(a.x * b.x + a.y * b.y,
                               a.x * b.y - a.y * b.x);
}

__device__ __forceinline__ float cabsval(Complex a)
{
    return sqrtf(a.x * a.x + a.y * a.y);
}

__device__ __forceinline__ float cnorm2(Complex a)
{
    return a.x * a.x + a.y * a.y;
}

__device__ __forceinline__ Complex readFftValue(const Complex *fftBuffer,
                                                int ascanLen,
                                                int bscanLen,
                                                int repeat,
                                                int x,
                                                int zAbs)
{
    const int row = repeat * bscanLen + x;
    return fftBuffer[static_cast<size_t>(row) * ascanLen + zAbs];
}

__device__ __forceinline__ Complex sampleShiftedFft(const Complex *fftBuffer,
                                                    int ascanLen,
                                                    int bscanLen,
                                                    int repeat,
                                                    int x,
                                                    int cropStart0,
                                                    int cropDepth,
                                                    float sourceZ)
{
    if (sourceZ < 0.0f || sourceZ > static_cast<float>(cropDepth - 1)) {
        return make_cuFloatComplex(0.0f, 0.0f);
    }

    const int z0 = static_cast<int>(floorf(sourceZ));
    const int z1 = min(z0 + 1, cropDepth - 1);
    const float t = sourceZ - static_cast<float>(z0);
    const Complex a = readFftValue(fftBuffer, ascanLen, bscanLen, repeat, x, cropStart0 + z0);
    const Complex b = readFftValue(fftBuffer, ascanLen, bscanLen, repeat, x, cropStart0 + z1);
    return cadd(cscale(a, 1.0f - t), cscale(b, t));
}

__device__ void normalizeVector(Complex vector[kCudaMaxAngioRep], int angioRep)
{
    float normSq = 0.0f;
    for (int i = 0; i < angioRep; ++i) {
        normSq += cnorm2(vector[i]);
    }
    if (normSq < 1e-20f) {
        for (int i = 0; i < angioRep; ++i) {
            vector[i] = make_cuFloatComplex(0.0f, 0.0f);
        }
        vector[0] = make_cuFloatComplex(1.0f, 0.0f);
        return;
    }
    const float invNorm = rsqrtf(normSq);
    for (int i = 0; i < angioRep; ++i) {
        vector[i] = cscale(vector[i], invNorm);
    }
}

__device__ void multiplyMatrixVector(const Complex matrix[kCudaMaxAngioRep * kCudaMaxAngioRep],
                                     const Complex vector[kCudaMaxAngioRep],
                                     Complex result[kCudaMaxAngioRep],
                                     int angioRep)
{
    for (int row = 0; row < angioRep; ++row) {
        Complex value = make_cuFloatComplex(0.0f, 0.0f);
        for (int col = 0; col < angioRep; ++col) {
            value = cadd(value, cmul(matrix[row * kCudaMaxAngioRep + col], vector[col]));
        }
        result[row] = value;
    }
}

__device__ float rayleighQuotient(const Complex matrix[kCudaMaxAngioRep * kCudaMaxAngioRep],
                                  const Complex vector[kCudaMaxAngioRep],
                                  int angioRep)
{
    Complex mv[kCudaMaxAngioRep];
    multiplyMatrixVector(matrix, vector, mv, angioRep);
    Complex value = make_cuFloatComplex(0.0f, 0.0f);
    for (int i = 0; i < angioRep; ++i) {
        value = cadd(value, conjmul(vector[i], mv[i]));
    }
    return value.x;
}

__device__ void dominantEigenvector(const Complex matrix[kCudaMaxAngioRep * kCudaMaxAngioRep],
                                    int seed,
                                    int angioRep,
                                    Complex vector[kCudaMaxAngioRep])
{
    for (int i = 0; i < angioRep; ++i) {
        vector[i] = make_cuFloatComplex(1.0f + 0.13f * seed * (i + 1),
                                        0.07f * (seed + 1) * i);
    }
    normalizeVector(vector, angioRep);

    for (int iter = 0; iter < 32; ++iter) {
        Complex next[kCudaMaxAngioRep];
        multiplyMatrixVector(matrix, vector, next, angioRep);
        for (int i = 0; i < angioRep; ++i) {
            vector[i] = next[i];
        }
        normalizeVector(vector, angioRep);
    }
}

__global__ void rawToComplexKernel(const uint16_t *rawBlock,
                                   const float *spectralWindow,
                                   Complex *fftBuffer,
                                   int ascanLen,
                                   int bscanLen,
                                   int angioRep)
{
    const int z = blockIdx.x * blockDim.x + threadIdx.x;
    const int line = blockIdx.y * blockDim.y + threadIdx.y;
    const int lineCount = bscanLen * angioRep;
    if (z >= ascanLen || line >= lineCount) {
        return;
    }

    const size_t index = static_cast<size_t>(line) * ascanLen + z;
    fftBuffer[index] = make_cuFloatComplex(
        (static_cast<float>(rawBlock[index]) / static_cast<float>(kAdScale) - kAdPlotOffset) *
            spectralWindow[z],
        0.0f);
}

__global__ void extractMagnitudeKernel(const Complex *fftBuffer,
                                       float *magnitude,
                                       int ascanLen,
                                       int bscanLen,
                                       int repeat,
                                       int cropStart0,
                                       int cropDepth)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int z = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= bscanLen || z >= cropDepth) {
        return;
    }

    const Complex value = readFftValue(fftBuffer, ascanLen, bscanLen, repeat, x, cropStart0 + z);
    magnitude[static_cast<size_t>(z) * bscanLen + x] = cabsval(value);
}

__global__ void estimateAxialShiftScoresKernel(const float *referenceMagnitude,
                                               const float *movingMagnitude,
                                               float *scores,
                                               int bscanLen,
                                               int cropDepth,
                                               int maxShift)
{
    extern __shared__ float shared[];
    float *sharedNumerator = shared;
    float *sharedReferenceEnergy = shared + blockDim.x;
    float *sharedMovingEnergy = shared + blockDim.x * 2;

    const int shiftIndex = blockIdx.x;
    const int shift = shiftIndex - maxShift;
    const int voxelCount = bscanLen * cropDepth;

    float numerator = 0.0f;
    float referenceEnergy = 0.0f;
    float movingEnergy = 0.0f;
    for (int index = threadIdx.x; index < voxelCount; index += blockDim.x) {
        const int z = index / bscanLen;
        const int x = index - z * bscanLen;
        const float referenceValue = referenceMagnitude[index];

        float movingValue = 0.0f;
        const int movingZ = z + shift;
        if (movingZ >= 0 && movingZ < cropDepth) {
            movingValue = movingMagnitude[static_cast<size_t>(movingZ) * bscanLen + x];
        }

        numerator += referenceValue * movingValue;
        referenceEnergy += referenceValue * referenceValue;
        movingEnergy += movingValue * movingValue;
    }

    sharedNumerator[threadIdx.x] = numerator;
    sharedReferenceEnergy[threadIdx.x] = referenceEnergy;
    sharedMovingEnergy[threadIdx.x] = movingEnergy;
    __syncthreads();

    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (threadIdx.x < stride) {
            sharedNumerator[threadIdx.x] += sharedNumerator[threadIdx.x + stride];
            sharedReferenceEnergy[threadIdx.x] += sharedReferenceEnergy[threadIdx.x + stride];
            sharedMovingEnergy[threadIdx.x] += sharedMovingEnergy[threadIdx.x + stride];
        }
        __syncthreads();
    }

    if (threadIdx.x == 0) {
        const float denominator = sqrtf(sharedReferenceEnergy[0] * sharedMovingEnergy[0]);
        scores[shiftIndex] = denominator > 0.0f ? sharedNumerator[0] / denominator : -3.402823466e+38F;
    }
}

__global__ void selectAxialShiftKernel(const float *scores,
                                       float *shifts,
                                       int repeat,
                                       int maxShift)
{
    const int shiftCount = maxShift * 2 + 1;
    int bestIndex = maxShift;
    float bestScore = -3.402823466e+38F;
    for (int i = 0; i < shiftCount; ++i) {
        const float score = scores[i];
        if (score > bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }

    float shift = static_cast<float>(bestIndex - maxShift);
    if (bestIndex > 0 && bestIndex < shiftCount - 1) {
        const float left = scores[bestIndex - 1];
        const float center = scores[bestIndex];
        const float right = scores[bestIndex + 1];
        const float denominator = left - 2.0f * center + right;
        if (fabsf(denominator) > 1.0e-12f) {
            const float subpixelOffset = 0.5f * (left - right) / denominator;
            if (fabsf(subpixelOffset) <= 1.0f) {
                shift += subpixelOffset;
            }
        }
    }

    shifts[repeat] = isfinite(shift) ? shift : 0.0f;
}

__global__ void shiftMagnitudeKernel(const Complex *fftBuffer,
                                     float *magnitude,
                                     const float *shifts,
                                     int ascanLen,
                                     int bscanLen,
                                     int repeat,
                                     int cropStart0,
                                     int cropDepth)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int z = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= bscanLen || z >= cropDepth) {
        return;
    }

    const Complex value = sampleShiftedFft(fftBuffer,
                                          ascanLen,
                                          bscanLen,
                                          repeat,
                                          x,
                                          cropStart0,
                                          cropDepth,
                                          static_cast<float>(z) + shifts[repeat]);
    magnitude[static_cast<size_t>(z) * bscanLen + x] = cabsval(value);
}

__global__ void computeStructuralFlowKernel(const Complex *frames,
                                            int cropDepth,
                                            int bscanLen,
                                            int angioRep,
                                            float *structuralBlock,
                                            float *flowBlock)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    if (x >= bscanLen) {
        return;
    }

    const size_t frameSize = static_cast<size_t>(cropDepth) * bscanLen;
    Complex covariance[kCudaMaxAngioRep * kCudaMaxAngioRep];
    for (int i = 0; i < kCudaMaxAngioRep * kCudaMaxAngioRep; ++i) {
        covariance[i] = make_cuFloatComplex(0.0f, 0.0f);
    }

    for (int z = 0; z < cropDepth; ++z) {
        Complex d[kCudaMaxAngioRep];
        for (int repeat = 0; repeat < angioRep; ++repeat) {
            d[repeat] = frames[static_cast<size_t>(repeat) * frameSize +
                static_cast<size_t>(z) + static_cast<size_t>(cropDepth) * x];
        }
        for (int row = 0; row < angioRep; ++row) {
            for (int col = 0; col < angioRep; ++col) {
                const int idx = row * kCudaMaxAngioRep + col;
                covariance[idx] = cadd(covariance[idx], conjmul(d[row], d[col]));
            }
        }
    }
    for (int row = 0; row < angioRep; ++row) {
        for (int col = 0; col < angioRep; ++col) {
            const int idx = row * kCudaMaxAngioRep + col;
            covariance[idx] = cscale(covariance[idx], 1.0f / cropDepth);
        }
    }

    Complex tissueVectors[2][kCudaMaxAngioRep];
    Complex deflated[kCudaMaxAngioRep * kCudaMaxAngioRep];
    for (int i = 0; i < kCudaMaxAngioRep * kCudaMaxAngioRep; ++i) {
        deflated[i] = covariance[i];
    }

    for (int eig = 0; eig < 2; ++eig) {
        dominantEigenvector(deflated, eig, angioRep, tissueVectors[eig]);
        const float lambda = fmaxf(0.0f, rayleighQuotient(deflated, tissueVectors[eig], angioRep));
        for (int row = 0; row < angioRep; ++row) {
            for (int col = 0; col < angioRep; ++col) {
                const int idx = row * kCudaMaxAngioRep + col;
                deflated[idx] = csub(deflated[idx],
                                     cscale(cmul(tissueVectors[eig][row],
                                                 cconj(tissueVectors[eig][col])),
                                            lambda));
            }
        }
    }

    for (int z = 0; z < cropDepth; ++z) {
        Complex d[kCudaMaxAngioRep];
        float structural = 0.0f;
        for (int repeat = 0; repeat < angioRep; ++repeat) {
            d[repeat] = frames[static_cast<size_t>(repeat) * frameSize +
                static_cast<size_t>(z) + static_cast<size_t>(cropDepth) * x];
            structural += cabsval(d[repeat]);
        }

        Complex tissue[kCudaMaxAngioRep];
        for (int repeat = 0; repeat < angioRep; ++repeat) {
            tissue[repeat] = make_cuFloatComplex(0.0f, 0.0f);
        }
        for (int eig = 0; eig < 2; ++eig) {
            Complex coefficient = make_cuFloatComplex(0.0f, 0.0f);
            for (int repeat = 0; repeat < angioRep; ++repeat) {
                coefficient = cadd(coefficient, cmul(d[repeat], tissueVectors[eig][repeat]));
            }
            for (int repeat = 0; repeat < angioRep; ++repeat) {
                tissue[repeat] = cadd(tissue[repeat],
                                      cmul(cconj(tissueVectors[eig][repeat]), coefficient));
            }
        }

        float flow = 0.0f;
        for (int repeat = 0; repeat < angioRep; ++repeat) {
            flow += cabsval(csub(d[repeat], tissue[repeat]));
        }

        const size_t outputIndex = static_cast<size_t>(z) * bscanLen + x;
        structuralBlock[outputIndex] = structural / angioRep;
        flowBlock[outputIndex] = flow / angioRep;
    }
}

__global__ void computeStructuralFlowFromFftKernel(const Complex *fftBuffer,
                                                   const float *shifts,
                                                   int ascanLen,
                                                   int bscanLen,
                                                   int angioRep,
                                                   int cropStart0,
                                                   int cropDepth,
                                                   float *structuralBlock,
                                                   float *flowBlock)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    if (x >= bscanLen) {
        return;
    }

    Complex covariance[kCudaMaxAngioRep * kCudaMaxAngioRep];
    for (int i = 0; i < kCudaMaxAngioRep * kCudaMaxAngioRep; ++i) {
        covariance[i] = make_cuFloatComplex(0.0f, 0.0f);
    }

    for (int z = 0; z < cropDepth; ++z) {
        Complex d[kCudaMaxAngioRep];
        for (int repeat = 0; repeat < angioRep; ++repeat) {
            d[repeat] = sampleShiftedFft(fftBuffer,
                                         ascanLen,
                                         bscanLen,
                                         repeat,
                                         x,
                                         cropStart0,
                                         cropDepth,
                                         static_cast<float>(z) + shifts[repeat]);
        }
        for (int row = 0; row < angioRep; ++row) {
            for (int col = 0; col < angioRep; ++col) {
                const int idx = row * kCudaMaxAngioRep + col;
                covariance[idx] = cadd(covariance[idx], conjmul(d[row], d[col]));
            }
        }
    }
    for (int row = 0; row < angioRep; ++row) {
        for (int col = 0; col < angioRep; ++col) {
            const int idx = row * kCudaMaxAngioRep + col;
            covariance[idx] = cscale(covariance[idx], 1.0f / cropDepth);
        }
    }

    Complex tissueVectors[2][kCudaMaxAngioRep];
    Complex deflated[kCudaMaxAngioRep * kCudaMaxAngioRep];
    for (int i = 0; i < kCudaMaxAngioRep * kCudaMaxAngioRep; ++i) {
        deflated[i] = covariance[i];
    }

    for (int eig = 0; eig < 2; ++eig) {
        dominantEigenvector(deflated, eig, angioRep, tissueVectors[eig]);
        const float lambda = fmaxf(0.0f, rayleighQuotient(deflated, tissueVectors[eig], angioRep));
        for (int row = 0; row < angioRep; ++row) {
            for (int col = 0; col < angioRep; ++col) {
                const int idx = row * kCudaMaxAngioRep + col;
                deflated[idx] = csub(deflated[idx],
                                     cscale(cmul(tissueVectors[eig][row],
                                                 cconj(tissueVectors[eig][col])),
                                            lambda));
            }
        }
    }

    for (int z = 0; z < cropDepth; ++z) {
        Complex d[kCudaMaxAngioRep];
        float structural = 0.0f;
        for (int repeat = 0; repeat < angioRep; ++repeat) {
            d[repeat] = sampleShiftedFft(fftBuffer,
                                         ascanLen,
                                         bscanLen,
                                         repeat,
                                         x,
                                         cropStart0,
                                         cropDepth,
                                         static_cast<float>(z) + shifts[repeat]);
            structural += cabsval(d[repeat]);
        }

        Complex tissue[kCudaMaxAngioRep];
        for (int repeat = 0; repeat < angioRep; ++repeat) {
            tissue[repeat] = make_cuFloatComplex(0.0f, 0.0f);
        }
        for (int eig = 0; eig < 2; ++eig) {
            Complex coefficient = make_cuFloatComplex(0.0f, 0.0f);
            for (int repeat = 0; repeat < angioRep; ++repeat) {
                coefficient = cadd(coefficient, cmul(d[repeat], tissueVectors[eig][repeat]));
            }
            for (int repeat = 0; repeat < angioRep; ++repeat) {
                tissue[repeat] = cadd(tissue[repeat],
                                      cmul(cconj(tissueVectors[eig][repeat]), coefficient));
            }
        }

        float flow = 0.0f;
        for (int repeat = 0; repeat < angioRep; ++repeat) {
            flow += cabsval(csub(d[repeat], tissue[repeat]));
        }

        const size_t outputIndex = static_cast<size_t>(z) * bscanLen + x;
        structuralBlock[outputIndex] = structural / angioRep;
        flowBlock[outputIndex] = flow / angioRep;
    }
}

} // namespace

struct VesselCudaProcessor
{
    int deviceId = 0;
    int ascanLen = 0;
    int bscanLen = 0;
    int angioRep = 0;
    int cropDepth = 0;
    size_t rawSampleCount = 0;
    size_t complexSampleCount = 0;
    size_t frameComplexCount = 0;
    size_t blockFloatCount = 0;
    uint16_t *dRaw = nullptr;
    float *dWindow = nullptr;
    Complex *dFft = nullptr;
    Complex *dFrames = nullptr;
    float *dReferenceMagnitude = nullptr;
    float *dMovingMagnitude = nullptr;
    float *dShiftScores = nullptr;
    float *dShifts = nullptr;
    float *dStructural = nullptr;
    float *dFlow = nullptr;
    cufftHandle fftPlan = 0;
};

bool createVesselCudaProcessor(const float *spectralWindow,
                               int ascanLen,
                               int bscanLen,
                               int angioRep,
                               int cropDepth,
                               VesselCudaProcessor **processor,
                               std::string *deviceDescription,
                               std::string *errorMessage)
{
    if (!processor) {
        setError(errorMessage, "Invalid CUDA processor output pointer.");
        return false;
    }
    *processor = nullptr;

    if (ascanLen <= 0 || bscanLen <= 0 || cropDepth <= 0) {
        setError(errorMessage, "Invalid CUDA OCTA dimensions.");
        return false;
    }
    if (angioRep <= 0 || angioRep > kCudaMaxAngioRep) {
        std::ostringstream stream;
        stream << "CUDA backend supports AngioRep 1.." << kCudaMaxAngioRep
               << ", got " << angioRep << ".";
        setError(errorMessage, stream.str());
        return false;
    }

    int deviceCount = 0;
    cudaError_t status = cudaGetDeviceCount(&deviceCount);
    if (status != cudaSuccess) {
        std::ostringstream stream;
        stream << "cudaGetDeviceCount failed (" << static_cast<int>(status)
               << "): " << cudaErrorText(status);
        setError(errorMessage, stream.str());
        return false;
    }
    if (deviceCount <= 0) {
        setError(errorMessage, "No available NVIDIA CUDA device was detected.");
        return false;
    }

    int selectedDevice = 0;
    cudaDeviceProp prop;
    if (!checkCuda(cudaGetDeviceProperties(&prop, selectedDevice), errorMessage, "cudaGetDeviceProperties")) {
        return false;
    }
    if (!checkCuda(cudaSetDevice(selectedDevice), errorMessage, "cudaSetDevice")) {
        return false;
    }

    VesselCudaProcessor *created = new VesselCudaProcessor;
    created->deviceId = selectedDevice;
    created->ascanLen = ascanLen;
    created->bscanLen = bscanLen;
    created->angioRep = angioRep;
    created->cropDepth = cropDepth;
    created->rawSampleCount = static_cast<size_t>(ascanLen) * bscanLen * angioRep;
    created->complexSampleCount = created->rawSampleCount;
    created->frameComplexCount = static_cast<size_t>(cropDepth) * bscanLen * angioRep;
    created->blockFloatCount = static_cast<size_t>(cropDepth) * bscanLen;

    int n[] = {ascanLen};
    cufftResult cufftStatus = cufftPlanMany(&created->fftPlan,
                                            1,
                                            n,
                                            nullptr,
                                            1,
                                            ascanLen,
                                            nullptr,
                                            1,
                                            ascanLen,
                                            CUFFT_C2C,
                                            bscanLen * angioRep);
    if (!checkCufft(cufftStatus, errorMessage, "cufftPlanMany")) {
        delete created;
        return false;
    }

    bool ok =
        checkCuda(cudaMalloc(&created->dRaw, created->rawSampleCount * sizeof(uint16_t)), errorMessage, "cudaMalloc raw") &&
        checkCuda(cudaMalloc(&created->dWindow, static_cast<size_t>(ascanLen) * sizeof(float)), errorMessage, "cudaMalloc window") &&
        checkCuda(cudaMalloc(&created->dFft, created->complexSampleCount * sizeof(Complex)), errorMessage, "cudaMalloc fft") &&
        checkCuda(cudaMalloc(&created->dFrames, created->frameComplexCount * sizeof(Complex)), errorMessage, "cudaMalloc frames") &&
        checkCuda(cudaMalloc(&created->dReferenceMagnitude, created->blockFloatCount * sizeof(float)), errorMessage, "cudaMalloc reference magnitude") &&
        checkCuda(cudaMalloc(&created->dMovingMagnitude, created->blockFloatCount * sizeof(float)), errorMessage, "cudaMalloc moving magnitude") &&
        checkCuda(cudaMalloc(&created->dShiftScores, (kCudaMaxRegistrationShiftPixels * 2 + 1) * sizeof(float)), errorMessage, "cudaMalloc shift scores") &&
        checkCuda(cudaMalloc(&created->dShifts, static_cast<size_t>(created->angioRep) * sizeof(float)), errorMessage, "cudaMalloc shifts") &&
        checkCuda(cudaMalloc(&created->dStructural, created->blockFloatCount * sizeof(float)), errorMessage, "cudaMalloc structural") &&
        checkCuda(cudaMalloc(&created->dFlow, created->blockFloatCount * sizeof(float)), errorMessage, "cudaMalloc flow") &&
        checkCuda(cudaMemcpy(created->dWindow,
                             spectralWindow,
                             static_cast<size_t>(ascanLen) * sizeof(float),
                             cudaMemcpyHostToDevice),
                  errorMessage,
                  "cudaMemcpy window");

    if (!ok) {
        destroyVesselCudaProcessor(created);
        return false;
    }

    if (deviceDescription) {
        std::ostringstream stream;
        stream << prop.name << " (CUDA capability " << prop.major << "." << prop.minor << ")";
        *deviceDescription = stream.str();
    }

    *processor = created;
    return true;
}

void destroyVesselCudaProcessor(VesselCudaProcessor *processor)
{
    if (!processor) {
        return;
    }

    cudaSetDevice(processor->deviceId);
    if (processor->fftPlan) {
        cufftDestroy(processor->fftPlan);
    }
    cudaFree(processor->dRaw);
    cudaFree(processor->dWindow);
    cudaFree(processor->dFft);
    cudaFree(processor->dFrames);
    cudaFree(processor->dReferenceMagnitude);
    cudaFree(processor->dMovingMagnitude);
    cudaFree(processor->dShiftScores);
    cudaFree(processor->dShifts);
    cudaFree(processor->dStructural);
    cudaFree(processor->dFlow);
    delete processor;
}

bool computeFftRowsCuda(VesselCudaProcessor *processor,
                        const uint16_t *rawBlock,
                        float *complexOutputInterleaved,
                        std::string *errorMessage)
{
    if (!processor || !rawBlock || !complexOutputInterleaved) {
        setError(errorMessage, "Invalid CUDA FFT input.");
        return false;
    }

    if (!checkCuda(cudaSetDevice(processor->deviceId), errorMessage, "cudaSetDevice")) {
        return false;
    }
    if (!checkCuda(cudaMemcpy(processor->dRaw,
                              rawBlock,
                              processor->rawSampleCount * sizeof(uint16_t),
                              cudaMemcpyHostToDevice),
                   errorMessage,
                   "cudaMemcpy raw")) {
        return false;
    }

    const dim3 block(256, 1);
    const dim3 grid((processor->ascanLen + block.x - 1) / block.x,
                    processor->bscanLen * processor->angioRep);
    rawToComplexKernel<<<grid, block>>>(processor->dRaw,
                                        processor->dWindow,
                                        processor->dFft,
                                        processor->ascanLen,
                                        processor->bscanLen,
                                        processor->angioRep);
    if (!checkCuda(cudaGetLastError(), errorMessage, "rawToComplexKernel")) {
        return false;
    }

    if (!checkCufft(cufftExecC2C(processor->fftPlan, processor->dFft, processor->dFft, CUFFT_FORWARD),
                    errorMessage,
                    "cufftExecC2C")) {
        return false;
    }

    return checkCuda(cudaMemcpy(complexOutputInterleaved,
                                processor->dFft,
                                processor->complexSampleCount * sizeof(Complex),
                                cudaMemcpyDeviceToHost),
                     errorMessage,
                     "cudaMemcpy fft output");
}

bool computeStructuralAndFlowCuda(VesselCudaProcessor *processor,
                                  const float *registeredFramesInterleaved,
                                  float *structuralBlock,
                                  float *flowBlock,
                                  std::string *errorMessage)
{
    if (!processor || !registeredFramesInterleaved || !structuralBlock || !flowBlock) {
        setError(errorMessage, "Invalid CUDA flow input.");
        return false;
    }

    if (!checkCuda(cudaSetDevice(processor->deviceId), errorMessage, "cudaSetDevice")) {
        return false;
    }
    if (!checkCuda(cudaMemcpy(processor->dFrames,
                              registeredFramesInterleaved,
                              processor->frameComplexCount * sizeof(Complex),
                              cudaMemcpyHostToDevice),
                   errorMessage,
                   "cudaMemcpy registered frames")) {
        return false;
    }

    const int threads = 128;
    const int blocks = (processor->bscanLen + threads - 1) / threads;
    computeStructuralFlowKernel<<<blocks, threads>>>(processor->dFrames,
                                                     processor->cropDepth,
                                                     processor->bscanLen,
                                                     processor->angioRep,
                                                     processor->dStructural,
                                                     processor->dFlow);
    if (!checkCuda(cudaGetLastError(), errorMessage, "computeStructuralFlowKernel")) {
        return false;
    }

    return
        checkCuda(cudaMemcpy(structuralBlock,
                             processor->dStructural,
                             processor->blockFloatCount * sizeof(float),
                             cudaMemcpyDeviceToHost),
                  errorMessage,
                  "cudaMemcpy structural block") &&
        checkCuda(cudaMemcpy(flowBlock,
                             processor->dFlow,
                             processor->blockFloatCount * sizeof(float),
                             cudaMemcpyDeviceToHost),
                  errorMessage,
                  "cudaMemcpy flow block");
}

bool computeRegisteredStructuralAndFlowCuda(VesselCudaProcessor *processor,
                                            const uint16_t *rawBlock,
                                            int cropStart0,
                                            int maxRegistrationShiftPixels,
                                            float *structuralBlock,
                                            float *flowBlock,
                                            std::string *errorMessage)
{
    if (!processor || !rawBlock || !structuralBlock || !flowBlock) {
        setError(errorMessage, "Invalid CUDA registered flow input.");
        return false;
    }

    if (!checkCuda(cudaSetDevice(processor->deviceId), errorMessage, "cudaSetDevice")) {
        return false;
    }
    if (!checkCuda(cudaMemcpy(processor->dRaw,
                              rawBlock,
                              processor->rawSampleCount * sizeof(uint16_t),
                              cudaMemcpyHostToDevice),
                   errorMessage,
                   "cudaMemcpy raw")) {
        return false;
    }

    const dim3 rawBlockSize(256, 1);
    const dim3 rawGrid((processor->ascanLen + rawBlockSize.x - 1) / rawBlockSize.x,
                       processor->bscanLen * processor->angioRep);
    rawToComplexKernel<<<rawGrid, rawBlockSize>>>(processor->dRaw,
                                                 processor->dWindow,
                                                 processor->dFft,
                                                 processor->ascanLen,
                                                 processor->bscanLen,
                                                 processor->angioRep);
    if (!checkCuda(cudaGetLastError(), errorMessage, "rawToComplexKernel")) {
        return false;
    }

    if (!checkCufft(cufftExecC2C(processor->fftPlan, processor->dFft, processor->dFft, CUFFT_FORWARD),
                    errorMessage,
                    "cufftExecC2C")) {
        return false;
    }

    if (!checkCuda(cudaMemset(processor->dShifts,
                              0,
                              static_cast<size_t>(processor->angioRep) * sizeof(float)),
                   errorMessage,
                   "cudaMemset shifts")) {
        return false;
    }

    const int maxShift = std::max(0, std::min(std::min(maxRegistrationShiftPixels, kCudaMaxRegistrationShiftPixels),
                                              processor->cropDepth - 1));
    const dim3 magnitudeBlock(16, 16);
    const dim3 magnitudeGrid((processor->bscanLen + magnitudeBlock.x - 1) / magnitudeBlock.x,
                             (processor->cropDepth + magnitudeBlock.y - 1) / magnitudeBlock.y);
    extractMagnitudeKernel<<<magnitudeGrid, magnitudeBlock>>>(processor->dFft,
                                                             processor->dReferenceMagnitude,
                                                             processor->ascanLen,
                                                             processor->bscanLen,
                                                             0,
                                                             cropStart0,
                                                             processor->cropDepth);
    if (!checkCuda(cudaGetLastError(), errorMessage, "extractMagnitudeKernel reference")) {
        return false;
    }

    if (maxShift > 0) {
        const int shiftCount = maxShift * 2 + 1;
        const int scoreThreads = 256;
        const size_t scoreSharedBytes = static_cast<size_t>(scoreThreads) * 3 * sizeof(float);
        for (int repeat = 1; repeat < processor->angioRep; ++repeat) {
            extractMagnitudeKernel<<<magnitudeGrid, magnitudeBlock>>>(processor->dFft,
                                                                     processor->dMovingMagnitude,
                                                                     processor->ascanLen,
                                                                     processor->bscanLen,
                                                                     repeat,
                                                                     cropStart0,
                                                                     processor->cropDepth);
            if (!checkCuda(cudaGetLastError(), errorMessage, "extractMagnitudeKernel moving")) {
                return false;
            }

            estimateAxialShiftScoresKernel<<<shiftCount, scoreThreads, scoreSharedBytes>>>(
                processor->dReferenceMagnitude,
                processor->dMovingMagnitude,
                processor->dShiftScores,
                processor->bscanLen,
                processor->cropDepth,
                maxShift);
            if (!checkCuda(cudaGetLastError(), errorMessage, "estimateAxialShiftScoresKernel")) {
                return false;
            }

            selectAxialShiftKernel<<<1, 1>>>(processor->dShiftScores,
                                             processor->dShifts,
                                             repeat,
                                             maxShift);
            if (!checkCuda(cudaGetLastError(), errorMessage, "selectAxialShiftKernel")) {
                return false;
            }

            shiftMagnitudeKernel<<<magnitudeGrid, magnitudeBlock>>>(processor->dFft,
                                                                   processor->dReferenceMagnitude,
                                                                   processor->dShifts,
                                                                   processor->ascanLen,
                                                                   processor->bscanLen,
                                                                   repeat,
                                                                   cropStart0,
                                                                   processor->cropDepth);
            if (!checkCuda(cudaGetLastError(), errorMessage, "shiftMagnitudeKernel")) {
                return false;
            }
        }
    }

    const int flowThreads = 128;
    const int flowBlocks = (processor->bscanLen + flowThreads - 1) / flowThreads;
    computeStructuralFlowFromFftKernel<<<flowBlocks, flowThreads>>>(processor->dFft,
                                                                    processor->dShifts,
                                                                    processor->ascanLen,
                                                                    processor->bscanLen,
                                                                    processor->angioRep,
                                                                    cropStart0,
                                                                    processor->cropDepth,
                                                                    processor->dStructural,
                                                                    processor->dFlow);
    if (!checkCuda(cudaGetLastError(), errorMessage, "computeStructuralFlowFromFftKernel")) {
        return false;
    }

    return
        checkCuda(cudaMemcpy(structuralBlock,
                             processor->dStructural,
                             processor->blockFloatCount * sizeof(float),
                             cudaMemcpyDeviceToHost),
                  errorMessage,
                  "cudaMemcpy structural block") &&
        checkCuda(cudaMemcpy(flowBlock,
                             processor->dFlow,
                             processor->blockFloatCount * sizeof(float),
                             cudaMemcpyDeviceToHost),
                  errorMessage,
                  "cudaMemcpy flow block");
}
