#include "KLinearCalibration.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStringList>
#include <QTextStream>
#include <QtGlobal>
#include <mkl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

const char *kSampleTypeUint16Raw = "uint16_raw";
const char *kSampleTypeFloat32Spectrum = "float32_spectrum";

struct FileMetadata
{
    bool hasSidecar = false;
    QString sidecarPath;
    int ascanLen = 0;
    QString dtypeName;
    QString sampleType;
};

struct AveragedMeasurement
{
    std::vector<double> spectrum;
    int ascanLen = 0;
    int lineCount = 0;
    QString dtypeName;
    QString sidecarPath;
};

struct FitAttempt
{
    int degree = 0;
    double minDelta = 0.0;
};

struct PhaseFit
{
    bool ok = false;
    QString errorMessage;
    std::vector<double> fittedPhase;
    std::vector<double> rawCoefficientsHighToLow;
    std::vector<FitAttempt> attempts;
    int requestedDegree = 0;
    int selectedDegree = 0;
    bool autoDowngraded = false;
    double minDelta = 0.0;
};

QString sidecarPathForDataFile(const QString &path)
{
    const QFileInfo info(path);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".json"));
}

QString sampleTypeToDtype(const QString &sampleType)
{
    const QString normalized = sampleType.trimmed().toLower();
    if (normalized == QString::fromLatin1(kSampleTypeUint16Raw))
        return QStringLiteral("uint16");
    if (normalized == QString::fromLatin1(kSampleTypeFloat32Spectrum))
        return QStringLiteral("float32");
    return QString();
}

bool readJsonObject(const QString &path, QJsonObject *object, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法打开 JSON 文件 %1：%2。")
                .arg(path, file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("JSON 文件 %1 格式无效：%2。")
                .arg(path, parseError.errorString());
        return false;
    }

    if (object)
        *object = document.object();
    return true;
}

FileMetadata readSidecarMetadata(const QString &dataPath, QString *errorMessage)
{
    FileMetadata metadata;
    const QString sidecarPath = sidecarPathForDataFile(dataPath);
    if (!QFileInfo::exists(sidecarPath))
        return metadata;

    QJsonObject root;
    if (!readJsonObject(sidecarPath, &root, errorMessage))
        return metadata;

    metadata.hasSidecar = true;
    metadata.sidecarPath = sidecarPath;

    const QJsonObject acquisition = root.value(QStringLiteral("acquisition")).toObject();
    if (acquisition.contains(QStringLiteral("ascanLen")))
        metadata.ascanLen = acquisition.value(QStringLiteral("ascanLen")).toInt();
    metadata.sampleType = acquisition.value(QStringLiteral("sampleType")).toString();
    metadata.dtypeName = sampleTypeToDtype(metadata.sampleType);

    if (metadata.ascanLen <= 0) {
        const QJsonObject settings = root.value(QStringLiteral("settings")).toObject();
        const QJsonObject mainSettings = settings.value(QStringLiteral("mainWidget")).toObject();
        metadata.ascanLen = mainSettings.value(QStringLiteral("AscanLen")).toInt();
    }

    return metadata;
}

QString inferDtypeName(const QString &path, const FileMetadata &metadata)
{
    if (!metadata.dtypeName.isEmpty())
        return metadata.dtypeName;

    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("2d") || suffix == QStringLiteral("3d"))
        return QStringLiteral("uint16");
    return QStringLiteral("float32");
}

bool resolveAscanLen(const QString &path,
                     const FileMetadata &metadata,
                     int expectedAscanLen,
                     int *ascanLen,
                     QString *errorMessage)
{
    if (ascanLen == nullptr)
        return false;

    int resolved = metadata.ascanLen > 0 ? metadata.ascanLen : expectedAscanLen;
    if (resolved <= 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("%1 缺少 AscanLen。请确认同名 .json 存在，或先在界面中设置有效的 AscanLen。")
                .arg(path);
        return false;
    }

    if (expectedAscanLen > 0 && metadata.ascanLen > 0 && metadata.ascanLen != expectedAscanLen) {
        if (errorMessage)
            *errorMessage = QStringLiteral("%1 的同名 JSON 记录 AscanLen=%2，但当前设置为 %3。")
                .arg(path)
                .arg(metadata.ascanLen)
                .arg(expectedAscanLen);
        return false;
    }

    *ascanLen = resolved;
    return true;
}

