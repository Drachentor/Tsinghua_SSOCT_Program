#include "VesselFindingDialog.h"
#include "VesselFindingDialogUISetup.h"
#include "ui_VesselFindingDialog.h"
#include "vessel_colorMap.h"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QIODevice>
#include <QLayout>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QSettings>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QVector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <vector>

namespace {

QString settingsFilePath()
{
    return QFileInfo(QString::fromLocal8Bit(__FILE__)).absolutePath()
        + QStringLiteral("/settings.ini");
}

QString defaultDialogPath()
{
    return QFileInfo(QString::fromLocal8Bit(__FILE__)).absolutePath();
}

QString vesselImageDialogPath()
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    const QString path = settings.value(QStringLiteral("paths/vesselImagePath")).toString();
    if (!path.isEmpty() && QFileInfo(path).exists()) {
        return path;
    }
    return defaultDialogPath();
}

void saveVesselImagePath(const QString &filePath)
{
    if (filePath.isEmpty()) {
        return;
    }

    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("paths/vesselImagePath"), filePath);
    settings.sync();
}

const char *kLightDialogStyleSheet =
    "QFileDialog, QMessageBox { background-color: white; color: black; }"
    "QFileDialog QLabel, QMessageBox QLabel { background-color: white; color: black; }"
    "QFileDialog QCheckBox { background-color: white; color: black; }"
    "QFileDialog QLineEdit { background-color: white; color: black; }"
    "QFileDialog QComboBox { background-color: white; color: black; }"
    "QFileDialog QTreeView, QFileDialog QListView { background-color: white; color: black; }"
    "QFileDialog QPushButton, QMessageBox QPushButton { background-color: white; color: black; }"
    "QFileDialog QPushButton:hover, QMessageBox QPushButton:hover { background-color: rgb(235,235,235); }";

int styledMessageBox(QWidget *parent,
                     QMessageBox::Icon icon,
                     const QString &title,
                     const QString &text,
                     QMessageBox::StandardButtons buttons,
                     QMessageBox::StandardButton defaultButton)
{
    QMessageBox box(icon, title, text, buttons, parent);
    box.setDefaultButton(defaultButton);
    box.setStyleSheet(kLightDialogStyleSheet);
    return box.exec();
}

cv::Mat readImageFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return cv::Mat();
    }

    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) {
        return cv::Mat();
    }

    std::vector<uchar> encoded(bytes.begin(), bytes.end());
    return cv::imdecode(encoded, cv::IMREAD_COLOR);
}

cv::Mat readRgbImageFile(const QString &filePath)
{
    cv::Mat bgrImage = readImageFile(filePath);
    if (bgrImage.empty()) {
        return cv::Mat();
    }

    cv::Mat rgbImage;
    cv::cvtColor(bgrImage, rgbImage, cv::COLOR_BGR2RGB);
    return rgbImage;
}

void appendUniquePath(QStringList &paths, const QString &path)
{
    if (!path.isEmpty() && !paths.contains(path)) {
        paths.append(path);
    }
}

QStringList depthProjectionPathCandidates(const QString &imagePath)
{
    QStringList candidates;
    const QFileInfo fileInfo(imagePath);
    const QString directory = fileInfo.absolutePath();
    const QString baseName = fileInfo.completeBaseName();

    if (baseName.endsWith(QStringLiteral("_depth"), Qt::CaseInsensitive)) {
        appendUniquePath(candidates, imagePath);
    }

    QStringList sourceBaseNames;
    sourceBaseNames << baseName;
    const QStringList removableSuffixes = {
        QStringLiteral("_grayscale"),
        QStringLiteral("_vascular"),
        QStringLiteral("_roi")
    };
    for (const QString &suffix : removableSuffixes) {
        if (baseName.endsWith(suffix, Qt::CaseInsensitive)) {
            sourceBaseNames << baseName.left(baseName.size() - suffix.size());
        }
    }

    for (const QString &sourceBaseName : sourceBaseNames) {
        appendUniquePath(candidates, directory + QStringLiteral("/") + sourceBaseName + QStringLiteral("_depth.tiff"));
        appendUniquePath(candidates, directory + QStringLiteral("/") + sourceBaseName + QStringLiteral("_depth.tif"));
        appendUniquePath(candidates, directory + QStringLiteral("/") + sourceBaseName + QStringLiteral("_depth.png"));
        appendUniquePath(candidates, directory + QStringLiteral("/") + sourceBaseName + QStringLiteral("_depthColor.tiff"));
        appendUniquePath(candidates, directory + QStringLiteral("/") + sourceBaseName + QStringLiteral("_depth_color.tiff"));
    }

    return candidates;
}

const int kMinVesselArea = 200;
const int kHoleRingRadius = 4;
const int kNoSignalGrayThreshold = 8;
const int kManualSearchRadius = 30;
const int kDisplayWidgetWidth = 720;
const int kDisplayWidgetHeight = 400;
const int kPathSmoothRadius = 8;
const int kPathSmoothPasses = 3;
const int kInteractionNone = 0;
const int kInteractionSelectSeed = 1;
const int kInteractionFixSkeleton = 2;
const int kInteractionDrawPath = 3;
const int kSkeletonFixSearchRadius = 45;
const int kSkeletonFixHalfLength = 24;
const int kMinSkeletonFixPathLength = 7;
const int kSkeletonFixSpurMaxLength = 12;
const double kSkeletonFixBandRadius = 4.0;
const double kSkeletonFixSpurCleanupRadius = 12.0;
const double kDiameterWeight = 1.0;
const double kSharedStemOverlapPenalty = 0.25;
const unsigned short kPositionDacZeroCode = 32768;
const int kInvalidDepthIndex = -1;
const int kDefaultDepthTolerance = 20;
const int kSeedDepthSampleRadius = 6;
const int kSkeletonDepthSampleRadius = 3;
const int kDepthFillRadius = 2;
const double kMinDepthColorSignal = 18.0 / 255.0;
const double kMinDepthColorfulness = 0.08;
const double kMaxDepthColorDistance = 0.16;
const int kVesselWavSampleRate = 48000;
const int kVesselWavBitsPerSample = 24;
const int kVesselWavChannels = 2;
const double kVesselWavMaxAmplitudeRatio = 0.8;
const int kVesselWavMaxSampleValue = 8388607;

QSize scaledImageSize(const QSize &imageSize, const QSize &canvasSize)
{
    if (imageSize.isEmpty() || canvasSize.isEmpty()) {
        return QSize();
    }

    double scale = 1.0;
    if (imageSize.width() > canvasSize.width() || imageSize.height() > canvasSize.height()) {
        scale = std::min(static_cast<double>(canvasSize.width()) / imageSize.width(),
                         static_cast<double>(canvasSize.height()) / imageSize.height());
    }

    return QSize(std::max(1, static_cast<int>(std::round(imageSize.width() * scale))),
                 std::max(1, static_cast<int>(std::round(imageSize.height() * scale))));
}

struct PathCandidate
{
    cv::Mat mask;
    std::vector<int> indices;
    cv::Point2d direction;
    cv::Point2d seedDirection;
    int seedNeighborIndex = -1;
    double score = 0.0;
};

struct PathPairSelection
{
    int first = -1;
    int second = -1;
};

struct SkeletonFixResult
{
    bool changed = false;
    int changedPixels = 0;
    int pathPixels = 0;
    double clickDistance = 0.0;
    QPoint firstAnchor;
    QPoint secondAnchor;
};

bool isMaskPixel(const cv::Mat &mask, int row, int col)
{
    return row >= 0 && row < mask.rows && col >= 0 && col < mask.cols
        && mask.at<uchar>(row, col) != 0;
}

int linearIndex(int row, int col, int cols)
{
    return row * cols + col;
}

cv::Point indexPoint(int index, int cols)
{
    return cv::Point(index % cols, index / cols);
}

cv::Mat diskElement(int radius)
{
    const int size = radius * 2 + 1;
    return cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(size, size));
}

cv::Mat toBinaryMask(const cv::Mat &input)
{
    cv::Mat mask;
    if (input.empty()) {
        return mask;
    }
    if (input.type() == CV_8UC1) {
        cv::threshold(input, mask, 0, 255, cv::THRESH_BINARY);
    } else {
        input.convertTo(mask, CV_8U);
        cv::threshold(mask, mask, 0, 255, cv::THRESH_BINARY);
    }
    return mask;
}

double clampUnit(double value)
{
    return std::max(0.0, std::min(1.0, value));
}

cv::Mat areaOpen(const cv::Mat &inputMask, int minArea)
{
    cv::Mat mask = toBinaryMask(inputMask);
    if (mask.empty() || minArea <= 1) {
        return mask;
    }

    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int count = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8, CV_32S);
    cv::Mat output = cv::Mat::zeros(mask.size(), CV_8UC1);
    for (int label = 1; label < count; ++label) {
        if (stats.at<int>(label, cv::CC_STAT_AREA) >= minArea) {
            output.setTo(255, labels == label);
        }
    }
    return output;
}

const std::vector<cv::Vec3f> &normalizedDepthPalette()
{
    static const std::vector<cv::Vec3f> palette = []() {
        std::vector<cv::Vec3f> values;
        values.reserve(CMAP_ROWS);
        for (int i = 0; i < CMAP_ROWS; ++i) {
            const float r = static_cast<float>(cmap[i][0]);
            const float g = static_cast<float>(cmap[i][1]);
            const float b = static_cast<float>(cmap[i][2]);
            const float maxChannel = std::max(r, std::max(g, b));
            if (maxChannel <= 0.0f) {
                values.push_back(cv::Vec3f(0.0f, 0.0f, 0.0f));
            } else {
                values.push_back(cv::Vec3f(r / maxChannel, g / maxChannel, b / maxChannel));
            }
        }
        return values;
    }();
    return palette;
}

