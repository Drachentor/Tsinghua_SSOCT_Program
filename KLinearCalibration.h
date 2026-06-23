#ifndef KLINEARCALIBRATION_H
#define KLINEARCALIBRATION_H

#include <QJsonObject>
#include <QString>
#include <vector>

namespace KLinearCalibration {

struct GenerateOptions
{
    QString positivePath;
    QString negativePath;
    QString backgroundPath;
    QString outputPath;
    QString diagnosticsPath;
    QString sweptSourceId;
    QString sweptSourceName;
    int expectedAscanLen = 0;
    int trimLeft = 5;
    int trimRight = 5;
    int polyDegree = 5;
    int highPassSamples = 0;
    int rawShiftBits = 4;
    int maxLines = 0;
    bool keepDc = false;
};

struct ImportOptions
{
    QString inputPath;
    QString inputDiagnosticsPath;
    QString outputPath;
    QString diagnosticsPath;
    QString sweptSourceId;
    QString sweptSourceName;
    int expectedAscanLen = 0;
    bool rescaleToExpectedAscanLen = false;
};

struct Result
{
    bool ok = false;
    QString errorMessage;
    QString outputPath;
    QString diagnosticsPath;
    int ascanLen = 0;
    int sourceAscanLen = 0;
    bool ascanLenMismatch = false;
    bool rescaled = false;
    int lineCountPositive = 0;
    int lineCountNegative = 0;
    int polyDegree = 0;
    int requestedPolyDegree = 0;
    bool polyDegreeAutoDowngraded = false;
    double correctionRmsSamples = 0.0;
    double correctionMaxAbsSamples = 0.0;
    std::vector<float> resampleIndices;
    QJsonObject diagnostics;
};

Result generateFromMirrorFiles(const GenerateOptions &options);
Result importFromIndexFile(const ImportOptions &options);

} // namespace KLinearCalibration

#endif // KLINEARCALIBRATION_H