bool loadAveragedMeasurement(const QString &path,
                             int expectedAscanLen,
                             int rawShiftBits,
                             int maxLines,
                             AveragedMeasurement *measurement,
                             QString *errorMessage)
{
    if (measurement == nullptr)
        return false;

    const QFileInfo fileInfo(path);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("文件不存在：%1。").arg(path);
        return false;
    }

    QString metadataError;
    const FileMetadata metadata = readSidecarMetadata(path, &metadataError);
    if (!metadataError.isEmpty()) {
        if (errorMessage)
            *errorMessage = metadataError;
        return false;
    }

    int ascanLen = 0;
    if (!resolveAscanLen(path, metadata, expectedAscanLen, &ascanLen, errorMessage))
        return false;

    const QString dtypeName = inferDtypeName(path, metadata);
    if (dtypeName != QStringLiteral("uint16") && dtypeName != QStringLiteral("float32")) {
        if (errorMessage)
            *errorMessage = QStringLiteral("%1 的数据类型 %2 暂不支持。")
                .arg(path, dtypeName);
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法打开 %1：%2。").arg(path, file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();

    const int bytesPerSample = (dtypeName == QStringLiteral("uint16")) ? 2 : 4;
    if (bytes.isEmpty() || bytes.size() % bytesPerSample != 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("%1 的文件大小不是 %2 字节采样的整数倍。")
                .arg(path)
                .arg(bytesPerSample);
        return false;
    }

    const qint64 sampleCount = bytes.size() / bytesPerSample;
    if (sampleCount % ascanLen != 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("%1 含有 %2 个采样点，不能整除 AscanLen=%3。")
                .arg(path)
                .arg(sampleCount)
                .arg(ascanLen);
        return false;
    }

    int lineCount = static_cast<int>(sampleCount / ascanLen);
    if (maxLines > 0)
        lineCount = std::min(lineCount, maxLines);
    if (lineCount <= 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("%1 中没有可用于标定的 A-line。").arg(path);
        return false;
    }

    std::vector<double> average(static_cast<size_t>(ascanLen), 0.0);
    const char *raw = bytes.constData();
    for (int line = 0; line < lineCount; ++line) {
        for (int z = 0; z < ascanLen; ++z) {
            const qint64 sampleIndex = static_cast<qint64>(line) * ascanLen + z;
            double value = 0.0;
            if (dtypeName == QStringLiteral("uint16")) {
                quint16 rawValue = 0;
                std::memcpy(&rawValue, raw + sampleIndex * bytesPerSample, sizeof(rawValue));
                if (rawShiftBits > 0)
                    rawValue = static_cast<quint16>(rawValue >> rawShiftBits);
                value = static_cast<double>(rawValue);
            } else {
                float rawValue = 0.0f;
                std::memcpy(&rawValue, raw + sampleIndex * bytesPerSample, sizeof(rawValue));
                value = static_cast<double>(rawValue);
            }
            average[static_cast<size_t>(z)] += value;
        }
    }

    const double scale = 1.0 / static_cast<double>(lineCount);
    for (double &value : average)
        value *= scale;

    measurement->spectrum.swap(average);
    measurement->ascanLen = ascanLen;
    measurement->lineCount = lineCount;
    measurement->dtypeName = dtypeName;
    measurement->sidecarPath = metadata.sidecarPath;
    return true;
}

int reflectIndex(int index, int size)
{
    if (size <= 1)
        return 0;

    while (index < 0 || index >= size) {
        if (index < 0)
            index = -index;
        else
            index = 2 * size - 2 - index;
    }
    return index;
}

std::vector<double> movingAverageReflect(const std::vector<double> &values, int width)
{
    const int size = static_cast<int>(values.size());
    if (width <= 1 || size <= 0)
        return values;

    width = std::min(width, size);
    const int left = width / 2;
    const int right = width - 1 - left;
    std::vector<double> averaged(static_cast<size_t>(size), 0.0);
    for (int i = 0; i < size; ++i) {
        double sum = 0.0;
        for (int offset = -left; offset <= right; ++offset)
            sum += values[static_cast<size_t>(reflectIndex(i + offset, size))];
        averaged[static_cast<size_t>(i)] = sum / static_cast<double>(width);
    }
    return averaged;
}

bool preprocessSpectrum(const std::vector<double> &measurement,
                        const std::vector<double> *background,
                        int highPassSamples,
                        bool keepDc,
                        std::vector<double> *spectrum,
                        QString *errorMessage)
{
    if (spectrum == nullptr || measurement.empty())
        return false;

    std::vector<double> processed = measurement;
    if (background != nullptr && !background->empty()) {
        if (background->size() != processed.size()) {
            if (errorMessage)
                *errorMessage = QStringLiteral("本底长度 %1 与 A-line 长度 %2 不一致。")
                    .arg(static_cast<qulonglong>(background->size()))
                    .arg(static_cast<qulonglong>(processed.size()));
            return false;
        }
        for (size_t i = 0; i < processed.size(); ++i)
            processed[i] -= (*background)[i];
    }

    if (!keepDc) {
        double mean = 0.0;
        for (double value : processed)
            mean += value;
        mean /= static_cast<double>(processed.size());
        for (double &value : processed)
            value -= mean;
    }

    if (highPassSamples > 1) {
        const std::vector<double> lowPass = movingAverageReflect(processed, highPassSamples);
        for (size_t i = 0; i < processed.size(); ++i)
            processed[i] -= lowPass[i];
    }

    spectrum->swap(processed);
    return true;
}

bool unwrappedPhase(const std::vector<double> &realSignal,
                    std::vector<double> *phase,
                    QString *errorMessage)
{
    if (phase == nullptr || realSignal.empty())
        return false;

    const int n = static_cast<int>(realSignal.size());
    std::vector<MKL_Complex16> input(static_cast<size_t>(n));
    std::vector<MKL_Complex16> spectrum(static_cast<size_t>(n));
    std::vector<MKL_Complex16> analytic(static_cast<size_t>(n));

    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(realSignal[static_cast<size_t>(i)])) {
            if (errorMessage)
                *errorMessage = QStringLiteral("输入光谱包含 NaN 或 inf。");
            return false;
        }
        input[static_cast<size_t>(i)].real = realSignal[static_cast<size_t>(i)];
        input[static_cast<size_t>(i)].imag = 0.0;
    }

    DFTI_DESCRIPTOR_HANDLE fftHandle = nullptr;
    MKL_LONG status = DftiCreateDescriptor(&fftHandle, DFTI_DOUBLE, DFTI_COMPLEX, 1, n);
    if (status == DFTI_NO_ERROR)
        status = DftiSetValue(fftHandle, DFTI_PLACEMENT, DFTI_NOT_INPLACE);
    if (status == DFTI_NO_ERROR)
        status = DftiCommitDescriptor(fftHandle);
    if (status == DFTI_NO_ERROR)
        status = DftiComputeForward(fftHandle, input.data(), spectrum.data());

    if (status == DFTI_NO_ERROR) {
        for (int i = 0; i < n; ++i) {
            double filter = 0.0;
            if (i == 0)
                filter = 1.0;
            else if (n % 2 == 0 && i == n / 2)
                filter = 1.0;
            else if (i < (n + 1) / 2)
                filter = 2.0;
            spectrum[static_cast<size_t>(i)].real *= filter;
            spectrum[static_cast<size_t>(i)].imag *= filter;
        }
        status = DftiComputeBackward(fftHandle, spectrum.data(), analytic.data());
    }

    if (fftHandle != nullptr)
        DftiFreeDescriptor(&fftHandle);

    if (status != DFTI_NO_ERROR) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Hilbert 相位提取 FFT 失败：%1。")
                .arg(QString::fromLatin1(DftiErrorMessage(status)));
        return false;
    }

    std::vector<double> unwrapped(static_cast<size_t>(n), 0.0);
    const double pi = 3.14159265358979323846;
    const double twoPi = 2.0 * pi;
    double previousRaw = std::atan2(analytic[0].imag, analytic[0].real);
    unwrapped[0] = previousRaw;
    for (int i = 1; i < n; ++i) {
        const double rawPhase = std::atan2(analytic[static_cast<size_t>(i)].imag,
                                           analytic[static_cast<size_t>(i)].real);
        double delta = rawPhase - previousRaw;
        while (delta > pi)
            delta -= twoPi;
        while (delta < -pi)
            delta += twoPi;
        unwrapped[static_cast<size_t>(i)] = unwrapped[static_cast<size_t>(i - 1)] + delta;
        previousRaw = rawPhase;
    }

    if (unwrapped.back() < unwrapped.front()) {
        for (double &value : unwrapped)
            value = -value;
    }

    phase->swap(unwrapped);
    return true;
}