int nearestDepthIndexForRgb(const cv::Vec3b &rgb)
{
    const float r = static_cast<float>(rgb[0]) / 255.0f;
    const float g = static_cast<float>(rgb[1]) / 255.0f;
    const float b = static_cast<float>(rgb[2]) / 255.0f;
    const float maxChannel = std::max(r, std::max(g, b));
    const float minChannel = std::min(r, std::min(g, b));
    if (maxChannel < kMinDepthColorSignal || maxChannel - minChannel < kMinDepthColorfulness) {
        return kInvalidDepthIndex;
    }

    const cv::Vec3f normalized(r / maxChannel, g / maxChannel, b / maxChannel);
    const std::vector<cv::Vec3f> &palette = normalizedDepthPalette();
    double bestDistance = std::numeric_limits<double>::infinity();
    int bestIndex = kInvalidDepthIndex;
    for (int i = 0; i < static_cast<int>(palette.size()); ++i) {
        const cv::Vec3f delta = normalized - palette[static_cast<size_t>(i)];
        const double distance = delta.dot(delta);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    return bestDistance <= kMaxDepthColorDistance ? bestIndex : kInvalidDepthIndex;
}

cv::Mat makeDepthIndexMapFromRgb(const cv::Mat &rgbImage, const cv::Mat &validMask)
{
    cv::Mat depthIndexMap(rgbImage.size(), CV_32S, cv::Scalar(kInvalidDepthIndex));
    if (rgbImage.empty() || rgbImage.type() != CV_8UC3) {
        return depthIndexMap;
    }

    const cv::Mat mask = toBinaryMask(validMask);
    for (int row = 0; row < rgbImage.rows; ++row) {
        for (int col = 0; col < rgbImage.cols; ++col) {
            if (!mask.empty() && !mask.at<uchar>(row, col)) {
                continue;
            }
            depthIndexMap.at<int>(row, col) = nearestDepthIndexForRgb(rgbImage.at<cv::Vec3b>(row, col));
        }
    }
    return depthIndexMap;
}

cv::Mat fillMissingDepthIndices(const cv::Mat &depthIndexMap, const cv::Mat &validMask, int radius)
{
    if (depthIndexMap.empty() || radius <= 0) {
        return depthIndexMap.clone();
    }

    const cv::Mat mask = toBinaryMask(validMask);
    cv::Mat filled = depthIndexMap.clone();
    cv::Mat next = filled.clone();
    for (int row = 0; row < filled.rows; ++row) {
        for (int col = 0; col < filled.cols; ++col) {
            if (!mask.empty() && !mask.at<uchar>(row, col)) {
                continue;
            }
            if (filled.at<int>(row, col) != kInvalidDepthIndex) {
                continue;
            }

            std::vector<int> neighborDepths;
            for (int y = std::max(0, row - radius); y <= std::min(filled.rows - 1, row + radius); ++y) {
                for (int x = std::max(0, col - radius); x <= std::min(filled.cols - 1, col + radius); ++x) {
                    const int depth = filled.at<int>(y, x);
                    if (depth != kInvalidDepthIndex) {
                        neighborDepths.push_back(depth);
                    }
                }
            }
            if (!neighborDepths.empty()) {
                std::sort(neighborDepths.begin(), neighborDepths.end());
                next.at<int>(row, col) = neighborDepths[neighborDepths.size() / 2];
            }
        }
    }
    return next;
}

int medianDepthNearPoint(const cv::Mat &depthIndexMap, const cv::Mat &supportMask,
                         const QPoint &point, int radius)
{
    if (depthIndexMap.empty() || point.x() < 0 || point.y() < 0
        || point.x() >= depthIndexMap.cols || point.y() >= depthIndexMap.rows) {
        return kInvalidDepthIndex;
    }

    const cv::Mat mask = toBinaryMask(supportMask);
    std::vector<int> values;
    const int radius2 = radius * radius;
    for (int row = std::max(0, point.y() - radius);
         row <= std::min(depthIndexMap.rows - 1, point.y() + radius);
         ++row) {
        for (int col = std::max(0, point.x() - radius);
             col <= std::min(depthIndexMap.cols - 1, point.x() + radius);
             ++col) {
            const int dx = col - point.x();
            const int dy = row - point.y();
            if (dx * dx + dy * dy > radius2) {
                continue;
            }
            if (!mask.empty() && !mask.at<uchar>(row, col)) {
                continue;
            }
            const int depth = depthIndexMap.at<int>(row, col);
            if (depth != kInvalidDepthIndex) {
                values.push_back(depth);
            }
        }
    }

    if (values.empty()) {
        return kInvalidDepthIndex;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

int countValidDepthPixels(const cv::Mat &depthIndexMap, const cv::Mat &validMask)
{
    if (depthIndexMap.empty()) {
        return 0;
    }
    const cv::Mat mask = toBinaryMask(validMask);
    int count = 0;
    for (int row = 0; row < depthIndexMap.rows; ++row) {
        for (int col = 0; col < depthIndexMap.cols; ++col) {
            if (!mask.empty() && !mask.at<uchar>(row, col)) {
                continue;
            }
            if (depthIndexMap.at<int>(row, col) != kInvalidDepthIndex) {
                ++count;
            }
        }
    }
    return count;
}

cv::Mat fillLikelyVesselHoles(const cv::Mat &inputMask, int maxHoleArea, int ringRadius, double minWhiteRatio)
{
    cv::Mat mask = toBinaryMask(inputMask);
    if (mask.empty() || maxHoleArea <= 0) {
        return mask;
    }

    cv::Mat background;
    cv::bitwise_not(mask, background);

    cv::Mat flood = background.clone();
    for (int col = 0; col < flood.cols; ++col) {
        if (flood.at<uchar>(0, col)) {
            cv::floodFill(flood, cv::Point(col, 0), 0);
        }
        if (flood.at<uchar>(flood.rows - 1, col)) {
            cv::floodFill(flood, cv::Point(col, flood.rows - 1), 0);
        }
    }
    for (int row = 0; row < flood.rows; ++row) {
        if (flood.at<uchar>(row, 0)) {
            cv::floodFill(flood, cv::Point(0, row), 0);
        }
        if (flood.at<uchar>(row, flood.cols - 1)) {
            cv::floodFill(flood, cv::Point(flood.cols - 1, row), 0);
        }
    }

    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int count = cv::connectedComponentsWithStats(flood, labels, stats, centroids, 8, CV_32S);
    cv::Mat fillMask = cv::Mat::zeros(mask.size(), CV_8UC1);
    const cv::Mat ringElement = diskElement(ringRadius);

    for (int label = 1; label < count; ++label) {
        const int area = stats.at<int>(label, cv::CC_STAT_AREA);
        if (area > maxHoleArea) {
            continue;
        }

        cv::Mat singleHole = labels == label;
        cv::Mat localRing;
        cv::dilate(singleHole, localRing, ringElement);
        localRing.setTo(0, singleHole);
        const int ringArea = cv::countNonZero(localRing);
        if (ringArea == 0) {
            continue;
        }

        cv::Mat whiteRing;
        cv::bitwise_and(mask, localRing, whiteRing);
        const double whiteRatio = static_cast<double>(cv::countNonZero(whiteRing)) / ringArea;
        if (whiteRatio >= minWhiteRatio) {
            fillMask.setTo(255, singleHole);
        }
    }

    cv::Mat output;
    cv::bitwise_or(mask, fillMask, output);
    return output;
}

cv::Mat makeVesselGrayImage(const cv::Mat &rgbImage)
{
    cv::Mat inputFloat;
    rgbImage.convertTo(inputFloat, CV_32F, 1.0 / 255.0);

    cv::Mat gray;
    if (inputFloat.channels() == 3) {
        std::vector<cv::Mat> channels;
        cv::split(inputFloat, channels);
        gray = channels[0] + channels[2] + 0.25f * channels[1];
    } else {
        gray = inputFloat;
    }

    cv::normalize(gray, gray, 0.0, 1.0, cv::NORM_MINMAX);
    cv::Mat gray8;
    gray.convertTo(gray8, CV_8U, 255.0);
    cv::medianBlur(gray8, gray8, 3);

    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    clahe->apply(gray8, gray8);
    return gray8;
}

cv::Mat segmentVesselMask(const cv::Mat &grayImage, double brightness, int minArea,
                          int closeRadius, int maxHoleArea, int holeFillRingRadius,
                          double minHoleFillWhiteRatio)
{
    cv::Mat work;
    grayImage.convertTo(work, CV_8U);

    double minValue = 0.0;
    double maxValue = 0.0;
    cv::minMaxLoc(work, &minValue, &maxValue);
    cv::Mat mask;
    if (minValue >= 0.0 && maxValue <= 1.0) {
        cv::threshold(work, mask, 0, 255, cv::THRESH_BINARY);
    } else {
        const int blockSize = std::max(31, (std::min(work.rows, work.cols) / 20) | 1);
        const double normalizedBrightness = clampUnit(brightness);
        const double cValue = (normalizedBrightness - 0.5) * 60.0;
        cv::adaptiveThreshold(work, mask, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                              cv::THRESH_BINARY, blockSize, cValue);

        cv::Mat signalMask;
        cv::threshold(work, signalMask, kNoSignalGrayThreshold, 255, cv::THRESH_BINARY);
        cv::bitwise_and(mask, signalMask, mask);
    }

    mask = areaOpen(mask, minArea);
    if (closeRadius > 0) {
        cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, diskElement(closeRadius));
    }
    mask = fillLikelyVesselHoles(mask, maxHoleArea, holeFillRingRadius, minHoleFillWhiteRatio);
    mask = areaOpen(mask, minArea);
    return mask;
}

cv::Mat skeletonize(const cv::Mat &inputMask)
{
    cv::Mat image = toBinaryMask(inputMask);
    if (image.empty()) {
        return image;
    }
    image /= 255;

    cv::Mat previous = cv::Mat::zeros(image.size(), CV_8UC1);
    cv::Mat current = image.clone();
    cv::Mat marker = cv::Mat::zeros(image.size(), CV_8UC1);

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

cv::Mat removeThinSkeletonBranches(const cv::Mat &skeletonImage, const cv::Mat &diameterMap,
                                   double minDiameter, int minLength, int spurIterations)
{
    cv::Mat pruned = cv::Mat::zeros(skeletonImage.size(), CV_8UC1);
    for (int row = 0; row < skeletonImage.rows; ++row) {
        for (int col = 0; col < skeletonImage.cols; ++col) {
            if (skeletonImage.at<uchar>(row, col) && diameterMap.at<float>(row, col) >= minDiameter) {
                pruned.at<uchar>(row, col) = 255;
            }
        }
    }

    if (spurIterations > 0) {
        pruned = pruneSpurs(pruned, spurIterations);
    }
    pruned = areaOpen(pruned, minLength);
    if (cv::countNonZero(pruned) == 0) {
        return skeletonImage.clone();
    }
    return pruned;
}

cv::Mat buildSkeletonFromMask(const cv::Mat &mask, const cv::Mat &diameterMap,
                              double minDiameter, int spurIterations, int minLength)
{
    cv::Mat skeleton = skeletonize(mask);
    if (spurIterations > 0) {
        skeleton = pruneSpurs(skeleton, spurIterations);
    }
    skeleton = areaOpen(skeleton, 5);
    if (!diameterMap.empty()) {
        skeleton = removeThinSkeletonBranches(skeleton, diameterMap, minDiameter,
                                              minLength, spurIterations);
    }
    return skeleton;
}

std::vector<int> neighborIndices(const cv::Mat &componentMask, int index)
{
    std::vector<int> neighbors;
    const cv::Point point = indexPoint(index, componentMask.cols);
    for (int y = point.y - 1; y <= point.y + 1; ++y) {
        for (int x = point.x - 1; x <= point.x + 1; ++x) {
            if (y == point.y && x == point.x) {
                continue;
            }
            if (isMaskPixel(componentMask, y, x)) {
                neighbors.push_back(linearIndex(y, x, componentMask.cols));
            }
        }
    }
    return neighbors;
}

int nearestSkeletonIndex(const cv::Mat &skeletonImage, const QPoint &preferredPoint,
                         int maxDistance, bool preferSimplePoint, double *bestDistance)
{
    const cv::Mat skeleton = toBinaryMask(skeletonImage);
    if (bestDistance) {
        *bestDistance = std::numeric_limits<double>::infinity();
    }
    if (skeleton.empty()) {
        return -1;
    }

    double best = std::numeric_limits<double>::infinity();
    int bestIndex = -1;
    auto tryPass = [&](bool requireSimplePoint) {
        for (int row = 0; row < skeleton.rows; ++row) {
            for (int col = 0; col < skeleton.cols; ++col) {
                if (!skeleton.at<uchar>(row, col)) {
                    continue;
                }
                if (requireSimplePoint && neighborCount8(skeleton, row, col) != 2) {
                    continue;
                }
                const double dx = col - preferredPoint.x();
                const double dy = row - preferredPoint.y();
                const double distance = std::sqrt(dx * dx + dy * dy);
                if (distance < best) {
                    best = distance;
                    bestIndex = linearIndex(row, col, skeleton.cols);
                }
            }
        }
    };

    if (preferSimplePoint) {
        tryPass(true);
    }
    if (bestIndex < 0) {
        tryPass(false);
    }

    if (best > maxDistance) {
        return -1;
    }
    if (bestDistance) {
        *bestDistance = best;
    }
    return bestIndex;
}

std::vector<int> traceSkeletonSide(const cv::Mat &skeleton, int centerIndex,
                                   int firstIndex, int maxSteps)
{
    std::vector<int> side;
    int previousIndex = centerIndex;
    int currentIndex = firstIndex;
    for (int step = 0; step < maxSteps && currentIndex >= 0; ++step) {
        side.push_back(currentIndex);
        const cv::Point currentPoint = indexPoint(currentIndex, skeleton.cols);
        if (neighborCount8(skeleton, currentPoint.y, currentPoint.x) != 2) {
            break;
        }

        int nextIndex = -1;
        const std::vector<int> neighbors = neighborIndices(skeleton, currentIndex);
        for (int neighborIndex : neighbors) {
            if (neighborIndex != previousIndex) {
                nextIndex = neighborIndex;
                break;
            }
        }
        if (nextIndex < 0) {
            break;
        }
        previousIndex = currentIndex;
        currentIndex = nextIndex;
    }
    return side;
}

std::vector<int> collectLocalSkeletonIndices(const cv::Mat &skeleton,
                                             int centerIndex,
                                             int maxSteps)
{
    std::vector<int> indices;
    if (skeleton.empty() || centerIndex < 0
        || centerIndex >= skeleton.rows * skeleton.cols) {
        return indices;
    }

    std::vector<int> distance(static_cast<size_t>(skeleton.rows * skeleton.cols), -1);
    std::queue<int> queue;
    distance[static_cast<size_t>(centerIndex)] = 0;
    queue.push(centerIndex);

    while (!queue.empty()) {
        const int currentIndex = queue.front();
        queue.pop();
        indices.push_back(currentIndex);

        const int currentDistance = distance[static_cast<size_t>(currentIndex)];
        if (currentDistance >= maxSteps) {
            continue;
        }

        const std::vector<int> neighbors = neighborIndices(skeleton, currentIndex);
        for (int neighborIndex : neighbors) {
            if (distance[static_cast<size_t>(neighborIndex)] >= 0) {
                continue;
            }
            distance[static_cast<size_t>(neighborIndex)] = currentDistance + 1;
            queue.push(neighborIndex);
        }
    }

    return indices;
}

bool estimateLocalSkeletonDirection(const cv::Mat &skeleton,
                                    const std::vector<int> &indices,
                                    cv::Point2d *direction)
{
    if (!direction || indices.size() < 2) {
        return false;
    }

    double meanX = 0.0;
    double meanY = 0.0;
    for (int index : indices) {
        const cv::Point point = indexPoint(index, skeleton.cols);
        meanX += point.x;
        meanY += point.y;
    }
    meanX /= static_cast<double>(indices.size());
    meanY /= static_cast<double>(indices.size());

    double covarianceXX = 0.0;
    double covarianceXY = 0.0;
    double covarianceYY = 0.0;
    for (int index : indices) {
        const cv::Point point = indexPoint(index, skeleton.cols);
        const double dx = point.x - meanX;
        const double dy = point.y - meanY;
        covarianceXX += dx * dx;
        covarianceXY += dx * dy;
        covarianceYY += dy * dy;
    }

    if (covarianceXX + covarianceYY < 1.0) {
        return false;
    }

    const double angle = 0.5 * std::atan2(2.0 * covarianceXY,
                                          covarianceXX - covarianceYY);
    *direction = cv::Point2d(std::cos(angle), std::sin(angle));
    return true;
}

double projectionOnDirection(const cv::Point &point,
                             const cv::Point2d &origin,
                             const cv::Point2d &direction)
{
    return (point.x - origin.x) * direction.x
        + (point.y - origin.y) * direction.y;
}

double pointToSegmentDistance(const cv::Point &point,
                              const cv::Point &first,
                              const cv::Point &second)
{
    const double vx = second.x - first.x;
    const double vy = second.y - first.y;
    const double lengthSquared = vx * vx + vy * vy;
    if (lengthSquared <= 0.0) {
        const double dx = point.x - first.x;
        const double dy = point.y - first.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    double t = ((point.x - first.x) * vx + (point.y - first.y) * vy) / lengthSquared;
    t = std::max(0.0, std::min(1.0, t));
    const double nearestX = first.x + t * vx;
    const double nearestY = first.y + t * vy;
    const double dx = point.x - nearestX;
    const double dy = point.y - nearestY;
    return std::sqrt(dx * dx + dy * dy);
}

cv::Point nearestPointOnSegment(const cv::Point &point,
                                const cv::Point &first,
                                const cv::Point &second)
{
    const double vx = second.x - first.x;
    const double vy = second.y - first.y;
    const double lengthSquared = vx * vx + vy * vy;
    if (lengthSquared <= 0.0) {
        return first;
    }

    double t = ((point.x - first.x) * vx + (point.y - first.y) * vy) / lengthSquared;
    t = std::max(0.0, std::min(1.0, t));
    return cv::Point(static_cast<int>(std::round(first.x + t * vx)),
                     static_cast<int>(std::round(first.y + t * vy)));
}

cv::Point clampPointToImage(const QPoint &point, const cv::Size &imageSize)
{
    return cv::Point(std::max(0, std::min(imageSize.width - 1, point.x())),
                     std::max(0, std::min(imageSize.height - 1, point.y())));
}

double pointToPolylineDistance(const cv::Point &point,
                               const std::vector<cv::Point> &polyline)
{
    if (polyline.empty()) {
        return std::numeric_limits<double>::infinity();
    }
    if (polyline.size() == 1) {
        const double dx = point.x - polyline.front().x;
        const double dy = point.y - polyline.front().y;
        return std::sqrt(dx * dx + dy * dy);
    }

    double bestDistance = std::numeric_limits<double>::infinity();
    for (int i = 1; i < static_cast<int>(polyline.size()); ++i) {
        bestDistance = std::min(bestDistance,
                                pointToSegmentDistance(point,
                                                       polyline[static_cast<size_t>(i - 1)],
                                                       polyline[static_cast<size_t>(i)]));
    }
    return bestDistance;
}

cv::Point nearestPointOnPolyline(const cv::Point &point,
                                 const std::vector<cv::Point> &polyline)
{
    if (polyline.empty()) {
        return point;
    }
    if (polyline.size() == 1) {
        return polyline.front();
    }

    double bestDistance = std::numeric_limits<double>::infinity();
    cv::Point bestPoint = polyline.front();
    for (int i = 1; i < static_cast<int>(polyline.size()); ++i) {
        const cv::Point candidate = nearestPointOnSegment(point,
                                                          polyline[static_cast<size_t>(i - 1)],
                                                          polyline[static_cast<size_t>(i)]);
        const double distance = pointToSegmentDistance(point,
                                                       polyline[static_cast<size_t>(i - 1)],
                                                       polyline[static_cast<size_t>(i)]);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestPoint = candidate;
        }
    }
    return bestPoint;
}

std::vector<int> shortestSkeletonPath(const cv::Mat &skeleton,
                                      int firstIndex,
                                      int secondIndex,
                                      const std::vector<uchar> &allowed)
{
    std::vector<int> path;
    const int pixelCount = skeleton.rows * skeleton.cols;
    if (firstIndex < 0 || firstIndex >= pixelCount
        || secondIndex < 0 || secondIndex >= pixelCount
        || allowed.empty()
        || !allowed[static_cast<size_t>(firstIndex)]
        || !allowed[static_cast<size_t>(secondIndex)]) {
        return path;
    }

    std::vector<int> previous(static_cast<size_t>(pixelCount), -1);
    std::queue<int> queue;
    previous[static_cast<size_t>(firstIndex)] = firstIndex;
    queue.push(firstIndex);

    while (!queue.empty() && previous[static_cast<size_t>(secondIndex)] < 0) {
        const int currentIndex = queue.front();
        queue.pop();

        const std::vector<int> neighbors = neighborIndices(skeleton, currentIndex);
        for (int neighborIndex : neighbors) {
            if (!allowed[static_cast<size_t>(neighborIndex)]
                || previous[static_cast<size_t>(neighborIndex)] >= 0) {
                continue;
            }
            previous[static_cast<size_t>(neighborIndex)] = currentIndex;
            queue.push(neighborIndex);
        }
    }

    if (previous[static_cast<size_t>(secondIndex)] < 0) {
        return path;
    }

    int currentIndex = secondIndex;
    while (currentIndex != firstIndex) {
        path.push_back(currentIndex);
        currentIndex = previous[static_cast<size_t>(currentIndex)];
    }
    path.push_back(firstIndex);
    std::reverse(path.begin(), path.end());
    return path;
}

bool isNearPolyline(const cv::Point &point,
                    const std::vector<cv::Point> &polyline,
                    double maxDistance)
{
    return pointToPolylineDistance(point, polyline) <= maxDistance;
}

std::vector<int> traceLocalEndpointSpur(const cv::Mat &skeleton,
                                        int endpointIndex,
                                        const std::vector<cv::Point> &replacementPolyline,
                                        const cv::Mat &protectedMask,
                                        const cv::Mat &keepMask,
                                        int maxLength,
                                        double cleanupRadius)
{
    std::vector<int> branch;
    const cv::Point endpoint = indexPoint(endpointIndex, skeleton.cols);
    if (isMaskPixel(keepMask, endpoint.y, endpoint.x)
        || protectedMask.at<uchar>(endpoint.y, endpoint.x)
        || !isNearPolyline(endpoint, replacementPolyline, cleanupRadius)
        || neighborCount8(skeleton, endpoint.y, endpoint.x) > 1) {
        return branch;
    }

    branch.push_back(endpointIndex);
    int previousIndex = -1;
    int currentIndex = endpointIndex;
    while (static_cast<int>(branch.size()) < maxLength) {
        const std::vector<int> neighbors = neighborIndices(skeleton, currentIndex);
        std::vector<int> forwardNeighbors;
        for (int neighborIndex : neighbors) {
            if (neighborIndex != previousIndex) {
                forwardNeighbors.push_back(neighborIndex);
            }
        }

        if (forwardNeighbors.empty()) {
            return branch;
        }
        if (forwardNeighbors.size() != 1) {
            return branch;
        }

        const int nextIndex = forwardNeighbors.front();
        const cv::Point nextPoint = indexPoint(nextIndex, skeleton.cols);
        if (isMaskPixel(keepMask, nextPoint.y, nextPoint.x)) {
            return std::vector<int>();
        }
        if (protectedMask.at<uchar>(nextPoint.y, nextPoint.x)) {
            return branch;
        }
        if (!isNearPolyline(nextPoint, replacementPolyline, cleanupRadius)) {
            return std::vector<int>();
        }

        const int nextDegree = neighborCount8(skeleton, nextPoint.y, nextPoint.x);
        if (nextDegree != 2) {
            return branch;
        }

        branch.push_back(nextIndex);
        previousIndex = currentIndex;
        currentIndex = nextIndex;
    }

    return std::vector<int>();
}

int removeLocalShortSpurs(cv::Mat &skeleton,
                          const std::vector<cv::Point> &replacementPolyline,
                          const cv::Mat &keepMask,
                          int maxLength,
                          double cleanupRadius)
{
    if (skeleton.empty() || replacementPolyline.empty()) {
        return 0;
    }

    cv::Mat protectedMask = cv::Mat::zeros(skeleton.size(), CV_8UC1);
    for (int i = 1; i < static_cast<int>(replacementPolyline.size()); ++i) {
        cv::line(protectedMask,
                 replacementPolyline[static_cast<size_t>(i - 1)],
                 replacementPolyline[static_cast<size_t>(i)],
                 cv::Scalar(255),
                 1,
                 cv::LINE_8);
    }

    int removedCount = 0;
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<uchar> removeMask(static_cast<size_t>(skeleton.rows * skeleton.cols), 0);
        int passRemovedCount = 0;
        for (int row = 0; row < skeleton.rows; ++row) {
            for (int col = 0; col < skeleton.cols; ++col) {
                if (!skeleton.at<uchar>(row, col) || neighborCount8(skeleton, row, col) > 1) {
                    continue;
                }

                const int endpointIndex = linearIndex(row, col, skeleton.cols);
                const std::vector<int> branch = traceLocalEndpointSpur(skeleton,
                                                                        endpointIndex,
                                                                       replacementPolyline,
                                                                       protectedMask,
                                                                       keepMask,
                                                                       maxLength,
                                                                       cleanupRadius);
                for (int index : branch) {
                    if (!removeMask[static_cast<size_t>(index)]) {
                        removeMask[static_cast<size_t>(index)] = 1;
                        ++passRemovedCount;
                    }
                }
            }
        }

        if (passRemovedCount == 0) {
            break;
        }

        for (int index = 0; index < skeleton.rows * skeleton.cols; ++index) {
            if (!removeMask[static_cast<size_t>(index)]) {
                continue;
            }
            const cv::Point point = indexPoint(index, skeleton.cols);
            skeleton.at<uchar>(point.y, point.x) = 0;
        }
        removedCount += passRemovedCount;
    }

    return removedCount;
}

bool flattenSkeletonNearPoint(cv::Mat &skeletonImage, const QPoint &imagePoint,
                              SkeletonFixResult *result)
{
    cv::Mat skeleton = toBinaryMask(skeletonImage);
    if (skeleton.empty()) {
        return false;
    }

    double clickDistance = 0.0;
    const int centerIndex = nearestSkeletonIndex(skeleton, imagePoint,
                                                 kSkeletonFixSearchRadius,
                                                 false,
                                                 &clickDistance);
    if (centerIndex < 0) {
        return false;
    }

    const cv::Point centerPoint = indexPoint(centerIndex, skeleton.cols);
    const cv::Point controlPoint = clampPointToImage(imagePoint, skeleton.size());
    const std::vector<int> localIndices = collectLocalSkeletonIndices(skeleton,
                                                                      centerIndex,
                                                                      kSkeletonFixHalfLength);
    if (localIndices.size() < static_cast<size_t>(kMinSkeletonFixPathLength)) {
        return false;
    }

    cv::Point2d direction;
    if (!estimateLocalSkeletonDirection(skeleton, localIndices, &direction)) {
        return false;
    }

    const cv::Point2d origin(controlPoint.x, controlPoint.y);
    double minProjection = 0.0;
    double maxProjection = 0.0;
    int firstAnchorIndex = centerIndex;
    int secondAnchorIndex = centerIndex;
    for (int index : localIndices) {
        const cv::Point point = indexPoint(index, skeleton.cols);
        const double projection = projectionOnDirection(point, origin, direction);
        if (projection < minProjection) {
            minProjection = projection;
            firstAnchorIndex = index;
        }
        if (projection > maxProjection) {
            maxProjection = projection;
            secondAnchorIndex = index;
        }
    }

    if (firstAnchorIndex == centerIndex || secondAnchorIndex == centerIndex
        || maxProjection - minProjection < kMinSkeletonFixPathLength) {
        return false;
    }

    std::vector<uchar> localMask(static_cast<size_t>(skeleton.rows * skeleton.cols), 0);
    for (int index : localIndices) {
        localMask[static_cast<size_t>(index)] = 1;
    }

    std::vector<int> firstHalf = shortestSkeletonPath(skeleton,
                                                      firstAnchorIndex,
                                                      centerIndex,
                                                      localMask);
    std::vector<int> secondHalf = shortestSkeletonPath(skeleton,
                                                       centerIndex,
                                                       secondAnchorIndex,
                                                       localMask);
    std::vector<int> path;
    if (!firstHalf.empty() && !secondHalf.empty()) {
        path = firstHalf;
        path.insert(path.end(), secondHalf.begin() + 1, secondHalf.end());
    } else {
        path = shortestSkeletonPath(skeleton, firstAnchorIndex, secondAnchorIndex, localMask);
    }
    if (path.size() < kMinSkeletonFixPathLength) {
        return false;
    }

    std::vector<uchar> pathMask(static_cast<size_t>(skeleton.rows * skeleton.cols), 0);
    for (int index : path) {
        pathMask[static_cast<size_t>(index)] = 1;
    }

    const cv::Point firstAnchor = indexPoint(firstAnchorIndex, skeleton.cols);
    const cv::Point secondAnchor = indexPoint(secondAnchorIndex, skeleton.cols);
    std::vector<cv::Point> replacementPolyline;
    replacementPolyline.push_back(firstAnchor);
    if (pointToSegmentDistance(controlPoint, firstAnchor, secondAnchor) > 1.5) {
        replacementPolyline.push_back(controlPoint);
    }
    replacementPolyline.push_back(secondAnchor);

    std::vector<int> branchAttachmentIndices;
    std::vector<uchar> branchAttachmentMask(static_cast<size_t>(skeleton.rows * skeleton.cols), 0);
    for (int i = 1; i + 1 < static_cast<int>(path.size()); ++i) {
        const std::vector<int> neighbors = neighborIndices(skeleton, path[static_cast<size_t>(i)]);
        for (int neighborIndex : neighbors) {
            if (pathMask[static_cast<size_t>(neighborIndex)]) {
                continue;
            }
            if (!branchAttachmentMask[static_cast<size_t>(neighborIndex)]) {
                branchAttachmentMask[static_cast<size_t>(neighborIndex)] = 1;
                branchAttachmentIndices.push_back(neighborIndex);
            }
        }
    }

    cv::Mat refined = skeleton.clone();
    for (int i = 1; i + 1 < static_cast<int>(path.size()); ++i) {
        const cv::Point point = indexPoint(path[static_cast<size_t>(i)], skeleton.cols);
        refined.at<uchar>(point.y, point.x) = 0;
    }

    for (int i = 1; i < static_cast<int>(replacementPolyline.size()); ++i) {
        cv::line(refined,
                 replacementPolyline[static_cast<size_t>(i - 1)],
                 replacementPolyline[static_cast<size_t>(i)],
                 cv::Scalar(255),
                 1,
                 cv::LINE_8);
    }
    cv::Mat branchKeepMask = cv::Mat::zeros(skeleton.size(), CV_8UC1);
    for (int attachmentIndex : branchAttachmentIndices) {
        const cv::Point attachmentPoint = indexPoint(attachmentIndex, skeleton.cols);
        const cv::Point reconnectPoint = nearestPointOnPolyline(attachmentPoint,
                                                                replacementPolyline);
        cv::line(refined, attachmentPoint, reconnectPoint, cv::Scalar(255), 1, cv::LINE_8);
        cv::line(branchKeepMask, attachmentPoint, reconnectPoint, cv::Scalar(255), 1, cv::LINE_8);
        branchKeepMask.at<uchar>(attachmentPoint.y, attachmentPoint.x) = 255;
    }
    removeLocalShortSpurs(refined,
                          replacementPolyline,
                          branchKeepMask,
                          kSkeletonFixSpurMaxLength,
                          kSkeletonFixSpurCleanupRadius);

    cv::Mat diff;
    cv::absdiff(skeleton, refined, diff);
    const int changedPixels = cv::countNonZero(diff);
    if (changedPixels == 0) {
        return false;
    }

    skeletonImage = refined;
    if (result) {
        result->changed = true;
        result->changedPixels = changedPixels;
        result->pathPixels = static_cast<int>(path.size());
        result->clickDistance = clickDistance;
        result->firstAnchor = QPoint(firstAnchor.x, firstAnchor.y);
        result->secondAnchor = QPoint(secondAnchor.x, secondAnchor.y);
    }
    return true;
}

bool isDepthContinuousStep(int currentDepth, int nextDepth, int tolerance)
{
    if (currentDepth == kInvalidDepthIndex || nextDepth == kInvalidDepthIndex) {
        return true;
    }
    return std::abs(nextDepth - currentDepth) <= tolerance;
}

cv::Mat makeDepthContinuousSkeleton(const cv::Mat &skeletonImage,
                                    const cv::Mat &depthIndexMap,
                                    const cv::Mat &supportMask,
                                    int seedIndex,
                                    int tolerance,
                                    int *validDepthCount)
{
    const cv::Mat skeleton = toBinaryMask(skeletonImage);
    cv::Mat output = cv::Mat::zeros(skeleton.size(), CV_8UC1);
    if (skeleton.empty() || depthIndexMap.empty() || seedIndex < 0
        || seedIndex >= skeleton.rows * skeleton.cols) {
        return output;
    }
    if (!skeleton.at<uchar>(seedIndex / skeleton.cols, seedIndex % skeleton.cols)) {
        return output;
    }

    std::vector<int> skeletonDepths(static_cast<size_t>(skeleton.rows * skeleton.cols),
                                    kInvalidDepthIndex);
    int validCount = 0;
    for (int row = 0; row < skeleton.rows; ++row) {
        for (int col = 0; col < skeleton.cols; ++col) {
            if (!skeleton.at<uchar>(row, col)) {
                continue;
            }

            const int index = linearIndex(row, col, skeleton.cols);
            const int localDepth = medianDepthNearPoint(depthIndexMap,
                                                        supportMask,
                                                        QPoint(col, row),
                                                        kSkeletonDepthSampleRadius);
            skeletonDepths[static_cast<size_t>(index)] = localDepth;
            if (localDepth != kInvalidDepthIndex) {
                ++validCount;
            }
        }
    }
    if (validDepthCount) {
        *validDepthCount = validCount;
    }

    std::vector<uchar> visited(static_cast<size_t>(skeleton.rows * skeleton.cols), 0);
    std::queue<int> queue;
    visited[static_cast<size_t>(seedIndex)] = 1;
    queue.push(seedIndex);
    output.at<uchar>(seedIndex / skeleton.cols, seedIndex % skeleton.cols) = 255;

    while (!queue.empty()) {
        const int currentIndex = queue.front();
        queue.pop();
        const int currentDepth = skeletonDepths[static_cast<size_t>(currentIndex)];
        for (int neighborIndex : neighborIndices(skeleton, currentIndex)) {
            if (visited[static_cast<size_t>(neighborIndex)]) {
                continue;
            }

            const int nextDepth = skeletonDepths[static_cast<size_t>(neighborIndex)];
            if (!isDepthContinuousStep(currentDepth, nextDepth, tolerance)) {
                continue;
            }

            visited[static_cast<size_t>(neighborIndex)] = 1;
            queue.push(neighborIndex);
            output.at<uchar>(neighborIndex / skeleton.cols, neighborIndex % skeleton.cols) = 255;
        }
    }

    return output;
}

std::vector<float> geodesicDistance(const cv::Mat &componentMask, int seedIndex)
{
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> distance(componentMask.rows * componentMask.cols, inf);
    using QueueItem = std::pair<float, int>;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;

    distance[seedIndex] = 0.0f;
    queue.push(QueueItem(0.0f, seedIndex));

    while (!queue.empty()) {
        const QueueItem item = queue.top();
        queue.pop();
        if (item.first != distance[item.second]) {
            continue;
        }

        const cv::Point currentPoint = indexPoint(item.second, componentMask.cols);
        for (int neighborIndex : neighborIndices(componentMask, item.second)) {
            const cv::Point neighborPoint = indexPoint(neighborIndex, componentMask.cols);
            const bool diagonal = currentPoint.x != neighborPoint.x && currentPoint.y != neighborPoint.y;
            const float step = diagonal ? 1.41421356f : 1.0f;
            const float nextDistance = item.first + step;
            if (nextDistance < distance[neighborIndex]) {
                distance[neighborIndex] = nextDistance;
                queue.push(QueueItem(nextDistance, neighborIndex));
            }
        }
    }
    return distance;
}

QVector<QPoint> orderedPathFromMask(const cv::Mat &inputMask, bool hasSeed, const QPoint &seedPoint)
{
    QVector<QPoint> path;
    if (inputMask.empty()) {
        return path;
    }

    cv::Mat pathMask = toBinaryMask(inputMask);
    if (cv::countNonZero(pathMask) == 0) {
        return path;
    }

    std::vector<int> endpoints;
    for (int row = 0; row < pathMask.rows; ++row) {
        for (int col = 0; col < pathMask.cols; ++col) {
            if (pathMask.at<uchar>(row, col) && neighborCount8(pathMask, row, col) <= 1) {
                endpoints.push_back(linearIndex(row, col, pathMask.cols));
            }
        }
    }

    int startIndex = -1;
    int endIndex = -1;
    if (!endpoints.empty()) {
        startIndex = endpoints.front();
        if (endpoints.size() > 1) {
            double bestDistance = -1.0;
            for (int first : endpoints) {
                const std::vector<float> distanceMap = geodesicDistance(pathMask, first);
                for (int second : endpoints) {
                    if (first == second || !std::isfinite(distanceMap[second])) {
                        continue;
                    }
                    if (distanceMap[second] > bestDistance) {
                        bestDistance = distanceMap[second];
                        startIndex = first;
                        endIndex = second;
                    }
                }
            }
        }
    } else if (hasSeed && isMaskPixel(pathMask, seedPoint.y(), seedPoint.x())) {
        startIndex = linearIndex(seedPoint.y(), seedPoint.x(), pathMask.cols);
    } else {
        for (int row = 0; row < pathMask.rows && startIndex < 0; ++row) {
            for (int col = 0; col < pathMask.cols; ++col) {
                if (pathMask.at<uchar>(row, col)) {
                    startIndex = linearIndex(row, col, pathMask.cols);
                    break;
                }
            }
        }
    }

    if (startIndex < 0) {
        return path;
    }

    if (endIndex >= 0) {
        const std::vector<float> distanceMap = geodesicDistance(pathMask, startIndex);
        if (std::isfinite(distanceMap[endIndex])) {
            std::vector<int> reversedPath;
            int currentIndex = endIndex;
            reversedPath.push_back(currentIndex);
            for (int step = 0; step < pathMask.rows * pathMask.cols && currentIndex != startIndex; ++step) {
                int bestIndex = -1;
                float bestDistance = distanceMap[currentIndex];
                for (int neighborIndex : neighborIndices(pathMask, currentIndex)) {
                    if (std::isfinite(distanceMap[neighborIndex]) && distanceMap[neighborIndex] < bestDistance) {
                        bestDistance = distanceMap[neighborIndex];
                        bestIndex = neighborIndex;
                    }
                }
                if (bestIndex < 0) {
                    reversedPath.clear();
                    break;
                }
                currentIndex = bestIndex;
                reversedPath.push_back(currentIndex);
            }

            if (!reversedPath.empty() && reversedPath.back() == startIndex) {
                for (auto it = reversedPath.rbegin(); it != reversedPath.rend(); ++it) {
                    const cv::Point point = indexPoint(*it, pathMask.cols);
                    path.push_back(QPoint(point.x, point.y));
                }
                return path;
            }
        }
    }

    std::vector<uchar> visited(pathMask.rows * pathMask.cols, 0);
    int currentIndex = startIndex;
    int previousIndex = -1;

    while (currentIndex >= 0) {
        const cv::Point currentPoint = indexPoint(currentIndex, pathMask.cols);
        path.push_back(QPoint(currentPoint.x, currentPoint.y));
        visited[currentIndex] = 1;

        std::vector<int> neighbors = neighborIndices(pathMask, currentIndex);
        int nextIndex = -1;
        for (int neighborIndex : neighbors) {
            if (neighborIndex != previousIndex && !visited[neighborIndex]) {
                nextIndex = neighborIndex;
                break;
            }
        }

        if (nextIndex < 0) {
            break;
        }
        previousIndex = currentIndex;
        currentIndex = nextIndex;
    }

    return path;
}

QVector<QPoint> smoothPathPoints(const QVector<QPoint> &path, const cv::Size &size)
{
    if (path.isEmpty() || size.width <= 0 || size.height <= 0) {
        return QVector<QPoint>();
    }
    if (path.size() < 3) {
        return path;
    }

    QVector<QPointF> current;
    current.reserve(path.size());
    for (const QPoint &point : path) {
        current.push_back(QPointF(point));
    }

    for (int pass = 0; pass < kPathSmoothPasses; ++pass) {
        QVector<QPointF> next = current;
        for (int i = 1; i + 1 < current.size(); ++i) {
            const int first = std::max(0, i - kPathSmoothRadius);
            const int last = std::min(current.size() - 1, i + kPathSmoothRadius);
            double sumX = 0.0;
            double sumY = 0.0;
            double weightSum = 0.0;
            for (int j = first; j <= last; ++j) {
                const double distance = std::abs(i - j);
                const double weight = kPathSmoothRadius + 1 - distance;
                sumX += current[j].x() * weight;
                sumY += current[j].y() * weight;
                weightSum += weight;
            }
            next[i] = QPointF(sumX / weightSum, sumY / weightSum);
        }
        current.swap(next);
    }

    QVector<QPoint> smoothed;
    smoothed.reserve(current.size());
    for (const QPointF &pointF : current) {
        const QPoint point(std::max(0, std::min(size.width - 1, static_cast<int>(std::round(pointF.x())))),
                           std::max(0, std::min(size.height - 1, static_cast<int>(std::round(pointF.y())))));
        if (smoothed.isEmpty() || smoothed.last() != point) {
            smoothed.push_back(point);
        }
    }
    return smoothed;
}

QVector<QPoint> rasterizePathPoints(const QVector<QPoint> &path, const cv::Size &size)
{
    QVector<QPoint> rasterized;
    if (path.isEmpty() || size.width <= 0 || size.height <= 0) {
        return rasterized;
    }

    rasterized.reserve(path.size());
    rasterized.push_back(path.first());

    cv::Mat canvas = cv::Mat::zeros(size, CV_8UC1);
    for (int i = 1; i < path.size(); ++i) {
        const cv::Point start(path[i - 1].x(), path[i - 1].y());
        const cv::Point end(path[i].x(), path[i].y());
        cv::LineIterator iterator(canvas, start, end, 8);
        for (int j = 1; j < iterator.count; ++j) {
            ++iterator;
            const QPoint point(iterator.pos().x, iterator.pos().y);
            if (rasterized.last() != point) {
                rasterized.push_back(point);
            }
        }
    }
    return rasterized;
}

QVector<QPoint> smoothPathThroughSeed(const QVector<QPoint> &path, const cv::Size &size, const QPoint &seedPoint)
{
    if (path.isEmpty()) {
        return path;
    }

    int seedPosition = -1;
    for (int i = 0; i < path.size(); ++i) {
        if (path[i] == seedPoint) {
            seedPosition = i;
            break;
        }
    }
    if (seedPosition < 0) {
        return smoothPathPoints(path, size);
    }

    QVector<QPoint> firstSegment;
    firstSegment.reserve(seedPosition + 1);
    for (int i = 0; i <= seedPosition; ++i) {
        firstSegment.push_back(path[i]);
    }

    QVector<QPoint> secondSegment;
    secondSegment.reserve(path.size() - seedPosition);
    for (int i = seedPosition; i < path.size(); ++i) {
        secondSegment.push_back(path[i]);
    }

    QVector<QPoint> smoothedFirst = smoothPathPoints(firstSegment, size);
    QVector<QPoint> smoothedSecond = smoothPathPoints(secondSegment, size);

    QVector<QPoint> combined = smoothedFirst;
    combined.reserve(smoothedFirst.size() + std::max(0, smoothedSecond.size() - 1));
    for (int i = 1; i < smoothedSecond.size(); ++i) {
        if (combined.isEmpty() || combined.last() != smoothedSecond[i]) {
            combined.push_back(smoothedSecond[i]);
        }
    }
    return combined;
}

cv::Mat makePathMask(const QVector<QPoint> &path, const cv::Size &size)
{
    cv::Mat mask = cv::Mat::zeros(size, CV_8UC1);
    if (path.isEmpty() || size.width <= 0 || size.height <= 0) {
        return mask;
    }

    if (path.size() == 1) {
        mask.at<uchar>(path.first().y(), path.first().x()) = 255;
        return mask;
    }

    for (int i = 1; i < path.size(); ++i) {
        cv::line(mask,
                 cv::Point(path[i - 1].x(), path[i - 1].y()),
                 cv::Point(path[i].x(), path[i].y()),
                 cv::Scalar(255),
                 1,
                 cv::LINE_8);
    }
    return mask;
}

QVector<QPoint> orderedPathThroughSeed(const PathCandidate &firstPath, const PathCandidate *secondPath,
                                       int seedIndex, int cols)
{
    QVector<QPoint> path;
    if (firstPath.indices.empty() || firstPath.indices.back() != seedIndex) {
        return path;
    }

    path.reserve(static_cast<int>(firstPath.indices.size() + (secondPath ? secondPath->indices.size() : 0)));
    for (int index : firstPath.indices) {
        const cv::Point point = indexPoint(index, cols);
        path.push_back(QPoint(point.x, point.y));
    }

    if (!secondPath) {
        return path;
    }
    if (secondPath->indices.empty() || secondPath->indices.back() != seedIndex) {
        return path;
    }

    for (auto it = secondPath->indices.rbegin() + 1; it != secondPath->indices.rend(); ++it) {
        const cv::Point point = indexPoint(*it, cols);
        if (path.isEmpty() || path.last() != QPoint(point.x, point.y)) {
            path.push_back(QPoint(point.x, point.y));
        }
    }
    return path;
}

PathCandidate tracePathToSeed(const cv::Mat &componentMask, const cv::Mat &diameterMap,
                              const std::vector<float> &distanceMap, int endpointIndex,
                              int seedIndex, double diameterWeight, double lengthWeight)
{
    PathCandidate candidate;
    candidate.mask = cv::Mat::zeros(componentMask.size(), CV_8UC1);

    int currentIndex = endpointIndex;
    candidate.indices.push_back(currentIndex);
    candidate.mask.at<uchar>(currentIndex / componentMask.cols, currentIndex % componentMask.cols) = 255;

    for (int step = 0; step < componentMask.rows * componentMask.cols; ++step) {
        if (currentIndex == seedIndex) {
            break;
        }

        int bestIndex = -1;
        float bestDistance = distanceMap[currentIndex];
        for (int neighborIndex : neighborIndices(componentMask, currentIndex)) {
            if (std::isfinite(distanceMap[neighborIndex]) && distanceMap[neighborIndex] < bestDistance) {
                bestDistance = distanceMap[neighborIndex];
                bestIndex = neighborIndex;
            }
        }
        if (bestIndex < 0) {
            break;
        }
        currentIndex = bestIndex;
        candidate.indices.push_back(currentIndex);
        candidate.mask.at<uchar>(currentIndex / componentMask.cols, currentIndex % componentMask.cols) = 255;
    }

    double diameterSum = 0.0;
    for (int index : candidate.indices) {
        diameterSum += diameterMap.at<float>(index / componentMask.cols, index % componentMask.cols);
    }
    const double meanDiameter = candidate.indices.empty() ? 0.0 : diameterSum / candidate.indices.size();
    candidate.score = diameterWeight * meanDiameter + lengthWeight * std::sqrt(static_cast<double>(candidate.indices.size()));

    if (!candidate.indices.empty() && candidate.indices.back() == seedIndex) {
        const cv::Point seedPoint = indexPoint(seedIndex, componentMask.cols);
        if (candidate.indices.size() >= 2) {
            candidate.seedNeighborIndex = candidate.indices[candidate.indices.size() - 2];
        }

        const cv::Point targetPoint = indexPoint(candidate.indices.front(), componentMask.cols);
        cv::Point2d direction(targetPoint.x - seedPoint.x, targetPoint.y - seedPoint.y);
        const double norm = std::sqrt(direction.x * direction.x + direction.y * direction.y);
        if (norm > 0.0) {
            direction.x /= norm;
            direction.y /= norm;
        }
        candidate.direction = direction;

        const int seedPosition = static_cast<int>(candidate.indices.size()) - 1;
        const int localTargetPosition = std::max(0, seedPosition - 11);
        const cv::Point localTargetPoint = indexPoint(candidate.indices[localTargetPosition], componentMask.cols);
        cv::Point2d seedDirection(localTargetPoint.x - seedPoint.x, localTargetPoint.y - seedPoint.y);
        const double seedNorm = std::sqrt(seedDirection.x * seedDirection.x + seedDirection.y * seedDirection.y);
        if (seedNorm > 0.0) {
            seedDirection.x /= seedNorm;
            seedDirection.y /= seedNorm;
        }
        candidate.seedDirection = seedDirection;
    }

    return candidate;
}

void keepBestPathPerSeedBranch(std::vector<PathCandidate> &paths, const PathCandidate &candidate)
{
    if (candidate.indices.empty() || candidate.seedNeighborIndex < 0) {
        return;
    }

    for (PathCandidate &existing : paths) {
        if (existing.seedNeighborIndex == candidate.seedNeighborIndex) {
            if (candidate.score > existing.score) {
                existing = candidate;
            }
            return;
        }
    }

    paths.push_back(candidate);
}

std::vector<PathCandidate> collectSeedBranchPaths(const cv::Mat &componentMask, const cv::Mat &diameterMap,
                                                  const std::vector<float> &distanceMap, int seedIndex,
                                                  double diameterWeight, double lengthWeight)
{
    std::vector<PathCandidate> paths;
    for (int row = 0; row < componentMask.rows; ++row) {
        for (int col = 0; col < componentMask.cols; ++col) {
            if (!componentMask.at<uchar>(row, col)) {
                continue;
            }

            const int targetIndex = linearIndex(row, col, componentMask.cols);
            if (targetIndex == seedIndex || !std::isfinite(distanceMap[targetIndex])) {
                continue;
            }

            const PathCandidate candidate = tracePathToSeed(componentMask, diameterMap, distanceMap,
                                                            targetIndex, seedIndex,
                                                            diameterWeight, lengthWeight);
            if (!candidate.indices.empty() && candidate.indices.back() == seedIndex) {
                keepBestPathPerSeedBranch(paths, candidate);
            }
        }
    }
    return paths;
}

PathPairSelection chooseBestEndpointPair(const std::vector<PathCandidate> &paths, double oppositeDirectionWeight)
{
    PathPairSelection selection;
    if (paths.empty()) {
        return selection;
    }
    if (paths.size() == 1) {
        selection.first = 0;
        return selection;
    }

    double bestScore = -std::numeric_limits<double>::infinity();
    int bestFirst = 0;
    int bestSecond = 1;
    double fallbackScore = -std::numeric_limits<double>::infinity();
    int fallbackFirst = 0;
    int fallbackSecond = 1;
    double sameBranchFallbackScore = -std::numeric_limits<double>::infinity();
    int sameBranchFallbackFirst = 0;
    int sameBranchFallbackSecond = 1;

    for (int i = 0; i < static_cast<int>(paths.size()) - 1; ++i) {
        for (int j = i + 1; j < static_cast<int>(paths.size()); ++j) {
            cv::Mat overlap;
            cv::bitwise_and(paths[i].mask, paths[j].mask, overlap);
            const int shorterLength = std::min(cv::countNonZero(paths[i].mask), cv::countNonZero(paths[j].mask));
            const int overlapCount = cv::countNonZero(overlap);

            const double directionDot = paths[i].direction.x * paths[j].direction.x
                + paths[i].direction.y * paths[j].direction.y;
            const double seedDirectionDot = paths[i].seedDirection.x * paths[j].seedDirection.x
                + paths[i].seedDirection.y * paths[j].seedDirection.y;
            const double oppositeScore = (1.0 - directionDot) / 2.0;
            const double seedOppositeScore = (1.0 - seedDirectionDot) / 2.0;
            const double pairScore = paths[i].score + paths[j].score
                + oppositeDirectionWeight * (0.35 * oppositeScore + 0.65 * seedOppositeScore);
            const bool sameSeedBranch = paths[i].seedNeighborIndex >= 0
                && paths[i].seedNeighborIndex == paths[j].seedNeighborIndex;

            if (sameSeedBranch || seedDirectionDot > 0.55) {
                const double sameBranchScore = pairScore
                    - kSharedStemOverlapPenalty * overlapCount
                    - oppositeDirectionWeight * seedDirectionDot;
                if (sameBranchScore > sameBranchFallbackScore) {
                    sameBranchFallbackScore = sameBranchScore;
                    sameBranchFallbackFirst = i;
                    sameBranchFallbackSecond = j;
                }
                continue;
            }

            if (overlapCount > std::max(4, static_cast<int>(std::round(0.15 * shorterLength)))) {
                const double sharedStemScore = pairScore - kSharedStemOverlapPenalty * overlapCount;
                if (sharedStemScore > fallbackScore) {
                    fallbackScore = sharedStemScore;
                    fallbackFirst = i;
                    fallbackSecond = j;
                }
                continue;
            }

            if (pairScore > bestScore) {
                bestScore = pairScore;
                bestFirst = i;
                bestSecond = j;
            }
        }
    }

    if (!std::isfinite(bestScore) && std::isfinite(fallbackScore)) {
        bestFirst = fallbackFirst;
        bestSecond = fallbackSecond;
    } else if (!std::isfinite(bestScore) && std::isfinite(sameBranchFallbackScore)) {
        bestFirst = sameBranchFallbackFirst;
        bestSecond = sameBranchFallbackSecond;
    }

    selection.first = bestFirst;
    selection.second = bestSecond;
    return selection;
}

unsigned short toDaUnsignedShort(double voltage, double normalizedPosition)
{
    const double rawValue = kPositionDacZeroCode + voltage * 65536.0 / 10000.0 * normalizedPosition + 0.5;
    const double clampedValue = std::max(0.0, std::min(65535.0, rawValue));
    return static_cast<unsigned short>(clampedValue);
}

void appendAscii(QByteArray &data, const char *text)
{
    data.append(text, 4);
}

void appendU16Le(QByteArray &data, quint16 value)
{
    data.append(static_cast<char>(value & 0xff));
    data.append(static_cast<char>((value >> 8) & 0xff));
}

void appendU32Le(QByteArray &data, quint32 value)
{
    data.append(static_cast<char>(value & 0xff));
    data.append(static_cast<char>((value >> 8) & 0xff));
    data.append(static_cast<char>((value >> 16) & 0xff));
    data.append(static_cast<char>((value >> 24) & 0xff));
}

void appendS24Le(QByteArray &data, qint32 value)
{
    const quint32 rawValue = static_cast<quint32>(value) & 0x00ffffffu;
    data.append(static_cast<char>(rawValue & 0xff));
    data.append(static_cast<char>((rawValue >> 8) & 0xff));
    data.append(static_cast<char>((rawValue >> 16) & 0xff));
}

qint32 toWavSample24(double normalizedPosition)
{
    const double clampedPosition = std::max(-1.0, std::min(1.0, normalizedPosition));
    const double scaledValue = clampedPosition * kVesselWavMaxAmplitudeRatio * kVesselWavMaxSampleValue;
    const double clampedValue = std::max(-kVesselWavMaxAmplitudeRatio * kVesselWavMaxSampleValue,
                                         std::min(kVesselWavMaxAmplitudeRatio * kVesselWavMaxSampleValue,
                                                  scaledValue));
    return static_cast<qint32>(std::round(clampedValue));
}

QPointF normalizedWavPoint(const QPoint &point, int cols, int rows)
{
    const double xPosition = cols > 1
        ? (2.0 * point.x() / (cols - 1) - 1.0) : 0.0;
    const double yPosition = rows > 1
        ? (2.0 * point.y() / (rows - 1) - 1.0) : 0.0;
    return QPointF(xPosition, yPosition);
}

QPointF interpolateWavPoint(const QPointF &start, const QPointF &end, int index, int count)
{
    if (count <= 1) {
        return end;
    }
    const double fraction = static_cast<double>(index + 1) / static_cast<double>(count);
    return QPointF(start.x() + (end.x() - start.x()) * fraction,
                   start.y() + (end.y() - start.y()) * fraction);
}

QVector<QPointF> normalizedPathPoints(const QVector<QPoint> &path, int cols, int rows)
{
    QVector<QPointF> sourcePoints;
    sourcePoints.reserve(path.size());
    for (const QPoint &point : path) {
        sourcePoints.append(normalizedWavPoint(point, cols, rows));
    }
    return sourcePoints;
}

QVector<QPointF> resamplePointSeries(const QVector<QPointF> &sourcePoints, int targetCount)
{
    if (sourcePoints.isEmpty()) {
        return sourcePoints;
    }

    targetCount = std::max(1, targetCount);
    QVector<QPointF> resampledPoints;
    resampledPoints.reserve(targetCount);
    if (targetCount == 1 || sourcePoints.size() == 1) {
        resampledPoints.append(sourcePoints.first());
        return resampledPoints;
    }

    for (int i = 0; i < targetCount; ++i) {
        const double sourcePosition = static_cast<double>(i) * (sourcePoints.size() - 1)
            / static_cast<double>(targetCount - 1);
        const int leftIndex = static_cast<int>(std::floor(sourcePosition));
        const int rightIndex = std::min(leftIndex + 1, sourcePoints.size() - 1);
        const double fraction = sourcePosition - leftIndex;
        const QPointF &left = sourcePoints[leftIndex];
        const QPointF &right = sourcePoints[rightIndex];
        resampledPoints.append(QPointF(left.x() + (right.x() - left.x()) * fraction,
                                       left.y() + (right.y() - left.y()) * fraction));
    }
    return resampledPoints;
}

QVector<QPointF> resamplePathToCount(const QVector<QPoint> &path, int cols, int rows, int targetCount)
{
    return resamplePointSeries(normalizedPathPoints(path, cols, rows), targetCount);
}

QImage matToQImage(const cv::Mat &mat)
{
    if (mat.empty()) {
        return QImage();
    }

    if (mat.type() == CV_8UC1) {
        QImage image(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step), QImage::Format_Grayscale8);
        return image.copy();
    }
    if (mat.type() == CV_8UC3) {
        QImage image(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step), QImage::Format_RGB888);
        return image.copy();
    }
    return QImage();
}

