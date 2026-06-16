#include "VesselSegmenter.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <queue>
#include <vector>

namespace VesselSegmenter {
namespace {

cv::Mat toBinaryMask(const cv::Mat &input)
{
    cv::Mat mask;
    if (input.empty()) {
        return mask;
    }
    if (input.type() == CV_8U) {
        cv::threshold(input, mask, 0, 255, cv::THRESH_BINARY);
    } else {
        input.convertTo(mask, CV_8U);
        cv::threshold(mask, mask, 0, 255, cv::THRESH_BINARY);
    }
    return mask;
}

bool isMaskPixel(const cv::Mat &mask, int row, int col)
{
    return row >= 0 && row < mask.rows && col >= 0 && col < mask.cols
        && mask.at<uchar>(row, col) != 0;
}

int neighborCount8(const cv::Mat &mask, int row, int col)
{
    int count = 0;
    for (int y = row - 1; y <= row + 1; ++y) {
        for (int x = col - 1; x <= col + 1; ++x) {
            if (y == row && x == col) {
                continue;
            }
            count += isMaskPixel(mask, y, x) ? 1 : 0;
        }
    }
    return count;
}

cv::Mat skeletonize(const cv::Mat &inputMask)
{
    cv::Mat image = toBinaryMask(inputMask);
    if (image.empty()) {
        return image;
    }
    image /= 255;

    cv::Mat previous = cv::Mat::zeros(image.size(), CV_8U);
    cv::Mat current = image.clone();
    cv::Mat marker = cv::Mat::zeros(image.size(), CV_8U);

    do {
        marker.setTo(0);
        for (int row = 1; row < current.rows - 1; ++row) {
            for (int col = 1; col < current.cols - 1; ++col) {
                const uchar p1 = current.at<uchar>(row, col);
                if (!p1) {
                    continue;
                }
                const uchar p2 = current.at<uchar>(row - 1, col);
                const uchar p3 = current.at<uchar>(row - 1, col + 1);
                const uchar p4 = current.at<uchar>(row, col + 1);
                const uchar p5 = current.at<uchar>(row + 1, col + 1);
                const uchar p6 = current.at<uchar>(row + 1, col);
                const uchar p7 = current.at<uchar>(row + 1, col - 1);
                const uchar p8 = current.at<uchar>(row, col - 1);
                const uchar p9 = current.at<uchar>(row - 1, col - 1);

                const int transitions = (p2 == 0 && p3 == 1) + (p3 == 0 && p4 == 1)
                    + (p4 == 0 && p5 == 1) + (p5 == 0 && p6 == 1)
                    + (p6 == 0 && p7 == 1) + (p7 == 0 && p8 == 1)
                    + (p8 == 0 && p9 == 1) + (p9 == 0 && p2 == 1);
                const int neighbors = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;

                if (transitions == 1 && neighbors >= 2 && neighbors <= 6
                    && p2 * p4 * p6 == 0 && p4 * p6 * p8 == 0) {
                    marker.at<uchar>(row, col) = 1;
                }
            }
        }
        current.setTo(0, marker);

        marker.setTo(0);
        for (int row = 1; row < current.rows - 1; ++row) {
            for (int col = 1; col < current.cols - 1; ++col) {
                const uchar p1 = current.at<uchar>(row, col);
                if (!p1) {
                    continue;
                }
                const uchar p2 = current.at<uchar>(row - 1, col);
                const uchar p3 = current.at<uchar>(row - 1, col + 1);
                const uchar p4 = current.at<uchar>(row, col + 1);
                const uchar p5 = current.at<uchar>(row + 1, col + 1);
                const uchar p6 = current.at<uchar>(row + 1, col);
                const uchar p7 = current.at<uchar>(row + 1, col - 1);
                const uchar p8 = current.at<uchar>(row, col - 1);
                const uchar p9 = current.at<uchar>(row - 1, col - 1);

                const int transitions = (p2 == 0 && p3 == 1) + (p3 == 0 && p4 == 1)
                    + (p4 == 0 && p5 == 1) + (p5 == 0 && p6 == 1)
                    + (p6 == 0 && p7 == 1) + (p7 == 0 && p8 == 1)
                    + (p8 == 0 && p9 == 1) + (p9 == 0 && p2 == 1);
                const int neighbors = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;

                if (transitions == 1 && neighbors >= 2 && neighbors <= 6
                    && p2 * p4 * p8 == 0 && p2 * p6 * p8 == 0) {
                    marker.at<uchar>(row, col) = 1;
                }
            }
        }
        current.setTo(0, marker);

        cv::absdiff(current, previous, marker);
        current.copyTo(previous);
    } while (cv::countNonZero(marker) > 0);

    current *= 255;
    return current;
}

cv::Mat pruneSpurs(const cv::Mat &inputSkeleton, int iterations)
{
    cv::Mat skeleton = toBinaryMask(inputSkeleton);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        std::vector<cv::Point> endpoints;
        for (int row = 0; row < skeleton.rows; ++row) {
            for (int col = 0; col < skeleton.cols; ++col) {
                if (skeleton.at<uchar>(row, col) && neighborCount8(skeleton, row, col) <= 1) {
                    endpoints.push_back(cv::Point(col, row));
                }
            }
        }
        if (endpoints.empty()) {
            break;
        }
        for (const cv::Point &point : endpoints) {
            skeleton.at<uchar>(point.y, point.x) = 0;
        }
    }
    return skeleton;
}

cv::Mat labelSkeletonSegments(const cv::Mat &skeleton,
                              const Params &params,
                              int *segmentCount)
{
    cv::Mat chainMask = cv::Mat::zeros(skeleton.size(), CV_8U);
    for (int row = 0; row < skeleton.rows; ++row) {
        for (int col = 0; col < skeleton.cols; ++col) {
            if (!skeleton.at<uchar>(row, col)) {
                continue;
            }
            if (neighborCount8(skeleton, row, col) == 2) {
                chainMask.at<uchar>(row, col) = 255;
            }
        }
    }

    cv::Mat rawLabels;
    cv::Mat stats;
    cv::Mat centroids;
    int rawCount = cv::connectedComponentsWithStats(chainMask, rawLabels, stats, centroids, 8, CV_32S);
    cv::Mat labels = cv::Mat::zeros(skeleton.size(), CV_32S);
    int nextLabel = 1;
    for (int label = 1; label < rawCount; ++label) {
        if (stats.at<int>(label, cv::CC_STAT_AREA) < params.minSkeletonSegmentPixels) {
            continue;
        }
        labels.setTo(nextLabel, rawLabels == label);
        ++nextLabel;
    }

    if (nextLabel == 1 && cv::countNonZero(skeleton) > 0) {
        rawCount = cv::connectedComponentsWithStats(skeleton, rawLabels, stats, centroids, 8, CV_32S);
        for (int label = 1; label < rawCount; ++label) {
            if (stats.at<int>(label, cv::CC_STAT_AREA) < params.minSkeletonSegmentPixels) {
                continue;
            }
            labels.setTo(nextLabel, rawLabels == label);
            ++nextLabel;
        }
    }

    if (segmentCount) {
        *segmentCount = nextLabel - 1;
    }
    return labels;
}

cv::Mat assignVesselPixelsToSegments(const cv::Mat &vesselMask,
                                     const cv::Mat &skeletonLabels)
{
    cv::Mat labels = cv::Mat::zeros(vesselMask.size(), CV_32S);
    std::queue<cv::Point> queue;

    for (int row = 0; row < skeletonLabels.rows; ++row) {
        const int *src = skeletonLabels.ptr<int>(row);
        int *dst = labels.ptr<int>(row);
        for (int col = 0; col < skeletonLabels.cols; ++col) {
            if (src[col] <= 0) {
                continue;
            }
            dst[col] = src[col];
            queue.push(cv::Point(col, row));
        }
    }

    const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    const int dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    while (!queue.empty()) {
        const cv::Point point = queue.front();
        queue.pop();
        const int label = labels.at<int>(point.y, point.x);
        for (int i = 0; i < 8; ++i) {
            const int x = point.x + dx[i];
            const int y = point.y + dy[i];
            if (!isMaskPixel(vesselMask, y, x) || labels.at<int>(y, x) != 0) {
                continue;
            }
            labels.at<int>(y, x) = label;
            queue.push(cv::Point(x, y));
        }
    }
    return labels;
}

cv::Mat filterAndRenumberLabels(const cv::Mat &inputLabels,
                                int inputCount,
                                int minAssignedPixels,
                                int *outputCount,
                                int *assignedPixelCount)
{
    std::vector<int> counts(static_cast<size_t>(inputCount) + 1, 0);
    for (int row = 0; row < inputLabels.rows; ++row) {
        const int *labels = inputLabels.ptr<int>(row);
        for (int col = 0; col < inputLabels.cols; ++col) {
            const int label = labels[col];
            if (label > 0 && label <= inputCount) {
                ++counts[static_cast<size_t>(label)];
            }
        }
    }

    std::vector<int> remap(static_cast<size_t>(inputCount) + 1, 0);
    int nextLabel = 1;
    for (int label = 1; label <= inputCount; ++label) {
        if (counts[static_cast<size_t>(label)] >= minAssignedPixels) {
            remap[static_cast<size_t>(label)] = nextLabel++;
        }
    }

    cv::Mat output = cv::Mat::zeros(inputLabels.size(), CV_32S);
    int assigned = 0;
    for (int row = 0; row < inputLabels.rows; ++row) {
        const int *src = inputLabels.ptr<int>(row);
        int *dst = output.ptr<int>(row);
        for (int col = 0; col < inputLabels.cols; ++col) {
            const int label = src[col];
            if (label > 0 && label <= inputCount) {
                dst[col] = remap[static_cast<size_t>(label)];
                if (dst[col] > 0) {
                    ++assigned;
                }
            }
        }
    }

    if (outputCount) {
        *outputCount = nextLabel - 1;
    }
    if (assignedPixelCount) {
        *assignedPixelCount = assigned;
    }
    return output;
}

} // namespace

Result segmentVesselMask(const cv::Mat &vesselMask, const Params &params)
{
    Result result;
    const cv::Mat binary = toBinaryMask(vesselMask);
    if (binary.empty() || cv::countNonZero(binary) == 0) {
        return result;
    }

    cv::Mat skeleton = skeletonize(binary);
    if (params.spurPruneIterations > 0) {
        skeleton = pruneSpurs(skeleton, params.spurPruneIterations);
    }
    result.skeleton = skeleton;
    if (cv::countNonZero(skeleton) == 0) {
        return result;
    }

    int skeletonSegmentCount = 0;
    const cv::Mat skeletonLabels = labelSkeletonSegments(skeleton, params, &skeletonSegmentCount);
    result.skeletonSegmentCount = skeletonSegmentCount;
    if (skeletonSegmentCount <= 0) {
        return result;
    }

    const cv::Mat assigned = assignVesselPixelsToSegments(binary, skeletonLabels);
    result.segmentLabels = filterAndRenumberLabels(assigned,
                                                   skeletonSegmentCount,
                                                   params.minAssignedPixels,
                                                   &result.segmentCount,
                                                   &result.assignedPixelCount);
    return result;
}

} // namespace VesselSegmenter