bool solveLinearSystem(std::vector<std::vector<double>> matrix,
                       std::vector<double> rhs,
                       std::vector<double> *solution)
{
    const int n = static_cast<int>(rhs.size());
    if (n <= 0 || static_cast<int>(matrix.size()) != n)
        return false;

    for (int row = 0; row < n; ++row)
        matrix[static_cast<size_t>(row)].push_back(rhs[static_cast<size_t>(row)]);

    for (int col = 0; col < n; ++col) {
        int pivotRow = col;
        double pivotValue = std::fabs(matrix[static_cast<size_t>(col)][static_cast<size_t>(col)]);
        for (int row = col + 1; row < n; ++row) {
            const double candidate = std::fabs(matrix[static_cast<size_t>(row)][static_cast<size_t>(col)]);
            if (candidate > pivotValue) {
                pivotValue = candidate;
                pivotRow = row;
            }
        }

        if (pivotValue <= 1.0e-14 || !std::isfinite(pivotValue))
            return false;

        if (pivotRow != col)
            std::swap(matrix[static_cast<size_t>(pivotRow)], matrix[static_cast<size_t>(col)]);

        const double pivot = matrix[static_cast<size_t>(col)][static_cast<size_t>(col)];
        for (int item = col; item <= n; ++item)
            matrix[static_cast<size_t>(col)][static_cast<size_t>(item)] /= pivot;

        for (int row = 0; row < n; ++row) {
            if (row == col)
                continue;
            const double factor = matrix[static_cast<size_t>(row)][static_cast<size_t>(col)];
            if (factor == 0.0)
                continue;
            for (int item = col; item <= n; ++item)
                matrix[static_cast<size_t>(row)][static_cast<size_t>(item)] -=
                    factor * matrix[static_cast<size_t>(col)][static_cast<size_t>(item)];
        }
    }

    solution->assign(static_cast<size_t>(n), 0.0);
    for (int row = 0; row < n; ++row)
        (*solution)[static_cast<size_t>(row)] = matrix[static_cast<size_t>(row)][static_cast<size_t>(n)];
    return true;
}

double evaluatePolynomialLowToHigh(const std::vector<double> &coefficients, double x)
{
    double value = 0.0;
    for (int i = static_cast<int>(coefficients.size()) - 1; i >= 0; --i)
        value = value * x + coefficients[static_cast<size_t>(i)];
    return value;
}

double binomialCoefficient(int n, int k)
{
    if (k < 0 || k > n)
        return 0.0;
    if (k == 0 || k == n)
        return 1.0;

    double result = 1.0;
    for (int i = 1; i <= k; ++i)
        result = result * static_cast<double>(n - k + i) / static_cast<double>(i);
    return result;
}

std::vector<double> rawCoefficientsHighToLow(const std::vector<double> &scaledCoefficientsLowToHigh,
                                             double center,
                                             double scale)
{
    const int degree = static_cast<int>(scaledCoefficientsLowToHigh.size()) - 1;
    std::vector<double> rawLowToHigh(static_cast<size_t>(degree + 1), 0.0);

    for (int power = 0; power <= degree; ++power) {
        const double scaledCoefficient =
            scaledCoefficientsLowToHigh[static_cast<size_t>(power)] / std::pow(scale, power);
        for (int rawPower = 0; rawPower <= power; ++rawPower) {
            rawLowToHigh[static_cast<size_t>(rawPower)] +=
                scaledCoefficient
                * binomialCoefficient(power, rawPower)
                * std::pow(-center, power - rawPower);
        }
    }

    std::vector<double> highToLow(static_cast<size_t>(degree + 1), 0.0);
    for (int i = 0; i <= degree; ++i)
        highToLow[static_cast<size_t>(i)] = rawLowToHigh[static_cast<size_t>(degree - i)];
    return highToLow;
}