void paintSkeletonOverlay(cv::Mat &overlay, const cv::Mat &skeletonMask, const cv::Scalar &skeletonColor)
{
    if (overlay.empty() || skeletonMask.empty()) {
        return;
    }

    cv::Mat thickSkeleton;
    cv::dilate(toBinaryMask(skeletonMask), thickSkeleton, diskElement(1));
    overlay.setTo(skeletonColor, thickSkeleton);
}

cv::Mat makeSkeletonOverlay(const cv::Mat &rgbImage, const cv::Mat &skeletonMask,
                            const cv::Scalar &skeletonColor, bool hasSeed, const QPoint &seedPoint,
                            const cv::Mat &secondarySkeletonMask = cv::Mat(),
                            const cv::Scalar &secondarySkeletonColor = cv::Scalar())
{
    cv::Mat overlay = rgbImage.clone();
    if (overlay.empty()) {
        return overlay;
    }

    paintSkeletonOverlay(overlay, secondarySkeletonMask, secondarySkeletonColor);
    paintSkeletonOverlay(overlay, skeletonMask, skeletonColor);

    if (hasSeed && seedPoint.x() >= 0 && seedPoint.y() >= 0
        && seedPoint.x() < overlay.cols && seedPoint.y() < overlay.rows) {
        cv::Mat seedMask = cv::Mat::zeros(overlay.size(), CV_8UC1);
        cv::circle(seedMask, cv::Point(seedPoint.x(), seedPoint.y()), 4, 255, -1);
        overlay.setTo(cv::Scalar(0, 255, 0), seedMask);
    }
    return overlay;
}

void drawPointMarker(cv::Mat &image, const QPoint &point, const cv::Scalar &color)
{
    if (image.empty() || point.x() < 0 || point.y() < 0
        || point.x() >= image.cols || point.y() >= image.rows) {
        return;
    }

    const cv::Point center(point.x(), point.y());
    cv::circle(image, center, 6, cv::Scalar(0, 0, 0), -1, cv::LINE_AA);
    cv::circle(image, center, 4, color, -1, cv::LINE_AA);
}

} // namespace

class VesselImageCanvas : public QWidget
{
public:
    explicit VesselImageCanvas(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_selectable(false)
        , m_drawingEnabled(false)
        , m_isDrawing(false)
    {
        setFixedSize(kDisplayWidgetWidth, kDisplayWidgetHeight);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setMouseTracking(true);
    }

    void setImage(const QImage &image)
    {
        m_image = image;
        update();
    }

    void setSelectable(bool selectable)
    {
        m_selectable = selectable;
        updateCursorShape();
    }

    void setDrawingEnabled(bool enabled)
    {
        m_drawingEnabled = enabled;
        if (!enabled) {
            m_isDrawing = false;
            m_drawnPath.clear();
        }
        updateCursorShape();
        update();
    }

    void setPathOverlay(const QVector<QPoint> &path)
    {
        m_overlayPath = path;
        update();
    }

    void setClickHandler(const std::function<void(const QPoint &)> &handler)
    {
        m_clickHandler = handler;
    }

    void setPathHandler(const std::function<void(const QVector<QPoint> &)> &handler)
    {
        m_pathHandler = handler;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (m_image.isNull()) {
            painter.setPen(QColor(180, 180, 180));
            painter.drawText(rect(), Qt::AlignCenter, tr("请选择血管图"));
            return;
        }
        const QRect drawRect = targetRect();
        painter.drawImage(drawRect, m_image);

        const QVector<QPoint> &path = m_isDrawing ? m_drawnPath : m_overlayPath;
        if (path.size() >= 2) {
            QPen pen(QColor(0, 120, 255));
            pen.setWidth(3);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(pen);
            for (int i = 1; i < path.size(); ++i) {
                painter.drawLine(widgetPointFromImagePoint(path[i - 1], drawRect),
                                 widgetPointFromImagePoint(path[i], drawRect));
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (m_drawingEnabled && event->button() == Qt::LeftButton) {
            QPoint imagePoint;
            if (!imagePointFromWidgetPoint(event->pos(), &imagePoint)) {
                return;
            }

            m_isDrawing = true;
            m_drawnPath.clear();
            appendDrawPoint(imagePoint);
            m_overlayPath = m_drawnPath;
            update();
            event->accept();
            return;
        }

        if (!m_selectable || !m_clickHandler || event->button() != Qt::LeftButton) {
            return;
        }

        QPoint imagePoint;
        if (imagePointFromWidgetPoint(event->pos(), &imagePoint)) {
            m_clickHandler(imagePoint);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_drawingEnabled || !m_isDrawing || !(event->buttons() & Qt::LeftButton)) {
            return;
        }

        QPoint imagePoint;
        if (imagePointFromWidgetPoint(event->pos(), &imagePoint)) {
            appendDrawPoint(imagePoint);
            m_overlayPath = m_drawnPath;
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_drawingEnabled || !m_isDrawing || event->button() != Qt::LeftButton) {
            return;
        }

        QPoint imagePoint;
        if (imagePointFromWidgetPoint(event->pos(), &imagePoint)) {
            appendDrawPoint(imagePoint);
        }
        m_isDrawing = false;
        m_overlayPath = m_drawnPath;
        update();

        if (m_pathHandler && m_drawnPath.size() >= 2) {
            m_pathHandler(m_drawnPath);
        }
        event->accept();
    }

private:
    bool imagePointFromWidgetPoint(const QPoint &widgetPoint, QPoint *imagePoint) const
    {
        if (m_image.isNull() || !imagePoint) {
            return false;
        }

        const QRect drawRect = targetRect();
        if (!drawRect.contains(widgetPoint) || drawRect.width() <= 0 || drawRect.height() <= 0) {
            return false;
        }

        const double scaleX = static_cast<double>(m_image.width()) / drawRect.width();
        const double scaleY = static_cast<double>(m_image.height()) / drawRect.height();
        const int imageX = static_cast<int>(std::round((widgetPoint.x() - drawRect.x()) * scaleX));
        const int imageY = static_cast<int>(std::round((widgetPoint.y() - drawRect.y()) * scaleY));
        *imagePoint = QPoint(std::max(0, std::min(m_image.width() - 1, imageX)),
                             std::max(0, std::min(m_image.height() - 1, imageY)));
        return true;
    }

    QPoint widgetPointFromImagePoint(const QPoint &imagePoint, const QRect &drawRect) const
    {
        if (m_image.isNull() || m_image.width() <= 0 || m_image.height() <= 0) {
            return QPoint();
        }

        const double scaleX = static_cast<double>(drawRect.width()) / m_image.width();
        const double scaleY = static_cast<double>(drawRect.height()) / m_image.height();
        return QPoint(drawRect.x() + static_cast<int>(std::round((imagePoint.x() + 0.5) * scaleX)),
                      drawRect.y() + static_cast<int>(std::round((imagePoint.y() + 0.5) * scaleY)));
    }

    void appendDrawPoint(const QPoint &point)
    {
        if (m_drawnPath.isEmpty() || m_drawnPath.last() != point) {
            m_drawnPath.push_back(point);
        }
    }

    void updateCursorShape()
    {
        setCursor((m_selectable || m_drawingEnabled) ? Qt::CrossCursor : Qt::ArrowCursor);
    }

    QRect targetRect() const
    {
        if (m_image.isNull()) {
            return QRect();
        }

        const QSize drawSize = scaledImageSize(m_image.size(), size());
        return QRect((width() - drawSize.width()) / 2,
                     (height() - drawSize.height()) / 2,
                     drawSize.width(),
                     drawSize.height());
    }

    QImage m_image;
    bool m_selectable;
    bool m_drawingEnabled;
    bool m_isDrawing;
    QVector<QPoint> m_overlayPath;
    QVector<QPoint> m_drawnPath;
    std::function<void(const QPoint &)> m_clickHandler;
    std::function<void(const QVector<QPoint> &)> m_pathHandler;
};

struct VesselFindingDialog::MaskParams
{
    double brightness = 0.50;
    int bridgingRadius = 2;
    int maxHoleArea = 50;
    double vesselRatio = 0.50;
};

struct VesselFindingDialog::RouteParams
{
    double minDiameter = 8.0;
    int spurIterations = 12;
    double lengthWeight = 0.90;
    double pairWeight = 22.0;
};

struct VesselFindingDialog::VesselResult
{
    cv::Mat skeleton;
    double maxDiameter = 0.0;
    double meanDiameter = 0.0;
};

VesselFindingDialog::VesselFindingDialog(QWidget *parent)
    : VesselFindingDialog(1500.0, 48000, 0, 0, parent)
{
}

VesselFindingDialog::VesselFindingDialog(double voltage, QWidget *parent)
    : VesselFindingDialog(voltage, 48000, 0, 0, parent)
{
}

VesselFindingDialog::VesselFindingDialog(double voltage, int ascanFreq, QWidget *parent)
    : VesselFindingDialog(voltage, ascanFreq, 0, 0, parent)
{
}

VesselFindingDialog::VesselFindingDialog(double voltage,
                                         int ascanFreq,
                                         int bscanLen,
                                         int bscanCycleLen,
                                         QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::VesselFindingDialog)
    , m_selectionCanvas(nullptr)
    , m_maskCanvas(nullptr)
    , m_routeCanvas(nullptr)
    , m_interactionMode(kInteractionNone)
    , m_hasSeed(false)
    , m_maxDiameter(0.0)
    , m_meanDiameter(0.0)
    , m_mainVesselLen(0)
    , m_voltage(voltage)
    , m_ascanFreq(ascanFreq > 0 ? ascanFreq : 48000)
    , m_bscanLen(bscanLen > 0 ? bscanLen : 0)
    , m_bscanCycleLen(bscanCycleLen > 0 ? bscanCycleLen : 0)
{
    ui->setupUi(this);
    vesselFindingDialogUISetup(this);
    setupCanvases();
    setupTextsAndDefaults();
    setSelectionActive(false);
    QTimer::singleShot(0, this, &VesselFindingDialog::loadImageFromDialog);
}

VesselFindingDialog::~VesselFindingDialog()
{
    saveSettings(false);
    delete ui;
}

void VesselFindingDialog::setupCanvases()
{
    auto installCanvas = [](QWidget *placeholder) -> VesselImageCanvas * {
        VesselImageCanvas *canvas = new VesselImageCanvas(placeholder);
        canvas->setObjectName(placeholder->objectName() + QStringLiteral("_canvas"));
        QVBoxLayout *layout = new QVBoxLayout(placeholder);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(canvas);
        return canvas;
    };

    m_selectionCanvas = installCanvas(ui->widget_sel);
    m_maskCanvas = installCanvas(ui->widget_mask);
    m_routeCanvas = installCanvas(ui->widget_path);
    m_selectionCanvas->setClickHandler([this](const QPoint &point) {
        handleSeedClick(point);
    });
    m_selectionCanvas->setPathHandler([this](const QVector<QPoint> &path) {
        handleDrawnPath(path);
    });
}

void VesselFindingDialog::setupTextsAndDefaults()
{
    setWindowTitle(QStringLiteral("血管路径生成"));
    // ui->label_sel->setText(QStringLiteral("点击图像选择需要扫描的血管"));
    // ui->button_fixSkeleton->setText(QStringLiteral("精修骨架"));
    // ui->button_sel_redo->setText(QStringLiteral("重新选择"));
    // ui->button_sel_confirm->setText(QStringLiteral("确定"));
    // ui->label_n_maxDiameter->setText(QStringLiteral("最大直径："));
    // ui->label_n_meanDiameter->setText(QStringLiteral("平均直径："));
    // ui->label_n_mainVesselLen->setText(QStringLiteral("血管长度："));
    // ui->button_mask_redo->setText(QStringLiteral("重置"));
    // ui->label_gen_brightness->setText(QStringLiteral("亮度"));
    // ui->label_gen_bridgingRadius->setText(QStringLiteral("连接强度"));
    // ui->label_fill_maxHoleArea->setText(QStringLiteral("填充孔洞上限"));
    // ui->label_fill_vesselRatio->setText(QStringLiteral("邻域强度下限"));
    // ui->button_route_redo->setText(QStringLiteral("重置"));
    // ui->label_sel_minDiameter->setText(QStringLiteral("血管直径下限"));
    // ui->label_sel_spurIterations->setText(QStringLiteral("去杂散强度"));
    // ui->label_endPt_LenWeight->setText(QStringLiteral("长度权重"));
    // ui->label_endPt_PairWeight->setText(QStringLiteral("方向权重"));
    // ui->button_route_save->setText(QStringLiteral("更新"));
    // ui->CB_depthDependency->setText(QStringLiteral("深度选择范围"));
    // ui->button_show_reselect->setText(QStringLiteral("重选图片"));
    // ui->button_show_cancel->setText(QStringLiteral("取消"));
    // ui->button_show_confirm->setText(QStringLiteral("确定"));

    ui->SB_gen_brightness->setDecimals(2);
    ui->SB_fill_vesselRatio->setDecimals(2);
    ui->SB_sel_minDiameter->setDecimals(1);
    ui->SB_endPt_LenWeight->setDecimals(2);
    ui->SB_endPt_PairWeight->setDecimals(1);
    ui->SB_depthDependencyRange->setRange(0, 255);
    ui->SB_depthDependencyRange->setValue(kDefaultDepthTolerance);
    ui->button_fixSkeleton->setCheckable(true);
    ui->button_draw->setCheckable(true);
    ui->CB_depthDependency->setChecked(false);
    ui->CB_noReturn->setChecked(false);
    ui->CB_generateWAV->setChecked(false);
    ui->SB_depthDependencyRange->setEnabled(ui->CB_depthDependency->isChecked());
    connect(ui->CB_depthDependency, &QCheckBox::toggled,
            ui->SB_depthDependencyRange, &QSpinBox::setEnabled);
    connect(ui->CB_depthDependency, &QCheckBox::toggled, this, [this]() {
        restoreFullSkeletonForSelection(QStringLiteral("深度选择开关已改变"), kInteractionSelectSeed);
    });
    connect(ui->SB_depthDependencyRange, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, [this](int) {
        restoreFullSkeletonForSelection(QStringLiteral("深度选择范围已改变"), kInteractionSelectSeed);
    });
    resetMaskParams();
    resetRouteParams();
    resetEndpointWeightParams();
    loadSettings();
    updateStats();
}

void VesselFindingDialog::loadImageFromDialog()
{
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("选择血管彩色图"),
        vesselImageDialogPath(),
        tr("Image files (*.png *.jpg *.jpeg *.tif *.tiff *.bmp);;All files (*.*)"));
    if (filePath.isEmpty()) {
        appendLog(QStringLiteral("未选择图片。"));
        return;
    }
    appendLog(QStringLiteral("正在读取图片..."));
    loadImage(filePath);
}

bool VesselFindingDialog::loadImage(const QString &filePath)
{
    cv::Mat bgrImage = readImageFile(filePath);
    if (bgrImage.empty()) {
        styledMessageBox(this,
                         QMessageBox::Warning,
                         tr("读取失败"),
                         tr("无法读取所选图片。"),
                         QMessageBox::Ok,
                         QMessageBox::Ok);
        appendLog(QStringLiteral("读取图片失败：%1").arg(filePath));
        return false;
    }

    cv::cvtColor(bgrImage, m_rgbImage, cv::COLOR_BGR2RGB);
    m_inputImagePath = filePath;
    m_hasSeed = false;
    m_fullVesselSkeleton.release();
    m_mainVesselSkeleton.release();
    m_depthRemovedSkeleton.release();
    m_depthIndexMap.release();
    m_depthProjectionSource.clear();
    m_smoothedMainVesselPath.clear();
    clearManualPath();
    m_maxDiameter = 0.0;
    m_meanDiameter = 0.0;
    m_mainVesselLen = 0;

    appendLog(QStringLiteral("读取图片：%1").arg(QFileInfo(filePath).fileName()));
    saveVesselImagePath(filePath);
    appendImageSizeLog();
    if (!rebuildMaskAndSkeleton()) {
        return false;
    }
    setSelectionActive(true);
    return true;
}

bool VesselFindingDialog::rebuildMaskAndSkeleton(bool rebuildExistingRoute)
{
    if (m_rgbImage.empty()) {
        appendLog(QStringLiteral("没有可处理的血管图。"));
        return false;
    }

    appendLog(QStringLiteral("正在绘制二值掩膜..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const MaskParams params = maskParams();
    const RouteParams route = routeParams();

    m_grayImage = makeVesselGrayImage(m_rgbImage);
    m_initialVesselMask = segmentVesselMask(m_grayImage, params.brightness, kMinVesselArea,
                                            params.bridgingRadius, params.maxHoleArea,
                                            kHoleRingRadius, params.vesselRatio);

    cv::distanceTransform(m_initialVesselMask, m_diameterMap, cv::DIST_L2, 3);
    m_diameterMap *= 2.0f;

    m_depthIndexMap.release();
    m_depthProjectionSource.clear();
    m_depthRemovedSkeleton.release();
    clearManualPath();
    m_fullVesselSkeleton = buildSkeletonFromMask(m_initialVesselMask, m_diameterMap,
                                                 route.minDiameter, route.spurIterations,
                                                 std::max(1, m_rgbImage.cols / 4));
    m_vesselSkeleton = m_fullVesselSkeleton.clone();

    if (m_hasSeed && rebuildExistingRoute) {
        moveSeedToNearestSkeleton(m_seedPoint, std::numeric_limits<int>::max());
        rebuildRoute();
    } else {
        if (m_hasSeed) {
            moveSeedToNearestSkeleton(m_seedPoint, std::numeric_limits<int>::max());
        }
        m_mainVesselSkeleton.release();
        m_smoothedMainVesselPath.clear();
        m_maxDiameter = 0.0;
        m_meanDiameter = 0.0;
        m_mainVesselLen = 0;
    }

    showMaskViews();
    showSelectionView();
    showRouteView();
    updateStats();
    QApplication::restoreOverrideCursor();

    appendLog(QStringLiteral("已更新二值投影和骨架，可在左侧图像上点击种子点。"));
    return true;
}

bool VesselFindingDialog::rebuildRoute()
{
    m_smoothedMainVesselPath.clear();
    clearManualPath();

    if (m_initialVesselMask.empty() || cv::countNonZero(m_initialVesselMask) == 0) {
        appendLog(QStringLiteral("当前没有可用骨架，请调整第一行参数。"));
        return false;
    }
    if (!m_hasSeed) {
        appendLog(QStringLiteral("请先在左侧图像中选择种子点。"));
        return false;
    }

    const RouteParams route = routeParams();
    cv::Mat routeSkeleton = m_fullVesselSkeleton.clone();
    if (routeSkeleton.empty() || cv::countNonZero(routeSkeleton) == 0) {
        appendLog(QStringLiteral("当前没有可用骨架，请调整第一行参数。"));
        return false;
    }
    m_depthRemovedSkeleton.release();
    m_vesselSkeleton = routeSkeleton;

    if (!moveSeedToNearestSkeleton(m_seedPoint, std::numeric_limits<int>::max())) {
        return false;
    }

    if (ui->CB_depthDependency->isChecked()) {
        if (ensureDepthIndexMap()) {
            const int seedDepth = medianDepthNearPoint(m_depthIndexMap, m_initialVesselMask,
                                                       m_seedPoint, kSeedDepthSampleRadius);
            if (seedDepth == kInvalidDepthIndex) {
                appendLog(QStringLiteral("深度选择未找到种子点附近的有效深度，已使用完整骨架。"));
            } else {
                const int depthTolerance = ui->SB_depthDependencyRange->value();
                int validSkeletonDepths = 0;
                const cv::Mat depthSkeleton = makeDepthContinuousSkeleton(
                    routeSkeleton,
                    m_depthIndexMap,
                    m_initialVesselMask,
                    linearIndex(m_seedPoint.y(), m_seedPoint.x(), routeSkeleton.cols),
                    depthTolerance,
                    &validSkeletonDepths);
                const int fullSkeletonPixels = cv::countNonZero(routeSkeleton);
                const int depthSkeletonPixels = cv::countNonZero(depthSkeleton);
                if (depthSkeletonPixels > 0) {
                    m_vesselSkeleton = depthSkeleton;
                    cv::Mat retainedInverse;
                    cv::bitwise_not(toBinaryMask(depthSkeleton), retainedInverse);
                    cv::bitwise_and(routeSkeleton, retainedInverse, m_depthRemovedSkeleton);
                    if (!moveSeedToNearestSkeleton(m_seedPoint, std::numeric_limits<int>::max())) {
                        appendLog(QStringLiteral("深度过滤后无法定位种子点，已使用完整骨架。"));
                        m_vesselSkeleton = routeSkeleton;
                        m_depthRemovedSkeleton.release();
                        moveSeedToNearestSkeleton(m_seedPoint, std::numeric_limits<int>::max());
                    } else {
                        appendLog(QStringLiteral("深度选择已启用：来源 %1，种子深度 %2，局部跳变范围 ±%3，骨架点 %4/%5，有效深度点 %6。")
                                  .arg(m_depthProjectionSource)
                                  .arg(seedDepth)
                                  .arg(depthTolerance)
                                  .arg(depthSkeletonPixels)
                                  .arg(fullSkeletonPixels)
                                  .arg(validSkeletonDepths));
                    }
                } else {
                    appendLog(QStringLiteral("深度连续性过滤后骨架为空，已使用完整骨架。"));
                }
            }
        } else {
            appendLog(QStringLiteral("深度选择没有可用深度图，已使用完整骨架。"));
        }
    }

    appendLog(QStringLiteral("正在构建最终寻找路径..."));
    const int seedIndex = linearIndex(m_seedPoint.y(), m_seedPoint.x(), m_vesselSkeleton.cols);

    cv::Mat labels;
    const int componentCount = cv::connectedComponents(m_vesselSkeleton, labels, 8, CV_32S);
    if (componentCount <= 1) {
        appendLog(QStringLiteral("当前骨架为空。"));
        return false;
    }
    const int seedLabel = labels.at<int>(m_seedPoint.y(), m_seedPoint.x());
    if (seedLabel == 0) {
        appendLog(QStringLiteral("种子点不在骨架上。"));
        return false;
    }

    cv::Mat seedComponent = labels == seedLabel;
    seedComponent.convertTo(seedComponent, CV_8U, 255);

    std::vector<int> endpointIndices;
    for (int row = 0; row < m_vesselSkeleton.rows; ++row) {
        for (int col = 0; col < m_vesselSkeleton.cols; ++col) {
            if (m_vesselSkeleton.at<uchar>(row, col) && labels.at<int>(row, col) == seedLabel
                && neighborCount8(m_vesselSkeleton, row, col) <= 1) {
                endpointIndices.push_back(linearIndex(row, col, m_vesselSkeleton.cols));
            }
        }
    }
    if (endpointIndices.size() < 2) {
        endpointIndices.clear();
        for (int row = 0; row < seedComponent.rows; ++row) {
            for (int col = 0; col < seedComponent.cols; ++col) {
                if (seedComponent.at<uchar>(row, col)) {
                    endpointIndices.push_back(linearIndex(row, col, seedComponent.cols));
                }
            }
        }
    }

    const std::vector<float> distanceMap = geodesicDistance(seedComponent, seedIndex);
    std::vector<PathCandidate> paths = collectSeedBranchPaths(seedComponent, m_diameterMap, distanceMap,
                                                              seedIndex, kDiameterWeight, route.lengthWeight);
    if (!paths.empty()) {
        appendLog(QStringLiteral("已按种子点局部分支生成候选路径：%1 条。")
                  .arg(paths.size()));
    }

    for (int endpointIndex : endpointIndices) {
        if (!std::isfinite(distanceMap[endpointIndex])) {
            continue;
        }
        const PathCandidate candidate = tracePathToSeed(seedComponent, m_diameterMap, distanceMap, endpointIndex,
                                                        seedIndex, kDiameterWeight, route.lengthWeight);
        if (candidate.indices.empty() || candidate.indices.back() != seedIndex) {
            continue;
        }
        if (paths.empty()) {
            paths.push_back(candidate);
        } else {
            keepBestPathPerSeedBranch(paths, candidate);
        }
    }

    if (paths.empty()) {
        m_mainVesselSkeleton = seedComponent.clone();
    } else {
        const PathPairSelection selectedPair = chooseBestEndpointPair(paths, route.pairWeight);
        if (selectedPair.first >= 0) {
            const PathCandidate *secondPath = selectedPair.second >= 0 ? &paths[selectedPair.second] : nullptr;
            const QVector<QPoint> rawPath = orderedPathThroughSeed(paths[selectedPair.first],
                                                                   secondPath,
                                                                   seedIndex,
                                                                   m_vesselSkeleton.cols);
            if (!rawPath.isEmpty()) {
                const QVector<QPoint> smoothedPath = smoothPathThroughSeed(rawPath, m_vesselSkeleton.size(), m_seedPoint);
                m_smoothedMainVesselPath = rasterizePathPoints(smoothedPath, m_vesselSkeleton.size());
                m_mainVesselSkeleton = makePathMask(m_smoothedMainVesselPath, m_vesselSkeleton.size());
                m_mainVesselSkeleton.at<uchar>(m_seedPoint.y(), m_seedPoint.x()) = 255;
                appendLog(QStringLiteral("已对主血管路径进行平滑：半径 %1，轮数 %2，原始点数 %3，平滑后点数 %4。")
                          .arg(kPathSmoothRadius)
                          .arg(kPathSmoothPasses)
                          .arg(rawPath.size())
                          .arg(m_smoothedMainVesselPath.size()));
            }
        }
    }
    if (m_mainVesselSkeleton.empty()) {
        m_mainVesselSkeleton = seedComponent.clone();
    }

    m_mainVesselSkeleton.at<uchar>(m_seedPoint.y(), m_seedPoint.x()) = 255;
    m_mainVesselSkeleton = pruneSpurs(m_mainVesselSkeleton, 1);
    m_mainVesselSkeleton.at<uchar>(m_seedPoint.y(), m_seedPoint.x()) = 255;

    const QVector<QPoint> finalPath = orderedMainVesselPath();
    updatePathStats(finalPath);

    showSelectionView();
    showRouteView();
    updateStats();
    appendLog(QStringLiteral("已生成主血管路径：种子点 row=%1, col=%2。")
              .arg(m_seedPoint.y()).arg(m_seedPoint.x()));
    return true;
}

void VesselFindingDialog::resetMaskParams()
{
    ui->SB_gen_brightness->setValue(0.50);
    ui->SB_gen_bridgingRadius->setValue(2);
    ui->SB_fill_maxHoleArea->setValue(50);
    ui->SB_fill_vesselRatio->setValue(0.50);
}

void VesselFindingDialog::resetRouteParams()
{
    ui->SB_sel_minDiameter->setValue(8.0);
    ui->SB_sel_spurIterations->setValue(12);
}

void VesselFindingDialog::resetEndpointWeightParams()
{
    ui->SB_endPt_LenWeight->setValue(0.90);
    ui->SB_endPt_PairWeight->setValue(22.0);
}

void VesselFindingDialog::loadSettings()
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("VesselFindingDialog"));
    ui->SB_gen_brightness->setValue(settings.value(QStringLiteral("brightness"), ui->SB_gen_brightness->value()).toDouble());
    ui->SB_gen_bridgingRadius->setValue(settings.value(QStringLiteral("bridgingRadius"), ui->SB_gen_bridgingRadius->value()).toInt());
    ui->SB_fill_maxHoleArea->setValue(settings.value(QStringLiteral("maxHoleArea"), ui->SB_fill_maxHoleArea->value()).toInt());
    ui->SB_fill_vesselRatio->setValue(settings.value(QStringLiteral("vesselRatio"), ui->SB_fill_vesselRatio->value()).toDouble());
    ui->SB_sel_minDiameter->setValue(settings.value(QStringLiteral("minDiameter"), ui->SB_sel_minDiameter->value()).toDouble());
    ui->SB_sel_spurIterations->setValue(settings.value(QStringLiteral("spurIterations"), ui->SB_sel_spurIterations->value()).toInt());
    ui->SB_endPt_LenWeight->setValue(settings.value(QStringLiteral("lengthWeight"), ui->SB_endPt_LenWeight->value()).toDouble());
    ui->SB_endPt_PairWeight->setValue(settings.value(QStringLiteral("pairWeight"), ui->SB_endPt_PairWeight->value()).toDouble());
    ui->CB_depthDependency->setChecked(settings.value(QStringLiteral("depthDependency"), ui->CB_depthDependency->isChecked()).toBool());
    ui->SB_depthDependencyRange->setValue(settings.value(QStringLiteral("depthDependencyRange"), ui->SB_depthDependencyRange->value()).toInt());
    ui->CB_noReturn->setChecked(settings.value(QStringLiteral("noReturn"), ui->CB_noReturn->isChecked()).toBool());
    ui->CB_generateWAV->setChecked(settings.value(QStringLiteral("generateWAV"), ui->CB_generateWAV->isChecked()).toBool());
    ui->SB_depthDependencyRange->setEnabled(ui->CB_depthDependency->isChecked());
    settings.endGroup();
}

void VesselFindingDialog::saveSettings(bool includeEndpointWeights) const
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("VesselFindingDialog"));
    settings.setValue(QStringLiteral("brightness"), ui->SB_gen_brightness->value());
    settings.setValue(QStringLiteral("bridgingRadius"), ui->SB_gen_bridgingRadius->value());
    settings.setValue(QStringLiteral("maxHoleArea"), ui->SB_fill_maxHoleArea->value());
    settings.setValue(QStringLiteral("vesselRatio"), ui->SB_fill_vesselRatio->value());
    settings.setValue(QStringLiteral("minDiameter"), ui->SB_sel_minDiameter->value());
    settings.setValue(QStringLiteral("spurIterations"), ui->SB_sel_spurIterations->value());
    settings.setValue(QStringLiteral("depthDependency"), ui->CB_depthDependency->isChecked());
    settings.setValue(QStringLiteral("depthDependencyRange"), ui->SB_depthDependencyRange->value());
    settings.setValue(QStringLiteral("noReturn"), ui->CB_noReturn->isChecked());
    settings.setValue(QStringLiteral("generateWAV"), ui->CB_generateWAV->isChecked());
    if (includeEndpointWeights) {
        settings.setValue(QStringLiteral("lengthWeight"), ui->SB_endPt_LenWeight->value());
        settings.setValue(QStringLiteral("pairWeight"), ui->SB_endPt_PairWeight->value());
    }
    settings.endGroup();
    settings.sync();
}