bool fitPolynomialScaled(const std::vector<double> &indices,
                         const std::vector<double> &phase,
                         int degree,
                         std::vector<double> *fitted,
                         std::vector<double> *rawCoefficients)
{
    if (indices.size() != phase.size() || indices.empty() || degree < 1)
        return false;

    const int coefficientCount = degree + 1;
    const double center = 0.5 * (indices.front() + indices.back());
    const double scale = 0.5 * (indices.back() - indices.front());
    if (scale <= 0.0)
        return false;

    std::vector<std::vector<double>> normalMatrix(
        static_cast<size_t>(coefficientCount),
        std::vector<double>(static_cast<size_t>(coefficientCount), 0.0));
    std::vector<double> rhs(static_cast<size_t>(coefficientCount), 0.0);

    for (size_t i = 0; i < indices.size(); ++i) {
        const double x = (indices[i] - center) / scale;
        std::vector<double> powers(static_cast<size_t>(2 * degree + 1), 1.0);
        for (int p = 1; p <= 2 * degree; ++p)
            powers[static_cast<size_t>(p)] = powers[static_cast<size_t>(p - 1)] * x;

        for (int row = 0; row < coefficientCount; ++row) {
            rhs[static_cast<size_t>(row)] += phase[i] * powers[static_cast<size_t>(row)];
            for (int col = 0; col < coefficientCount; ++col)
                normalMatrix[static_cast<size_t>(row)][static_cast<size_t>(col)] +=
                    powers[static_cast<size_t>(row + col)];
        }
    }

    std::vector<double> coefficientsLowToHigh;
    if (!solveLinearSystem(normalMatrix, rhs, &coefficientsLowToHigh))
        return false;

    fitted->assign(indices.size(), 0.0);
    for (size_t i = 0; i < indices.size(); ++i) {
        const double x = (indices[i] - center) / scale;
        (*fitted)[i] = evaluatePolynomialLowToHigh(coefficientsLowToHigh, x);
    }

    if (rawCoefficients)
        *rawCoefficients = rawCoefficientsHighToLow(coefficientsLowToHigh, center, scale);
    return true;
}

QJsonArray fitAttemptsToJson(const std::vector<FitAttempt> &attempts)
{
    QJsonArray array;
    for (const FitAttempt &attempt : attempts) {
        QJsonObject item;
        item.insert(QStringLiteral("degree"), attempt.degree);
        item.insert(QStringLiteral("min_delta"), attempt.minDelta);
        array.append(item);
    }
    return array;
}

PhaseFit fitMonotonicPhase(const std::vector<double> &indices,
                           const std::vector<double> &phase,
                           int degree)
{
    PhaseFit result;
    result.requestedDegree = std::max(1, std::min(degree, static_cast<int>(indices.size()) - 1));

    for (int candidateDegree = result.requestedDegree; candidateDegree >= 1; --candidateDegree) {
        std::vector<double> fitted;
        std::vector<double> coefficients;
        if (!fitPolynomialScaled(indices, phase, candidateDegree, &fitted, &coefficients)) {
            FitAttempt attempt;
            attempt.degree = candidateDegree;
            attempt.minDelta = -std::numeric_limits<double>::infinity();
            result.attempts.push_back(attempt);
            continue;
        }

        if (fitted.back() < fitted.front()) {
            for (double &value : fitted)
                value = -value;
            for (double &value : coefficients)
                value = -value;
        }

        double minDelta = std::numeric_limits<double>::infinity();
        for (size_t i = 1; i < fitted.size(); ++i)
            minDelta = std::min(minDelta, fitted[i] - fitted[i - 1]);

        FitAttempt attempt;
        attempt.degree = candidateDegree;
        attempt.minDelta = minDelta;
        result.attempts.push_back(attempt);

        if (minDelta > 0.0) {
            result.ok = true;
            result.fittedPhase.swap(fitted);
            result.rawCoefficientsHighToLow.swap(coefficients);
            result.selectedDegree = candidateDegree;
            result.autoDowngraded = candidateDegree != result.requestedDegree;
            result.minDelta = minDelta;
            return result;
        }
    }

    const double phaseSlope = phase.empty() ? 0.0 : phase.back() - phase.front();
    result.errorMessage =
        QStringLiteral("拟合相位不是严格递增，即使自动降低多项式阶数后仍失败。有效相位斜率=%1。请确认正/负光程差文件分别是在固定位置重复采集、两组位于零延迟两侧，并且镜面条纹清晰；也可以尝试重新采集或在命令行工具中增加 trim/high-pass 参数。")
            .arg(phaseSlope, 0, 'g', 10);
    return result;
}

bool interpolateResampleIndices(const std::vector<double> &fittedPhase,
                                const std::vector<double> &validIndices,
                                int ascanLen,
                                std::vector<double> *indices)
{
    if (indices == nullptr || fittedPhase.size() != validIndices.size()
        || fittedPhase.size() < 2 || ascanLen <= 1)
        return false;

    indices->assign(static_cast<size_t>(ascanLen), 0.0);
    const double startPhase = fittedPhase.front();
    const double endPhase = fittedPhase.back();
    size_t searchStart = 0;
    for (int i = 0; i < ascanLen; ++i) {
        const double target = startPhase
            + (endPhase - startPhase) * static_cast<double>(i) / static_cast<double>(ascanLen - 1);

        if (target <= fittedPhase.front()) {
            (*indices)[static_cast<size_t>(i)] = validIndices.front();
            continue;
        }
        if (target >= fittedPhase.back()) {
            (*indices)[static_cast<size_t>(i)] = validIndices.back();
            continue;
        }

        while (searchStart + 1 < fittedPhase.size()
               && fittedPhase[searchStart + 1] < target)
            ++searchStart;
        const size_t right = std::min(searchStart + 1, fittedPhase.size() - 1);
        const size_t left = right - 1;
        const double span = fittedPhase[right] - fittedPhase[left];
        if (span <= 0.0)
            return false;
        const double fraction = (target - fittedPhase[left]) / span;
        (*indices)[static_cast<size_t>(i)] =
            validIndices[left] * (1.0 - fraction) + validIndices[right] * fraction;
    }

    return true;
}

QJsonArray doubleVectorToJson(const std::vector<double> &values)
{
    QJsonArray array;
    for (double value : values)
        array.append(value);
    return array;
}

QJsonArray stringListToJson(const QStringList &values)
{
    QJsonArray array;
    for (const QString &value : values)
        array.append(QDir::toNativeSeparators(value));
    return array;
}