VesselFindingDialog::MaskParams VesselFindingDialog::maskParams() const
{
    MaskParams params;
    params.brightness = ui->SB_gen_brightness->value();
    params.bridgingRadius = ui->SB_gen_bridgingRadius->value();
    params.maxHoleArea = ui->SB_fill_maxHoleArea->value();
    params.vesselRatio = ui->SB_fill_vesselRatio->value();
    return params;
}

VesselFindingDialog::RouteParams VesselFindingDialog::routeParams() const
{
    RouteParams params;
    params.minDiameter = ui->SB_sel_minDiameter->value();
    params.spurIterations = ui->SB_sel_spurIterations->value();
    params.lengthWeight = ui->SB_endPt_LenWeight->value();
    params.pairWeight = ui->SB_endPt_PairWeight->value();
    return params;
}

void VesselFindingDialog::showMaskViews()
{
    m_maskCanvas->setImage(matToQImage(m_initialVesselMask));
}

void VesselFindingDialog::showSelectionView()
{
    cv::Mat overlay;
    if (m_interactionMode == kInteractionDrawPath) {
        overlay = m_rgbImage.clone();
    } else {
        overlay = makeSkeletonOverlay(m_rgbImage, m_vesselSkeleton, cv::Scalar(0, 255, 255),
                                      m_hasSeed, m_seedPoint,
                                      m_depthRemovedSkeleton, cv::Scalar(0, 45, 170));
    }
    m_selectionCanvas->setImage(matToQImage(overlay));
    m_selectionCanvas->setPathOverlay(m_manualVesselPath);
}

void VesselFindingDialog::showRouteView()
{
    if (m_mainVesselSkeleton.empty()) {
        m_routeCanvas->setImage(QImage());
        return;
    }
    cv::Mat overlay = makeSkeletonOverlay(m_rgbImage, m_mainVesselSkeleton, cv::Scalar(255, 0, 0),
                                          m_hasSeed, m_seedPoint);
    const QVector<QPoint> path = orderedMainVesselPath();
    if (!path.isEmpty()) {
        drawPointMarker(overlay, path.first(), cv::Scalar(255, 255, 0));
        if (path.last() != path.first()) {
            drawPointMarker(overlay, path.last(), cv::Scalar(0, 0, 255));
        }
    }
    m_routeCanvas->setImage(matToQImage(overlay));
}

void VesselFindingDialog::setSelectionActive(bool active)
{
    setInteractionMode(active ? kInteractionSelectSeed : kInteractionNone);
}

void VesselFindingDialog::setInteractionMode(int mode)
{
    m_interactionMode = mode;
    if (m_selectionCanvas) {
        m_selectionCanvas->setSelectable(mode == kInteractionSelectSeed || mode == kInteractionFixSkeleton);
        m_selectionCanvas->setDrawingEnabled(mode == kInteractionDrawPath);
    }
    ui->button_sel_confirm->setEnabled(mode == kInteractionSelectSeed || m_hasSeed || !m_manualVesselPath.isEmpty());
    ui->button_fixSkeleton->setChecked(mode == kInteractionFixSkeleton);
    ui->button_draw->setChecked(mode == kInteractionDrawPath);

    if (mode == kInteractionSelectSeed) {
        appendLog(QStringLiteral("选择模式已开启：请点击青色骨架上的目标血管。"));
    } else if (mode == kInteractionFixSkeleton) {
        appendLog(QStringLiteral("精修模式已开启：请点击需要拉平的骨架附近位置。"));
    } else if (mode == kInteractionDrawPath) {
        appendLog(QStringLiteral("手绘模式已开启：请按住鼠标左键直接在左侧图片上绘制路径。"));
    } else {
        appendLog(QStringLiteral("选择模式已关闭。"));
    }
}