void insertIndexDiagnostics(QJsonObject *diagnostics,
                            const std::vector<float> &indices,
                            const QString &outputPath)
{
    if (diagnostics == nullptr || indices.empty())
        return;

    double minValue = std::numeric_limits<double>::infinity();
    double maxValue = -std::numeric_limits<double>::infinity();
    double rms = 0.0;
    double maxAbs = 0.0;
    for (size_t i = 0; i < indices.size(); ++i) {
        const double value = indices[i];
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        const double correction = value - static_cast<double>(i);
        rms += correction * correction;
        maxAbs = std::max(maxAbs, std::fabs(correction));
    }
    rms = std::sqrt(rms / static_cast<double>(indices.size()));

    diagnostics->insert(QStringLiteral("ascan_len"), static_cast<int>(indices.size()));
    diagnostics->insert(QStringLiteral("resample_min"), minValue);
    diagnostics->insert(QStringLiteral("resample_max"), maxValue);
    diagnostics->insert(QStringLiteral("correction_rms_samples"), rms);
    diagnostics->insert(QStringLiteral("correction_max_abs_samples"), maxAbs);
    diagnostics->insert(QStringLiteral("output"), QDir::toNativeSeparators(outputPath));
}

QJsonObject buildGenerateDiagnostics(const KLinearCalibration::GenerateOptions &options,
                                     const AveragedMeasurement &positive,
                                     const AveragedMeasurement &negative,
                                     const AveragedMeasurement *background,
                                     const std::vector<double> &phasePositive,
                                     const std::vector<double> &phaseNegative,
                                     const std::vector<double> &phaseLinear,
                                     const PhaseFit &fit,
                                     const std::vector<float> &indices)
{
    QJsonObject diagnostics;
    diagnostics.insert(QStringLiteral("ascan_len"), positive.ascanLen);
    diagnostics.insert(QStringLiteral("trim_left"), options.trimLeft);
    diagnostics.insert(QStringLiteral("trim_right"), options.trimRight);
    diagnostics.insert(QStringLiteral("swept_source_id"), options.sweptSourceId);
    diagnostics.insert(QStringLiteral("swept_source_name"), options.sweptSourceName);
    diagnostics.insert(QStringLiteral("poly_degree"), fit.selectedDegree);
    diagnostics.insert(QStringLiteral("requested_poly_degree"), fit.requestedDegree);
    diagnostics.insert(QStringLiteral("poly_degree_auto_downgraded"), fit.autoDowngraded);
    diagnostics.insert(QStringLiteral("fit_attempts"), fitAttemptsToJson(fit.attempts));
    diagnostics.insert(QStringLiteral("fit_min_delta"), fit.minDelta);
    diagnostics.insert(QStringLiteral("phase_slope_positive"), phasePositive.back() - phasePositive.front());
    diagnostics.insert(QStringLiteral("phase_slope_negative"), phaseNegative.back() - phaseNegative.front());
    diagnostics.insert(QStringLiteral("phase_slope_linear"), phaseLinear.back() - phaseLinear.front());
    diagnostics.insert(QStringLiteral("poly_coefficients_high_to_low"),
                       doubleVectorToJson(fit.rawCoefficientsHighToLow));
    diagnostics.insert(QStringLiteral("averaging_assumption"),
                       QStringLiteral("positive A-lines are repeated at one fixed mirror position; negative A-lines are repeated at one fixed mirror position"));
    diagnostics.insert(QStringLiteral("positive_files"), stringListToJson(QStringList() << options.positivePath));
    diagnostics.insert(QStringLiteral("negative_files"), stringListToJson(QStringList() << options.negativePath));
    diagnostics.insert(QStringLiteral("positive_background_files"),
                       stringListToJson(background == nullptr ? QStringList() : (QStringList() << options.backgroundPath)));
    diagnostics.insert(QStringLiteral("negative_background_files"),
                       stringListToJson(background == nullptr ? QStringList() : (QStringList() << options.backgroundPath)));
    diagnostics.insert(QStringLiteral("sidecar"), QDir::toNativeSeparators(positive.sidecarPath));
    diagnostics.insert(QStringLiteral("positive_input_file_count"), 1);
    diagnostics.insert(QStringLiteral("negative_input_file_count"), 1);
    diagnostics.insert(QStringLiteral("positive_lines"), positive.lineCount);
    diagnostics.insert(QStringLiteral("negative_lines"), negative.lineCount);
    diagnostics.insert(QStringLiteral("dtype"), positive.dtypeName);
    diagnostics.insert(QStringLiteral("background_dtype"), background == nullptr ? QString() : background->dtypeName);
    diagnostics.insert(QStringLiteral("raw_shift_bits"), options.rawShiftBits);
    diagnostics.insert(QStringLiteral("high_pass_samples"), options.highPassSamples);
    diagnostics.insert(QStringLiteral("keep_dc"), options.keepDc);
    diagnostics.insert(QStringLiteral("generated_by"), QStringLiteral("KLinearCalibration.cpp"));
    insertIndexDiagnostics(&diagnostics, indices, options.outputPath);
    return diagnostics;
}