void VesselFindingDialog::restoreFullSkeletonForSelection(const QString &reason, int nextMode)
{
    if (m_initialVesselMask.empty() || m_diameterMap.empty()) {
        return;
    }

    cv::Mat fullSkeleton = m_fullVesselSkeleton.clone();
    if (fullSkeleton.empty() || cv::countNonZero(fullSkeleton) == 0) {
        appendLog(QStringLiteral("当前没有缓存的完整骨架，请先点击“更新”重新生成骨架。"));
        return;
    }

    m_vesselSkeleton = fullSkeleton;
    m_depthRemovedSkeleton.release();
    m_mainVesselSkeleton.release();
    m_smoothedMainVesselPath.clear();
    clearManualPath();
    m_maxDiameter = 0.0;
    m_meanDiameter = 0.0;
    m_mainVesselLen = 0;

    if (m_hasSeed) {
        moveSeedToNearestSkeleton(m_seedPoint, std::numeric_limits<int>::max());
    }

    showSelectionView();
    showRouteView();
    updateStats();
    setInteractionMode(nextMode);
    if (nextMode == kInteractionFixSkeleton) {
        appendLog(reason + QStringLiteral("：已恢复完整骨架，请点击需要拉平的骨架附近位置。"));
    } else {
        appendLog(reason + QStringLiteral("：已恢复完整骨架，请重新选择种子点或点击“确定”重新生成路径。"));
    }
}

void VesselFindingDialog::handleSeedClick(const QPoint &imagePoint)
{
    if (m_interactionMode == kInteractionFixSkeleton) {
        handleSkeletonFixClick(imagePoint);
        return;
    }
    if (m_interactionMode != kInteractionSelectSeed) {
        return;
    }
    if (moveSeedToNearestSkeleton(imagePoint, kManualSearchRadius)) {
        m_mainVesselSkeleton.release();
        m_depthRemovedSkeleton.release();
        m_smoothedMainVesselPath.clear();
        clearManualPath();
        showSelectionView();
        showRouteView();
        updateStats();
        appendLog(QStringLiteral("已选择种子点：X=%1, Y=%2。点击“确定”生成路径。")
                  .arg(m_seedPoint.x()).arg(m_seedPoint.y()));
    }
}

void VesselFindingDialog::handleSkeletonFixClick(const QPoint &imagePoint)
{
    if (m_fullVesselSkeleton.empty() || cv::countNonZero(m_fullVesselSkeleton) == 0) {
        appendLog(QStringLiteral("当前没有可精修的完整骨架，请先生成骨架。"));
        return;
    }

    SkeletonFixResult fixResult;
    cv::Mat refinedSkeleton = m_fullVesselSkeleton.clone();
    if (!flattenSkeletonNearPoint(refinedSkeleton, imagePoint, &fixResult)) {
        appendLog(QStringLiteral("精修失败：点击位置附近没有可连接并拉平的局部骨架。"));
        return;
    }

    m_fullVesselSkeleton = refinedSkeleton;
    m_vesselSkeleton = m_fullVesselSkeleton.clone();
    m_depthRemovedSkeleton.release();
    m_mainVesselSkeleton.release();
    m_smoothedMainVesselPath.clear();
    clearManualPath();
    m_maxDiameter = 0.0;
    m_meanDiameter = 0.0;
    m_mainVesselLen = 0;

    if (m_hasSeed) {
        moveSeedToNearestSkeleton(m_seedPoint, std::numeric_limits<int>::max());
    }

    showSelectionView();
    showRouteView();
    updateStats();
    appendLog(QStringLiteral("已精修骨架：点击 X=%1, Y=%2，最近骨架距离 %3 像素，拉平骨架点 %4，改动像素 %5。")
              .arg(imagePoint.x())
              .arg(imagePoint.y())
              .arg(fixResult.clickDistance, 0, 'f', 1)
              .arg(fixResult.pathPixels)
              .arg(fixResult.changedPixels));
}

void VesselFindingDialog::handleDrawnPath(const QVector<QPoint> &path)
{
    if (m_interactionMode != kInteractionDrawPath) {
        return;
    }
    if (m_rgbImage.empty()) {
        appendLog(QStringLiteral("没有可手绘的血管图。"));
        return;
    }

    const QVector<QPoint> rasterizedPath = rasterizePathPoints(path, m_rgbImage.size());
    if (rasterizedPath.size() < 2) {
        appendLog(QStringLiteral("手绘路径太短，请按住鼠标左键沿血管拖动画线。"));
        return;
    }

    m_manualVesselPath = rasterizedPath;
    m_smoothedMainVesselPath = rasterizedPath;
    m_mainVesselSkeleton = makePathMask(m_smoothedMainVesselPath, m_rgbImage.size());
    m_seedPoint = m_smoothedMainVesselPath.first();
    m_hasSeed = true;
    m_depthRemovedSkeleton.release();

    updatePathStats(m_smoothedMainVesselPath);
    if (m_selectionCanvas) {
        m_selectionCanvas->setPathOverlay(m_manualVesselPath);
    }
    showSelectionView();
    showRouteView();
    updateStats();
    ui->button_sel_confirm->setEnabled(true);
    appendLog(QStringLiteral("已生成手绘路径：起点 X=%1, Y=%2；终点 X=%3, Y=%4；点数 %5。")
              .arg(m_smoothedMainVesselPath.first().x())
              .arg(m_smoothedMainVesselPath.first().y())
              .arg(m_smoothedMainVesselPath.last().x())
              .arg(m_smoothedMainVesselPath.last().y())
              .arg(m_smoothedMainVesselPath.size()));
}

void VesselFindingDialog::clearManualPath()
{
    m_manualVesselPath.clear();
    if (m_selectionCanvas) {
        m_selectionCanvas->setPathOverlay(QVector<QPoint>());
    }
}

void VesselFindingDialog::updatePathStats(const QVector<QPoint> &path)
{
    m_mainVesselLen = path.size();
    double diameterSum = 0.0;
    m_maxDiameter = 0.0;

    for (const QPoint &point : path) {
        if (!m_diameterMap.empty()
            && point.y() >= 0 && point.y() < m_diameterMap.rows
            && point.x() >= 0 && point.x() < m_diameterMap.cols) {
            const double diameter = m_diameterMap.at<float>(point.y(), point.x());
            diameterSum += diameter;
            m_maxDiameter = std::max(m_maxDiameter, diameter);
        }
    }

    m_meanDiameter = m_mainVesselLen > 0 && !m_diameterMap.empty()
        ? diameterSum / m_mainVesselLen : 0.0;
}

bool VesselFindingDialog::moveSeedToNearestSkeleton(const QPoint &preferredPoint, int maxDistance)
{
    if (m_vesselSkeleton.empty() || cv::countNonZero(m_vesselSkeleton) == 0) {
        appendLog(QStringLiteral("没有可选择的骨架点。"));
        return false;
    }

    double bestDistance = std::numeric_limits<double>::infinity();
    QPoint bestPoint;
    for (int row = 0; row < m_vesselSkeleton.rows; ++row) {
        for (int col = 0; col < m_vesselSkeleton.cols; ++col) {
            if (!m_vesselSkeleton.at<uchar>(row, col)) {
                continue;
            }
            const double dx = col - preferredPoint.x();
            const double dy = row - preferredPoint.y();
            const double distance = std::sqrt(dx * dx + dy * dy);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestPoint = QPoint(col, row);
            }
        }
    }

    if (bestDistance > maxDistance) {
        appendLog(QStringLiteral("点击位置距离最近骨架点 %1 像素，超过搜索半径 %2。")
                  .arg(bestDistance, 0, 'f', 1).arg(maxDistance));
        return false;
    }

    m_seedPoint = bestPoint;
    m_hasSeed = true;
    return true;
}

void VesselFindingDialog::appendImageSizeLog()
{
    if (m_rgbImage.empty()) {
        return;
    }

    const QSize originalSize(m_rgbImage.cols, m_rgbImage.rows);
    const QSize displaySize = scaledImageSize(originalSize, QSize(kDisplayWidgetWidth, kDisplayWidgetHeight));
    if (displaySize == originalSize) {
        appendLog(QStringLiteral("图片原本大小：%1 x %2；未缩小显示。")
                  .arg(originalSize.width())
                  .arg(originalSize.height()));
    } else {
        appendLog(QStringLiteral("图片原本大小：%1 x %2；缩小后显示大小：%3 x %4。")
                  .arg(originalSize.width())
                  .arg(originalSize.height())
                  .arg(displaySize.width())
                  .arg(displaySize.height()));
    }
}

void VesselFindingDialog::updateStats()
{
    ui->num_n_maxDiameter->setText(QString::number(m_maxDiameter, 'f', 2));
    ui->num_n_meanDiameter->setText(QString::number(m_meanDiameter, 'f', 2));
    ui->num_n_mainVesselLen->setText(QString::number(m_mainVesselLen));
}

void VesselFindingDialog::appendLog(const QString &message)
{
    if (ui && ui->textEdit_sel) {
        ui->textEdit_sel->append(message);
        QApplication::processEvents();
    }
}

bool VesselFindingDialog::exportPathFile()
{
    if (m_mainVesselSkeleton.empty() || cv::countNonZero(m_mainVesselSkeleton) == 0) {
        appendLog(QStringLiteral("还没有可导出的路径。"));
        return false;
    }

    const QVector<QPoint> path = orderedMainVesselPath();
    if (path.isEmpty()) {
        appendLog(QStringLiteral("路径排序失败，无法导出。"));
        return false;
    }

    const QString dirPath = QFileInfo(QString::fromLocal8Bit(__FILE__)).absolutePath();

    if (ui->CB_generateWAV->isChecked()) {
        return exportWavPathFile(path, dirPath);
    }

    QFile fileX(dirPath + QStringLiteral("/scanX.txt"));
    QFile fileY(dirPath + QStringLiteral("/scanY.txt"));
    if (!fileX.open(QFile::WriteOnly | QFile::Text) || !fileY.open(QFile::WriteOnly | QFile::Text)) {
        styledMessageBox(this,
                         QMessageBox::Warning,
                         tr("保存失败"),
                         tr("无法写入 scanX.txt 或 scanY.txt。"),
                         QMessageBox::Ok,
                         QMessageBox::Ok);
        return false;
    }

    QTextStream streamX(&fileX);
    QTextStream streamY(&fileY);
    for (const QPoint &point : path) {
        const double xPosition = m_mainVesselSkeleton.cols > 1
            ? (2.0 * point.x() / (m_mainVesselSkeleton.cols - 1) - 1.0) : 0.0;
        const double yPosition = m_mainVesselSkeleton.rows > 1
            ? (2.0 * point.y() / (m_mainVesselSkeleton.rows - 1) - 1.0) : 0.0;
        streamX << toDaUnsignedShort(m_voltage, xPosition) << "\n";
        streamY << toDaUnsignedShort(m_voltage, yPosition) << "\n";
    }

    appendLog(QStringLiteral("扫描路径已保存：%1/scanX.txt, %1/scanY.txt；点数 %2，电压 %3；返回模式：%4。")
              .arg(dirPath)
              .arg(path.size())
              .arg(m_voltage, 0, 'f', 2)
              .arg(ui->CB_noReturn->isChecked()
                   ? QStringLiteral("终点线性返回起点")
                   : QStringLiteral("0V 到起点，终点回 0V")));
    return true;
}

bool VesselFindingDialog::exportWavPathFile(const QVector<QPoint> &path, const QString &dirPath)
{
    if (path.isEmpty()) {
        appendLog(QStringLiteral("WAV 生成失败：路径为空。"));
        return false;
    }

    const QString wavPath = dirPath + QStringLiteral("/vessel_scan_path.wav");
    const QString scanXPath = dirPath + QStringLiteral("/scanX.txt");
    const QString scanYPath = dirPath + QStringLiteral("/scanY.txt");
    QFile wavFile(wavPath);
    if (!wavFile.open(QFile::WriteOnly)) {
        styledMessageBox(this,
                         QMessageBox::Warning,
                         tr("保存失败"),
                         tr("无法写入 vessel_scan_path.wav。"),
                         QMessageBox::Ok,
                         QMessageBox::Ok);
        return false;
    }

    const int bytesPerSample = kVesselWavBitsPerSample / 8;
    const quint16 blockAlign = static_cast<quint16>(kVesselWavChannels * bytesPerSample);
    const quint32 byteRate = static_cast<quint32>(kVesselWavSampleRate * blockAlign);

    const int validAlineCount = (m_bscanLen > 0) ? m_bscanLen : path.size();
    const int cycleAlineCount = (m_bscanCycleLen > 0) ? m_bscanCycleLen : validAlineCount;
    if (validAlineCount <= 0 || cycleAlineCount < validAlineCount) {
        appendLog(QStringLiteral("WAV 生成失败：BscanLength=%1，BscanCycleLen=%2，无法生成完整周期路径。")
                  .arg(validAlineCount)
                  .arg(cycleAlineCount));
        return false;
    }

    const QVector<QPointF> pathPoints = resamplePathToCount(path,
                                                            m_mainVesselSkeleton.cols,
                                                            m_mainVesselSkeleton.rows,
                                                            validAlineCount);
    if (pathPoints.isEmpty()) {
        appendLog(QStringLiteral("WAV 生成失败：路径重采样为空。"));
        return false;
    }

    const bool noReturnToZero = ui->CB_noReturn->isChecked();
    const int totalMoveAlineCount = cycleAlineCount - validAlineCount;
    int initialMoveAlineCount = 0;
    int returnMoveAlineCount = totalMoveAlineCount;
    if (!noReturnToZero) {
        if (totalMoveAlineCount % 2 != 0) {
            appendLog(QStringLiteral("WAV 生成失败：BscanCycleLen(%1) - BscanLength(%2) 不能平均分配到起止两个线性运动段。")
                      .arg(cycleAlineCount)
                      .arg(validAlineCount));
            return false;
        }
        initialMoveAlineCount = totalMoveAlineCount / 2;
        returnMoveAlineCount = totalMoveAlineCount / 2;
    }

    QVector<QPointF> daPoints;
    daPoints.reserve(cycleAlineCount);
    const QPointF zeroPoint(0.0, 0.0);
    const QPointF startPoint = pathPoints.first();
    const QPointF endPoint = pathPoints.last();
    if (!noReturnToZero) {
        for (int i = 0; i < initialMoveAlineCount; ++i) {
            daPoints.append(interpolateWavPoint(zeroPoint, startPoint, i, initialMoveAlineCount));
        }
    }
    for (const QPointF &point : pathPoints) {
        daPoints.append(point);
    }
    const QPointF returnTarget = noReturnToZero ? startPoint : zeroPoint;
    for (int i = 0; i < returnMoveAlineCount; ++i) {
        daPoints.append(interpolateWavPoint(endPoint, returnTarget, i, returnMoveAlineCount));
    }

    if (daPoints.size() != cycleAlineCount) {
        appendLog(QStringLiteral("WAV 生成失败：完整周期路径点数异常，期望 %1，实际 %2。")
                  .arg(cycleAlineCount)
                  .arg(daPoints.size()));
        return false;
    }

    const int wavFrameCount = std::max(1, static_cast<int>(std::llround(
        static_cast<double>(cycleAlineCount) * kVesselWavSampleRate
        / static_cast<double>(std::max(1, m_ascanFreq)))));
    const QVector<QPointF> wavPoints = resamplePointSeries(daPoints, wavFrameCount);

    const quint32 dataSize = static_cast<quint32>(wavPoints.size() * blockAlign);

    QByteArray pcmData;
    pcmData.reserve(static_cast<int>(dataSize));
    for (const QPointF &point : wavPoints) {
        appendS24Le(pcmData, toWavSample24(point.x()));
        appendS24Le(pcmData, toWavSample24(point.y()));
    }

    QFile fileX(scanXPath);
    QFile fileY(scanYPath);
    if (!fileX.open(QFile::WriteOnly | QFile::Text) || !fileY.open(QFile::WriteOnly | QFile::Text)) {
        styledMessageBox(this,
                         QMessageBox::Warning,
                         tr("保存失败"),
                         tr("无法写入 scanX.txt 或 scanY.txt。"),
                         QMessageBox::Ok,
                         QMessageBox::Ok);
        return false;
    }

    QTextStream streamX(&fileX);
    QTextStream streamY(&fileY);
    for (const QPointF &point : daPoints) {
        streamX << toDaUnsignedShort(m_voltage, point.x()) << "\n";
        streamY << toDaUnsignedShort(m_voltage, point.y()) << "\n";
    }

    QByteArray data;
    data.reserve(44 + static_cast<int>(dataSize));
    appendAscii(data, "RIFF");
    appendU32Le(data, 36u + dataSize);
    appendAscii(data, "WAVE");
    appendAscii(data, "fmt ");
    appendU32Le(data, 16u);
    appendU16Le(data, 1u);
    appendU16Le(data, static_cast<quint16>(kVesselWavChannels));
    appendU32Le(data, static_cast<quint32>(kVesselWavSampleRate));
    appendU32Le(data, byteRate);
    appendU16Le(data, blockAlign);
    appendU16Le(data, static_cast<quint16>(kVesselWavBitsPerSample));
    appendAscii(data, "data");
    appendU32Le(data, dataSize);
    data.append(pcmData);

    if (wavFile.write(data) != data.size()) {
        styledMessageBox(this,
                         QMessageBox::Warning,
                         tr("保存失败"),
                         tr("写入 vessel_scan_path.wav 时发生错误。"),
                         QMessageBox::Ok,
                         QMessageBox::Ok);
        return false;
    }

    appendLog(QStringLiteral("音频路径已保存：%1；24-bit, 48000 Hz, 双声道，有效路径 %2 Aline，完整周期 %3 Aline / %4 音频点，AscanFreq=%5 Hz，幅度限制为满量程的 80%；循环模式：%6。")
              .arg(wavPath)
              .arg(pathPoints.size())
              .arg(daPoints.size())
              .arg(wavPoints.size())
              .arg(m_ascanFreq)
              .arg(noReturnToZero
                   ? QStringLiteral("终点线性返回起点")
                   : QStringLiteral("0V 到起点，终点回 0V")));
    appendLog(QStringLiteral("扫描路径已按 DA 周期保存：%1/scanX.txt, %1/scanY.txt；点数 %2。")
              .arg(dirPath)
              .arg(daPoints.size()));
    return true;
}

QVector<QPoint> VesselFindingDialog::orderedMainVesselPath() const
{
    if (!m_smoothedMainVesselPath.isEmpty()) {
        return m_smoothedMainVesselPath;
    }
    return orderedPathFromMask(m_mainVesselSkeleton, m_hasSeed, m_seedPoint);
}

bool VesselFindingDialog::ensureDepthIndexMap()
{
    if (m_rgbImage.empty() || m_initialVesselMask.empty()) {
        return false;
    }
    if (!m_depthIndexMap.empty() && m_depthIndexMap.size() == m_rgbImage.size()) {
        return true;
    }

    cv::Mat depthRgbImage;
    QString sourceLabel;
    const QStringList candidates = depthProjectionPathCandidates(m_inputImagePath);
    for (const QString &candidate : candidates) {
        if (!QFileInfo(candidate).exists()) {
            continue;
        }
        cv::Mat candidateImage = readRgbImageFile(candidate);
        if (candidateImage.empty()) {
            continue;
        }
        if (candidateImage.size() != m_rgbImage.size()) {
            appendLog(QStringLiteral("深度图尺寸不匹配，已跳过：%1").arg(QFileInfo(candidate).fileName()));
            continue;
        }
        depthRgbImage = candidateImage;
        sourceLabel = QFileInfo(candidate).fileName();
        break;
    }

    if (depthRgbImage.empty()) {
        depthRgbImage = m_rgbImage;
        sourceLabel = QStringLiteral("当前血管图估算");
    }

    cv::Mat depthIndexMap = makeDepthIndexMapFromRgb(depthRgbImage, m_initialVesselMask);
    depthIndexMap = fillMissingDepthIndices(depthIndexMap, m_initialVesselMask, kDepthFillRadius);
    const int validDepthPixels = countValidDepthPixels(depthIndexMap, m_initialVesselMask);
    if (validDepthPixels <= 0) {
        m_depthIndexMap.release();
        m_depthProjectionSource.clear();
        return false;
    }

    m_depthIndexMap = depthIndexMap;
    m_depthProjectionSource = sourceLabel;
    appendLog(QStringLiteral("深度图已准备：%1，可用像素 %2。")
              .arg(m_depthProjectionSource)
              .arg(validDepthPixels));
    return true;
}

void VesselFindingDialog::on_button_fixSkeleton_clicked()
{
    appendLog(QStringLiteral("正在进入精修骨架模式..."));
    if (m_rgbImage.empty()) {
        loadImageFromDialog();
        return;
    }
    restoreFullSkeletonForSelection(QStringLiteral("精修骨架"), kInteractionFixSkeleton);
}

void VesselFindingDialog::on_button_draw_clicked()
{
    appendLog(QStringLiteral("正在进入手绘路径模式..."));
    if (m_rgbImage.empty()) {
        loadImageFromDialog();
        return;
    }

    clearManualPath();
    m_mainVesselSkeleton.release();
    m_smoothedMainVesselPath.clear();
    m_hasSeed = false;
    m_seedPoint = QPoint();
    m_maxDiameter = 0.0;
    m_meanDiameter = 0.0;
    m_mainVesselLen = 0;
    updateStats();
    setInteractionMode(kInteractionDrawPath);
    showSelectionView();
    showRouteView();
}

void VesselFindingDialog::on_button_sel_redo_clicked()
{
    appendLog(QStringLiteral("正在重新开启选择模式..."));
    if (m_rgbImage.empty()) {
        loadImageFromDialog();
        return;
    }
    restoreFullSkeletonForSelection(QStringLiteral("重新选择"), kInteractionSelectSeed);
}

void VesselFindingDialog::on_button_sel_confirm_clicked()
{
    appendLog(QStringLiteral("正在确认选择并生成路径..."));
    if (!m_manualVesselPath.isEmpty()) {
        saveSettings(true);
        setSelectionActive(false);
        showSelectionView();
        appendLog(QStringLiteral("手绘路径已确认，可点击右下角“确定”导出路径。"));
        return;
    }

    if (rebuildRoute()) {
        saveSettings(true);
        setSelectionActive(false);
    }
}

void VesselFindingDialog::on_button_mask_redo_clicked()
{
    appendLog(QStringLiteral("正在重置二值掩膜和骨架参数..."));
    resetMaskParams();
    resetRouteParams();
    saveSettings(false);
    appendLog(QStringLiteral("二值掩膜和骨架参数已重置为默认值。"));
}

void VesselFindingDialog::on_button_route_redo_clicked()
{
    appendLog(QStringLiteral("正在重置终点权重参数..."));
    resetEndpointWeightParams();
    saveSettings(true);
    appendLog(QStringLiteral("终点权重参数已重置为默认值。"));
}

void VesselFindingDialog::on_button_route_save_clicked()
{
    appendLog(QStringLiteral("正在更新二值掩膜和骨架参数..."));
    if (rebuildMaskAndSkeleton(false)) {
        setSelectionActive(true);
        saveSettings(false);
    }
}

void VesselFindingDialog::on_button_show_reselect_clicked()
{
    appendLog(QStringLiteral("正在重新选择图片..."));
    loadImageFromDialog();
}

void VesselFindingDialog::on_button_show_cancel_clicked()
{
    reject();
}

void VesselFindingDialog::on_button_show_confirm_clicked()
{
    appendLog(QStringLiteral("正在确认并导出路径..."));
    bool rebuiltRoute = false;
    if (m_mainVesselSkeleton.empty()) {
        if (!rebuildRoute()) {
            return;
        }
        rebuiltRoute = true;
    }
    if (exportPathFile()) {
        saveSettings(rebuiltRoute);
        accept();
    }
}