bool writeResampleIndices(const QString &path,
                          const std::vector<float> &indices,
                          const QJsonObject &diagnostics,
                          QString *errorMessage)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法写入 %1：%2。").arg(path, file.errorString());
        return false;
    }

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    stream << "# k-linearization source indices generated by KLinearCalibration.cpp\n";
    stream << "# ascan_len: " << diagnostics.value(QStringLiteral("ascan_len")).toInt() << "\n";
    stream << "# correction_rms_samples: "
           << QString::number(diagnostics.value(QStringLiteral("correction_rms_samples")).toDouble(), 'g', 9)
           << "\n";
    stream << "# correction_max_abs_samples: "
           << QString::number(diagnostics.value(QStringLiteral("correction_max_abs_samples")).toDouble(), 'g', 9)
           << "\n";
    for (float value : indices)
        stream << QString::number(static_cast<double>(value), 'f', 9) << "\n";

    if (!file.commit()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("保存 %1 失败：%2。").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool writeDiagnostics(const QString &path,
                      const QJsonObject &diagnostics,
                      QString *errorMessage)
{
    if (path.isEmpty())
        return true;

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法写入 %1：%2。").arg(path, file.errorString());
        return false;
    }

    const QJsonDocument document(diagnostics);
    file.write(document.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("保存 %1 失败：%2。").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool parseMapValue(const QStringList &tokens,
                   int expectedTargetIndex,
                   float *value)
{
    if (value == nullptr)
        return false;

    if (tokens.size() >= 2) {
        bool firstOk = false;
        const double firstValue = tokens.at(0).toDouble(&firstOk);
        bool secondOk = false;
        const double secondValue = tokens.at(1).toDouble(&secondOk);
        if (firstOk && secondOk
            && std::fabs(firstValue - static_cast<double>(expectedTargetIndex)) < 0.5) {
            *value = static_cast<float>(secondValue);
            return true;
        }
    }

    for (const QString &token : tokens) {
        bool ok = false;
        const double parsed = token.toDouble(&ok);
        if (ok) {
            *value = static_cast<float>(parsed);
            return true;
        }
    }
    return false;
}

bool readRawResampleIndexFile(const QString &filePath,
                              std::vector<float> *indices,
                              QString *errorMessage)
{
    if (indices == nullptr)
        return false;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法打开 %1：%2。").arg(filePath, file.errorString());
        return false;
    }

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    indices->clear();
    int lineNumber = 0;
    while (!stream.atEnd()) {
        ++lineNumber;
        QString line = stream.readLine().trimmed();
        const int commentIndex = line.indexOf(QLatin1Char('#'));
        if (commentIndex >= 0)
            line = line.left(commentIndex).trimmed();
        if (line.isEmpty())
            continue;

        line.replace(QLatin1Char(','), QLatin1Char(' '));
        line.replace(QLatin1Char(';'), QLatin1Char(' '));
        line.replace(QLatin1Char('\t'), QLatin1Char(' '));
        const QStringList tokens = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);

        float value = 0.0f;
        if (!parseMapValue(tokens, static_cast<int>(indices->size()), &value))
            continue;
        if (!std::isfinite(value) || value < 0.0f) {
            if (errorMessage)
                *errorMessage = QStringLiteral("%1 第 %2 行的源索引 %3 无效。")
                    .arg(filePath)
                    .arg(lineNumber)
                    .arg(value);
            indices->clear();
            return false;
        }
        if (!indices->empty() && value + 1.0e-6f < indices->back()) {
            if (errorMessage)
                *errorMessage = QStringLiteral("%1 第 %2 行的源索引 %3 小于上一项 %4，重采样表应保持单调递增。")
                    .arg(filePath)
                    .arg(lineNumber)
                    .arg(value)
                    .arg(indices->back());
            indices->clear();
            return false;
        }

        indices->push_back(value);
    }

    if (indices->empty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("%1 中没有有效的重采样索引。").arg(filePath);
        return false;
    }

    return true;
}

bool validateResampleIndexRange(const QString &filePath,
                                const std::vector<float> &indices,
                                int ascanLen,
                                QString *errorMessage)
{
    if (ascanLen <= 0)
        return false;

    for (size_t i = 0; i < indices.size(); ++i) {
        const float value = indices[i];
        if (!std::isfinite(value) || value < 0.0f || value > static_cast<float>(ascanLen - 1)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("%1 第 %2 个源索引 %3 超出范围 [0, %4]。")
                    .arg(filePath)
                    .arg(static_cast<qulonglong>(i + 1))
                    .arg(value)
                    .arg(ascanLen - 1);
            }
            return false;
        }
    }
    return true;
}

std::vector<float> rescaleResampleIndices(const std::vector<float> &sourceIndices,
                                          int targetAscanLen)
{
    const int sourceAscanLen = static_cast<int>(sourceIndices.size());
    std::vector<float> scaled(static_cast<size_t>(targetAscanLen), 0.0f);
    if (sourceAscanLen <= 0 || targetAscanLen <= 0)
        return scaled;
    if (sourceAscanLen == 1 || targetAscanLen == 1) {
        std::fill(scaled.begin(), scaled.end(), 0.0f);
        return scaled;
    }

    const double xScale = static_cast<double>(sourceAscanLen - 1)
        / static_cast<double>(targetAscanLen - 1);
    const double yScale = static_cast<double>(targetAscanLen - 1)
        / static_cast<double>(sourceAscanLen - 1);
    for (int i = 0; i < targetAscanLen; ++i) {
        const double sourceX = static_cast<double>(i) * xScale;
        const int left = std::max(0, std::min(sourceAscanLen - 1, static_cast<int>(std::floor(sourceX))));
        const int right = std::min(sourceAscanLen - 1, left + 1);
        const double fraction = sourceX - static_cast<double>(left);
        const double value = static_cast<double>(sourceIndices[static_cast<size_t>(left)]) * (1.0 - fraction)
            + static_cast<double>(sourceIndices[static_cast<size_t>(right)]) * fraction;
        scaled[static_cast<size_t>(i)] = static_cast<float>(value * yScale);
    }
    return scaled;
}

KLinearCalibration::Result failure(const QString &message)
{
    KLinearCalibration::Result result;
    result.ok = false;
    result.errorMessage = message;
    return result;
}

KLinearCalibration::Result lengthMismatchFailure(const QString &message, int sourceAscanLen)
{
    KLinearCalibration::Result result = failure(message);
    result.ascanLenMismatch = true;
    result.sourceAscanLen = sourceAscanLen;
    return result;
}

} // namespace

namespace KLinearCalibration {

Result generateFromMirrorFiles(const GenerateOptions &options)
{
    if (options.expectedAscanLen <= 0)
        return failure(QStringLiteral("当前 AscanLen 无效，无法生成波数线性化表。"));
    if (options.positivePath.isEmpty() || options.negativePath.isEmpty())
        return failure(QStringLiteral("请同时选择正光程差和负光程差 .3d 文件。"));
    if (options.backgroundPath.isEmpty())
        return failure(QStringLiteral("请先选择本底 .3d 文件。"));
    if (options.outputPath.isEmpty())
        return failure(QStringLiteral("输出路径为空。"));
    if (options.sweptSourceId.trimmed().isEmpty())
        return failure(QStringLiteral("当前扫频光源为空，无法生成可绑定的波数线性化标定文件。"));
    if (options.trimLeft < 0 || options.trimRight < 0
        || options.trimLeft + options.trimRight >= options.expectedAscanLen - 2) {
        return failure(QStringLiteral("trim 参数无效，剩余相位点数太少。"));
    }

    QString errorMessage;
    AveragedMeasurement positive;
    if (!loadAveragedMeasurement(options.positivePath,
                                 options.expectedAscanLen,
                                 options.rawShiftBits,
                                 options.maxLines,
                                 &positive,
                                 &errorMessage)) {
        return failure(errorMessage);
    }

    AveragedMeasurement negative;
    if (!loadAveragedMeasurement(options.negativePath,
                                 options.expectedAscanLen,
                                 options.rawShiftBits,
                                 options.maxLines,
                                 &negative,
                                 &errorMessage)) {
        return failure(errorMessage);
    }

    if (positive.ascanLen != negative.ascanLen) {
        return failure(QStringLiteral("正/负光程差文件的 AscanLen 不一致：%1 vs %2。")
                           .arg(positive.ascanLen)
                           .arg(negative.ascanLen));
    }

    AveragedMeasurement background;
    if (!loadAveragedMeasurement(options.backgroundPath,
                                 positive.ascanLen,
                                 options.rawShiftBits,
                                 options.maxLines,
                                 &background,
                                 &errorMessage)) {
        return failure(errorMessage);
    }

    std::vector<double> positiveSpectrum;
    if (!preprocessSpectrum(positive.spectrum,
                            &background.spectrum,
                            options.highPassSamples,
                            options.keepDc,
                            &positiveSpectrum,
                            &errorMessage)) {
        return failure(errorMessage);
    }

    std::vector<double> negativeSpectrum;
    if (!preprocessSpectrum(negative.spectrum,
                            &background.spectrum,
                            options.highPassSamples,
                            options.keepDc,
                            &negativeSpectrum,
                            &errorMessage)) {
        return failure(errorMessage);
    }

    std::vector<double> phasePositive;
    if (!unwrappedPhase(positiveSpectrum, &phasePositive, &errorMessage))
        return failure(errorMessage);

    std::vector<double> phaseNegative;
    if (!unwrappedPhase(negativeSpectrum, &phaseNegative, &errorMessage))
        return failure(errorMessage);

    std::vector<double> phaseLinear(static_cast<size_t>(positive.ascanLen), 0.0);
    for (int i = 0; i < positive.ascanLen; ++i) {
        phaseLinear[static_cast<size_t>(i)] =
            0.5 * (phasePositive[static_cast<size_t>(i)] + phaseNegative[static_cast<size_t>(i)]);
    }
    if (phaseLinear.back() < phaseLinear.front()) {
        for (double &value : phaseLinear)
            value = -value;
    }

    const int validStart = options.trimLeft;
    const int validEnd = positive.ascanLen - options.trimRight;
    std::vector<double> validIndices;
    std::vector<double> validPhase;
    validIndices.reserve(static_cast<size_t>(validEnd - validStart));
    validPhase.reserve(static_cast<size_t>(validEnd - validStart));
    for (int i = validStart; i < validEnd; ++i) {
        validIndices.push_back(static_cast<double>(i));
        validPhase.push_back(phaseLinear[static_cast<size_t>(i)]);
    }

    const PhaseFit fit = fitMonotonicPhase(validIndices, validPhase, options.polyDegree);
    if (!fit.ok)
        return failure(fit.errorMessage);

    std::vector<double> resampleDouble;
    if (!interpolateResampleIndices(fit.fittedPhase,
                                    validIndices,
                                    positive.ascanLen,
                                    &resampleDouble)) {
        return failure(QStringLiteral("相位反插值失败，无法生成重采样索引。"));
    }

    std::vector<float> resampleIndices(static_cast<size_t>(positive.ascanLen), 0.0f);
    for (int i = 0; i < positive.ascanLen; ++i)
        resampleIndices[static_cast<size_t>(i)] = static_cast<float>(resampleDouble[static_cast<size_t>(i)]);

    QJsonObject diagnostics = buildGenerateDiagnostics(options,
                                                       positive,
                                                       negative,
                                                       &background,
                                                       phasePositive,
                                                       phaseNegative,
                                                       phaseLinear,
                                                       fit,
                                                       resampleIndices);

    if (!writeResampleIndices(options.outputPath, resampleIndices, diagnostics, &errorMessage))
        return failure(errorMessage);
    if (!writeDiagnostics(options.diagnosticsPath, diagnostics, &errorMessage))
        return failure(errorMessage);

    Result result;
    result.ok = true;
    result.outputPath = options.outputPath;
    result.diagnosticsPath = options.diagnosticsPath;
    result.ascanLen = positive.ascanLen;
    result.lineCountPositive = positive.lineCount;
    result.lineCountNegative = negative.lineCount;
    result.polyDegree = fit.selectedDegree;
    result.requestedPolyDegree = fit.requestedDegree;
    result.polyDegreeAutoDowngraded = fit.autoDowngraded;
    result.resampleIndices.swap(resampleIndices);
    result.diagnostics = diagnostics;
    result.correctionRmsSamples = diagnostics.value(QStringLiteral("correction_rms_samples")).toDouble();
    result.correctionMaxAbsSamples = diagnostics.value(QStringLiteral("correction_max_abs_samples")).toDouble();
    return result;
}

Result importFromIndexFile(const ImportOptions &options)
{
    if (options.expectedAscanLen <= 0)
        return failure(QStringLiteral("当前 AscanLen 无效，无法检查重采样表。"));
    if (options.inputPath.isEmpty())
        return failure(QStringLiteral("请选择一个 .txt 重采样表。"));
    if (options.outputPath.isEmpty())
        return failure(QStringLiteral("输出路径为空。"));
    if (options.sweptSourceId.trimmed().isEmpty())
        return failure(QStringLiteral("当前扫频光源为空，无法导入可绑定的波数线性化标定文件。"));

    QString errorMessage;
    std::vector<float> indices;
    if (!readRawResampleIndexFile(options.inputPath, &indices, &errorMessage))
        return failure(errorMessage);

    const int sourceAscanLen = static_cast<int>(indices.size());
    if (!validateResampleIndexRange(options.inputPath, indices, sourceAscanLen, &errorMessage))
        return failure(errorMessage);

    QJsonObject diagnostics;
    if (!options.inputDiagnosticsPath.isEmpty()) {
        if (!readJsonObject(options.inputDiagnosticsPath, &diagnostics, &errorMessage))
            return failure(errorMessage);
        if (diagnostics.contains(QStringLiteral("ascan_len"))
            && diagnostics.value(QStringLiteral("ascan_len")).toInt() != sourceAscanLen) {
            return failure(QStringLiteral("%1 中的 ascan_len=%2，但所选重采样表包含 %3 个索引。")
                               .arg(options.inputDiagnosticsPath)
                               .arg(diagnostics.value(QStringLiteral("ascan_len")).toInt())
                               .arg(sourceAscanLen));
        }
        const QString diagnosticsSourceId =
            diagnostics.value(QStringLiteral("swept_source_id")).toString().trimmed();
        if (!diagnosticsSourceId.isEmpty()
            && !options.sweptSourceId.isEmpty()
            && diagnosticsSourceId.compare(options.sweptSourceId, Qt::CaseInsensitive) != 0) {
            return failure(QStringLiteral("%1 对应的激光器为 %2，但当前激光器为 %3。")
                               .arg(options.inputDiagnosticsPath,
                                    diagnosticsSourceId,
                                    options.sweptSourceId));
        }
    }

    if (sourceAscanLen != options.expectedAscanLen) {
        if (!options.rescaleToExpectedAscanLen) {
            return lengthMismatchFailure(QStringLiteral("%1 中有 %2 个重采样索引，但当前 AscanLen=%3。")
                                             .arg(options.inputPath)
                                             .arg(sourceAscanLen)
                                             .arg(options.expectedAscanLen),
                                         sourceAscanLen);
        }
        indices = rescaleResampleIndices(indices, options.expectedAscanLen);
        if (!validateResampleIndexRange(options.inputPath, indices, options.expectedAscanLen, &errorMessage))
            return failure(errorMessage);
    }

    diagnostics.insert(QStringLiteral("imported_from"), QDir::toNativeSeparators(options.inputPath));
    if (!options.inputDiagnosticsPath.isEmpty())
        diagnostics.insert(QStringLiteral("source_diagnostics"), QDir::toNativeSeparators(options.inputDiagnosticsPath));
    diagnostics.insert(QStringLiteral("swept_source_id"), options.sweptSourceId);
    diagnostics.insert(QStringLiteral("swept_source_name"), options.sweptSourceName);
    diagnostics.insert(QStringLiteral("import_source_ascan_len"), sourceAscanLen);
    diagnostics.insert(QStringLiteral("import_target_ascan_len"), options.expectedAscanLen);
    diagnostics.insert(QStringLiteral("rescaled_to_current_ascan_len"), sourceAscanLen != options.expectedAscanLen);
    if (sourceAscanLen != options.expectedAscanLen) {
        diagnostics.insert(QStringLiteral("rescale_x_factor"),
                           sourceAscanLen > 1 && options.expectedAscanLen > 1
                               ? static_cast<double>(sourceAscanLen - 1) / static_cast<double>(options.expectedAscanLen - 1)
                               : 1.0);
        diagnostics.insert(QStringLiteral("rescale_y_factor"),
                           sourceAscanLen > 1 && options.expectedAscanLen > 1
                               ? static_cast<double>(options.expectedAscanLen - 1) / static_cast<double>(sourceAscanLen - 1)
                               : 1.0);
    }
    diagnostics.insert(QStringLiteral("diagnostics_filled_by"), QStringLiteral("KLinearCalibration.cpp"));
    insertIndexDiagnostics(&diagnostics, indices, options.outputPath);

    if (!writeResampleIndices(options.outputPath, indices, diagnostics, &errorMessage))
        return failure(errorMessage);
    if (!writeDiagnostics(options.diagnosticsPath, diagnostics, &errorMessage))
        return failure(errorMessage);

    Result result;
    result.ok = true;
    result.outputPath = options.outputPath;
    result.diagnosticsPath = options.diagnosticsPath;
    result.ascanLen = options.expectedAscanLen;
    result.sourceAscanLen = sourceAscanLen;
    result.rescaled = sourceAscanLen != options.expectedAscanLen;
    result.resampleIndices.swap(indices);
    result.diagnostics = diagnostics;
    result.correctionRmsSamples = diagnostics.value(QStringLiteral("correction_rms_samples")).toDouble();
    result.correctionMaxAbsSamples = diagnostics.value(QStringLiteral("correction_max_abs_samples")).toDouble();
    result.polyDegree = diagnostics.value(QStringLiteral("poly_degree")).toInt();
    result.requestedPolyDegree = diagnostics.value(QStringLiteral("requested_poly_degree")).toInt();
    result.polyDegreeAutoDowngraded = diagnostics.value(QStringLiteral("poly_degree_auto_downgraded")).toBool();
    return result;
}

} // namespace KLinearCalibration
