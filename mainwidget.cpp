#include "mainwidget.h"
#include "ui_mainwidget.h"
#include "qcolor.h"
#include "MainWidgetUISetup.h"
#include "VesselAudioPlayer.h"
#include "VesselFindingDialog.h"
#include "VesselProjectionProcessor.h"
#include "AppVersion.h"
#include "DeviceSettings.h"
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextEdit>
#include <QTextStream>
#include <opencv2/core/ocl.hpp>
#include <math.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>

#ifndef and
#define and &&
#endif
#ifndef or
#define or ||
#endif
// 专门为了减少代码的字符数量而定义的宏
#ifndef m_AscanLen
#define m_AscanLen mainWidget::AscanLen
#endif
#ifndef m_BscanLen
#define m_BscanLen mainWidget::BscanLen
#endif
#ifndef m_CscanLen
#define m_CscanLen mainWidget::CscanLen
#endif
#ifndef m_AngioRep
#define m_AngioRep mainWidget::AngioRep
#endif

// int mainWidget::AscanLen = 400;     // 这个值在这里初始化, 之后不变! (至少现在如此, 之后或许会换)
int mainWidget::AscanLen = 640;         // 调试用
int mainWidget::BscanLen = 800;
int mainWidget::CscanLen = 800;
int mainWidget::AngioRep = mainWidget::DefaultAngioRep;
int mainWidget::TriggerOffsetSamples = 0;
bool mainWidget::ContinuousModeEnabled = false;
int mainWidget::ContinuousAlineCount = 640000;
const int ZeroBuffer = 50; // 这个值在这里初始化, 之后不变! 这个是 DA 卡采集时, 零点缓冲的点数

int mainWidget::captureflag = 0;// 0表示该状态不进行采集
int mainWidget::scanMode = 1;
mainWidget::ADTriggerMode mainWidget::triggerMode = mainWidget::ADTriggerMode::Internal;
mainWidget::ADClockMode mainWidget::clockMode = mainWidget::ADClockMode::Internal;
bool mainWidget::BGReductionFlag = true;
bool mainWidget::Wflag = true;//初始为不加窗

const double mainWidget::FFT_SCALE = 20.0;


using namespace cv;

namespace {
const int kReadAdCountUnit = 2048;
const int kSymphonicScanMode = 43;

QString settingsFilePath()
{
    return DeviceSettings::settingsFilePath();
}

QString logFilePath(const QString &fileName)
{
    return QDir(QApplication::applicationDirPath()).filePath(fileName);
}

void truncateLogFile(const QString &fileName)
{
    QFile file(logFilePath(fileName));
    file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
}

void appendLogLines(const QString &fileName, const QString &level, const QString &text)
{
    QString normalizedText = text;
    normalizedText.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalizedText.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    const QStringList lines = normalizedText.split(QLatin1Char('\n'));
    QFile file(logFilePath(fileName));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    for (const QString &line : lines) {
        if (line.isEmpty())
            continue;

        stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
               << QStringLiteral(" [") << level << QStringLiteral("] ")
               << line << QLatin1Char('\n');
    }
}

QString defaultDialogPath()
{
    return QFileInfo(QString::fromLocal8Bit(__FILE__)).absolutePath();
}

QString vesselScanWavPath()
{
    return defaultDialogPath() + QStringLiteral("/vessel_scan_path.wav");
}

struct VesselWavInfo
{
    int sampleRate = 0;
    int channelCount = 0;
    int bitsPerSample = 0;
    int blockAlign = 0;
    int frameCount = 0;
};

struct DaPathInfo
{
    int pointCount = 0;
};

quint16 readU16LeAt(const QByteArray &data, int offset)
{
    return static_cast<quint16>(static_cast<unsigned char>(data[offset]))
        | (static_cast<quint16>(static_cast<unsigned char>(data[offset + 1])) << 8);
}

quint32 readU32LeAt(const QByteArray &data, int offset)
{
    return static_cast<quint32>(static_cast<unsigned char>(data[offset]))
        | (static_cast<quint32>(static_cast<unsigned char>(data[offset + 1])) << 8)
        | (static_cast<quint32>(static_cast<unsigned char>(data[offset + 2])) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(data[offset + 3])) << 24);
}

bool readVesselWavInfo(const QString &wavPath, VesselWavInfo *info, QString *errorMessage)
{
    QFile file(wavPath);
    if (!file.open(QFile::ReadOnly)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法打开音频文件：%1。").arg(wavPath);
        return false;
    }

    const QByteArray wavData = file.readAll();
    if (wavData.size() < 44 || wavData.mid(0, 4) != "RIFF" || wavData.mid(8, 4) != "WAVE") {
        if (errorMessage)
            *errorMessage = QStringLiteral("不是有效的 RIFF/WAVE 文件：%1。").arg(wavPath);
        return false;
    }

    bool foundFormat = false;
    bool foundData = false;
    quint16 audioFormat = 0;
    quint32 dataSize = 0;
    VesselWavInfo parsedInfo;

    int offset = 12;
    while (offset + 8 <= wavData.size()) {
        const QByteArray chunkId = wavData.mid(offset, 4);
        const quint32 chunkSize = readU32LeAt(wavData, offset + 4);
        const int chunkDataOffset = offset + 8;
        if (chunkDataOffset + static_cast<int>(chunkSize) > wavData.size()) {
            if (errorMessage)
                *errorMessage = QStringLiteral("WAV chunk 长度无效：%1。").arg(wavPath);
            return false;
        }

        if (chunkId == "fmt ") {
            if (chunkSize < 16) {
                if (errorMessage)
                    *errorMessage = QStringLiteral("WAV fmt chunk 过短：%1。").arg(wavPath);
                return false;
            }
            audioFormat = readU16LeAt(wavData, chunkDataOffset);
            parsedInfo.channelCount = readU16LeAt(wavData, chunkDataOffset + 2);
            parsedInfo.sampleRate = static_cast<int>(readU32LeAt(wavData, chunkDataOffset + 4));
            parsedInfo.blockAlign = readU16LeAt(wavData, chunkDataOffset + 12);
            parsedInfo.bitsPerSample = readU16LeAt(wavData, chunkDataOffset + 14);
            foundFormat = true;
        } else if (chunkId == "data") {
            dataSize = chunkSize;
            foundData = true;
        }

        offset = chunkDataOffset + static_cast<int>(chunkSize);
        if (offset % 2 != 0)
            ++offset;
    }

    if (!foundFormat || !foundData || parsedInfo.blockAlign <= 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("WAV 文件缺少有效的 fmt/data chunk：%1。").arg(wavPath);
        return false;
    }
    if (audioFormat != 1 || parsedInfo.sampleRate != 48000
        || parsedInfo.channelCount != 2 || parsedInfo.bitsPerSample != 24
        || parsedInfo.blockAlign != 6) {
        if (errorMessage)
            *errorMessage = QStringLiteral("WAV 格式必须为 PCM 24-bit, 48000 Hz, 双声道。");
        return false;
    }
    if (dataSize == 0 || dataSize % static_cast<quint32>(parsedInfo.blockAlign) != 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("WAV data chunk 为空或不是完整双声道帧。");
        return false;
    }

    parsedInfo.frameCount = static_cast<int>(dataSize / static_cast<quint32>(parsedInfo.blockAlign));
    if (info)
        *info = parsedInfo;
    return true;
}

bool readDaPathInfo(const QString &filePath, DaPathInfo *info, QString *errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法打开路径文件：%1。").arg(filePath);
        return false;
    }

    QTextStream stream(&file);
    int pointCount = 0;
    int lineNumber = 0;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        ++lineNumber;
        if (line.isEmpty())
            continue;

        bool ok = false;
        const int value = line.toInt(&ok);
        if (!ok || value < 0 || value > 65535) {
            if (errorMessage)
                *errorMessage = QStringLiteral("%1 第 %2 行不是有效的 DA 值。").arg(filePath).arg(lineNumber);
            return false;
        }
        ++pointCount;
    }

    if (pointCount <= 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("路径文件为空：%1。").arg(filePath);
        return false;
    }

    if (info)
        info->pointCount = pointCount;
    return true;
}

bool vesselPathNoReturnToZeroFromSettings()
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("VesselFindingDialog"));
    const bool noReturn = settings.value(QStringLiteral("noReturn"), false).toBool();
    settings.endGroup();
    return noReturn;
}

QString savedPathValue(const QString &key)
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("paths/") + key).toString();
}

QString dialogPathFromSavedPath(const QString &filePath)
{
    if (!filePath.isEmpty()) {
        const QFileInfo fileInfo(filePath);
        if (fileInfo.exists()) {
            return fileInfo.absoluteFilePath();
        }
        if (fileInfo.absoluteDir().exists()) {
            return fileInfo.absolutePath();
        }
    }
    return defaultDialogPath();
}

void savePathValue(const QString &key, const QString &filePath)
{
    if (filePath.isEmpty()) {
        return;
    }

    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("paths/") + key, filePath);
    settings.sync();
}

void saveVesselImagePath(const QString &filePath)
{
    savePathValue(QStringLiteral("vesselImagePath"), filePath);
}

void saveAngio3dPath(const QString &filePath)
{
    savePathValue(QStringLiteral("angio3dPath"), filePath);
}

QString angio3dDialogPath()
{
    return dialogPathFromSavedPath(savedPathValue(QStringLiteral("angio3dPath")));
}

struct AngioFileSelection
{
    QString filePath;
    bool generatePreviewImage = false;
    bool generateGrayscaleImage = false;
    bool generatePixelWiseFlowSpeedImage = true;
    bool generateAveragedFlowSpeedImage = true;
    bool generateSegmentWiseFlowSpeedImage = true;
    bool generateFlowSpeedFitCorrelationImage = false;
    bool useFlowSpeedSkeletonDenoise = false;
    bool useFlowSpeedManualMask = false;
    int flowSpeedCropTop = 0;
    int flowSpeedCropBottom = 0;
    int flowSpeedCropLeft = 0;
    int flowSpeedCropRight = 0;
};

struct FftFileSelection
{
    QString filePath;
    bool applyWindow = true;
    bool removeBackground = true;
    bool applyLogScale = true;
};

struct FourierSourceMetadata
{
    bool hasSidecar = false;
    QString scanModeText;
    int scanMode = 0;
    int ascanLen = 0;
    int bscanLen = 0;
    int cscanLen = 0;
    int angioRep = mainWidget::AngioRep;
    qint64 storedAlineCount = 0;
    int storedBscanCount = 0;
    QString sampleType;
    bool preprocessedSpectrum = false;
    double dispersionW0 = 0.0;
    double dispersionA1 = 0.0;
    double dispersionA2 = 0.0;
};

const char *kSampleTypeUint16Raw = "uint16_raw";
const char *kSampleTypeFloat32Spectrum = "float32_spectrum";

struct PeriodicityWindowResult
{
    QString label;
    bool valid = false;
    int sampleCount = 0;
    int bestPeriodSamples = 0;
    double peakCorrelation = 0.0;
    double adjacentMeanCorrelation = 0.0;
    double adjacentStdCorrelation = 0.0;
    double adjacentMinCorrelation = 0.0;
    int adjacentPairCount = 0;
};

const char *kLightPathDialogStyleSheet =
    "QDialog { background-color: white; color: black; }"
    "QLabel { background-color: white; color: black; }"
    "QCheckBox { background-color: white; color: black; }"
    "QLineEdit { background-color: white; color: black; }"
    "QSpinBox { background-color: white; color: black; }"
    "QPushButton { background-color: white; color: black; }"
    "QPushButton:hover { background-color: rgb(235,235,235); }";

QString cleanPastedPath(QString filePath)
{
    filePath = filePath.trimmed();
    if (filePath.size() >= 2 && filePath.startsWith('"') && filePath.endsWith('"')) {
        filePath = filePath.mid(1, filePath.size() - 2).trimmed();
    }
    return QDir::fromNativeSeparators(filePath);
}

QString dialogStartPathFromText(const QString &filePath)
{
    const QString cleanedPath = cleanPastedPath(filePath);
    if (!cleanedPath.isEmpty()) {
        const QFileInfo fileInfo(cleanedPath);
        if (fileInfo.exists()) {
            return fileInfo.absoluteFilePath();
        }
        if (fileInfo.absoluteDir().exists()) {
            return fileInfo.absolutePath();
        }
    }
    return angio3dDialogPath();
}

bool selectAngioFileAndOptions(QWidget *parent, AngioFileSelection *selection)
{
    if (selection == nullptr) {
        return false;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("选择 3D Angio 文件"));
    dialog.setStyleSheet(kLightPathDialogStyleSheet);

    QGridLayout *layout = new QGridLayout(&dialog);
    QLabel *pathLabel = new QLabel(QStringLiteral("文件路径"), &dialog);
    QLineEdit *pathEdit = new QLineEdit(&dialog);
    QPushButton *browseButton = new QPushButton(QStringLiteral("浏览..."), &dialog);
    QCheckBox *previewCheckBox = new QCheckBox(QStringLiteral("生成多帧平均预览图"), &dialog);
    QCheckBox *grayscaleCheckBox = new QCheckBox(QStringLiteral("生成血管灰度图"), &dialog);
    QCheckBox *flowSpeedPixelWiseCheckBox =
        new QCheckBox(QStringLiteral("生成 pixel-wise 血流速度图（按像素）"), &dialog);
    QCheckBox *flowSpeedAveragedCheckBox =
        new QCheckBox(QStringLiteral("生成快速版血流速度图（血管段内平均）"), &dialog);
    QCheckBox *flowSpeedSegmentWiseCheckBox =
        new QCheckBox(QStringLiteral("生成 segment-wise 血流速度图（按血管分段拟合，需要 Angio 重复数较高）"), &dialog);
    QCheckBox *flowSpeedFitCorrelationCheckBox =
        new QCheckBox(QStringLiteral("生成 segment-wise 拟合相关系数图（调试）"), &dialog);
    QCheckBox *flowSpeedSkeletonDenoiseCheckBox =
        new QCheckBox(QStringLiteral("使用自动血管骨架抑制速度图噪点"), &dialog);
    QCheckBox *flowSpeedManualMaskCheckBox =
        new QCheckBox(QStringLiteral("打开手动速度图掩膜/裁剪窗口"), &dialog);
    QLabel *cropLabel = new QLabel(QStringLiteral("速度图裁剪像素"), &dialog);
    QLabel *cropTopLabel = new QLabel(QStringLiteral("上"), &dialog);
    QLabel *cropBottomLabel = new QLabel(QStringLiteral("下"), &dialog);
    QLabel *cropLeftLabel = new QLabel(QStringLiteral("左"), &dialog);
    QLabel *cropRightLabel = new QLabel(QStringLiteral("右"), &dialog);
    QSpinBox *cropTopSpinBox = new QSpinBox(&dialog);
    QSpinBox *cropBottomSpinBox = new QSpinBox(&dialog);
    QSpinBox *cropLeftSpinBox = new QSpinBox(&dialog);
    QSpinBox *cropRightSpinBox = new QSpinBox(&dialog);
    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                                     &dialog);

    flowSpeedPixelWiseCheckBox->setChecked(true);
    flowSpeedAveragedCheckBox->setChecked(true);
    flowSpeedSegmentWiseCheckBox->setChecked(true);
    flowSpeedFitCorrelationCheckBox->setChecked(false);
    flowSpeedSkeletonDenoiseCheckBox->setChecked(false);
    flowSpeedManualMaskCheckBox->setChecked(false);
    for (QSpinBox *spinBox : {cropTopSpinBox, cropBottomSpinBox, cropLeftSpinBox, cropRightSpinBox}) {
        spinBox->setRange(0, 100000);
        spinBox->setValue(0);
        spinBox->setMaximumWidth(80);
    }
    const QString savedPath = savedPathValue(QStringLiteral("angio3dPath"));
    pathEdit->setText(QDir::toNativeSeparators(savedPath.isEmpty() ? angio3dDialogPath() : savedPath));
    pathEdit->setMinimumWidth(520);

    layout->addWidget(pathLabel, 0, 0);
    layout->addWidget(pathEdit, 0, 1);
    layout->addWidget(browseButton, 0, 2);
    layout->addWidget(previewCheckBox, 1, 1, 1, 2);
    layout->addWidget(grayscaleCheckBox, 2, 1, 1, 2);
    layout->addWidget(flowSpeedPixelWiseCheckBox, 3, 1, 1, 2);
    layout->addWidget(flowSpeedAveragedCheckBox, 4, 1, 1, 2);
    layout->addWidget(flowSpeedSegmentWiseCheckBox, 5, 1, 1, 2);
    layout->addWidget(flowSpeedFitCorrelationCheckBox, 6, 1, 1, 2);
    layout->addWidget(flowSpeedSkeletonDenoiseCheckBox, 7, 1, 1, 2);
    layout->addWidget(flowSpeedManualMaskCheckBox, 8, 1, 1, 2);
    layout->addWidget(cropLabel, 9, 0);
    layout->addWidget(cropTopLabel, 9, 1);
    layout->addWidget(cropTopSpinBox, 9, 2);
    layout->addWidget(cropBottomLabel, 10, 1);
    layout->addWidget(cropBottomSpinBox, 10, 2);
    layout->addWidget(cropLeftLabel, 11, 1);
    layout->addWidget(cropLeftSpinBox, 11, 2);
    layout->addWidget(cropRightLabel, 12, 1);
    layout->addWidget(cropRightSpinBox, 12, 2);
    layout->addWidget(buttons, 13, 0, 1, 3);

    QObject::connect(browseButton, &QPushButton::clicked, &dialog, [&]() {
        const QString selectedPath = QFileDialog::getOpenFileName(
            &dialog,
            QStringLiteral("选择 3D Angio 文件"),
            dialogStartPathFromText(pathEdit->text()),
            QStringLiteral("3D 文件 (*.3d);;所有文件 (*.*)"));
        if (!selectedPath.isEmpty()) {
            pathEdit->setText(QDir::toNativeSeparators(selectedPath));
        }
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    while (dialog.exec() == QDialog::Accepted) {
        const QString filePath = cleanPastedPath(pathEdit->text());
        const QFileInfo fileInfo(filePath);
        if (!filePath.isEmpty() && fileInfo.exists() && fileInfo.isFile()) {
            selection->filePath = fileInfo.absoluteFilePath();
            selection->generatePreviewImage = previewCheckBox->isChecked();
            selection->generateGrayscaleImage = grayscaleCheckBox->isChecked();
            selection->generatePixelWiseFlowSpeedImage = flowSpeedPixelWiseCheckBox->isChecked();
            selection->generateAveragedFlowSpeedImage = flowSpeedAveragedCheckBox->isChecked();
            selection->generateSegmentWiseFlowSpeedImage = flowSpeedSegmentWiseCheckBox->isChecked();
            selection->generateFlowSpeedFitCorrelationImage = flowSpeedFitCorrelationCheckBox->isChecked();
            selection->useFlowSpeedSkeletonDenoise = flowSpeedSkeletonDenoiseCheckBox->isChecked();
            selection->useFlowSpeedManualMask = flowSpeedManualMaskCheckBox->isChecked();
            selection->flowSpeedCropTop = cropTopSpinBox->value();
            selection->flowSpeedCropBottom = cropBottomSpinBox->value();
            selection->flowSpeedCropLeft = cropLeftSpinBox->value();
            selection->flowSpeedCropRight = cropRightSpinBox->value();
            return true;
        }
        QMessageBox::warning(&dialog,
                             QStringLiteral("文件无效"),
                             QStringLiteral("请选择存在的 3D 文件。"));
    }

    return false;
}

void saveFftSourcePath(const QString &filePath)
{
    savePathValue(QStringLiteral("fftSourcePath"), filePath);
}

QString fftDialogPath()
{
    return dialogPathFromSavedPath(savedPathValue(QStringLiteral("fftSourcePath")));
}

QString sidecarPathForDataFile(const QString &filePath)
{
    const QFileInfo fileInfo(filePath);
    return fileInfo.absolutePath() + QStringLiteral("/") + fileInfo.completeBaseName() + QStringLiteral(".json");
}

QString fftBinPathForDataFile(const QString &filePath)
{
    const QFileInfo fileInfo(filePath);
    return fileInfo.absolutePath() + QStringLiteral("/") + fileInfo.completeBaseName() + QStringLiteral("_fft.bin");
}

QString fftJsonPathForDataFile(const QString &filePath)
{
    const QFileInfo fileInfo(filePath);
    return fileInfo.absolutePath() + QStringLiteral("/") + fileInfo.completeBaseName() + QStringLiteral("_fft.json");
}

bool isOctDataFile(const QString &filePath)
{
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    return suffix == QStringLiteral("2d") || suffix == QStringLiteral("3d");
}

QString scanModeTextForMode(int mode)
{
    switch (mode) {
    case 1: return QStringLiteral("1D scan");
    case 2: return QStringLiteral("2D repeat");
    case 10: return QStringLiteral("2D cross scan");
    case 22: return QStringLiteral("2D angio");
    case 3: return QStringLiteral("3D scan");
    case 32: return QStringLiteral("3D angio");
    case 42: return QStringLiteral("Vessel scan");
    case kSymphonicScanMode: return QStringLiteral("Symphonic");
    default: return QStringLiteral("Unknown");
    }
}

int scanModeFromText(const QString &text, int fallback)
{
    if (text == QStringLiteral("1D scan"))
        return 1;
    if (text == QStringLiteral("2D repeat"))
        return 2;
    if (text == QStringLiteral("2D cross scan"))
        return 10;
    if (text == QStringLiteral("2D angio"))
        return 22;
    if (text == QStringLiteral("3D scan"))
        return 3;
    if (text == QStringLiteral("3D angio"))
        return 32;
    if (text == QStringLiteral("Vessel scan"))
        return 42;
    if (text == QStringLiteral("Symphonic"))
        return kSymphonicScanMode;
    return fallback;
}

qint64 storedAlineCountForMode(int mode, int bscanLen, int cscanLen, int angioRep)
{
    if (bscanLen <= 0)
        return 0;
    switch (mode) {
    case 1:
        return 1;
    case 2:
        return bscanLen;
    case 10:
        return static_cast<qint64>(bscanLen) * 2;
    case 22:
        return static_cast<qint64>(bscanLen) * angioRep;
    case 3:
        return static_cast<qint64>(bscanLen) * cscanLen;
    case 32:
        return static_cast<qint64>(bscanLen) * cscanLen * angioRep;
    case kSymphonicScanMode:
        return bscanLen;
    default:
        return 0;
    }
}

QJsonObject settingsGroupToJson(QSettings &settings)
{
    QJsonObject object;
    const QStringList keys = settings.childKeys();
    for (const QString &key : keys)
        object.insert(key, QJsonValue::fromVariant(settings.value(key)));

    const QStringList groups = settings.childGroups();
    for (const QString &group : groups) {
        settings.beginGroup(group);
        object.insert(group, settingsGroupToJson(settings));
        settings.endGroup();
    }
    return object;
}

QJsonObject settingsFileToJson()
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    return settingsGroupToJson(settings);
}

QString settingsIniText()
{
    QFile settingsFile(settingsFilePath());
    if (!settingsFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(settingsFile.readAll());
}

QString settingsKeyPath(const QString &group, const QString &key)
{
    return group + QLatin1Char('/') + key;
}

QVariant groupedSettingValue(QSettings &settings,
                             const QString &group,
                             const QString &key,
                             const QVariant &fallback)
{
    const QString legacyPath = settingsKeyPath(DeviceSettings::legacyMainWidgetGroup(), key);
    const QVariant legacyValue = settings.value(legacyPath, fallback);
    return settings.value(settingsKeyPath(group, key), legacyValue);
}

void setGroupedAndLegacyValue(QSettings &settings,
                              const QString &group,
                              const QString &key,
                              const QVariant &value)
{
    settings.setValue(settingsKeyPath(group, key), value);
    settings.setValue(settingsKeyPath(DeviceSettings::legacyMainWidgetGroup(), key), value);
}

QStringList dacSettingKeys()
{
    return {
        QStringLiteral("amplitude"),
        QStringLiteral("frameRate"),
        QStringLiteral("AscanFreq"),
        QStringLiteral("AscanDutyCycle"),
        QStringLiteral("BscanCycleLen"),
        QStringLiteral("dutycycle"),
        QStringLiteral("enableDAInSymphonic"),
        QStringLiteral("fastAxis")
    };
}

QStringList adcSettingKeys()
{
    return {
        QStringLiteral("AscanLen"),
        QStringLiteral("SampleRate"),
        QStringLiteral("triggerOffsetSamples"),
        QStringLiteral("adFileOffsetFrames"),
        QStringLiteral("continuousModeEnabled"),
        QStringLiteral("continuousAlineCount"),
        QStringLiteral("triggerMode"),
        QStringLiteral("clockMode")
    };
}

QStringList commonSettingKeys()
{
    return {
        QStringLiteral("AngioRep"),
        QStringLiteral("BscanLength"),
        QStringLiteral("Bscanlines"),
        QStringLiteral("show3DRealtimeData"),
        QStringLiteral("realtimeShowInterval"),
        QStringLiteral("fourierOnSaved"),
        QStringLiteral("windowApplied"),
        QStringLiteral("scanMode"),
        QStringLiteral("convertMin"),
        QStringLiteral("convertMax"),
        QStringLiteral("projectionDepth"),
        QStringLiteral("fftRangeStart"),
        QStringLiteral("fftRangeEnd"),
        QStringLiteral("displayMin"),
        QStringLiteral("displayMax"),
        QStringLiteral("filterX"),
        QStringLiteral("filterY"),
        QStringLiteral("dispersionW0"),
        QStringLiteral("dispersionA1"),
        QStringLiteral("dispersionA2")
    };
}

void seedGroupFromLegacy(QSettings &settings, const QString &group, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QString groupedPath = settingsKeyPath(group, key);
        const QString legacyPath = settingsKeyPath(DeviceSettings::legacyMainWidgetGroup(), key);
        if (!settings.contains(groupedPath) && settings.contains(legacyPath))
            settings.setValue(groupedPath, settings.value(legacyPath));
    }
}

void seedCategorizedSettingsFromLegacy(QSettings &settings)
{
    for (const DeviceSettings::DeviceOption &device : DeviceSettings::supportedDacDevices())
        seedGroupFromLegacy(settings, DeviceSettings::dacSettingsGroup(device.id), dacSettingKeys());
    for (const DeviceSettings::DeviceOption &device : DeviceSettings::supportedAdcDevices())
        seedGroupFromLegacy(settings, DeviceSettings::adcSettingsGroup(device.id), adcSettingKeys());
    seedGroupFromLegacy(settings, DeviceSettings::commonSettingsGroup(), commonSettingKeys());
}

bool writeJsonDocument(const QString &filePath, const QJsonObject &root, QString *errorMessage)
{
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage)
            *errorMessage = file.errorString();
        return false;
    }

    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) {
        if (errorMessage)
            *errorMessage = file.errorString();
        return false;
    }

    if (!file.commit()) {
        if (errorMessage)
            *errorMessage = file.errorString();
        return false;
    }
    return true;
}

bool selectFftFileAndOptions(QWidget *parent, FftFileSelection *selection)
{
    if (selection == nullptr)
        return false;

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("选择 OCT 数据文件"));
    dialog.setStyleSheet(kLightPathDialogStyleSheet);

    QGridLayout *layout = new QGridLayout(&dialog);
    QLabel *pathLabel = new QLabel(QStringLiteral("文件路径"), &dialog);
    QLineEdit *pathEdit = new QLineEdit(&dialog);
    QPushButton *browseButton = new QPushButton(QStringLiteral("浏览..."), &dialog);
    QCheckBox *windowCheckBox = new QCheckBox(QStringLiteral("加窗"), &dialog);
    QCheckBox *backgroundCheckBox = new QCheckBox(QStringLiteral("去本底"), &dialog);
    QCheckBox *logCheckBox = new QCheckBox(QStringLiteral("对数缩放"), &dialog);
    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                                     &dialog);

    windowCheckBox->setChecked(true);
    backgroundCheckBox->setChecked(true);
    logCheckBox->setChecked(true);
    pathEdit->setText(QDir::toNativeSeparators(fftDialogPath()));
    pathEdit->setMinimumWidth(520);

    layout->addWidget(pathLabel, 0, 0);
    layout->addWidget(pathEdit, 0, 1);
    layout->addWidget(browseButton, 0, 2);
    layout->addWidget(windowCheckBox, 1, 1, 1, 2);
    layout->addWidget(backgroundCheckBox, 2, 1, 1, 2);
    layout->addWidget(logCheckBox, 3, 1, 1, 2);
    layout->addWidget(buttons, 4, 0, 1, 3);

    QObject::connect(browseButton, &QPushButton::clicked, &dialog, [&]() {
        const QString selectedPath = QFileDialog::getOpenFileName(
            &dialog,
            QStringLiteral("选择 OCT 数据文件"),
            dialogStartPathFromText(pathEdit->text()),
            QStringLiteral("OCT 数据文件 (*.2d *.3d);;2D 文件 (*.2d);;3D 文件 (*.3d);;所有文件 (*.*)"));
        if (!selectedPath.isEmpty())
            pathEdit->setText(QDir::toNativeSeparators(selectedPath));
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    while (dialog.exec() == QDialog::Accepted) {
        const QString filePath = cleanPastedPath(pathEdit->text());
        const QFileInfo fileInfo(filePath);
        if (!filePath.isEmpty() && fileInfo.exists() && fileInfo.isFile() && isOctDataFile(filePath)) {
            selection->filePath = fileInfo.absoluteFilePath();
            selection->applyWindow = windowCheckBox->isChecked();
            selection->removeBackground = backgroundCheckBox->isChecked();
            selection->applyLogScale = logCheckBox->isChecked();
            return true;
        }
        QMessageBox::warning(&dialog,
                             QStringLiteral("文件无效"),
                             QStringLiteral("请选择存在的 .2d 或 .3d 文件。"));
    }

    return false;
}

QString colorProjectionPathForAngioFile(const QString &filePath)
{
    const QFileInfo fileInfo(filePath);
    return fileInfo.absolutePath() + QStringLiteral("/") + fileInfo.completeBaseName() + QStringLiteral(".tiff");
}

QString periodicityReportPathForDataFile(const QString &filePath)
{
    const QFileInfo fileInfo(filePath);
    return fileInfo.absolutePath() + QStringLiteral("/") + fileInfo.completeBaseName()
        + QStringLiteral("_periodicity.txt");
}

void setComboBoxText(QComboBox *comboBox, const QString &text)
{
    if (!comboBox) {
        return;
    }

    const int index = comboBox->findText(text);
    if (index >= 0) {
        comboBox->setCurrentIndex(index);
    }
}

mainWidget::ADTriggerMode triggerModeFromText(const QString &text)
{
    if (text.compare(QStringLiteral("External"), Qt::CaseInsensitive) == 0)
        return mainWidget::ADTriggerMode::External;
    if (text.compare(QStringLiteral("Continuous"), Qt::CaseInsensitive) == 0)
        return mainWidget::ADTriggerMode::Continuous;
    return mainWidget::ADTriggerMode::Internal;
}

QString triggerModeToText(mainWidget::ADTriggerMode mode)
{
    switch (mode)
    {
        case mainWidget::ADTriggerMode::External:
            return QStringLiteral("External");
        case mainWidget::ADTriggerMode::Continuous:
            return QStringLiteral("Continuous");
        case mainWidget::ADTriggerMode::Internal:
        default:
            return QStringLiteral("Internal");
    }
}

mainWidget::ADClockMode clockModeFromText(const QString &text)
{
    if (text.compare(QStringLiteral("External"), Qt::CaseInsensitive) == 0)
        return mainWidget::ADClockMode::External;
    return mainWidget::ADClockMode::Internal;
}

QString clockModeToText(mainWidget::ADClockMode mode)
{
    switch (mode)
    {
        case mainWidget::ADClockMode::External:
            return QStringLiteral("External");
        case mainWidget::ADClockMode::Internal:
        default:
            return QStringLiteral("Internal");
    }
}

int jsonIntValue(const QJsonObject &object, const QString &key, int fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isDouble())
        return value.toInt(fallback);
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        if (ok)
            return parsed;
    }
    return fallback;
}

qint64 jsonIntegerValue(const QJsonObject &object, const QString &key, qint64 fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isDouble())
        return static_cast<qint64>(value.toDouble(static_cast<double>(fallback)));
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().toLongLong(&ok);
        if (ok)
            return parsed;
    }
    return fallback;
}

double jsonDoubleValue(const QJsonObject &object, const QString &key, double fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isDouble())
        return value.toDouble(fallback);
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok)
            return parsed;
    }
    return fallback;
}

void applySettingsObjectToMetadata(const QJsonObject &mainSettings, FourierSourceMetadata *metadata)
{
    if (metadata == nullptr)
        return;

    metadata->ascanLen = jsonIntValue(mainSettings, QStringLiteral("AscanLen"), metadata->ascanLen);
    metadata->bscanLen = jsonIntValue(mainSettings, QStringLiteral("BscanLength"), metadata->bscanLen);
    metadata->cscanLen = jsonIntValue(mainSettings, QStringLiteral("Bscanlines"), metadata->cscanLen);
    metadata->dispersionW0 = jsonDoubleValue(mainSettings, QStringLiteral("dispersionW0"), metadata->dispersionW0);
    metadata->dispersionA1 = jsonDoubleValue(mainSettings, QStringLiteral("dispersionA1"), metadata->dispersionA1);
    metadata->dispersionA2 = jsonDoubleValue(mainSettings, QStringLiteral("dispersionA2"), metadata->dispersionA2);
    const QString scanModeText = mainSettings.value(QStringLiteral("scanMode")).toString();
    if (!scanModeText.isEmpty()) {
        metadata->scanModeText = scanModeText;
        metadata->scanMode = scanModeFromText(scanModeText, metadata->scanMode);
    }
}

bool readFourierSidecar(const QString &dataFilePath,
                        FourierSourceMetadata *metadata,
                        QString *errorMessage)
{
    if (metadata == nullptr)
        return false;

    const QString sidecarPath = sidecarPathForDataFile(dataFilePath);
    QFile sidecarFile(sidecarPath);
    if (!sidecarFile.exists())
        return false;
    if (!sidecarFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage)
            *errorMessage = sidecarFile.errorString();
        return false;
    }

    const QByteArray bytes = sidecarFile.readAll();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (!document.isObject()) {
        QSettings iniSidecar(sidecarPath, QSettings::IniFormat);
        if (!iniSidecar.childGroups().contains(QStringLiteral("mainWidget"))) {
            if (errorMessage)
                *errorMessage = parseError.errorString();
            return false;
        }
        iniSidecar.beginGroup(QStringLiteral("mainWidget"));
        metadata->ascanLen = iniSidecar.value(QStringLiteral("AscanLen"), metadata->ascanLen).toInt();
        metadata->bscanLen = iniSidecar.value(QStringLiteral("BscanLength"), metadata->bscanLen).toInt();
        metadata->cscanLen = iniSidecar.value(QStringLiteral("Bscanlines"), metadata->cscanLen).toInt();
        metadata->dispersionW0 = iniSidecar.value(QStringLiteral("dispersionW0"), metadata->dispersionW0).toDouble();
        metadata->dispersionA1 = iniSidecar.value(QStringLiteral("dispersionA1"), metadata->dispersionA1).toDouble();
        metadata->dispersionA2 = iniSidecar.value(QStringLiteral("dispersionA2"), metadata->dispersionA2).toDouble();
        const QString scanModeText = iniSidecar.value(QStringLiteral("scanMode"), metadata->scanModeText).toString();
        iniSidecar.endGroup();

        if (metadata->ascanLen > 0 && metadata->bscanLen > 0) {
            metadata->hasSidecar = true;
            metadata->scanModeText = scanModeText;
            metadata->scanMode = scanModeFromText(scanModeText, metadata->scanMode);
            return true;
        }

        if (errorMessage)
            *errorMessage = parseError.errorString();
        return false;
    }

    const QJsonObject root = document.object();
    const QJsonObject settingsObject = root.value(QStringLiteral("settings")).toObject();
    applySettingsObjectToMetadata(settingsObject.value(QStringLiteral("mainWidget")).toObject(), metadata);

    const QJsonObject acquisition = root.value(QStringLiteral("acquisition")).toObject();
    metadata->scanModeText = acquisition.value(QStringLiteral("scanModeText")).toString(metadata->scanModeText);
    metadata->scanMode = jsonIntValue(acquisition, QStringLiteral("scanMode"), metadata->scanMode);
    metadata->ascanLen = jsonIntValue(acquisition, QStringLiteral("ascanLen"), metadata->ascanLen);
    metadata->bscanLen = jsonIntValue(acquisition, QStringLiteral("bscanLen"), metadata->bscanLen);
    metadata->cscanLen = jsonIntValue(acquisition, QStringLiteral("cscanLen"), metadata->cscanLen);
    metadata->angioRep = jsonIntValue(acquisition, QStringLiteral("angioRep"), metadata->angioRep);
    metadata->storedAlineCount = jsonIntegerValue(acquisition, QStringLiteral("storedAlineCount"), metadata->storedAlineCount);
    metadata->storedBscanCount = jsonIntValue(acquisition, QStringLiteral("storedBscanCount"), metadata->storedBscanCount);
    metadata->sampleType = acquisition.value(QStringLiteral("sampleType")).toString(metadata->sampleType);
    metadata->preprocessedSpectrum =
        acquisition.value(QStringLiteral("preprocessedSpectrum")).toBool(metadata->preprocessedSpectrum);

    if (metadata->scanModeText.isEmpty())
        metadata->scanModeText = scanModeTextForMode(metadata->scanMode);

    metadata->hasSidecar = true;
    return true;
}

QString alignmentCheckText(qint64 fileSizeBytes,
                           int ascanLen,
                           qint64 bytesPerSample,
                           const QString &sampleTypeName)
{
    if (ascanLen <= 0)
        return QStringLiteral("%1：当前系统 AscanLen 无效，无法检查。").arg(sampleTypeName);

    const qint64 bytesPerAline = static_cast<qint64>(ascanLen) * bytesPerSample;
    if (bytesPerAline <= 0 || fileSizeBytes % bytesPerAline != 0) {
        return QStringLiteral("%1：不能被当前 AscanLen 整除。").arg(sampleTypeName);
    }

    return QStringLiteral("%1：可以被当前 AscanLen 整除，推断 Aline 数=%2。")
        .arg(sampleTypeName)
        .arg(fileSizeBytes / bytesPerAline);
}

bool canInferAlineCountFromCurrentAscanLen(qint64 fileSizeBytes, int ascanLen)
{
    if (ascanLen <= 0)
        return false;

    const qint64 uint16BytesPerAline = static_cast<qint64>(ascanLen) * static_cast<qint64>(sizeof(uint16_t));
    return uint16BytesPerAline > 0 && fileSizeBytes % uint16BytesPerAline == 0;
}

int styledMessageBox(QWidget *parent,
                     QMessageBox::Icon icon,
                     const QString &title,
                     const QString &text,
                     QMessageBox::StandardButtons buttons,
                     QMessageBox::StandardButton defaultButton);

bool confirmFourierWithoutSidecar(QWidget *parent, const QFileInfo &fileInfo, int currentAscanLen)
{
    const qint64 fileSizeBytes = fileInfo.size();
    const QString message = QStringLiteral(
        "未找到同名 .json 文件，将使用当前程序设置进行傅里叶变换。\n\n"
        "目标文件：%1\n"
        "目标文件长度：%2 字节\n"
        "目前系统 AscanLen：%3\n\n"
        "长度检查：\n%4\n\n"
        "是否继续 FFT？")
        .arg(QDir::toNativeSeparators(fileInfo.absoluteFilePath()))
        .arg(fileSizeBytes)
        .arg(currentAscanLen)
        .arg(alignmentCheckText(fileSizeBytes,
                                currentAscanLen,
                                static_cast<qint64>(sizeof(uint16_t)),
                                QStringLiteral("按 uint16 原始数据")));

    const bool canContinue = canInferAlineCountFromCurrentAscanLen(fileSizeBytes, currentAscanLen);
    const int ret = styledMessageBox(parent,
                                     canContinue ? QMessageBox::Question : QMessageBox::Warning,
                                     canContinue
                                         ? QStringLiteral("确认 FFT 参数")
                                         : QStringLiteral("FFT 参数检查失败"),
                                     canContinue
                                         ? message
                                         : message + QStringLiteral("\n\n目标文件长度未通过当前 AscanLen 检查，请先取消并确认参数。"),
                                     canContinue ? (QMessageBox::Ok | QMessageBox::Cancel) : QMessageBox::Cancel,
                                     QMessageBox::Cancel);
    return canContinue && ret == QMessageBox::Ok;
}

bool confirmReplaceFourierOutputs(QWidget *parent, const QString &binPath, const QString &jsonPath)
{
    QStringList existingFiles;
    if (QFileInfo::exists(binPath))
        existingFiles << QDir::toNativeSeparators(binPath);
    if (QFileInfo::exists(jsonPath))
        existingFiles << QDir::toNativeSeparators(jsonPath);
    if (existingFiles.isEmpty())
        return true;

    const QString message = QStringLiteral("以下 FFT 输出文件已存在：\n\n%1\n\n是否替换这些文件？")
        .arg(existingFiles.join(QStringLiteral("\n")));
    const int ret = styledMessageBox(parent,
                                     QMessageBox::Question,
                                     QStringLiteral("确认替换 FFT 输出"),
                                     message,
                                     QMessageBox::Yes | QMessageBox::Cancel,
                                     QMessageBox::Cancel);
    return ret == QMessageBox::Yes;
}

bool transformSpectraBlockOpenCl(const std::vector<float> &spectra,
                                 int ascanLen,
                                 int lineCount,
                                 double dispersionW0,
                                 double dispersionA1,
                                 double dispersionA2,
                                 bool applyLogScale,
                                 std::vector<float> *output);

bool canUseOpenClFftBackend(int ascanLen,
                            double dispersionW0,
                            double dispersionA1,
                            double dispersionA2)
{
    if (!cv::ocl::haveOpenCL() || !cv::ocl::useOpenCL() || ascanLen <= 0)
        return false;

    std::vector<float> testInput(static_cast<size_t>(ascanLen), 1.0f);
    std::vector<float> testOutput;
    return transformSpectraBlockOpenCl(testInput,
                                       ascanLen,
                                       1,
                                       dispersionW0,
                                       dispersionA1,
                                       dispersionA2,
                                       false,
                                       &testOutput);
}

std::vector<float> makeSpectralWindow(int ascanLen, bool applyWindow)
{
    std::vector<float> window(static_cast<size_t>(std::max(0, ascanLen)), 1.0f);
    if (!applyWindow || ascanLen <= 0)
        return window;

    const int zeroEdgeCount = 5;
    const int tukeyLength = ascanLen - zeroEdgeCount * 2;
    std::fill(window.begin(), window.end(), 0.0f);
    if (tukeyLength <= 0)
        return window;
    if (tukeyLength == 1) {
        window[static_cast<size_t>(zeroEdgeCount)] = 1.0f;
        return window;
    }

    const double alpha = 0.4;
    const double halfAlpha = alpha / 2.0;
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < tukeyLength; ++i) {
        const double x = static_cast<double>(i) / static_cast<double>(tukeyLength - 1);
        double value = 1.0;
        if (x < halfAlpha)
            value = 0.5 * (1.0 + cos(pi * (2.0 * x / alpha - 1.0)));
        else if (x > 1.0 - halfAlpha)
            value = 0.5 * (1.0 + cos(pi * (2.0 * x / alpha - 2.0 / alpha + 1.0)));
        window[static_cast<size_t>(i + zeroEdgeCount)] = static_cast<float>(value);
    }
    return window;
}

bool isFloatSampleType(const QString &sampleType)
{
    return sampleType.compare(QString::fromLatin1(kSampleTypeFloat32Spectrum), Qt::CaseInsensitive) == 0;
}

bool isUint16SampleType(const QString &sampleType)
{
    return sampleType.compare(QString::fromLatin1(kSampleTypeUint16Raw), Qt::CaseInsensitive) == 0;
}

QString inferSampleType(const QString &filePath, int ascanLen, qint64 storedAlineCount)
{
    const QFileInfo fileInfo(filePath);
    const qint64 fileSize = fileInfo.size();
    const qint64 expectedLines = storedAlineCount > 0 ? storedAlineCount : 0;
    const qint64 floatBytes = expectedLines * ascanLen * static_cast<qint64>(sizeof(float));
    const qint64 uint16Bytes = expectedLines * ascanLen * static_cast<qint64>(sizeof(uint16_t));
    if (expectedLines > 0 && fileSize == uint16Bytes)
        return QString::fromLatin1(kSampleTypeUint16Raw);
    if (expectedLines > 0 && fileSize == floatBytes)
        return QString::fromLatin1(kSampleTypeFloat32Spectrum);

    const QString suffix = fileInfo.suffix().toLower();
    if (suffix == QStringLiteral("3d") && ascanLen > 0
        && fileSize % (static_cast<qint64>(ascanLen) * static_cast<qint64>(sizeof(uint16_t))) == 0)
        return QString::fromLatin1(kSampleTypeUint16Raw);
    if (ascanLen > 0
        && fileSize % (static_cast<qint64>(ascanLen) * static_cast<qint64>(sizeof(float))) == 0)
        return QString::fromLatin1(kSampleTypeFloat32Spectrum);
    return QString();
}

bool readSpectraLines(QFile &file,
                      qint64 startLine,
                      int lineCount,
                      int ascanLen,
                      const QString &sampleType,
                      const std::vector<float> &background,
                      const std::vector<float> &window,
                      bool subtractBackground,
                      std::vector<float> *spectra,
                      QString *errorMessage)
{
    if (spectra == nullptr || ascanLen <= 0 || lineCount <= 0)
        return false;

    const bool floatInput = isFloatSampleType(sampleType);
    const qint64 sampleSize = floatInput ? static_cast<qint64>(sizeof(float))
                                         : static_cast<qint64>(sizeof(uint16_t));
    const qint64 sampleCount = static_cast<qint64>(lineCount) * ascanLen;
    const qint64 byteOffset = startLine * ascanLen * sampleSize;
    const qint64 byteCount = sampleCount * sampleSize;
    if (!file.seek(byteOffset)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法定位到文件中的第 %1 条 Aline。").arg(startLine);
        return false;
    }

    QByteArray bytes;
    bytes.resize(static_cast<int>(byteCount));
    if (file.read(bytes.data(), byteCount) != byteCount) {
        if (errorMessage)
            *errorMessage = QStringLiteral("读取 OCT 数据文件失败。");
        return false;
    }

    spectra->assign(static_cast<size_t>(sampleCount), 0.0f);
    const bool hasBackground = subtractBackground
        && background.size() == static_cast<std::vector<float>::size_type>(ascanLen);
    const bool hasWindow = window.size() == static_cast<std::vector<float>::size_type>(ascanLen);

    if (floatInput) {
        const float *source = reinterpret_cast<const float*>(bytes.constData());
        for (int line = 0; line < lineCount; ++line) {
            for (int z = 0; z < ascanLen; ++z) {
                float value = source[line * ascanLen + z];
                if (hasBackground)
                    value -= background[static_cast<size_t>(z)];
                if (hasWindow)
                    value *= window[static_cast<size_t>(z)];
                (*spectra)[static_cast<size_t>(line) * ascanLen + z] = value;
            }
        }
    } else {
        const uint16_t *source = reinterpret_cast<const uint16_t*>(bytes.constData());
        for (int line = 0; line < lineCount; ++line) {
            for (int z = 0; z < ascanLen; ++z) {
                float value = static_cast<float>(source[line * ascanLen + z] >> 4)
                    - static_cast<float>(mythread::plotOffset);
                if (hasBackground)
                    value -= background[static_cast<size_t>(z)];
                if (hasWindow)
                    value *= window[static_cast<size_t>(z)];
                (*spectra)[static_cast<size_t>(line) * ascanLen + z] = value;
            }
        }
    }
    return true;
}

bool computeBackgroundFromMiddleBscan(QFile &file,
                                      qint64 totalLines,
                                      int ascanLen,
                                      int bscanLen,
                                      const QString &sampleType,
                                      std::vector<float> *background,
                                      QString *errorMessage)
{
    if (background == nullptr || totalLines <= 0 || ascanLen <= 0)
        return false;

    const int backgroundLines = static_cast<int>(std::min<qint64>(
        totalLines,
        bscanLen > 0 ? static_cast<qint64>(bscanLen) : totalLines));
    qint64 startLine = 0;
    if (bscanLen > 0 && totalLines > backgroundLines) {
        const qint64 middleLine = totalLines / 2;
        startLine = (middleLine / bscanLen) * bscanLen;
        if (startLine + backgroundLines > totalLines)
            startLine = totalLines - backgroundLines;
    }

    const std::vector<float> emptyBackground;
    const std::vector<float> unityWindow(static_cast<size_t>(ascanLen), 1.0f);
    std::vector<float> spectra;
    if (!readSpectraLines(file,
                          startLine,
                          backgroundLines,
                          ascanLen,
                          sampleType,
                          emptyBackground,
                          unityWindow,
                          false,
                          &spectra,
                          errorMessage))
        return false;

    background->assign(static_cast<size_t>(ascanLen), 0.0f);
    for (int line = 0; line < backgroundLines; ++line) {
        for (int z = 0; z < ascanLen; ++z)
            (*background)[static_cast<size_t>(z)] += spectra[static_cast<size_t>(line) * ascanLen + z];
    }
    for (float &value : *background)
        value /= static_cast<float>(backgroundLines);
    return true;
}

bool transformSpectraBlockMkl(const std::vector<float> &spectra,
                              int ascanLen,
                              int lineCount,
                              double dispersionW0,
                              double dispersionA1,
                              double dispersionA2,
                              bool applyLogScale,
                              std::vector<float> *output,
                              QString *errorMessage)
{
    if (output == nullptr || ascanLen <= 0 || lineCount <= 0)
        return false;

    const int numel = ascanLen * lineCount;
    if (numel <= 0 || spectra.size() < static_cast<std::vector<float>::size_type>(numel))
        return false;

    MKL_Complex8 *compensated = static_cast<MKL_Complex8*>(mkl_malloc(numel * sizeof(MKL_Complex8), 32));
    MKL_Complex8 *fourierd = static_cast<MKL_Complex8*>(mkl_malloc(numel * sizeof(MKL_Complex8), 32));
    if (compensated == nullptr || fourierd == nullptr) {
        mkl_free(compensated);
        mkl_free(fourierd);
        if (errorMessage)
            *errorMessage = QStringLiteral("分配 FFT 工作内存失败。");
        return false;
    }

    std::vector<float> cosphy(static_cast<size_t>(ascanLen), 1.0f);
    std::vector<float> sinphy(static_cast<size_t>(ascanLen), 0.0f);
    for (int z = 0; z < ascanLen; ++z) {
        const double tmp = (z - dispersionW0) * (z - dispersionW0);
        const double phy = dispersionA1 * tmp / 10000.0
            + dispersionA2 * tmp * (z - dispersionW0) / 100000000.0;
        cosphy[static_cast<size_t>(z)] = static_cast<float>(cos(phy));
        sinphy[static_cast<size_t>(z)] = static_cast<float>(sin(phy));
    }

    for (int line = 0; line < lineCount; ++line) {
        for (int z = 0; z < ascanLen; ++z) {
            const int index = line * ascanLen + z;
            compensated[index].real = spectra[static_cast<size_t>(index)] * cosphy[static_cast<size_t>(z)];
            compensated[index].imag = spectra[static_cast<size_t>(index)] * sinphy[static_cast<size_t>(z)];
        }
    }

    DFTI_DESCRIPTOR_HANDLE fftHandle = nullptr;
    MKL_LONG status = DftiCreateDescriptor(&fftHandle, DFTI_SINGLE, DFTI_COMPLEX, 1, ascanLen);
    if (status == DFTI_NO_ERROR)
        status = DftiSetValue(fftHandle, DFTI_PLACEMENT, DFTI_NOT_INPLACE);
    if (status == DFTI_NO_ERROR)
        status = DftiSetValue(fftHandle, DFTI_NUMBER_OF_TRANSFORMS, lineCount);
    if (status == DFTI_NO_ERROR)
        status = DftiSetValue(fftHandle, DFTI_INPUT_DISTANCE, ascanLen);
    if (status == DFTI_NO_ERROR)
        status = DftiSetValue(fftHandle, DFTI_OUTPUT_DISTANCE, ascanLen);
    if (status == DFTI_NO_ERROR)
        status = DftiCommitDescriptor(fftHandle);
    if (status == DFTI_NO_ERROR)
        status = DftiComputeForward(fftHandle, compensated, fourierd);
    if (fftHandle != nullptr)
        DftiFreeDescriptor(&fftHandle);

    if (status != DFTI_NO_ERROR) {
        if (errorMessage)
            *errorMessage = QStringLiteral("MKL FFT 失败：%1").arg(DftiErrorMessage(status));
        mkl_free(compensated);
        mkl_free(fourierd);
        return false;
    }

    output->assign(static_cast<size_t>(numel), 0.0f);
    vcAbs(numel, fourierd, output->data());
    if (applyLogScale) {
        vslog10(&numel, output->data(), output->data());
        cblas_sscal(numel, static_cast<float>(mainWidget::FFT_SCALE), output->data(), 1);
    }

    mkl_free(compensated);
    mkl_free(fourierd);
    return true;
}

bool transformSpectraBlockOpenCl(const std::vector<float> &spectra,
                                 int ascanLen,
                                 int lineCount,
                                 double dispersionW0,
                                 double dispersionA1,
                                 double dispersionA2,
                                 bool applyLogScale,
                                 std::vector<float> *output)
{
    if (output == nullptr || ascanLen <= 0 || lineCount <= 0
        || !cv::ocl::haveOpenCL() || !cv::ocl::useOpenCL())
        return false;

    const int numel = ascanLen * lineCount;
    if (numel <= 0 || spectra.size() < static_cast<std::vector<float>::size_type>(numel))
        return false;

    try {
        cv::Mat compensated(lineCount, ascanLen, CV_32FC2);
        for (int z = 0; z < ascanLen; ++z) {
            const double tmp = (z - dispersionW0) * (z - dispersionW0);
            const double phy = dispersionA1 * tmp / 10000.0
                + dispersionA2 * tmp * (z - dispersionW0) / 100000000.0;
            const float cosValue = static_cast<float>(cos(phy));
            const float sinValue = static_cast<float>(sin(phy));
            for (int line = 0; line < lineCount; ++line) {
                const int index = line * ascanLen + z;
                cv::Vec2f &dst = compensated.at<cv::Vec2f>(line, z);
                dst[0] = spectra[static_cast<size_t>(index)] * cosValue;
                dst[1] = spectra[static_cast<size_t>(index)] * sinValue;
            }
        }

        cv::UMat compensatedGpu;
        compensated.copyTo(compensatedGpu);
        cv::UMat fftGpu;
        cv::dft(compensatedGpu, fftGpu, cv::DFT_ROWS | cv::DFT_COMPLEX_OUTPUT);

        std::vector<cv::UMat> channels;
        cv::split(fftGpu, channels);
        if (channels.size() != 2)
            return false;

        cv::UMat magnitudeGpu;
        cv::magnitude(channels[0], channels[1], magnitudeGpu);
        cv::Mat magnitudeHost;
        magnitudeGpu.copyTo(magnitudeHost);
        if (magnitudeHost.empty() || magnitudeHost.rows != lineCount || magnitudeHost.cols != ascanLen)
            return false;

        output->assign(static_cast<size_t>(numel), 0.0f);
        if (magnitudeHost.isContinuous()) {
            const float *src = magnitudeHost.ptr<float>(0);
            std::copy(src, src + numel, output->begin());
        } else {
            for (int line = 0; line < lineCount; ++line) {
                const float *src = magnitudeHost.ptr<float>(line);
                std::copy(src, src + ascanLen, output->begin() + static_cast<size_t>(line) * ascanLen);
            }
        }

        if (applyLogScale) {
            vslog10(&numel, output->data(), output->data());
            cblas_sscal(numel, static_cast<float>(mainWidget::FFT_SCALE), output->data(), 1);
        }
    } catch (...) {
        return false;
    }

    return true;
}

bool transformSpectraBlock(const std::vector<float> &spectra,
                           int ascanLen,
                           int lineCount,
                           double dispersionW0,
                           double dispersionA1,
                           double dispersionA2,
                           bool applyLogScale,
                           std::vector<float> *output,
                           bool *usedOpenCl,
                           QString *errorMessage)
{
    if (transformSpectraBlockOpenCl(spectra,
                                    ascanLen,
                                    lineCount,
                                    dispersionW0,
                                    dispersionA1,
                                    dispersionA2,
                                    applyLogScale,
                                    output)) {
        if (usedOpenCl)
            *usedOpenCl = true;
        return true;
    }

    return transformSpectraBlockMkl(spectra,
                                    ascanLen,
                                    lineCount,
                                    dispersionW0,
                                    dispersionA1,
                                    dispersionA2,
                                    applyLogScale,
                                    output,
                                     errorMessage);
}

void applyLogScaleToFftOutput(std::vector<float> *output)
{
    if (output == nullptr || output->empty())
        return;

    const int numel = static_cast<int>(output->size());
    vslog10(&numel, output->data(), output->data());
    cblas_sscal(numel, static_cast<float>(mainWidget::FFT_SCALE), output->data(), 1);
}

int ringIndexBack(int currentIndex, int offset, int bufferCount)
{
    if (bufferCount <= 0)
        return -1;

    int index = (currentIndex - offset) % bufferCount;
    if (index < 0)
        index += bufferCount;
    return index;
}

double normalizedCorrelationAtLag(const std::vector<float> &signal, int lag)
{
    const int sampleCount = static_cast<int>(signal.size());
    const int pairCount = sampleCount - lag;
    if (lag <= 0 || pairCount < 64)
        return 0.0;

    double xy = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    for (int i = 0; i < pairCount; ++i)
    {
        const double x = signal[i + lag];
        const double y = signal[i];
        xy += x * y;
        x2 += x * x;
        y2 += y * y;
    }

    if (x2 <= 0.0 || y2 <= 0.0)
        return 0.0;
    return xy / std::sqrt(x2 * y2);
}

double normalizedSegmentCorrelation(const std::vector<float> &signal,
                                    int firstOffset,
                                    int secondOffset,
                                    int sampleCount)
{
    if (firstOffset < 0 || secondOffset < 0 || sampleCount <= 0
        || firstOffset + sampleCount > static_cast<int>(signal.size())
        || secondOffset + sampleCount > static_cast<int>(signal.size()))
        return 0.0;

    double meanA = 0.0;
    double meanB = 0.0;
    for (int i = 0; i < sampleCount; ++i)
    {
        meanA += signal[firstOffset + i];
        meanB += signal[secondOffset + i];
    }
    meanA /= sampleCount;
    meanB /= sampleCount;

    double xy = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    for (int i = 0; i < sampleCount; ++i)
    {
        const double x = signal[firstOffset + i] - meanA;
        const double y = signal[secondOffset + i] - meanB;
        xy += x * y;
        x2 += x * x;
        y2 += y * y;
    }

    if (x2 <= 0.0 || y2 <= 0.0)
        return 0.0;
    return xy / std::sqrt(x2 * y2);
}

PeriodicityWindowResult analyzePeriodicityWindow(const QString &label,
                                                 const std::vector<float> &rawSamples,
                                                 int ascanLen)
{
    PeriodicityWindowResult result;
    result.label = label;
    result.sampleCount = static_cast<int>(rawSamples.size());
    if (result.sampleCount < 2048)
        return result;

    std::vector<float> diffSignal;
    diffSignal.reserve(rawSamples.size() - 1);
    for (int i = 1; i < result.sampleCount; ++i)
        diffSignal.push_back(rawSamples[i] - rawSamples[i - 1]);

    const int minLagRaw = 32;
    const int maxLagRaw = std::min(std::min(65536, std::max(4096, ascanLen * 64)),
                                   static_cast<int>(diffSignal.size() / 3));
    if (maxLagRaw <= minLagRaw)
        return result;

    const int maxDownSamples = 16384;
    const int stride = std::max(1, (static_cast<int>(diffSignal.size()) + maxDownSamples - 1) / maxDownSamples);
    const int downCount = static_cast<int>(diffSignal.size()) / stride;
    if (downCount < 256)
        return result;

    std::vector<float> downSignal;
    downSignal.reserve(downCount);
    double mean = 0.0;
    for (int i = 0; i < downCount; ++i)
    {
        const float value = diffSignal[i * stride];
        downSignal.push_back(value);
        mean += value;
    }
    mean /= downCount;
    for (float &value : downSignal)
        value = static_cast<float>(value - mean);

    const int minLagDown = std::max(1, minLagRaw / stride);
    const int maxLagDown = std::min(downCount / 3, std::max(minLagDown + 1, maxLagRaw / stride));
    int bestLagDown = 0;
    double bestCorrelation = 0.0;
    for (int lag = minLagDown; lag <= maxLagDown; ++lag)
    {
        const double corr = normalizedCorrelationAtLag(downSignal, lag);
        if (corr > bestCorrelation)
        {
            bestCorrelation = corr;
            bestLagDown = lag;
        }
    }
    if (bestLagDown <= 0)
        return result;

    double rawMean = 0.0;
    for (float value : diffSignal)
        rawMean += value;
    rawMean /= static_cast<double>(diffSignal.size());
    for (float &value : diffSignal)
        value = static_cast<float>(value - rawMean);

    const int coarseLagRaw = bestLagDown * stride;
    const int refineRadius = std::max(32, stride * 2);
    const int refineStart = std::max(minLagRaw, coarseLagRaw - refineRadius);
    const int refineEnd = std::min(maxLagRaw, coarseLagRaw + refineRadius);
    int bestLagRaw = coarseLagRaw;
    bestCorrelation = 0.0;
    for (int lag = refineStart; lag <= refineEnd; ++lag)
    {
        const double corr = normalizedCorrelationAtLag(diffSignal, lag);
        if (corr > bestCorrelation)
        {
            bestCorrelation = corr;
            bestLagRaw = lag;
        }
    }

    result.valid = bestLagRaw > 0;
    result.bestPeriodSamples = bestLagRaw;
    result.peakCorrelation = bestCorrelation;

    const int availablePairs = static_cast<int>(rawSamples.size()) / bestLagRaw - 1;
    const int pairCount = std::min(64, availablePairs);
    const int compareSamples = std::min(bestLagRaw, 4096);
    if (pairCount > 0 && compareSamples >= 64)
    {
        double sum = 0.0;
        double sum2 = 0.0;
        double minCorr = 1.0;
        for (int pair = 0; pair < pairCount; ++pair)
        {
            const int firstOffset = pair * bestLagRaw;
            const int secondOffset = firstOffset + bestLagRaw;
            const double corr = normalizedSegmentCorrelation(rawSamples,
                                                             firstOffset,
                                                             secondOffset,
                                                             compareSamples);
            sum += corr;
            sum2 += corr * corr;
            if (corr < minCorr)
                minCorr = corr;
        }
        result.adjacentPairCount = pairCount;
        result.adjacentMeanCorrelation = sum / pairCount;
        const double variance = std::max(0.0, sum2 / pairCount
            - result.adjacentMeanCorrelation * result.adjacentMeanCorrelation);
        result.adjacentStdCorrelation = std::sqrt(variance);
        result.adjacentMinCorrelation = minCorr;
    }

    return result;
}

bool isCycleCroppedBscanMode(int mode)
{
    return mode == 2 || mode == 3 || mode == 32 || mode == 42 || mode == kSymphonicScanMode;
}

int bscanDisplaySourceOffsetSamples(const mythread *thread, int ascanLen, int bscanLen)
{
    if (thread == nullptr || !isCycleCroppedBscanMode(mainWidget::scanMode))
        return 0;

    const long long cycleSamples = static_cast<long long>(thread->bscanCycleAlineCount()) * ascanLen;
    const int cyclesPerBuffer = std::max(1, thread->repeatCyclesPerBuffer());
    const long long offsetSamples =
        (static_cast<long long>(cyclesPerBuffer) - 1) * cycleSamples
        + static_cast<long long>(thread->transitionAlineCount()) * ascanLen;
    const long long displaySamples = static_cast<long long>(ascanLen) * bscanLen;
    if (cycleSamples <= 0
        || offsetSamples < 0 || offsetSamples + displaySamples > thread->singleBufferSize())
        return 0;
    if (offsetSamples > (std::numeric_limits<int>::max)())
        return 0;

    return static_cast<int>(offsetSamples);
}

bool writeCycleCroppedBscans(std::ofstream &file,
                             mythread *thread,
                             int ascanLen,
                             int bscanLen,
                             int logicalFrameCount,
                             int sourceBufferCount,
                             int sourceBufferStart = 0)
{
    if (thread == nullptr || ascanLen <= 0 || bscanLen <= 0
        || logicalFrameCount <= 0 || sourceBufferCount <= 0 || sourceBufferStart < 0)
        return false;

    const int cyclesPerBuffer = std::max(1, thread->repeatCyclesPerBuffer());
    const long long cycleSampleCount =
        static_cast<long long>(ascanLen) * thread->bscanCycleAlineCount();
    const long long sourceOffsetSamples =
        static_cast<long long>(ascanLen) * thread->transitionAlineCount();
    const long long bscanSampleCount = static_cast<long long>(ascanLen) * bscanLen;
    const long long requiredSamples =
        (static_cast<long long>(cyclesPerBuffer) - 1) * cycleSampleCount
        + sourceOffsetSamples + bscanSampleCount;
    if (cycleSampleCount <= 0 || sourceOffsetSamples < 0
        || requiredSamples > thread->singleBufferSize())
        return false;

    int logicalFramesWritten = 0;
    QMutexLocker lock(&thread->mutex1);
    if (thread->m_volumeMemBuffer.size() < sourceBufferStart + sourceBufferCount)
        return false;

    for (int buffer = 0; buffer < sourceBufferCount && logicalFramesWritten < logicalFrameCount; ++buffer)
    {
        U16 *base = thread->m_volumeMemBuffer[sourceBufferStart + buffer];
        if (base == nullptr)
            return false;

        for (int cycle = 0; cycle < cyclesPerBuffer && logicalFramesWritten < logicalFrameCount; ++cycle)
        {
            const U16 *source = base + cycle * cycleSampleCount + sourceOffsetSamples;
            file.write(reinterpret_cast<const char*>(source), sizeof(U16) * bscanSampleCount);
            if (!file.good())
                return false;
            ++logicalFramesWritten;
        }
    }
    return logicalFramesWritten == logicalFrameCount;
}

bool copyCrossScanBscans(U16 *firstBscan,
                         U16 *secondBscan,
                         mythread *thread,
                         int sourceIndex,
                         int ascanLen,
                         int bscanLen)
{
    if (firstBscan == nullptr || secondBscan == nullptr || thread == nullptr
        || sourceIndex < 0 || ascanLen <= 0 || bscanLen <= 0)
        return false;

    const int processesPerBuffer = std::max(1, thread->repeatCyclesPerBuffer());
    const long long processSampleCount =
        static_cast<long long>(ascanLen) * thread->xScanAlineCount();
    const long long bscanSampleCount = static_cast<long long>(ascanLen) * bscanLen;
    const long long baseOffset =
        (static_cast<long long>(processesPerBuffer) - 1) * processSampleCount;
    const long long firstOffset =
        baseOffset + static_cast<long long>(thread->transitionAlineCount()) * ascanLen;
    const long long secondOffset =
        baseOffset
        + static_cast<long long>(thread->linearScanAlineCount() + 2 * thread->transitionAlineCount()) * ascanLen;
    if (processSampleCount <= 0 || firstOffset < 0 || secondOffset < 0
        || secondOffset + bscanSampleCount > thread->singleBufferSize())
        return false;

    QMutexLocker lock(&thread->mutex1);
    if (sourceIndex >= thread->m_volumeMemBuffer.size()
        || thread->m_volumeMemBuffer[sourceIndex] == nullptr)
        return false;

    const U16 *base = thread->m_volumeMemBuffer[sourceIndex];
    memcpy(firstBscan, base + firstOffset, sizeof(U16) * bscanSampleCount);
    memcpy(secondBscan, base + secondOffset, sizeof(U16) * bscanSampleCount);
    return true;
}

bool copyAngioGroupBscans(U16 *destination,
                          mythread *thread,
                          int sourceIndex,
                          int ascanLen,
                          int bscanLen,
                          int frameCount)
{
    if (destination == nullptr || thread == nullptr || sourceIndex < 0
        || ascanLen <= 0 || bscanLen <= 0 || frameCount <= 0)
        return false;

    const int groupsPerBuffer = std::max(1, thread->repeatCyclesPerBuffer());
    const long long groupSampleCount =
        static_cast<long long>(ascanLen) * thread->xScanAlineCount();
    const long long cycleSampleCount =
        static_cast<long long>(ascanLen) * thread->bscanCycleAlineCount();
    const long long sourceOffsetSamples =
        static_cast<long long>(ascanLen) * thread->transitionAlineCount();
    const long long bscanSampleCount = static_cast<long long>(ascanLen) * bscanLen;
    const long long baseOffset =
        (static_cast<long long>(groupsPerBuffer) - 1) * groupSampleCount;
    const long long requiredSamples =
        baseOffset + (static_cast<long long>(frameCount) - 1) * cycleSampleCount
        + sourceOffsetSamples + bscanSampleCount;
    if (groupSampleCount <= 0 || cycleSampleCount <= 0 || sourceOffsetSamples < 0
        || requiredSamples > thread->singleBufferSize())
        return false;

    QMutexLocker lock(&thread->mutex1);
    if (sourceIndex >= thread->m_volumeMemBuffer.size()
        || thread->m_volumeMemBuffer[sourceIndex] == nullptr)
        return false;

    const U16 *base = thread->m_volumeMemBuffer[sourceIndex];
    for (int frame = 0; frame < frameCount; ++frame)
    {
        const U16 *source = base + baseOffset + frame * cycleSampleCount + sourceOffsetSamples;
        memcpy(destination + frame * bscanSampleCount, source, sizeof(U16) * bscanSampleCount);
    }
    return true;
}

long long ceilDiv(long long value, long long divisor)
{
    if (divisor <= 0)
        return 0;
    return (value + divisor - 1) / divisor;
}

int alignedAdReadCount(long long sampleCount)
{
    if (sampleCount <= 0)
        return 0;
    const long long aligned = ceilDiv(sampleCount, kReadAdCountUnit) * kReadAdCountUnit;
    if (aligned > (std::numeric_limits<int>::max)())
        return 0;
    return static_cast<int>(aligned);
}

long long absLongLong(long long value)
{
    return value < 0 ? -value : value;
}

long long gcdLongLong(long long a, long long b)
{
    a = absLongLong(a);
    b = absLongLong(b);
    while (b != 0)
    {
        const long long r = a % b;
        a = b;
        b = r;
    }
    return a == 0 ? 1 : a;
}

int volumeRepeatFactorForMode(int mode)
{
    return mode == 32 ? mainWidget::AngioRep : 1;
}

int dacBscanCycleAlineCount(int ascanFreq, double galvoFreq)
{
    if (ascanFreq <= 0 || galvoFreq <= 0.0)
        return 0;

    const double cycle = ascanFreq / galvoFreq + 0.5;
    if (cycle <= 0.0 || cycle > (std::numeric_limits<int>::max)())
        return 0;
    return static_cast<int>(cycle);
}

int cyclesPerVolumeReadBuffer(int ascanLen, int bscanCycleAlineCount)
{
    if (ascanLen <= 0 || bscanCycleAlineCount <= 0)
        return 0;
    return 1;
}

int styledMessageBox(QWidget *parent,
                     QMessageBox::Icon icon,
                     const QString &title,
                     const QString &text,
                     QMessageBox::StandardButtons buttons,
                     QMessageBox::StandardButton defaultButton)
{
    QMessageBox box(icon, title, text, buttons, parent);
    box.setDefaultButton(defaultButton);
    box.setStyleSheet(
        "QMessageBox { background-color: white; }"
        "QMessageBox QLabel { background-color: white; color: black; }"
        "QMessageBox QPushButton { background-color: white; color: black; }"
        "QMessageBox QPushButton:hover { background-color: rgb(235,235,235); }"
    );
    return box.exec();
}
}

template <typename T>
void mkl_ascend(T*& pointer)
{
    mkl_free(pointer);
    pointer = nullptr;
}

bool mainWidget::isContinuousSupportedMode(int mode)
{
    return mode == 1 || mode == 2 || mode == 22 || mode == 42 || mode == kSymphonicScanMode;
}

int mainWidget::continuousStoredBscanCount()
{
    if (!ContinuousModeEnabled || BscanLen <= 0 || scanMode == 1)
        return 0;
    return ContinuousAlineCount / BscanLen;
}

long long mainWidget::continuousTargetSampleCount()
{
    if (!ContinuousModeEnabled || AscanLen <= 0)
        return 0;
    if (scanMode == 1)
        return static_cast<long long>(ContinuousAlineCount) * AscanLen;

    const int bscanCount = continuousStoredBscanCount();
    return static_cast<long long>(bscanCount) * AscanLen * BscanLen;
}

int mainWidget::continuousSingleBufferSampleCount()
{
    if (!ContinuousModeEnabled || AscanLen <= 0)
        return 0;
    if (scanMode == 1)
        return AscanLen;
    if (BscanLen <= 0)
        return 0;
    return AscanLen * BscanLen;
}

int mainWidget::continuousBufferCount()
{
    if (!ContinuousModeEnabled)
        return 0;
    if (scanMode != 1)
        return continuousStoredBscanCount();

    const int singleBufferSamples = continuousSingleBufferSampleCount();
    if (singleBufferSamples <= 0)
        return 0;
    const long long bufferCount = ceilDiv(continuousTargetSampleCount(), singleBufferSamples);
    if (bufferCount > (std::numeric_limits<int>::max)())
        return 0;
    return static_cast<int>(bufferCount);
}

mainWidget::mainWidget(QWidget *parent):
    QWidget(parent),
    ui(new Ui::mainWidget),
    m_daState(DAState::NotReady),
    m_adReady(false),
    m_scanActive(false),
    m_activeVolumeSegmentIndex(0),
    m_infoLogTextLength(0),
    m_captureLogTextLength(0),
    m_logFilesInitialized(false),
    m_selectedDacDeviceId(DeviceSettings::defaultDacDeviceId()),
    m_selectedAdcDeviceId(DeviceSettings::defaultAdcDeviceId())
{
    // UI 设置
    ui->setupUi(this);
    initializeLogFiles();
    this->setMinimumSize(1700, 800);
    timerId1 = 0;
    timerId2 = 0;
    timerId3 = 0;
    crossSave = nullptr;
    m_curDisplayData = nullptr;
    m_curDisplayDataAngio1 = nullptr;
    m_curDisplayDataAngio2 = nullptr;
    m_last3DRealtimeDisplayBuffer = -1;
    m_realtimeShowInterval = 40;
    m_volumeScanTimingActive = false;
    m_volumeScanTimingMode = 0;
    p_F2 = nullptr;
    p_A1 = nullptr;
    p_A2 = nullptr;
    p_A3 = nullptr;
    p_A4 = nullptr;
    m_cosphy = nullptr;
    m_sinphy = nullptr;
    m_Cdata = nullptr;
    mainWidgetUISetup(ui);
    loadSettings();

    ui->textEdit->append("Tsinghua_SSOCT");
    ui->textEdit->append("清华大学 SSOCT 系统成像程序");
    ui->textEdit->append(QStringLiteral("Ver. ") + QString::fromLatin1(currentVersion));
    ui->textEdit->append(QStringLiteral("DAC 设备：%1").arg(selectedDacDeviceName()));
    ui->textEdit->append(QStringLiteral("ADC 设备：%1").arg(selectedAdcDeviceName()));
    ui->textEdit->append("沈逸然 石叶炅\n");
    QString audioStatusMessage;
    QString audioErrorMessage;
    if (VesselAudioPlayer::instance().probeWasapiOutput(&audioStatusMessage, &audioErrorMessage))
        ui->textEdit->append(audioStatusMessage);
    else
        ui->textEdit->append(QStringLiteral("Symphonic 声卡接口检查失败：%1").arg(audioErrorMessage));
    changeScanMode(ui->comboBox->currentText());
    applyFastAxisMode(ui->comboBox_2->currentText(), true);

    if (selectedAdcUsesPcie3640())
        PrintBoardInfo();
    else
        ui->textEdit->append(QStringLiteral("当前 ADC 设备尚未实现板卡信息读取。"));

    // 初始化相关的系数, 直接在 ui 里面设置
    m_BscanLen = ui->BscanLength->text().toUInt();
    m_CscanLen = ui->Bscanlines->text().toUInt();

    // 初始化本底和加窗数组

    m_BG.assign(m_AscanLen, 0.0f);
    resetSpectralWindow();
    // 实例化线程，先不启动该线程
    firstThread = new QThread;
    ssoctThread = new mythread;
    ssoctThread->moveToThread(firstThread);
    connect(ssoctThread, &mythread::acquisitionStatus, ui->textEdit_temp, &QTextEdit::append);
    connect(ssoctThread, &mythread::captureLoopFinished,
            this, &mainWidget::onAcquisitionLoopFinished);
    if (!selectedDacUsesPcie3640())
    {
        ui->textEdit->append(QStringLiteral("%1 DAC 适配层尚未实现，已跳过 PCIe3640 位置通道初始化。")
                             .arg(selectedDacDeviceName()));
    }
    else if (ssoctThread->InitializePositionOutputsToZero())
    {
        ui->textEdit->append(QStringLiteral("PCIe3640 位置通道已初始化为 0V（DACH1-DACH4）。"));
    }
    else
    {
        ui->textEdit->append(QStringLiteral("PCIe3640 位置通道 0V 初始化失败：%1")
                             .arg(ssoctThread->LastDAError()));
    }
    connect(ui->textEdit_temp, &QTextEdit::textChanged, this, [this]() {
        appendTextEditDeltaToLog(ui->textEdit_temp,
                                 QStringLiteral("capture.log"),
                                 QStringLiteral("CAPTURE"),
                                 m_captureLogTextLength);
        ui->textEdit_temp->moveCursor(QTextCursor::End);
    });
    auto continuousSettingsChanged = [this]() {
        bool continuousCountOk = false;
        const int continuousCount = ui->Line_continuousCount->text().toInt(&continuousCountOk);
        mainWidget::ContinuousModeEnabled = ui->CB_enableContinuousMode->isChecked();
        mainWidget::ContinuousAlineCount = continuousCountOk ? continuousCount : 0;
        if (m_daState == DAState::Ready)
        {
            stopCurrentScan(false, true, QStringLiteral("连续采集参数已改变，请重新准备 DA。"));
        }
        saveSettings();
        updateControlState();
    };
    connect(ui->CB_enableContinuousMode, &QCheckBox::toggled, this, continuousSettingsChanged);
    connect(ui->Line_continuousCount, &QLineEdit::editingFinished, this, continuousSettingsChanged);
    connect(ui->CB_fourierOnSaved, &QCheckBox::toggled, this, [this]() {
        saveSettings();
    });
    connect(ui->CB_3D_showReatimeData, &QCheckBox::toggled, this, [this]() {
        saveSettings();
    });
    connect(ui->CB_enableDAInSymphonic, &QCheckBox::toggled, this, [this]() {
        if (m_daState == DAState::Ready && mainWidget::scanMode == kSymphonicScanMode)
        {
            stopCurrentScan(false,
                            true,
                            QStringLiteral("Symphonic DA 测试输出设置已改变，请重新准备 DA。"),
                            true);
        }
        saveSettings();
        updateControlState();
    });
    connect(ui->LE_AngioRep, &QLineEdit::editingFinished, this, [this]() {
        const int previousAngioRep = m_AngioRep;
        readAngioRepFromUi(true);
        if (m_AngioRep != previousAngioRep && m_daState == DAState::Ready)
        {
            stopCurrentScan(false, true, QStringLiteral("Angio 重复数已改变，请重新准备 DA。"));
        }
        saveSettings();
        updateControlState();
    });
    connect(ui->LE_AscanLen, &QLineEdit::editingFinished, this, [this]() {
        clampAscanLenToLimit(true);
        saveSettings();
    });
    connect(ui->LE_AscanDutyCycle, &QLineEdit::editingFinished, this, [this]() {
        clampAscanLenToLimit(true);
        saveSettings();
    });
    connect(ui->SB_triggerOffsetSamples, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        readSysParametersFromUi();
        saveSettings();
    });
    connect(ui->SampleRate, &QLineEdit::editingFinished, this, [this]() {
        clampAscanLenToLimit(true);
        saveSettings();
    });
    connect(ui->AscanFreq, &QLineEdit::editingFinished, this, [this]() {
        clampAscanLenToLimit(true);
        updateBscanCycleLengthFromFrequencies();
        saveSettings();
    });
    connect(ui->frameRate, &QLineEdit::editingFinished, this, [this]() {
        updateBscanCycleLengthFromFrequencies();
        saveSettings();
    });
    connect(ui->LE_BscanCycleLen, &QLineEdit::editingFinished, this, [this]() {
        updateFrameRateFromBscanCycleLength();
        saveSettings();
    });
    // 把采集的线程从 mainWidget 这一图形界面的线程中分离出来，放到一个单独的线程中去执行，从而保持界面响应
    updateControlState();
}

void mainWidget::initializeLogFiles()
{
    truncateLogFile(QStringLiteral("info.log"));
    truncateLogFile(QStringLiteral("capture.log"));

    m_infoLogTextLength = ui->textEdit ? ui->textEdit->toPlainText().length() : 0;
    m_captureLogTextLength = ui->textEdit_temp ? ui->textEdit_temp->toPlainText().length() : 0;
    m_logFilesInitialized = true;
}

void mainWidget::appendTextEditDeltaToLog(QTextEdit *textEdit,
                                          const QString &fileName,
                                          const QString &level,
                                          int &lastTextLength)
{
    if (!m_logFilesInitialized || !textEdit)
        return;

    const QString currentText = textEdit->toPlainText();
    if (currentText.length() < lastTextLength) {
        lastTextLength = currentText.length();
        return;
    }

    const QString newText = currentText.mid(lastTextLength);
    lastTextLength = currentText.length();
    appendLogLines(fileName, level, newText);
}

void mainWidget::appendInfoLogLine(const QString &message)
{
    if (!m_logFilesInitialized)
        return;

    appendLogLines(QStringLiteral("info.log"), QStringLiteral("INFO"), message);
}

bool mainWidget::readScanParametersFromUi()
{
    Voltage = ui->amplitude->text().toDouble();
    duty_cycle = ui->dutycycle->text().toDouble();
    m_CscanLen = ui->Bscanlines->text().toUInt();
    m_BscanLen = ui->BscanLength->text().toInt();
    changeScanMode(ui->comboBox->currentText());
    triggerMode = triggerModeFromText(ui->combo_triggerMode->currentText());
    clockMode = clockModeFromText(ui->combo_clockMode->currentText());
    if (!readSysParametersFromUi())
        return false;

    if (m_BscanLen <= 0 || m_CscanLen <= 0)
    {
        ui->textEdit->append("扫描参数无效，请检查 Bscan 长度和 Bscan 数量。");
        return false;
    }

    bool continuousCountOk = false;
    const int continuousCount = ui->Line_continuousCount->text().toInt(&continuousCountOk);
    ContinuousModeEnabled = ui->CB_enableContinuousMode->isChecked();
    ContinuousAlineCount = continuousCountOk ? continuousCount : 0;

    bool realtimeShowIntervalOk = false;
    const int realtimeShowInterval = ui->Line_realtimeShowInterval->text().toInt(&realtimeShowIntervalOk);
    if (!realtimeShowIntervalOk || realtimeShowInterval <= 0)
    {
        ui->textEdit->append(QStringLiteral("3D 实时显示间隔无效，请输入正整数。"));
        return false;
    }
    m_realtimeShowInterval = realtimeShowInterval;

    if (ContinuousModeEnabled)
    {
        if (!isContinuousSupportedMode(mainWidget::scanMode))
        {
            ui->textEdit->append(QStringLiteral("连续采集模式仅支持 1D scan、2D repeat、2D angio、Vessel scan 和 Symphonic。"));
            return false;
        }
        if (!continuousCountOk || continuousCount <= 0)
        {
            ui->textEdit->append(QStringLiteral("连续采集 Aline 数无效，请输入正整数。"));
            return false;
        }
        if (mainWidget::scanMode != 1 && continuousStoredBscanCount() <= 0)
        {
            ui->textEdit->append(QStringLiteral("连续采集 Aline 数小于 Bscan 长度，无法形成完整 Bscan。"));
            return false;
        }
        if (continuousSingleBufferSampleCount() <= 0 || continuousBufferCount() <= 0)
        {
            ui->textEdit->append(QStringLiteral("连续采集参数过大或无效，无法创建采集缓冲区。"));
            return false;
        }
    }

    return true;
}

bool mainWidget::readSysParametersFromUi()
{
    bool ascanLenOk = false;
    int newAscanLen = ui->LE_AscanLen->text().toInt(&ascanLenOk);
    bool adFileOffsetFramesOk = false;
    const int adFileOffsetFrames = ui->LE_adFileOffsetFrames->text().toInt(&adFileOffsetFramesOk);
    bool triggerOffsetOk = true;
    int newTriggerOffsetSamples = ui->SB_triggerOffsetSamples->value();
    readAngioRepFromUi(true);
    galvoFreq = ui->frameRate->text().toDouble();
    AscanFreq = ui->AscanFreq->text().toInt();
    const bool ascanLimitOk = clampAscanLenToLimit(true);
    newAscanLen = ui->LE_AscanLen->text().toInt(&ascanLenOk);
    bool sampleRateOk = false;
    sampleRateMHzFromUi(&sampleRateOk);

    if (triggerMode != ADTriggerMode::Continuous && ascanLenOk && newAscanLen > 0)
    {
        const int triggerLengthQuantumSamples = TRIG_UNIT * 2;
        const int triggerCompatibleAscanLen =
            (newAscanLen / triggerLengthQuantumSamples) * triggerLengthQuantumSamples;
        if (triggerCompatibleAscanLen > 0 && triggerCompatibleAscanLen != newAscanLen)
        {
            ui->textEdit->append(QStringLiteral("当前触发模式使用硬件触发采集，TriggerLength 需要对应 16 样点整数倍；Ascan 长度已从 %1 调整为 %2。")
                                 .arg(newAscanLen)
                                 .arg(triggerCompatibleAscanLen));
            newAscanLen = triggerCompatibleAscanLen;
            const QSignalBlocker blocker(ui->LE_AscanLen);
            ui->LE_AscanLen->setText(QString::number(newAscanLen));
        }
    }
    if (triggerOffsetOk)
    {
        const int maxTriggerPreSamples = std::max(0, std::min(4096, newAscanLen - TRIG_UNIT * 2));
        const int minTriggerOffsetSamples = -maxTriggerPreSamples;
        const int maxTriggerOffsetSamples = 1000000;
        int quantizedTriggerOffsetSamples =
            std::max(minTriggerOffsetSamples,
                     std::min(maxTriggerOffsetSamples, newTriggerOffsetSamples));
        const int triggerOffsetQuantum =
            (quantizedTriggerOffsetSamples < 0) ? TRIG_UNIT * 2 : TRIG_UNIT;
        quantizedTriggerOffsetSamples =
            (quantizedTriggerOffsetSamples / triggerOffsetQuantum) * triggerOffsetQuantum;
        if (quantizedTriggerOffsetSamples != newTriggerOffsetSamples)
        {
            ui->textEdit->append(QStringLiteral("触发偏移按硬件 8 样点单位量化，已从 %1 调整为 %2。")
                                 .arg(newTriggerOffsetSamples)
                                 .arg(quantizedTriggerOffsetSamples));
            newTriggerOffsetSamples = quantizedTriggerOffsetSamples;
            const QSignalBlocker blocker(ui->SB_triggerOffsetSamples);
            ui->SB_triggerOffsetSamples->setValue(newTriggerOffsetSamples);
        }
    }

    if (!ascanLenOk || newAscanLen <= 0
        || !adFileOffsetFramesOk || adFileOffsetFrames < 0
        || !triggerOffsetOk
        || AscanFreq <= 0 || galvoFreq <= 0.0 || !ascanLimitOk || !sampleRateOk)
    {
        ui->textEdit->append("内部参数无效，请检查采样率、Ascan 长度、Ascan 频率、Ascan 占空比、触发偏移、振镜频率和 Angio 跳过 Bscan 帧数。采样率和 Ascan 长度需为正数，Ascan 占空比需在 0 到 1 之间，触发偏移需为整数，跳过帧数需为非负整数。");
        return false;
    }

    const bool ascanLenChanged = (mainWidget::AscanLen != newAscanLen);
    mainWidget::AscanLen = newAscanLen;
    mainWidget::TriggerOffsetSamples = newTriggerOffsetSamples;
    updateAscanLenDependentUiState();
    resetSpectralWindow();

    if (ascanLenChanged || m_BG.size() != static_cast<std::vector<float>::size_type>(m_AscanLen))
        m_BG.assign(m_AscanLen, 0.0f);

    if (ascanLenChanged)
    {
        m_lastAscanForSave.clear();
        if (m_cosphy != nullptr)
            mkl_ascend(m_cosphy);
        if (m_sinphy != nullptr)
            mkl_ascend(m_sinphy);
    }

    return true;
}

double mainWidget::sampleRateMHzFromUi(bool *ok) const
{
    bool sampleRateOk = false;
    const double sampleRateMHz = ui->SampleRate->text().toDouble(&sampleRateOk);
    const bool valid = sampleRateOk && sampleRateMHz > 0.0;
    if (ok)
        *ok = valid;
    return valid ? sampleRateMHz : 0.0;
}

int mainWidget::maxAscanLenFromUi(bool *ok) const
{
    bool ascanFreqOk = false;
    const int ascanFreq = ui->AscanFreq->text().toInt(&ascanFreqOk);
    bool ascanDutyOk = false;
    const double ascanDutyCycle = ui->LE_AscanDutyCycle->text().toDouble(&ascanDutyOk);
    bool sampleRateOk = false;
    const double sampleRateMHz = sampleRateMHzFromUi(&sampleRateOk);
    const bool valid = ascanFreqOk && ascanFreq > 0
        && ascanDutyOk && ascanDutyCycle > 0.0 && ascanDutyCycle <= 1.0
        && sampleRateOk;
    if (ok)
        *ok = valid;
    if (!valid)
        return 0;

    const double maxAscanLen = sampleRateMHz * 1.0e6 / static_cast<double>(ascanFreq) * ascanDutyCycle;
    if (maxAscanLen > static_cast<double>((std::numeric_limits<int>::max)()))
        return (std::numeric_limits<int>::max)();
    return static_cast<int>(std::floor(maxAscanLen));
}

bool mainWidget::clampAscanLenToLimit(bool appendMessage)
{
    bool maxOk = false;
    const int maxAscanLen = maxAscanLenFromUi(&maxOk);
    bool ascanLenOk = false;
    const int currentAscanLen = ui->LE_AscanLen->text().toInt(&ascanLenOk);
    if (!maxOk || maxAscanLen <= 0 || !ascanLenOk || currentAscanLen <= maxAscanLen)
        return maxOk;

    {
        const QSignalBlocker blocker(ui->LE_AscanLen);
        ui->LE_AscanLen->setText(QString::number(maxAscanLen));
    }
    if (appendMessage)
    {
        ui->textEdit->append(QStringLiteral("Ascan 长度超过当前 Ascan 频率和占空比允许的上限，已自动调整为 %1。")
                             .arg(maxAscanLen));
    }
    return true;
}

void mainWidget::updateBscanCycleLengthFromFrequencies()
{
    bool ascanFreqOk = false;
    const int ascanFreq = ui->AscanFreq->text().toInt(&ascanFreqOk);
    bool frameRateOk = false;
    const double frameRate = ui->frameRate->text().toDouble(&frameRateOk);
    if (!ascanFreqOk || ascanFreq <= 0 || !frameRateOk || frameRate <= 0.0)
        return;

    const int bscanCycleLen = std::max(1, static_cast<int>(std::llround(
        static_cast<double>(ascanFreq) / frameRate)));
    const QSignalBlocker blocker(ui->LE_BscanCycleLen);
    ui->LE_BscanCycleLen->setText(QString::number(bscanCycleLen));
}

void mainWidget::updateFrameRateFromBscanCycleLength()
{
    bool ascanFreqOk = false;
    const int ascanFreq = ui->AscanFreq->text().toInt(&ascanFreqOk);
    bool bscanCycleOk = false;
    const int bscanCycleLen = ui->LE_BscanCycleLen->text().toInt(&bscanCycleOk);
    if (!ascanFreqOk || ascanFreq <= 0 || !bscanCycleOk || bscanCycleLen <= 0)
        return;

    const double frameRate = static_cast<double>(ascanFreq) / static_cast<double>(bscanCycleLen);
    const QSignalBlocker blocker(ui->frameRate);
    ui->frameRate->setText(QString::number(frameRate, 'f', 2));
}

bool mainWidget::readAngioRepFromUi(bool appendMessage)
{
    bool angioRepOk = false;
    int newAngioRep = ui->LE_AngioRep->text().toInt(&angioRepOk);
    const bool validAngioRep =
        angioRepOk && newAngioRep >= MinAngioRep && newAngioRep <= MaxAngioRep;
    if (!validAngioRep)
    {
        newAngioRep = DefaultAngioRep;
        ui->LE_AngioRep->setText(QString::number(newAngioRep));
        if (appendMessage)
        {
            ui->textEdit->append(QStringLiteral("Angio 重复数超出范围（%1-%2），已重置为 %3。")
                                 .arg(MinAngioRep)
                                 .arg(MaxAngioRep)
                                 .arg(DefaultAngioRep));
        }
    }

    mainWidget::AngioRep = newAngioRep;
    return validAngioRep;
}

void mainWidget::updateAscanLenDependentUiState()
{
    ui->spinBox->setRange(0, m_AscanLen / 2);
    ui->spinBox_2->setRange(0, m_AscanLen / 2);
    ui->cosplot->xAxis->setRange(0, m_AscanLen);
    ui->fftplot->xAxis->setRange(1, m_AscanLen / 2);
}

void mainWidget::resetSpectralWindow()
{
    spectralWindow.assign(m_AscanLen, 1.0);

    if (mainWidget::Wflag)
        return;

    const int zeroEdgeCount = 5;
    const int tukeyLength = m_AscanLen - zeroEdgeCount * 2;
    spectralWindow.assign(m_AscanLen, 0.0);
    if (tukeyLength <= 0)
        return;
    if (tukeyLength == 1)
    {
        spectralWindow[zeroEdgeCount] = 1.0;
        return;
    }

    const double alpha = 0.4;
    const double halfAlpha = alpha / 2.0;
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < tukeyLength; ++i)
    {
        const double x = static_cast<double>(i) / static_cast<double>(tukeyLength - 1);
        double value = 1.0;
        if (x < halfAlpha)
            value = 0.5 * (1.0 + cos(pi * (2.0 * x / alpha - 1.0)));
        else if (x > 1.0 - halfAlpha)
            value = 0.5 * (1.0 + cos(pi * (2.0 * x / alpha - 2.0 / alpha + 1.0)));
        spectralWindow[i + zeroEdgeCount] = value;
    }
}

void mainWidget::loadSettings()
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    seedCategorizedSettingsFromLegacy(settings);
    m_selectedDacDeviceId = DeviceSettings::selectedDacDeviceId(settings);
    m_selectedAdcDeviceId = DeviceSettings::selectedAdcDeviceId(settings);

    const QString dacGroup = DeviceSettings::dacSettingsGroup(m_selectedDacDeviceId);
    const QString adcGroup = DeviceSettings::adcSettingsGroup(m_selectedAdcDeviceId);
    const QString commonGroup = DeviceSettings::commonSettingsGroup();
    auto dacValue = [&settings, &dacGroup](const QString &key, const QVariant &fallback) {
        return groupedSettingValue(settings, dacGroup, key, fallback);
    };
    auto adcValue = [&settings, &adcGroup](const QString &key, const QVariant &fallback) {
        return groupedSettingValue(settings, adcGroup, key, fallback);
    };
    auto commonValue = [&settings, &commonGroup](const QString &key, const QVariant &fallback) {
        return groupedSettingValue(settings, commonGroup, key, fallback);
    };

    ui->amplitude->setText(dacValue(QStringLiteral("amplitude"), ui->amplitude->text()).toString());
    ui->frameRate->setText(dacValue(QStringLiteral("frameRate"), ui->frameRate->text()).toString());
    ui->AscanFreq->setText(dacValue(QStringLiteral("AscanFreq"), ui->AscanFreq->text()).toString());
    ui->LE_AscanDutyCycle->setText(dacValue(QStringLiteral("AscanDutyCycle"), ui->LE_AscanDutyCycle->text()).toString());
    ui->LE_BscanCycleLen->setText(dacValue(QStringLiteral("BscanCycleLen"), ui->LE_BscanCycleLen->text()).toString());
    ui->dutycycle->setText(dacValue(QStringLiteral("dutycycle"), ui->dutycycle->text()).toString());
    ui->CB_enableDAInSymphonic->setChecked(dacValue(QStringLiteral("enableDAInSymphonic"), ui->CB_enableDAInSymphonic->isChecked()).toBool());
    setComboBoxText(ui->comboBox_2, dacValue(QStringLiteral("fastAxis"), ui->comboBox_2->currentText()).toString());

    ui->LE_AscanLen->setText(adcValue(QStringLiteral("AscanLen"), ui->LE_AscanLen->text()).toString());
    ui->SampleRate->setText(adcValue(QStringLiteral("SampleRate"), ui->SampleRate->text()).toString());
    ui->SB_triggerOffsetSamples->setValue(adcValue(QStringLiteral("triggerOffsetSamples"), ui->SB_triggerOffsetSamples->value()).toInt());
    ui->LE_adFileOffsetFrames->setText(adcValue(QStringLiteral("adFileOffsetFrames"), ui->LE_adFileOffsetFrames->text()).toString());
    ui->CB_enableContinuousMode->setChecked(adcValue(QStringLiteral("continuousModeEnabled"), ui->CB_enableContinuousMode->isChecked()).toBool());
    ui->Line_continuousCount->setText(adcValue(QStringLiteral("continuousAlineCount"), ui->Line_continuousCount->text()).toString());
    setComboBoxText(ui->combo_triggerMode, adcValue(QStringLiteral("triggerMode"), ui->combo_triggerMode->currentText()).toString());
    setComboBoxText(ui->combo_clockMode, adcValue(QStringLiteral("clockMode"), ui->combo_clockMode->currentText()).toString());
    triggerMode = triggerModeFromText(ui->combo_triggerMode->currentText());
    clockMode = clockModeFromText(ui->combo_clockMode->currentText());
    ContinuousModeEnabled = ui->CB_enableContinuousMode->isChecked();
    ContinuousAlineCount = ui->Line_continuousCount->text().toInt();

    ui->LE_AngioRep->setText(commonValue(QStringLiteral("AngioRep"), ui->LE_AngioRep->text()).toString());
    ui->BscanLength->setText(commonValue(QStringLiteral("BscanLength"), ui->BscanLength->text()).toString());
    ui->Bscanlines->setText(commonValue(QStringLiteral("Bscanlines"), ui->Bscanlines->text()).toString());
    ui->CB_3D_showReatimeData->setChecked(commonValue(QStringLiteral("show3DRealtimeData"), ui->CB_3D_showReatimeData->isChecked()).toBool());
    ui->Line_realtimeShowInterval->setText(commonValue(QStringLiteral("realtimeShowInterval"), ui->Line_realtimeShowInterval->text()).toString());
    ui->CB_fourierOnSaved->setChecked(commonValue(QStringLiteral("fourierOnSaved"), ui->CB_fourierOnSaved->isChecked()).toBool());
    setComboBoxText(ui->comboBox, commonValue(QStringLiteral("scanMode"), ui->comboBox->currentText()).toString());

    ui->SB_convert_min->setValue(commonValue(QStringLiteral("convertMin"), ui->SB_convert_min->value()).toInt());
    ui->SB_convert_max->setValue(commonValue(QStringLiteral("convertMax"), ui->SB_convert_max->value()).toInt());
    ui->SB_projectionDepth->setValue(commonValue(QStringLiteral("projectionDepth"), ui->SB_projectionDepth->value()).toInt());

    updateBscanCycleLengthFromFrequencies();
    readSysParametersFromUi();
    const bool windowApplied = commonValue(QStringLiteral("windowApplied"), !mainWidget::Wflag).toBool();
    mainWidget::Wflag = !windowApplied;
    ui->addWindow->setStyleSheet(windowApplied
        ? "QPushButton{border-image:url(:/new/prefix1/removeWindow.png);}"
        : "QPushButton{border-image:url(:/new/prefix1/addWindow.png);}");
    resetSpectralWindow();

    ui->spinBox->setValue(commonValue(QStringLiteral("fftRangeStart"), ui->spinBox->value()).toInt());
    ui->spinBox_2->setValue(commonValue(QStringLiteral("fftRangeEnd"), ui->spinBox_2->value()).toInt());
    ui->spinBox_3->setValue(commonValue(QStringLiteral("displayMin"), ui->spinBox_3->value()).toInt());
    ui->spinBox_4->setValue(commonValue(QStringLiteral("displayMax"), ui->spinBox_4->value()).toInt());
    ui->spinBox_5->setValue(commonValue(QStringLiteral("filterX"), ui->spinBox_5->value()).toInt());
    ui->spinBox_6->setValue(commonValue(QStringLiteral("filterY"), ui->spinBox_6->value()).toInt());

    ui->doubleSpinBoxw0->setValue(commonValue(QStringLiteral("dispersionW0"), ui->doubleSpinBoxw0->value()).toDouble());
    ui->doubleSpinBoxa1->setValue(commonValue(QStringLiteral("dispersionA1"), ui->doubleSpinBoxa1->value()).toDouble());
    ui->doubleSpinBoxa2->setValue(commonValue(QStringLiteral("dispersionA2"), ui->doubleSpinBoxa2->value()).toDouble());
}

void mainWidget::saveSettings() const
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    DeviceSettings::saveSelectedDevices(settings, m_selectedDacDeviceId, m_selectedAdcDeviceId);

    const QString dacGroup = DeviceSettings::dacSettingsGroup(m_selectedDacDeviceId);
    const QString adcGroup = DeviceSettings::adcSettingsGroup(m_selectedAdcDeviceId);
    const QString commonGroup = DeviceSettings::commonSettingsGroup();

    setGroupedAndLegacyValue(settings, dacGroup, QStringLiteral("amplitude"), ui->amplitude->text());
    setGroupedAndLegacyValue(settings, dacGroup, QStringLiteral("frameRate"), ui->frameRate->text());
    setGroupedAndLegacyValue(settings, dacGroup, QStringLiteral("AscanFreq"), ui->AscanFreq->text());
    setGroupedAndLegacyValue(settings, dacGroup, QStringLiteral("AscanDutyCycle"), ui->LE_AscanDutyCycle->text());
    setGroupedAndLegacyValue(settings, dacGroup, QStringLiteral("BscanCycleLen"), ui->LE_BscanCycleLen->text());
    setGroupedAndLegacyValue(settings, dacGroup, QStringLiteral("dutycycle"), ui->dutycycle->text());
    setGroupedAndLegacyValue(settings, dacGroup, QStringLiteral("enableDAInSymphonic"), ui->CB_enableDAInSymphonic->isChecked());
    setGroupedAndLegacyValue(settings, dacGroup, QStringLiteral("fastAxis"), ui->comboBox_2->currentText());

    setGroupedAndLegacyValue(settings, adcGroup, QStringLiteral("AscanLen"), ui->LE_AscanLen->text());
    setGroupedAndLegacyValue(settings, adcGroup, QStringLiteral("SampleRate"), ui->SampleRate->text());
    setGroupedAndLegacyValue(settings, adcGroup, QStringLiteral("triggerOffsetSamples"), ui->SB_triggerOffsetSamples->value());
    setGroupedAndLegacyValue(settings, adcGroup, QStringLiteral("adFileOffsetFrames"), ui->LE_adFileOffsetFrames->text());
    setGroupedAndLegacyValue(settings, adcGroup, QStringLiteral("continuousModeEnabled"), ui->CB_enableContinuousMode->isChecked());
    setGroupedAndLegacyValue(settings, adcGroup, QStringLiteral("continuousAlineCount"), ui->Line_continuousCount->text());
    setGroupedAndLegacyValue(settings, adcGroup, QStringLiteral("triggerMode"), ui->combo_triggerMode->currentText());
    setGroupedAndLegacyValue(settings, adcGroup, QStringLiteral("clockMode"), ui->combo_clockMode->currentText());

    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("AngioRep"), QString::number(m_AngioRep));
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("BscanLength"), ui->BscanLength->text());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("Bscanlines"), ui->Bscanlines->text());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("show3DRealtimeData"), ui->CB_3D_showReatimeData->isChecked());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("realtimeShowInterval"), ui->Line_realtimeShowInterval->text());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("fourierOnSaved"), ui->CB_fourierOnSaved->isChecked());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("windowApplied"), !mainWidget::Wflag);
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("scanMode"), ui->comboBox->currentText());

    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("convertMin"), ui->SB_convert_min->value());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("convertMax"), ui->SB_convert_max->value());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("projectionDepth"), ui->SB_projectionDepth->value());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("fftRangeStart"), ui->spinBox->value());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("fftRangeEnd"), ui->spinBox_2->value());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("displayMin"), ui->spinBox_3->value());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("displayMax"), ui->spinBox_4->value());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("filterX"), ui->spinBox_5->value());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("filterY"), ui->spinBox_6->value());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("dispersionW0"), ui->doubleSpinBoxw0->value());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("dispersionA1"), ui->doubleSpinBoxa1->value());
    setGroupedAndLegacyValue(settings, commonGroup, QStringLiteral("dispersionA2"), ui->doubleSpinBoxa2->value());
    settings.sync();
}

void mainWidget::updateControlState()
{
    ui->startButton->setEnabled(m_daState == DAState::Ready && !m_scanActive);
    ui->stopButton->setEnabled(m_scanActive);
    ui->connectDA->setEnabled(!m_scanActive);
    ui->change_sysParams->setEnabled(!m_scanActive);
    ui->combo_triggerMode->setEnabled(!m_scanActive);
    ui->combo_clockMode->setEnabled(!m_scanActive);
    ui->CB_enableContinuousMode->setEnabled(!m_scanActive && !m_adReady);
    ui->Line_continuousCount->setEnabled(!m_scanActive && !m_adReady && ui->CB_enableContinuousMode->isChecked());
    ui->Line_realtimeShowInterval->setEnabled(!m_scanActive);
    ui->button_FFT->setEnabled(!m_scanActive);
    ui->CB_fourierOnSaved->setEnabled(!m_scanActive);
    ui->CB_enableDAInSymphonic->setEnabled(!m_scanActive);

    if (m_scanActive)
    {
        ui->startButton->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/start2.png);}");
        ui->stopButton->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/stopInitial.png);}");
        ui->connectButton->setEnabled(false);
    }
    else
    {
        ui->startButton->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/start.png);}");
        ui->stopButton->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/stopFinish.png);}");
        ui->connectButton->setEnabled(!m_adReady);
        ui->connectButton->setStyleSheet(m_adReady
            ? "QPushButton{border-image:url(:/new/prefix1/disconnect.png);}"
            : "QPushButton{border-image:url(:/new/prefix1/connect.png);}");
    }
}

bool mainWidget::selectedDacUsesPcie3640() const
{
    return DeviceSettings::isFcctecPcie3640Dac(m_selectedDacDeviceId);
}

bool mainWidget::selectedAdcUsesPcie3640() const
{
    return DeviceSettings::isFcctecPcie3640Adc(m_selectedAdcDeviceId);
}

QString mainWidget::selectedDacDeviceName() const
{
    return DeviceSettings::dacDeviceDisplayName(m_selectedDacDeviceId);
}

QString mainWidget::selectedAdcDeviceName() const
{
    return DeviceSettings::adcDeviceDisplayName(m_selectedAdcDeviceId);
}

bool mainWidget::symphonicDaOutputEnabled() const
{
    return mainWidget::scanMode == kSymphonicScanMode
        && ui->CB_enableDAInSymphonic->isChecked();
}

void mainWidget::stopPreparedOrRunningDAIfNeeded(bool forceStopDA)
{
    if (ssoctThread == nullptr)
        return;
    if (!selectedDacUsesPcie3640())
        return;
    if (forceStopDA || mainWidget::scanMode != kSymphonicScanMode || m_daState == DAState::Ready || m_daState == DAState::Scanning)
        ssoctThread->StopDAScan();
}

void mainWidget::killActiveTimers()
{
    if (timerId1 != 0)
    {
        killTimer(timerId1);
        timerId1 = 0;
    }
    if (timerId2 != 0)
    {
        killTimer(timerId2);
        timerId2 = 0;
    }
    if (timerId3 != 0)
    {
        killTimer(timerId3);
        timerId3 = 0;
    }
}

bool mainWidget::isVolumeScanMode() const
{
    return mainWidget::scanMode == 3 || mainWidget::scanMode == 32;
}

bool mainWidget::isContinuousAcquisitionMode() const
{
    return mainWidget::ContinuousModeEnabled
        && mainWidget::isContinuousSupportedMode(mainWidget::scanMode);
}

int mainWidget::continuousAcquisitionBufferCount() const
{
    if (!isContinuousAcquisitionMode())
        return 0;
    if (ssoctThread != nullptr && ssoctThread->bufferCount() > 0)
        return ssoctThread->bufferCount();
    return continuousBufferCount();
}

int mainWidget::expectedStoredBscanCount() const
{
    if (isContinuousAcquisitionMode() && mainWidget::scanMode != 1)
        return continuousStoredBscanCount();
    if (mainWidget::scanMode == 32)
        return m_CscanLen * m_AngioRep;
    if (mainWidget::scanMode == 3)
        return m_CscanLen;
    return 1;
}

int mainWidget::expectedAcquisitionBufferCount() const
{
    if (isContinuousAcquisitionMode())
        return continuousAcquisitionBufferCount();
    if (hasSegmentedVolumeScan())
    {
        const VolumeScanSegment &lastSegment = m_volumeScanSegments.back();
        return lastSegment.sourceBufferStart + lastSegment.sourceBufferCount;
    }

    const int logicalFrameCount = expectedStoredBscanCount();
    if (isVolumeScanMode() && ssoctThread != nullptr)
    {
        const int cyclesPerBuffer = std::max(1, ssoctThread->repeatCyclesPerBuffer());
        return static_cast<int>(ceilDiv(logicalFrameCount, cyclesPerBuffer));
    }
    return logicalFrameCount;
}

bool mainWidget::hasSegmentedVolumeScan() const
{
    return isVolumeScanMode() && m_volumeScanSegments.size() > 1;
}

int mainWidget::currentVolumeSegmentEndBuffer() const
{
    if (!hasSegmentedVolumeScan()
        || m_activeVolumeSegmentIndex < 0
        || m_activeVolumeSegmentIndex >= static_cast<int>(m_volumeScanSegments.size()))
        return 0;

    const VolumeScanSegment &segment = m_volumeScanSegments[m_activeVolumeSegmentIndex];
    return segment.sourceBufferStart + segment.sourceBufferCount;
}

bool mainWidget::configureVolumeSegmentationFromCurrentParams()
{
    m_volumeScanSegments.clear();
    m_activeVolumeSegmentIndex = 0;

    if (!isVolumeScanMode())
    {
        ssoctThread->SetVolumeSegmentAcquisitionPlan(0, 0);
        return true;
    }

    const int bscanCycleAlineCount = dacBscanCycleAlineCount(AscanFreq, galvoFreq);
    if (bscanCycleAlineCount <= 0)
    {
        ui->textEdit->append(QStringLiteral("3D DAC 分段估算失败：Bscan 周期长度无效。"));
        return false;
    }

    const int repeatFactor = volumeRepeatFactorForMode(mainWidget::scanMode);
    const long long samplesPerCscan =
        static_cast<long long>(bscanCycleAlineCount) * repeatFactor;
    if (samplesPerCscan <= 0 || bscanCycleAlineCount > MAX_ADD_TRIG_LEN)
    {
        ui->textEdit->append(QStringLiteral("3D DAC 分段估算失败：单个 Bscan 周期已超过 DAC 点数上限，无法通过分段解决。"));
        return false;
    }

    const long long fullYPointCount = samplesPerCscan * m_CscanLen;
    if (fullYPointCount <= MAX_ADD_TRIG_LEN)
    {
        ssoctThread->SetVolumeSegmentAcquisitionPlan(0, 0);
        return true;
    }

    const int maxCscanPerSegment = static_cast<int>(MAX_ADD_TRIG_LEN / samplesPerCscan);
    if (maxCscanPerSegment <= 0)
    {
        ui->textEdit->append(QStringLiteral("3D DAC 分段估算失败：每段可容纳的 Bscan 数为 0。"));
        return false;
    }

    const int estimatedSegmentCount = static_cast<int>(ceilDiv(m_CscanLen, maxCscanPerSegment));
    const QString message =
        QStringLiteral("当前 %1 的 Y 路 DAC 需要 %2 点，超过 PCIe3640 每路 %3 点上限。\n\n"
                       "按当前帧率、占空比和 Angio 重复次数估算，每段最多约 %4 个 Bscan，预计需要分 %5 段。\n\n"
                       "是否接受多段扫描？程序会按 Bscan 数量平均分段，并自动连续扫描。")
            .arg(scanModeTextForMode(mainWidget::scanMode))
            .arg(static_cast<qlonglong>(fullYPointCount))
            .arg(MAX_ADD_TRIG_LEN)
            .arg(maxCscanPerSegment)
            .arg(estimatedSegmentCount);
    const int answer = styledMessageBox(this,
                                        QMessageBox::Question,
                                        QStringLiteral("DAC 点数超过上限"),
                                        message,
                                        QMessageBox::Yes | QMessageBox::No,
                                        QMessageBox::Yes);
    if (answer != QMessageBox::Yes)
    {
        ui->textEdit->append(QStringLiteral("已取消多段扫描。"));
        return false;
    }

    const int cyclesPerBuffer = cyclesPerVolumeReadBuffer(m_AscanLen, bscanCycleAlineCount);
    if (cyclesPerBuffer <= 0)
    {
        ui->textEdit->append(QStringLiteral("3D DAC 分段估算失败：逻辑缓存帧参数无效。"));
        return false;
    }

    const int baseCscanCount = m_CscanLen / estimatedSegmentCount;
    const int extraCscanCount = m_CscanLen % estimatedSegmentCount;
    int cscanStart = 0;
    int sourceBufferStart = 0;
    for (int segmentIndex = 0; segmentIndex < estimatedSegmentCount; ++segmentIndex)
    {
        VolumeScanSegment segment;
        segment.cscanStart = cscanStart;
        segment.cscanCount = baseCscanCount + (segmentIndex < extraCscanCount ? 1 : 0);
        segment.logicalFrameCount = segment.cscanCount * repeatFactor;
        segment.sourceBufferStart = sourceBufferStart;
        segment.sourceBufferCount = static_cast<int>(ceilDiv(segment.logicalFrameCount, cyclesPerBuffer));
        if (segment.cscanCount <= 0 || segment.cscanCount > maxCscanPerSegment
            || segment.sourceBufferCount <= 0)
        {
            ui->textEdit->append(QStringLiteral("3D DAC 分段估算失败：第 %1 段参数无效。").arg(segmentIndex + 1));
            m_volumeScanSegments.clear();
            return false;
        }

        m_volumeScanSegments.push_back(segment);
        cscanStart += segment.cscanCount;
        sourceBufferStart += segment.sourceBufferCount;
    }

    ssoctThread->SetVolumeSegmentAcquisitionPlan(sourceBufferStart,
                                                 m_volumeScanSegments.front().sourceBufferCount);
    ui->textEdit->append(QStringLiteral("已启用 3D 多段扫描：共 %1 段，每段约 %2 个 Bscan。")
                         .arg(estimatedSegmentCount)
                         .arg(baseCscanCount + (extraCscanCount > 0 ? 1 : 0)));
    return true;
}

bool mainWidget::prepareVolumeSegmentDA(int segmentIndex)
{
    if (segmentIndex < 0 || segmentIndex >= static_cast<int>(m_volumeScanSegments.size()))
        return false;

    const VolumeScanSegment &segment = m_volumeScanSegments[segmentIndex];
    const int totalBufferCount = expectedAcquisitionBufferCount();
    ssoctThread->SetVolumeSegmentAcquisitionPlan(totalBufferCount, segment.sourceBufferCount);
    const bool ok = ssoctThread->ConfigureDA(Voltage, galvoFreq, AscanFreq,
                                             duty_cycle, mainWidget::scanMode,
                                             segment.cscanStart,
                                             segment.cscanCount,
                                             m_CscanLen);
    if (!ok)
    {
        const QString daError = ssoctThread->LastDAError();
        ui->textEdit->append(daError.isEmpty()
                             ? QStringLiteral("分段扫描：准备下一段 DA 失败。")
                             : QStringLiteral("分段扫描：准备下一段 DA 失败：%1").arg(daError));
        return false;
    }

    return true;
}

void mainWidget::onAcquisitionLoopFinished(int completedBuffers)
{
    if (!m_scanActive || !isVolumeScanMode())
        return;

    if (!hasSegmentedVolumeScan())
    {
        if (completedBuffers >= expectedAcquisitionBufferCount())
        {
            ui->textEdit->append("3D 扫描完成！");
            stopCurrentScan(false, true);
            saveclicked();
        }
        else
        {
            stopCurrentScan(true, true, QStringLiteral("3D 采集提前结束，已停止扫描并用零电压填补未完成缓存。"));
        }
        return;
    }

    if (completedBuffers < currentVolumeSegmentEndBuffer())
    {
        stopCurrentScan(true, true, QStringLiteral("3D 分段采集提前结束，已停止扫描并用零电压填补未完成缓存。"));
        return;
    }

    if (m_activeVolumeSegmentIndex + 1 >= static_cast<int>(m_volumeScanSegments.size()))
    {
        ui->textEdit->append("3D 分段扫描完成！");
        stopCurrentScan(false, true);
        saveclicked();
        return;
    }

    if (!startNextVolumeSegment())
        stopCurrentScan(true, true, QStringLiteral("分段扫描失败，已停止当前扫描并保留已采集缓存。"));
}

bool mainWidget::startNextVolumeSegment()
{
    ssoctThread->StopDAScan();
    ++m_activeVolumeSegmentIndex;

    if (!prepareVolumeSegmentDA(m_activeVolumeSegmentIndex))
        return false;
    if (!ssoctThread->RestartADForNextSegment())
        return false;
    if (!ssoctThread->StartDAScan(mainWidget::scanMode))
    {
        ui->textEdit->append(QStringLiteral("分段扫描：启动下一段 DA 失败。"));
        return false;
    }
    if (!QMetaObject::invokeMethod(ssoctThread, "entry", Qt::QueuedConnection))
    {
        ui->textEdit->append(QStringLiteral("分段扫描：启动下一段采集线程失败。"));
        return false;
    }

    ui->textEdit->append(QStringLiteral("开始第 %1/%2 段 3D 扫描。")
                         .arg(m_activeVolumeSegmentIndex + 1)
                         .arg(static_cast<int>(m_volumeScanSegments.size())));
    return true;
}

bool mainWidget::writeCurrentVolumeBscans(std::ofstream &file, int volumeCount, int sourceBufferCount)
{
    if (!hasSegmentedVolumeScan())
    {
        return writeCycleCroppedBscans(file,
                                      ssoctThread,
                                      m_AscanLen,
                                      m_BscanLen,
                                      volumeCount,
                                      sourceBufferCount);
    }

    int logicalFramesWritten = 0;
    for (const VolumeScanSegment &segment : m_volumeScanSegments)
    {
        if (!writeCycleCroppedBscans(file,
                                    ssoctThread,
                                    m_AscanLen,
                                    m_BscanLen,
                                    segment.logicalFrameCount,
                                    segment.sourceBufferCount,
                                    segment.sourceBufferStart))
            return false;
        logicalFramesWritten += segment.logicalFrameCount;
    }

    return logicalFramesWritten == volumeCount;
}

void mainWidget::stopCurrentScan(bool fillUnfinishedVolume,
                                 bool releaseAD,
                                 const QString &reason,
                                 bool forceStopDA)
{
    if (!reason.isEmpty())
        ui->textEdit->append(reason);

    if (m_volumeScanTimingActive)
    {
        const double elapsedSeconds = static_cast<double>(m_volumeScanTimer.elapsed()) / 1000.0;
        ui->textEdit->append(QStringLiteral("%1 扫描耗时：%2 秒。")
                             .arg(scanModeTextForMode(m_volumeScanTimingMode))
                             .arg(elapsedSeconds, 0, 'f', 2));
        m_volumeScanTimingActive = false;
        m_volumeScanTimingMode = 0;
    }

    killActiveTimers();
    mainWidget::captureflag = 0;
    VesselAudioPlayer::instance().stop();

    if (forceStopDA || m_scanActive || m_daState == DAState::Scanning || m_daState == DAState::Ready)
        stopPreparedOrRunningDAIfNeeded(forceStopDA);

    if (releaseAD && m_adReady)
    {
        ssoctThread->StopADCapture();
        firstThread->quit();
        firstThread->wait();
        m_adReady = false;
    }

    if (fillUnfinishedVolume && isContinuousAcquisitionMode())
        ssoctThread->FillUncompletedBuffersWithZeroVoltage(continuousAcquisitionBufferCount());
    else if (fillUnfinishedVolume && isVolumeScanMode())
        ssoctThread->FillUncompletedBuffersWithZeroVoltage(expectedAcquisitionBufferCount());

    m_scanActive = false;
    m_daState = DAState::NotReady;
    updateControlState();
}

bool mainWidget::prepareDAFromUi()
{
    if (!readScanParametersFromUi())
    {
        m_daState = DAState::NotReady;
        return false;
    }
    saveSettings();

    if (!selectedDacUsesPcie3640())
    {
        m_daState = DAState::NotReady;
        ui->textEdit->append(QStringLiteral("%1 DAC 适配层尚未实现，当前不会调用 PCIe3640 DA 输出。")
                             .arg(selectedDacDeviceName()));
        return false;
    }

    if (mainWidget::scanMode == kSymphonicScanMode)
    {
        const QString wavPath = vesselScanWavPath();
        VesselWavInfo wavInfo;
        QString wavErrorMessage;
        if (!readVesselWavInfo(wavPath, &wavInfo, &wavErrorMessage))
        {
            m_daState = DAState::NotReady;
            ui->textEdit->append(QStringLiteral("Symphonic 模式需要有效的 vessel_scan_path.wav：%1").arg(wavErrorMessage));
            return false;
        }

        DaPathInfo scanXInfo;
        DaPathInfo scanYInfo;
        QString scanXPathError;
        QString scanYPathError;
        const QString scanXPath = defaultDialogPath() + QStringLiteral("/scanX.txt");
        const QString scanYPath = defaultDialogPath() + QStringLiteral("/scanY.txt");
        if (!readDaPathInfo(scanXPath, &scanXInfo, &scanXPathError)
            || !readDaPathInfo(scanYPath, &scanYInfo, &scanYPathError))
        {
            m_daState = DAState::NotReady;
            ui->textEdit->append(QStringLiteral("Symphonic 模式需要有效且已按同一 Bscan 周期生成的 scanX.txt/scanY.txt：%1%2")
                                 .arg(scanXPathError)
                                 .arg(scanYPathError));
            return false;
        }
        if (scanXInfo.pointCount != scanYInfo.pointCount)
        {
            m_daState = DAState::NotReady;
            ui->textEdit->append(QStringLiteral("Symphonic 路径文件长度不一致：scanX=%1, scanY=%2。请重新勾选“生成音频”并导出路径。")
                                 .arg(scanXInfo.pointCount)
                                 .arg(scanYInfo.pointCount));
            return false;
        }

        bool bscanCycleLenOk = false;
        const int bscanCycleLen = ui->LE_BscanCycleLen->text().toInt(&bscanCycleLenOk);
        if (!bscanCycleLenOk || bscanCycleLen <= 0)
        {
            m_daState = DAState::NotReady;
            ui->textEdit->append(QStringLiteral("Symphonic 模式需要有效的 Bscan 周期长度。"));
            return false;
        }
        if (scanXInfo.pointCount != bscanCycleLen)
        {
            m_daState = DAState::NotReady;
            ui->textEdit->append(QStringLiteral("Symphonic 路径文件应为完整 Bscan 周期：scanX/scanY=%1 Aline，但当前 BscanCycleLen=%2。请重新生成音频并导出路径。")
                                 .arg(scanXInfo.pointCount)
                                 .arg(bscanCycleLen));
            return false;
        }
        const int wavCycleAlineCount = static_cast<int>(std::llround(
            static_cast<double>(wavInfo.frameCount) * static_cast<double>(AscanFreq)
            / static_cast<double>(wavInfo.sampleRate)));
        if (wavCycleAlineCount != bscanCycleLen)
        {
            m_daState = DAState::NotReady;
            ui->textEdit->append(QStringLiteral("Symphonic WAV 时长与 Bscan 周期不匹配：wav=%1 音频点，按 AscanFreq=%2 Hz 换算为 %3 Aline，但当前 BscanCycleLen=%4。请重新生成音频并导出路径。")
                                 .arg(wavInfo.frameCount)
                                 .arg(AscanFreq)
                                 .arg(wavCycleAlineCount)
                                 .arg(bscanCycleLen));
            return false;
        }
        if (bscanCycleLen < m_BscanLen)
        {
            m_daState = DAState::NotReady;
            ui->textEdit->append(QStringLiteral("Symphonic BscanCycleLen(%1) 小于 BscanLength(%2)，无法从完整周期中分离有效路径。")
                                 .arg(bscanCycleLen)
                                 .arg(m_BscanLen));
            return false;
        }

        const bool noReturnToZero = vesselPathNoReturnToZeroFromSettings();
        const int totalMoveAlineCount = bscanCycleLen - m_BscanLen;
        int moveAlineCount = totalMoveAlineCount;
        if (!noReturnToZero)
        {
            if (totalMoveAlineCount % 2 != 0)
            {
                m_daState = DAState::NotReady;
                ui->textEdit->append(QStringLiteral("Symphonic 返回原点模式要求 BscanCycleLen(%1) - BscanLength(%2) 能平均分成起止两个线性运动段。")
                                     .arg(bscanCycleLen)
                                     .arg(m_BscanLen));
                return false;
            }
            moveAlineCount = totalMoveAlineCount / 2;
        }
        const bool enableSymphonicDA = symphonicDaOutputEnabled();
        ssoctThread->ConfigureSymphonicTiming(AscanFreq, moveAlineCount, noReturnToZero);
        const bool symphonicHardwareReady = enableSymphonicDA
            ? ssoctThread->ConfigureSymphonicDAFromBoundPath(AscanFreq)
            : ssoctThread->ConfigureSymphonicRfOnly(AscanFreq);
        if (!symphonicHardwareReady)
        {
            m_daState = DAState::NotReady;
            const QString daError = ssoctThread->LastDAError();
            ui->textEdit->append(daError.isEmpty()
                ? QStringLiteral("Symphonic PCIe3640 时基准备失败。")
                : QStringLiteral("Symphonic PCIe3640 时基准备失败：%1").arg(daError));
            return false;
        }

        m_daState = DAState::Ready;
        ui->textEdit->append(QStringLiteral("Symphonic 已准备，将从 3.5mm 耳机孔循环输出：%1%2；PCIe3640 RF 时基已准备，DACH1/DACH2 %3；完整周期 %4 Aline / %5 音频点，有效路径 %6 Aline，线性运动段 %7 Aline，返回模式：%8。")
                             .arg(wavPath)
                             .arg(enableSymphonicDA
                                  ? QStringLiteral("；并同步输出绑定的 DA 测试数据")
                                  : QString())
                             .arg(enableSymphonicDA
                                  ? QStringLiteral("输出绑定路径")
                                  : QStringLiteral("保持 0V"))
                             .arg(bscanCycleLen)
                             .arg(wavInfo.frameCount)
                             .arg(m_BscanLen)
                             .arg(moveAlineCount)
                             .arg(noReturnToZero
                                  ? QStringLiteral("终点线性返回起点")
                                  : QStringLiteral("0V 到起点，终点回 0V")));
        return true;
    }

    if (!configureVolumeSegmentationFromCurrentParams())
    {
        m_daState = DAState::NotReady;
        return false;
    }

    bool br = false;
    if (hasSegmentedVolumeScan())
        br = prepareVolumeSegmentDA(0);
    else
        br = ssoctThread->ConfigureDA(Voltage, galvoFreq, AscanFreq,
                                      duty_cycle, mainWidget::scanMode);
    if (br) {
        m_daState = DAState::Ready;
        if (hasSegmentedVolumeScan())
            ui->textEdit->append(QStringLiteral("DA 已准备，已传输第 1/%1 段扫描路径。")
                                 .arg(static_cast<int>(m_volumeScanSegments.size())));
        else
            ui->textEdit->append("DA 已准备，扫描路径已传输。");
        return true;
    }

    m_daState = DAState::NotReady;
    const QString daError = ssoctThread->LastDAError();
    if (daError.isEmpty())
        ui->textEdit->append(QStringLiteral("传输扫描参数失败！"));
    else
        ui->textEdit->append(QStringLiteral("传输扫描参数失败：%1").arg(daError));
    return false;
}

void mainWidget::on_AutoDecideAscanLength_clicked()
{
    bool maxOk = false;
    const int maxAscanLen = maxAscanLenFromUi(&maxOk);
    if (!maxOk || maxAscanLen <= 0)
    {
        ui->textEdit->append(QStringLiteral("Ascan 长度自动计算失败：请检查 Ascan 频率和 Ascan 占空比。"));
        return;
    }

    ui->LE_AscanLen->setText(QString::number(maxAscanLen));
    readSysParametersFromUi();
    saveSettings();
}

// 直接通过给定的数据计算出 m_BscanLen 的值，但是不会立即更新给 m_BscanLen, 需要点击 connectDA 按钮
void mainWidget::on_AutoDecideBscanLength_clicked()
{
    if (m_daState == DAState::Ready)
    {
        stopCurrentScan(false, true, "扫描参数已改变，请重新准备 DA。");
    }
    bool bscanCycleOk = false;
    const int bscanCycleLen = ui->LE_BscanCycleLen->text().toInt(&bscanCycleOk);
    bool dutyCycleOk = false;
    const double bscanDutyCycle = ui->dutycycle->text().toDouble(&dutyCycleOk);
    if (!bscanCycleOk || bscanCycleLen <= 0 || !dutyCycleOk || bscanDutyCycle <= 0.0)
    {
        ui->textEdit->append(QStringLiteral("Bscan 长度自动计算失败：请检查 Bscan 周期长度和 Bscan 占空比。"));
        return;
    }
    int temp_BscanLen = int(static_cast<double>(bscanCycleLen) * bscanDutyCycle);
    ui->BscanLength->setText(QString::number(temp_BscanLen));
    saveSettings();
    // m_BscanLen = temp_BscanLen;
}

void mainWidget::on_V_ConvertAngioToImage_clicked()
{
    saveSettings();

    AngioFileSelection fileSelection;
    if (!selectAngioFileAndOptions(this, &fileSelection)) {
        return;
    }
    const QString filePath = fileSelection.filePath;
    saveAngio3dPath(filePath);

    VesselProjectionParams params;
    params.ascanLen = m_AscanLen;
    params.bscanLen = ui->BscanLength->text().toInt();
    params.cscanLen = ui->Bscanlines->text().toInt();
    params.angioRep = m_AngioRep;
    bool adFileOffsetFramesOk = false;
    params.adFileOffsetFrames = ui->LE_adFileOffsetFrames->text().toInt(&adFileOffsetFramesOk);
    if (!adFileOffsetFramesOk || params.adFileOffsetFrames < 0) {
        ui->textEdit->append("Angio 跳过 Bscan 帧数无效，请输入非负整数。");
        return;
    }
    params.cropZStart = ui->SB_convert_min->value();
    params.cropZEnd = std::min(ui->SB_convert_max->value(), params.ascanLen);
    params.previewDepth = params.cropZEnd;
    params.projectionDepth = ui->SB_projectionDepth->value();
    params.generatePreviewImage = fileSelection.generatePreviewImage;
    params.generateGrayscaleImage = fileSelection.generateGrayscaleImage;
    params.generatePixelWiseFlowSpeedImage = fileSelection.generatePixelWiseFlowSpeedImage;
    params.generateAveragedFlowSpeedImage = fileSelection.generateAveragedFlowSpeedImage;
    params.generateSegmentWiseFlowSpeedImage = fileSelection.generateSegmentWiseFlowSpeedImage;
    params.generateFlowSpeedFitCorrelationImage = fileSelection.generateFlowSpeedFitCorrelationImage;
    params.useFlowSpeedSkeletonDenoise = fileSelection.useFlowSpeedSkeletonDenoise;
    params.useFlowSpeedManualMask = fileSelection.useFlowSpeedManualMask;
    params.flowSpeedCropTop = fileSelection.flowSpeedCropTop;
    params.flowSpeedCropBottom = fileSelection.flowSpeedCropBottom;
    params.flowSpeedCropLeft = fileSelection.flowSpeedCropLeft;
    params.flowSpeedCropRight = fileSelection.flowSpeedCropRight;
    if (params.cropZEnd != ui->SB_convert_max->value()) {
        ui->textEdit->append(QStringLiteral("转换深度上限超过 AscanLen，已使用 %1。").arg(params.cropZEnd));
    }

    ui->textEdit->append("开始从 Angio 文件生成彩色血管投影图。");
    ui->V_ConvertAngioToImage->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);

    QString errorMessage;
    const bool ok = convertAngio3dToColorProjection(
        filePath,
        params,
        [this](const QString &message) {
            ui->textEdit->append(message);
        },
        [this](const QString &message) {
            appendInfoLogLine(message);
        },
        [this](const VesselProjectionFileSizeInfo &sizeInfo) {
            ui->textEdit->append(QStringLiteral("3D Angio 文件大小和程序要求不匹配。"));
            ui->textEdit->append(QStringLiteral("本文件大小：%1 个样点。").arg(sizeInfo.fileSamples));
            ui->textEdit->append(QStringLiteral("程序要求：%1 个样点（Cscan 长度 %2）。")
                                 .arg(sizeInfo.requiredSamples)
                                 .arg(sizeInfo.programCscanLen));
            ui->textEdit->append(QStringLiteral("根据文件大小推断的 Cscan 长度：%1。")
                                 .arg(sizeInfo.inferredCscanLen));
            if (sizeInfo.unusedSamples > 0) {
                ui->textEdit->append(QStringLiteral("按推断 Cscan 长度计算后，末尾 %1 个样点不会参与计算。")
                                     .arg(sizeInfo.unusedSamples));
            }
            if (sizeInfo.hasPartialFrame) {
                ui->textEdit->append(QStringLiteral("提示：文件末尾存在不足一个完整 Angio 板块的数据。"));
            }

            QString message = QStringLiteral(
                "读取的 .3d 文件大小和程序要求不匹配。\n\n"
                "本文件大小：%1 个样点\n"
                "程序要求：%2 个样点（Cscan 长度 %3）\n"
                "推断出的 Cscan 长度：%4\n"
                "按推断长度读取：%5 个样点")
                .arg(sizeInfo.fileSamples)
                .arg(sizeInfo.requiredSamples)
                .arg(sizeInfo.programCscanLen)
                .arg(sizeInfo.inferredCscanLen)
                .arg(sizeInfo.inferredRequiredSamples);
            if (sizeInfo.unusedSamples > 0) {
                message += QStringLiteral("\n末尾 %1 个样点不会参与计算。").arg(sizeInfo.unusedSamples);
            }
            if (sizeInfo.hasPartialFrame) {
                message += QStringLiteral("\n文件末尾存在不足一个完整 Angio 板块的数据。");
            }
            message += QStringLiteral("\n\n是否使用新的 Cscan 长度进行计算？");

            QApplication::restoreOverrideCursor();
            const int ret = styledMessageBox(this,
                                             QMessageBox::Question,
                                             tr("文件大小不匹配"),
                                             message,
                                             QMessageBox::Yes | QMessageBox::No,
                                             QMessageBox::Yes);
            QApplication::setOverrideCursor(Qt::WaitCursor);
            return ret == QMessageBox::Yes;
        },
        &errorMessage);

    QApplication::restoreOverrideCursor();
    ui->V_ConvertAngioToImage->setEnabled(true);

    if (!ok) {
        ui->textEdit->append(QStringLiteral("生成彩色血管投影图失败：%1").arg(errorMessage));
        styledMessageBox(this,
                         QMessageBox::Warning,
                         tr("转换失败"),
                         QStringLiteral("生成彩色血管投影图失败：%1").arg(errorMessage),
                         QMessageBox::Ok,
                         QMessageBox::Ok);
        return;
    }

    saveVesselImagePath(colorProjectionPathForAngioFile(filePath));
    ui->textEdit->append("彩色血管投影图生成完成。");
}

void mainWidget::on_V_ConvertImageToPath_clicked()
{
    saveSettings();
    bool ascanFreqOk = false;
    const int pathAscanFreq = ui->AscanFreq->text().toInt(&ascanFreqOk);
    bool bscanLenOk = false;
    const int pathBscanLen = ui->BscanLength->text().toInt(&bscanLenOk);
    bool bscanCycleLenOk = false;
    const int pathBscanCycleLen = ui->LE_BscanCycleLen->text().toInt(&bscanCycleLenOk);
    VesselFindingDialog dialog(ui->amplitude->text().toDouble(),
                               ascanFreqOk ? pathAscanFreq : 48000,
                               bscanLenOk ? pathBscanLen : 0,
                               bscanCycleLenOk ? pathBscanCycleLen : 0,
                               this);
    if (dialog.exec() == QDialog::Accepted) {
        ui->textEdit->append(QStringLiteral("血管模式导出路径成功！"));
    }
}

void mainWidget::on_connectDA_clicked()
{
    if (m_scanActive)
        stopCurrentScan(true, true, "正在扫描，已先停止当前扫描并保留缓存数据。");
    else if (m_adReady)
    {
        ssoctThread->StopADCapture();
        firstThread->quit();
        firstThread->wait();
        m_adReady = false;
        m_daState = DAState::NotReady;
    }
    else if (m_daState == DAState::Ready)
    {
        stopPreparedOrRunningDAIfNeeded();
        m_daState = DAState::NotReady;
    }

    prepareDAFromUi();
    updateControlState();
    return;
}

void mainWidget::on_change_sysParams_clicked()
{
    if (m_scanActive)
    {
        ui->textEdit->append("正在扫描，无法更新内部参数。");
        updateControlState();
        return;
    }

    if (!readSysParametersFromUi())
    {
        updateControlState();
        return;
    }

    if (m_daState == DAState::Ready)
    {
        stopCurrentScan(false, true, "内部参数已更新，请重新准备 DA。");
    }
    else
    {
        ui->textEdit->append("内部参数已更新。");
    }

    saveSettings();
    updateControlState();
}

// 调用位置：mainWidget 构造函数，第 5 步（初始化 DA 卡）中
void mainWidget::PrintBoardInfo()
{
    PCIE_BUF pcieBuf = {0};
    HANDLE pcieHandle = PCIe3640_Link(0, &pcieBuf);
    if (pcieHandle == INVALID_HANDLE_VALUE)
    {
        ui->textEdit->append("PCIe3640 采集卡打开失败，无法读取板卡信息！");
        return;
    }

    CARD_INFO cardInfo = {0};
    if (!PCIe3640_GetDevInfo(pcieHandle, &cardInfo))
    {
        ui->textEdit->append("读取 PCIe3640 板卡信息失败！");
        PCIe3640_UnLink(pcieHandle);
        return;
    }

    ui->textEdit->append("PCIe3640 板卡信息：");
    ui->textEdit->append("板卡版本号：" + QString::number(cardInfo.CARD_VER));
    ui->textEdit->append("AD 位数：" + QString::number(cardInfo.AD_BIT) + " bit");
    ui->textEdit->append("AD 通道数：" + QString::number(cardInfo.AD_CHCNT) + " CH");
    ui->textEdit->append("AD 采样率：" + QString::number(cardInfo.AD_SPEED) + " MHz");
    ui->textEdit->append("AD 板载 FIFO：" + QString::number(cardInfo.AD_FIFO & 0xffff) + " G采样点");
    ui->textEdit->append("板卡温度：" + QString::number(PCIe3640_GetTemp(pcieHandle), 'f', 1) + " 摄氏度");
    PCIe3640_UnLink(pcieHandle);
}

mainWidget::~mainWidget()
{
    saveSettings();
    stopCurrentScan(false, true);

    firstThread->quit();
    firstThread->wait();

    killActiveTimers();

    delete ui;
}

// 下拉框，区别扫描模式
void mainWidget::changeScanMode(const QString &arg1)
{
    if(arg1 == "1D scan")
        mainWidget::scanMode = 1;
    else if(arg1 == "2D cross scan")
        mainWidget::scanMode = 10;
    else if(arg1 == "2D repeat")
        mainWidget::scanMode = 2;
    else if(arg1 == "2D angio")
        mainWidget::scanMode = 22;
    else if(arg1 == "3D scan")
        mainWidget::scanMode = 3;
    else if(arg1 == "3D angio")
        mainWidget::scanMode = 32;
    else if(arg1 == "Vessel scan")
        mainWidget::scanMode = 42;
    else if(arg1 == "Symphonic")
        mainWidget::scanMode = kSymphonicScanMode;
    // else if(arg1 == "2D fchange")
    //     mainWidget::scanMode = 23;
    // else if(arg1 == "2D coarsefc")
    //     mainWidget::scanMode = 24;
    else
    {
        ui->textEdit->append("未知的扫描模式! ");
        return;
    }

    ui->textEdit->append("扫描模式：" + QVariant(arg1).toString());
}

void mainWidget::on_comboBox_activated(const QString &arg1)
{
    if (m_daState == DAState::Ready)
    {
        stopCurrentScan(false, true, "扫描模式已改变，请重新准备 DA。");
    }
    changeScanMode(arg1);
    saveSettings();
}

void mainWidget::on_comboBox_2_activated(const QString &arg1)
{
    if (m_daState == DAState::Ready)
    {
        stopCurrentScan(false, true, "振镜模式已改变，请重新准备 DA。");
    }

    applyFastAxisMode(arg1, true);
    saveSettings();
}

void mainWidget::on_combo_triggerMode_activated(const QString &arg1)
{
    triggerMode = triggerModeFromText(arg1);
    if (m_adReady)
    {
        ssoctThread->StopADCapture();
        firstThread->quit();
        firstThread->wait();
        m_adReady = false;
        m_daState = DAState::NotReady;
        ui->textEdit->append(QStringLiteral("触发模式已改变，请重新连接采集卡。"));
    }

    ui->textEdit->append(QStringLiteral("触发模式：") + triggerModeToText(triggerMode));
    saveSettings();
    updateControlState();
}

void mainWidget::on_combo_clockMode_activated(const QString &arg1)
{
    clockMode = clockModeFromText(arg1);
    if (m_adReady)
    {
        ssoctThread->StopADCapture();
        firstThread->quit();
        firstThread->wait();
        m_adReady = false;
        m_daState = DAState::NotReady;
        ui->textEdit->append(QStringLiteral("时钟模式已改变，请重新连接采集卡。"));
    }

    ui->textEdit->append(QStringLiteral("时钟模式：") + clockModeToText(clockMode));
    if (clockMode == ADClockMode::External)
    {
        ui->textEdit->append(QStringLiteral("外部时钟需要按说明书将 J1 选择为板外参考时钟，并从 J2 接入 10 MHz 参考。"));
    }
    saveSettings();
    updateControlState();
}

void mainWidget::applyFastAxisMode(const QString &arg1, bool appendMessage)
{
    if (appendMessage) {
        ui->textEdit->append("振镜模式：" + QVariant(arg1).toString());
    }
}


void mainWidget::PrepareProcessing1D()
{
    mkl_free_buffers();
    mkl_thread_free_buffers();
}


void mainWidget::on_startButton_clicked()
{
    saveSettings();
    if (m_scanActive)
        return;
    if (m_daState != DAState::Ready)
    {
        ui->textEdit->append("DA 尚未准备，请先点击“更新数据并初始化 DAC”。");
        updateControlState();
        return;
    }
    if (!firstThread->isRunning())
        firstThread->start();

    const bool reinitializingPreparedAD = m_adReady;
    if (!selectedAdcUsesPcie3640())
    {
        ui->textEdit->append(QStringLiteral("%1 ADC 适配层尚未实现，无法开始采集。")
                             .arg(selectedAdcDeviceName()));
        firstThread->quit();
        firstThread->wait();
        m_adReady = false;
        updateControlState();
        return;
    }
    if (!ssoctThread->InitADForCapture())
    {
        ui->textEdit->append("采集卡初始化失败，无法开始扫描！");
        firstThread->quit();
        firstThread->wait();
        m_adReady = false;
        updateControlState();
        return;
    }
    m_adReady = true;
    ui->textEdit->append(reinitializingPreparedAD
                         ? QStringLiteral("采集卡已在开始扫描前重新初始化，FIFO 已清空。")
                         : QStringLiteral("采集卡初始化成功。"));

    mainWidget::captureflag = 1;
    m_scanActive = true;
    m_daState = DAState::Scanning;
    crossSave = nullptr;
    if (mainWidget::scanMode == 1)
        m_lastAscanForSave.clear();
    OCTplots();
    if (!m_scanActive)
    {
        updateControlState();
        return;
    }
    if (!QMetaObject::invokeMethod(ssoctThread, "entry", Qt::QueuedConnection))
    {
        ui->textEdit->append(QStringLiteral("启动采集线程失败！"));
        stopCurrentScan(false, true);
        return;
    }
    updateControlState();
    ui->textEdit->append("开始扫描...");
}

// 谁写的拼写错误？！之后可能需要修改 ui 里面的对象名称
void mainWidget::on_connectButton_clicked()
{
    if (m_adReady)
    {
        ui->textEdit->append("采集卡已经连接。");
        updateControlState();
        return;
    }
    if (m_daState != DAState::Ready && !prepareDAFromUi())
    {
        updateControlState();
        return;
    }

    firstThread->start();
    if (!selectedAdcUsesPcie3640())
    {
        ui->textEdit->append(QStringLiteral("%1 ADC 适配层尚未实现，无法连接采集卡。")
                             .arg(selectedAdcDeviceName()));
        firstThread->quit();
        firstThread->wait();
        m_adReady = false;
        updateControlState();
        return;
    }
    if (!ssoctThread->InitADForCapture())
    {
        ui->textEdit->append("连接采集卡失败！");
        firstThread->quit();
        firstThread->wait();
        m_adReady = false;
        updateControlState();
        return;
    }

    m_adReady = true;
    ui->textEdit->append("连接采集卡成功！");
    updateControlState();
}

void mainWidget::bufferRefreshIndex(int &bufferCompleted, int &curIndexInMEMbuffer, int &availbleIndex)
{
    QMutexLocker lock(&ssoctThread->mutex1);
    bufferCompleted = ssoctThread->m_buffersCompleted;
    curIndexInMEMbuffer = ssoctThread->m_curIndexInMemBuffer;
    availbleIndex = ssoctThread->m_lastAvailableIndex;
}

void mainWidget::removeBGForAscan(float* datainfer, unsigned short* AscanData)
{
    for (int i = 0; i < m_AscanLen; ++i)
        datainfer[i] = ((AscanData[i] >> 4) - m_BG[i] - mythread::plotOffset) * spectralWindow[i];
}

void mainWidget::removeBGForBscan(float* datainfer, unsigned short* BscanData)
{
    for (int j = 0; j < m_BscanLen; j++)
    {
        for (int i = 0; i < m_AscanLen; i++)
            datainfer[j * m_AscanLen + i ] = ((BscanData[j * m_AscanLen + i ] >> 4) - m_BG[i] - mythread::plotOffset) * spectralWindow[i];
    }
}

void mainWidget::removeBGForBscanDouble(float* datainfer, unsigned short* BscanDataAngio1, unsigned short* BscanDataAngio2)
{
    for (int j = 0; j < m_BscanLen; j++)
    {
        for (int i = 0; i < m_AscanLen; i++)
        {
            datainfer[j * m_AscanLen + i ] = ((BscanDataAngio1[j * m_AscanLen + i ] >> 4) - m_BG[i] - mythread::plotOffset) * spectralWindow[i];
            datainfer[(j + m_BscanLen) * m_AscanLen + i ] = ((BscanDataAngio2[j * m_AscanLen + i ] >> 4) - m_BG[i] - mythread::plotOffset) * spectralWindow[i];            
        }
    }
}

// 将输入的实空间 Bscan 数据直接按 tmp1 作为最小值、tmp2 作为最大值进行归一化处理，存入 img 矩阵
// normalizeBscan 和 writeBscan 函数输入的矩阵均为 AscanLength * BscanLength 的数组，而返回均为 (AscanLength/2) * BscanLength 的 cv::Mat 矩阵，因为 FFT 而截取了前半部分
cv::Mat mainWidget::normalizeBscan(float* num, int tmp1, int tmp2, int offset = 0)
{
    cv::Mat img(m_AscanLen / 2, m_BscanLen, CV_32F);
    for (int j = 0; j < img.cols; j++)
    {
        for (int i = 0; i < img.rows; i++)
        {
            float currentValue = num[j * m_AscanLen + i + offset];
            if (currentValue < tmp1)
                img.at<float>(i, j) = 0;    
            else if (currentValue > tmp2)
                img.at<float>(i, j) = 1;
            else
                img.at<float>(i, j) = (currentValue - tmp1) / (tmp2 - tmp1);
        }
    }
    return img;
}

cv::Mat mainWidget::normalizeBscanFlatten(float* num, int tmp1, int tmp2, int offset = 0)
{
    cv::Mat img(150, m_BscanLen, CV_32F);
    for (int j = 0; j < img.cols; j++)
    {
        for (int i = 0; i < img.rows; i++)
        {
            float currentValue = num[j * 150 + i + offset];
            if (currentValue < tmp1)
                img.at<float>(i, j) = 0;    
            else if (currentValue > tmp2)
                img.at<float>(i, j) = 1;
            else
                img.at<float>(i, j) = (currentValue - tmp1) / (tmp2 - tmp1);
        }
    }
    return img;
}

cv::Mat mainWidget::writeBscan(float* num)
{
    cv::Mat img(m_AscanLen / 2, m_BscanLen, CV_32F);
    for (int j = 0; j < img.cols; j++)
    {
        for (int i = 0; i < img.rows; i++)
            img.at<float>(i, j) = num[j * m_AscanLen + i];
    }
    return img;
}

// 把归一化到 [0,1] 之间的浮点型矩阵转为 8 位图像
// 要求矩阵的大小为 (AscanLength/2) * BscanLength
cv::Mat mainWidget::norm_mat_to_8bit(cv::Mat img)
{
    cv::Mat img2(m_AscanLen / 2, m_BscanLen, CV_32F);
    img2 = img * 255;
    Mat img_8bit;           // cv::Mat???
    img2.convertTo(img_8bit, CV_8UC1);
    img2.release();
    return img_8bit;
}

void mainWidget::timerEvent(QTimerEvent *event)
{
    // 只要通过 startTimer 启动了定时器（并保存返回的 int id），Qt 的事件循环会按设定间隔向该对象派发 QTimerEvent
    // 1. 如果收到了 timerId1 的定时器事件（1D scan），就更新索引并绘制余弦和 FFT 图像（所谓余弦图像，即 k 空间的原始图像）
    if (event->timerId() == timerId1)
    {
        int curIndexInMEMbuffer = 0;
        int bufferCompleted;
        int availbleIndex = -1;

        // 锁定 mutex1, 并更新索引；完成之后释放锁
        // 只有在调用 InitADForCapture() 函数且还未采集数据的时候，availbleIndex 会是 -1
        bufferRefreshIndex(bufferCompleted, curIndexInMEMbuffer, availbleIndex);
        if (isContinuousAcquisitionMode() && bufferCompleted >= continuousAcquisitionBufferCount())
        {
            ui->textEdit->append(QStringLiteral("连续采集完成。"));
            stopCurrentScan(false, true);
            saveclicked();
            return;
        }
        if (availbleIndex == -1)
            return;

        // 将采集卡收到的数据复制到 m_curDisplayData 中，并对其进行去本底与加窗、写入 datainfer 中，绘制余弦和 FFT 图像
        // 出于数据对齐？的原因，m_curDisplayData 的数据需要除以 16（可能是在某些操作上把它左移了4位）
        m_curDisplayData = new unsigned short[m_AscanLen];
        {
            QMutexLocker lock(&ssoctThread->mutex1);
            memcpy(m_curDisplayData, ssoctThread->m_volumeMemBuffer[availbleIndex], 
                m_AscanLen * sizeof(short));
        }
        m_lastAscanForSave.assign(m_curDisplayData, m_curDisplayData + m_AscanLen);

        float* datainfer = new float[m_AscanLen];
        removeBGForAscan(datainfer, m_curDisplayData);

        cosplot(datainfer);
        fftplot(datainfer);

        delete [] datainfer;
        delete [] m_curDisplayData;
        // qDebug()<<"still";
    }
    // 2. 如果收到了 timerId2 的定时器事件（2D scan, 3D scan, 3D angio），就更新相应的索引并绘制 2D FFT 图像
    else if (event->timerId() == timerId2)
    {
        int curIndexInMEMbuffer = 0;
        int bufferCompleted;
        int availbleIndex = -1;

        bufferRefreshIndex(bufferCompleted, curIndexInMEMbuffer, availbleIndex);
        if (isContinuousAcquisitionMode() && bufferCompleted >= continuousAcquisitionBufferCount())
        {
            ui->textEdit->append(QStringLiteral("连续采集完成。"));
            stopCurrentScan(false, true);
            saveclicked();
            return;
        }

        // 如果 3D 扫描完毕，则停止扫描; bufferCompleted 的值可能需要修改
        if (hasSegmentedVolumeScan() && bufferCompleted >= currentVolumeSegmentEndBuffer())
            return;

        if (isVolumeScanMode() && bufferCompleted >= expectedAcquisitionBufferCount())
        {
            ui->textEdit->append("3D 扫描完成！");
            stopCurrentScan(false, true);
            saveclicked();
            return;
        }

        if (availbleIndex == -1)
            return;

        const bool is3DRealtimeMode = (mainWidget::scanMode == 3 || mainWidget::scanMode == 32);
        if (is3DRealtimeMode)
        {
            if (!ui->CB_3D_showReatimeData->isChecked())
                return;

            const int currentDisplayBuffer = std::max(0, bufferCompleted);
            const int displayIntervalBuffers = std::max(1, m_realtimeShowInterval);
            if (m_last3DRealtimeDisplayBuffer >= 0
                && currentDisplayBuffer - m_last3DRealtimeDisplayBuffer < displayIntervalBuffers)
                return;
            m_last3DRealtimeDisplayBuffer = currentDisplayBuffer;
        }

        // 将采集卡收到的数据复制到 m_curDisplayData 中，并对其进行去本底与加窗、写入二维数组 p_F2 中，绘制 FFT 图像
        // 和 1D scan 不同，这里似乎不做锁定，也不必绘制余弦图像
        m_curDisplayData = new unsigned short[m_AscanLen * m_BscanLen];
        {
            QMutexLocker lock(&ssoctThread->mutex1);
            const int sourceOffsetSamples = bscanDisplaySourceOffsetSamples(ssoctThread, m_AscanLen, m_BscanLen);
            memcpy(m_curDisplayData,
                   ssoctThread->m_volumeMemBuffer[availbleIndex] + sourceOffsetSamples,
                   m_AscanLen * m_BscanLen * sizeof(short));
        }

        p_F2 = new float[m_AscanLen * m_BscanLen];
        removeBGForBscan(p_F2, m_curDisplayData);
        if (mainWidget::scanMode == 2
            || mainWidget::scanMode == 42
            || mainWidget::scanMode == kSymphonicScanMode
            || is3DRealtimeMode)
            plotMiddleBscanAline(p_F2);
        fftBscan(p_F2);

        // 删除了 image_storage 相关的代码
        delete [] m_curDisplayData;
        delete [] p_F2;
    }
    // 3. 如果收到了 timerId3 的定时器事件（2D angio, 2D cross scan, 2D focus change），就更新相应的索引并绘制 2D FFT 图像
    else if (event->timerId() == timerId3)
    {
        int curIndexInMEMbuffer = 0;
        int bufferCompleted;
        int availbleIndex = -1;
        bufferRefreshIndex(bufferCompleted, curIndexInMEMbuffer, availbleIndex);

        if (isContinuousAcquisitionMode() && bufferCompleted >= continuousAcquisitionBufferCount())
        {
            ui->textEdit->append(QStringLiteral("连续采集完成。"));
            stopCurrentScan(false, true);
            saveclicked();
            return;
        }

        if (availbleIndex == -1)
            return;
        //if(mainWidget::scanMode == 22 | mainWidget::scanMode == 32)
        if(mainWidget::scanMode == 22)// | mainWidget::scanMode == 32)
        {
            if (bufferCompleted >= 1)
            {
                // 把 m_volumeMemBuffer 中的数据复制到 m_curDisplayDataAngio1 和 m_curDisplayDataAngio2 中，间隔为 2 个 Bscan
                const int bscanSampleCount = m_AscanLen * m_BscanLen;
                std::vector<unsigned short> recentBscans(bscanSampleCount * m_AngioRep);
                if (!copyAngioGroupBscans(recentBscans.data(),
                                          ssoctThread,
                                          availbleIndex,
                                          m_AscanLen,
                                          m_BscanLen,
                                          m_AngioRep))
                    return;

                // 已优化，现在将两个 Bscan 合体产生的 p_A3 进行为 angio 设计的 FFT 运算
                p_A3 = new float[m_AscanLen * m_BscanLen * 2];
                removeBGForBscanDouble(p_A3,
                                       recentBscans.data(),
                                       recentBscans.data() + 2 * bscanSampleCount);
                
                fftBscanAngio2(p_A3);

                delete [] p_A3;
            }
        }
        else if(mainWidget::scanMode == 10)
        {
            if (bufferCompleted >= 1)
            {
                // 把 m_volumeMemBuffer 中的数据复制到 m_curDisplayDataAngio1 和 m_curDisplayDataAngio2 中，间隔为 +-1 个 Bscan
                m_curDisplayDataAngio1 = new unsigned short[sizeof(short) * m_AscanLen * m_BscanLen];
                m_curDisplayDataAngio2 = new unsigned short[sizeof(short) * m_AscanLen * m_BscanLen];

                if (!copyCrossScanBscans(m_curDisplayDataAngio1,
                                         m_curDisplayDataAngio2,
                                         ssoctThread,
                                         availbleIndex,
                                         m_AscanLen,
                                         m_BscanLen))
                {
                    delete [] m_curDisplayDataAngio1;
                    delete [] m_curDisplayDataAngio2;
                    return;
                }

                // 对 p_A1 与 p_A2 合体产生的 p_A3 进行为 2D cross scan 设计的 FFT 运算
                // 同时把同样的数据存储到 crossSave 里备用；crossSave 似乎是某种横截面数据
                p_A3 = new float[m_AscanLen * 2 * m_BscanLen];
                crossSave = new float[m_AscanLen * 2 * m_BscanLen];
                removeBGForBscanDouble(p_A3, m_curDisplayDataAngio1, m_curDisplayDataAngio2);
                memcpy(crossSave, p_A3, sizeof(float) * m_AscanLen * 2 * m_BscanLen);

                plotMiddleBscanAline(p_A3);
                fftBscanCross(p_A3);

                delete [] p_A3;
                delete [] m_curDisplayDataAngio1;
                delete [] m_curDisplayDataAngio2;
            }
        }
    }
}
void mainWidget::cosplot(float* y_in)
{
    // 向绘图区域QCustomPlot(从widget提升来的)添加一条曲线
    QVector<double> x_o(m_AscanLen);
    QVector<double> y_o(m_AscanLen);

    for (int i = 0; i < m_AscanLen; i++)
    {
        x_o[i] = i;
        y_o[i] = y_in[i];
    }

    ui->cosplot->graph(0)->setData(x_o,y_o);
    QColor color = QColor(133,238,255);
    ui->cosplot->graph(0)->setPen(QPen(color));

    ui->cosplot->replot();
}

// 把已经 FFT + 对数缩放过的实空间 Bscan 数据绘制成图像
void mainWidget::plotMiddleBscanAline(float* bscanData)
{
    if (bscanData == nullptr || m_AscanLen <= 0 || m_BscanLen <= 0)
        return;

    const int middleAline = m_BscanLen / 2;
    float* middleAscan = bscanData + middleAline * m_AscanLen;
    cosplot(middleAscan);
    fftplot(middleAscan);
}

void mainWidget::plotBscan(float* num)
{
    double tmp1 = ui->spinBox_3->text().toDouble();
    double tmp2 = ui->spinBox_4->text().toDouble();
    if (tmp2 > tmp1)
    {
        // 1. 将输入的 Bscan 数据直接按 tmp1 作为最小值、tmp2 作为最大值进行归一化处理，存入 img 矩阵
        cv::Mat img = normalizeBscan(num, tmp1, tmp2, 0);

        // 2. 把 img 转为 8 位图像，并分别保存到存储队列 image1_storage 与 image_storage 中
        Mat img_8bit = norm_mat_to_8bit(img);

        // 4. 显示图像，如果启动 yolo 识别，则进行检测并在 label2D 界面上显示带检测框的图像；否则直接显示图像
        QImage Qtemp = putImage(img_8bit);
        ui->label2D->setPixmap(QPixmap::fromImage(Qtemp.scaled(ui->label2D->size())));
    }
    else
        return;
}

float* mainWidget::calc_FFT_Bscan(int AscanLength, int BscanLength, int CscanLength, float* b_input, bool ignore_dispersion = false)
{
    const int numel = AscanLength * BscanLength * CscanLength;
    // 1. 根据输入的色散补偿参数计算相位修正量 phy 及其三角函数值，存入 m_cosphy 和 m_sinphy 数组
    // 对于 ignore_dispersion 为 true 的情况（如函数 fftBscanAngio2），直接把 m_cosphy 设为 1，m_sinphy 设为 0
    if(ignore_dispersion)
    {
        if (m_cosphy != nullptr) 
            mkl_ascend(m_cosphy);
        if (m_sinphy != nullptr) 
            mkl_ascend(m_sinphy);

        m_cosphy = (float *)mkl_malloc(AscanLength * sizeof(float), 64);
        m_sinphy = (float *)mkl_malloc(AscanLength * sizeof(float), 64);

        for (int i = 0; i < AscanLength; ++i)
        {
            m_cosphy[i] = 1.0;
            m_sinphy[i] = 0.0;
        }
    }
    else
        calc_dispersion_compensation();

    // 2. 创建复数数组 b_compensated = 输入 * exp(i*phy), b_fourierd 用于存放 FFT 结果
    MKL_Complex8* b_compensated = (MKL_Complex8*)mkl_malloc(numel * sizeof(MKL_Complex8), 32);
    MKL_Complex8* b_fourierd = (MKL_Complex8*)mkl_malloc(numel * sizeof(MKL_Complex8), 32);
    apply_dispersion_compensation(b_compensated, b_input, AscanLength, BscanLength * CscanLength);

    // 3. 初始化mkl的fft命令, 使用 DftiComputeForward 进行 FFT 计算, 把结果存入 b_fourierd
    DftiTransform(BscanLength * CscanLength, AscanLength, b_compensated, b_fourierd);

    // 4. 将 b_fourierd 取模后覆盖原有的 b_input 数组，并进行对数压缩和缩放 (FFT_SCALE = 20倍), 做出 Bscan 图像
    vcAbs(numel, b_fourierd, b_input);
    float* b_display = (float*)mkl_malloc(numel * sizeof(float), 32);
    // vslog10 的第一个参数似乎必须输入地址
    vslog10(&numel, b_input, b_display);

    cblas_sscal(numel, mainWidget::FFT_SCALE, b_display, 1);

    mkl_ascend(m_cosphy);
    mkl_ascend(m_sinphy);
    mkl_ascend(b_compensated);
    mkl_ascend(b_fourierd);

    return b_display;
}

void mainWidget::fftBscan(float* b_input)
{
    float* b_display = calc_FFT_Bscan(m_AscanLen, m_BscanLen, 1, b_input);

    plotBscan(b_display);
    mkl_ascend(b_display);
}

void mainWidget::DftiTransform(int NumOfTransforms, int distance, MKL_Complex8* data_compensated, MKL_Complex8* data_fourierd)
{
    DFTI_DESCRIPTOR_HANDLE m_FFThandle;
    DftiCreateDescriptor(&m_FFThandle, DFTI_SINGLE, DFTI_COMPLEX, 1, distance);
    DftiSetValue(m_FFThandle, DFTI_PLACEMENT, DFTI_NOT_INPLACE);
    DftiSetValue(m_FFThandle, DFTI_NUMBER_OF_TRANSFORMS, NumOfTransforms);
    DftiSetValue(m_FFThandle, DFTI_INPUT_DISTANCE, distance);
    DftiSetValue(m_FFThandle, DFTI_OUTPUT_DISTANCE, distance);
    DftiCommitDescriptor(m_FFThandle);
    DftiComputeForward(m_FFThandle, data_compensated, data_fourierd);
    DftiFreeDescriptor(&m_FFThandle);
}

// 计算色散补偿；在这个函数之后，必须 mkl_ascend m_cosphy, m_sinphy 等！
void mainWidget::calc_dispersion_compensation()
{
    // 释放之前分配的内存
    if (m_cosphy != nullptr)
        mkl_ascend(m_cosphy);
    if (m_sinphy != nullptr)
        mkl_ascend(m_sinphy);

    // 为色散补偿的余弦和正弦数组分配内存
    m_cosphy = (float *)mkl_malloc(m_AscanLen * sizeof(float), 64);   // 按每64字节对齐内存
    m_sinphy = (float *)mkl_malloc(m_AscanLen * sizeof(float), 64);

    const double m_dispersionW0 = ui->doubleSpinBoxw0->text().toDouble();
    const double m_dispersionA1 = ui->doubleSpinBoxa1->text().toDouble();
    const double m_dispersionA2 = ui->doubleSpinBoxa2->text().toDouble();
    float tmp, phy;
    for (int i = 0; i < m_AscanLen; ++i)
    {
        tmp = (i - m_dispersionW0) * (i - m_dispersionW0);
        phy = m_dispersionA1 * tmp / 10000.0 + m_dispersionA2 * tmp * (i - m_dispersionW0) / 100000000.0;
        // 相位修正量：A1*(k-k0)^2 + A2*(k-k0)^3 (量纲有点怪？)
        // 设计 tmp 可能是为了增加速度？
        m_cosphy[i] = cos(phy);
        m_sinphy[i] = sin(phy);
    }
}

void mainWidget::apply_dispersion_compensation(MKL_Complex8* data_compensated, float* data_input, int a, int b)
{
    for (int j = 0; j < b; ++j)
    {
        for (int i = 0; i < a; ++i)
        {
            data_compensated[j * a + i].real = data_input[j * a + i] * m_cosphy[i];
            data_compensated[j * a + i].imag = data_input[j * a + i] * m_sinphy[i];
        }
    }
}

void mainWidget::fftplot(float* a_input)
{
    // qDebug()<<"fftplot";
    // 1. 根据输入的色散补偿参数计算相位修正量 phy 及其三角函数值，存入 m_cosphy 和 m_sinphy 数组
    calc_dispersion_compensation();

    QVector<double> x_fft(m_AscanLen);
    QVector<double> y_fft(m_AscanLen);

    // 2. 创建复数数组 a_compensated = 输入 * exp(i*phy)
    MKL_Complex8* a_compensated = (MKL_Complex8*)mkl_malloc(m_AscanLen * sizeof(MKL_Complex8), 32);
    apply_dispersion_compensation(a_compensated, a_input, m_AscanLen, 1);

    MKL_Complex8* a_fourierd; // fft中的复数data
    a_fourierd = (MKL_Complex8*)mkl_malloc(m_AscanLen * sizeof(MKL_Complex8), 32);

    // 3. 初始化mkl的fft命令, 使用 DftiComputeForward 进行 FFT 计算, 把结果存入 a_fourierd, 其模长存入 a_output
    DftiTransform(1, m_AscanLen, a_compensated, a_fourierd);

    float* a_output = (float*)mkl_malloc(m_AscanLen * sizeof(float), 32);
    vcAbs(m_AscanLen, a_fourierd, a_output);

    // 4. Use the selected FFT ranges to compute the extinction ratio.
    int a = ui->spinBox->value();
    int b = ui->spinBox_2->value();

    if (b > a && b + 100 <= m_AscanLen)     // Invalid ranges skip the metric but still draw the FFT curve.
    {
        float maxF(0);                      // a_output 从指标 a 到 b-1 之间的最大值（注意和matlab不同，C++的指标从0开始）
        for (int i = a; i < b; ++i)
        {
            if (a_output[i] > maxF)
                maxF = a_output[i];
        }

        float meanF(0);                     // a_output 从指标 b 到 b+99 之间的背景均值
        for (int i = b; i < b + 100; ++i)
            meanF += a_output[i];
        meanF /= 100;

        if (meanF > 0.0f && maxF > 0.0f)
            ui->SNR->setText(QString::number(mainWidget::FFT_SCALE * log10(maxF / meanF), 'f', 2));
        else
            ui->SNR->setText(QStringLiteral("--"));
    }
    else
    {
        ui->SNR->setText(QStringLiteral("--"));
    }

    vslog10(&(m_AscanLen), a_output, a_output);  // 对 a_output 本身进行对数压缩

    mkl_ascend(a_compensated);
    mkl_ascend(a_fourierd);

    // 5. 将 a_output 放大之后绘制出来
    for (int i = 0; i < m_AscanLen; i++)    // 经过色散补偿的数据（不知道为什么 i 的范围除以了2, 我把它去掉了；如果程序产生问题就把它加回去，说不定和 FFT 相对原点对称有关）
    {
        x_fft[i] = i;
        y_fft[i] = mainWidget::FFT_SCALE * a_output[i];
    }


    ui->fftplot->graph(0)->setData(x_fft,y_fft);
    QColor color = QColor(133,238,255);
    ui->fftplot->graph(0)->setPen(QPen(color));

    ui->fftplot->replot();

    mkl_ascend(m_cosphy);
    mkl_ascend(m_sinphy);
    mkl_ascend(a_output);
}

// 用于二维交叉扫描模式下绘制两个方向的 Bscan 图像
void mainWidget::plotBscanCross1(float * num)
{
    double tmp1 = ui->spinBox_3->text().toDouble();
    double tmp2 = ui->spinBox_4->text().toDouble();
    if (tmp2 > tmp1)
    {
        cv::Mat xImage = normalizeBscan(num, tmp1, tmp2, 0);
        cv::Mat yImage = normalizeBscan(num, tmp1, tmp2, m_AscanLen * m_BscanLen);
        cv::Mat combinedImage;
        cv::hconcat(xImage, yImage, combinedImage);

        cv::Mat combined8bit;
        combinedImage.convertTo(combined8bit, CV_8UC1, 255.0);

        QImage Qtemp = putImage(combined8bit);
        ui->label2D->setPixmap(QPixmap::fromImage(Qtemp.scaled(ui->label2D->size())));
        ui->labelflow->clear();
    }
    else
        return;
}

// 把 OpenCV 的 cv::Mat 转为 Qt 的 QImage
QImage mainWidget::putImage(const Mat& mat)
{
    if (mat.type() == CV_8UC1)          // 单通道图像 (0~255), 8UC1 = 8bit unsigned 1-channel
    {
        // Set the color table (used to translate colour indexes to qRgb values)
        QVector<QRgb> colorTable;
        for (int i = 0; i < 256; i++)
            colorTable.push_back(qRgb(i, i, i));
        // Copy input Mat; Create QImage with same dimensions as input Mat
        const uchar *qImageBuffer = (const uchar*)mat.data;
        QImage img(qImageBuffer, mat.cols, mat.rows, mat.step, QImage::Format_Indexed8);
        img.setColorTable(colorTable);
        return img;
    }
    else if (mat.type() == CV_8UC3)     // RGB 彩色图像 (0~255)
    {
        const uchar *qImageBuffer = (const uchar*)mat.data;
        QImage img(qImageBuffer, mat.cols, mat.rows, mat.step, QImage::Format_RGB888);
        return img.rgbSwapped();
    }
    else
    {
        qDebug() << "[错误] putImage(): 矩阵数据类型错误！现为: " << mat.type() << ", 应为 CV_8UC1 (单色) 或 CV_8UC3 (彩色)";
        return QImage();
    }
}


void mainWidget::OCTplots()
{
    m_last3DRealtimeDisplayBuffer = -1;

    if (mainWidget::scanMode == kSymphonicScanMode)
    {
        QString audioErrorMessage;
        const QString wavPath = vesselScanWavPath();
        if (!VesselAudioPlayer::instance().startLoopFromWav(wavPath, &audioErrorMessage))
        {
            ui->textEdit->append(QStringLiteral("启动 Symphonic 声卡输出失败：%1").arg(audioErrorMessage));
            m_scanActive = false;
            m_daState = DAState::Ready;
            return;
        }
        ui->textEdit->append(QStringLiteral("Symphonic 声卡输出已启动：%1。").arg(wavPath));
        if (!ssoctThread->StartDAScan(mainWidget::scanMode))
        {
            VesselAudioPlayer::instance().stop();
            ui->textEdit->append(QStringLiteral("启动 Symphonic PCIe3640 时基失败！"));
            m_scanActive = false;
            m_daState = DAState::Ready;
            return;
        }
        ui->textEdit->append(symphonicDaOutputEnabled()
            ? QStringLiteral("Symphonic PCIe3640 RF 时基和 DA 测试输出已启动。")
            : QStringLiteral("Symphonic PCIe3640 RF 时基已启动，DACH1/DACH2 保持 0V。"));
    }
    else if (!ssoctThread->StartDAScan(mainWidget::scanMode))
    {
        ui->textEdit->append("启动 DA 扫描失败！");
        m_scanActive = false;
        m_daState = DAState::Ready;
        return;
    }

    m_volumeScanTimingActive = false;
    m_volumeScanTimingMode = 0;
    if (isVolumeScanMode())
    {
        m_volumeScanTimer.restart();
        m_volumeScanTimingActive = true;
        m_volumeScanTimingMode = mainWidget::scanMode;
    }

    switch(mainWidget::scanMode)
    {
        case 1:
            timerId1 = startTimer(60);
            break;
        case 2:
            timerId2 = startTimer(60);
            break;
        case 10:
            timerId3 = startTimer(100);
            break;
        case 22:
            timerId3 = startTimer(700);
            break;
        case 3:
            timerId2 = startTimer(100);
            break;
        case 32:
            timerId2 = startTimer(100);
            break;
        case 42:
            timerId2 = startTimer(60);
            break;
        case kSymphonicScanMode:
            timerId2 = startTimer(60);
            break;
    }
}
void mainWidget::on_stopButton_clicked()
{
    stopCurrentScan(true, true, "扫描已停止，缓存中的最近一次扫描数据已保留。");
    return;
}

void mainWidget::on_resetaxis_clicked()
{
    ui->cosplot->xAxis->setRange(0, m_AscanLen);
    ui->cosplot->yAxis->setRange(-mythread::plotOffset, mythread::plotOffset);
    //ui->cosplot->setSelectionRectMode(QCP::SelectionRectMode::srmZoom);

    //ui->fftplot->addGraph();
    //ui->fftplot->xAxis->setLabel("x");
    //ui->fftplot->yAxis->setLabel("f");

    //设置坐标轴显示范围,否则我们只能看到默认的范围
    ui->fftplot->xAxis->setRange(1, m_AscanLen/2);
    ui->fftplot->yAxis->setRange(20, 120);
}

// 产生选择对话框，用户确认后用 selectedFiles() 取到所选路径列表；name_filter = tr("...;; ...") 设定文件格式
QStringList mainWidget::save_dialog(const QString &name_filter,
                                    const QString &pathKey,
                                    const QString &defaultSuffix)
{
    // 需要检查是否对话框黑底黑字
    QFileDialog textsave(this,"save");
    textsave.setAcceptMode(QFileDialog::AcceptSave); // 设置文件对话框为保存模式
    textsave.setOptions(QFileDialog::DontResolveSymlinks);
                                                // 所以手册里就没有定义，如果你要使用Save的话就自行定义一下吧
    textsave.setFileMode(QFileDialog::AnyFile);
    textsave.setNameFilter(name_filter);          // 设定文件格式
    if (!defaultSuffix.isEmpty())
        textsave.setDefaultSuffix(defaultSuffix);
    const QString lastSavePathKey = QStringLiteral("lastSavePath");
    QString savedPath;
    if (!pathKey.isEmpty())
        savedPath = savedPathValue(pathKey);
    if (savedPath.isEmpty())
        savedPath = savedPathValue(lastSavePathKey);
    if (savedPath.isEmpty() && name_filter.contains(QStringLiteral("3D"), Qt::CaseInsensitive))
        savedPath = savedPathValue(QStringLiteral("volume3dSavePath"));
    if (savedPath.isEmpty() && name_filter.contains(QStringLiteral("3D"), Qt::CaseInsensitive))
        savedPath = savedPathValue(QStringLiteral("angio3dPath"));
    if (!savedPath.isEmpty())
    {
        const QFileInfo savedInfo(savedPath);
        if (savedInfo.absoluteDir().exists())
        {
            textsave.setDirectory(savedInfo.absolutePath());
            if (!savedInfo.fileName().isEmpty())
                textsave.selectFile(savedInfo.fileName());
        }
    }
    else
    {
        textsave.setDirectory(defaultDialogPath());
    }
    textsave.setViewMode(QFileDialog::Detail);    // 显示模式设为 Detail（详细），在 Qt 中会以“详细列表”形式显示文件/列（文件名、大小、修改时间等）

    QStringList selected_paths;
    if(textsave.exec())
    {
        selected_paths = textsave.selectedFiles();
        if (!defaultSuffix.isEmpty() && !selected_paths.isEmpty())
        {
            const QFileInfo selectedInfo(selected_paths.at(0));
            if (selectedInfo.suffix().compare(defaultSuffix, Qt::CaseInsensitive) != 0)
            {
                const QString baseName = selectedInfo.completeBaseName().isEmpty()
                    ? selectedInfo.fileName()
                    : selectedInfo.completeBaseName();
                selected_paths[0] = QDir(selectedInfo.absolutePath())
                    .filePath(baseName + QStringLiteral(".") + defaultSuffix);
            }
        }
        if (!selected_paths.isEmpty())
        {
            savePathValue(lastSavePathKey, selected_paths.at(0));
            if (!pathKey.isEmpty())
                savePathValue(pathKey, selected_paths.at(0));
        }
    }
    // 注意必须检查 textsave.exec() 返回并且 qt 非空后再使用 qt.at(0)，否则会崩溃。
    return selected_paths;
}

bool mainWidget::writeAcquisitionJson(const QString &filePath,
                                      const QString &sampleType,
                                      bool preprocessedSpectrum,
                                      qint64 storedAlineCount,
                                      int storedBscanCount,
                                      bool backgroundRemoved,
                                      bool windowApplied,
                                      bool logScaled)
{
    if (!isOctDataFile(filePath))
        return true;

    saveSettings();

    QJsonObject acquisition;
    acquisition.insert(QStringLiteral("sourceFile"), QDir::toNativeSeparators(filePath));
    acquisition.insert(QStringLiteral("scanModeText"), ui->comboBox->currentText());
    acquisition.insert(QStringLiteral("triggerMode"), ui->combo_triggerMode->currentText());
    acquisition.insert(QStringLiteral("clockMode"), ui->combo_clockMode->currentText());
    acquisition.insert(QStringLiteral("scanMode"), mainWidget::scanMode);
    acquisition.insert(QStringLiteral("ascanLen"), m_AscanLen);
    acquisition.insert(QStringLiteral("bscanLen"), m_BscanLen);
    acquisition.insert(QStringLiteral("cscanLen"), m_CscanLen);
    acquisition.insert(QStringLiteral("angioRep"), m_AngioRep);
    acquisition.insert(QStringLiteral("storedAlineCount"), static_cast<double>(storedAlineCount));
    acquisition.insert(QStringLiteral("storedBscanCount"), storedBscanCount);
    acquisition.insert(QStringLiteral("sampleType"), sampleType);
    acquisition.insert(QStringLiteral("preprocessedSpectrum"), preprocessedSpectrum);
    acquisition.insert(QStringLiteral("backgroundRemoved"), backgroundRemoved);
    acquisition.insert(QStringLiteral("windowApplied"), windowApplied);
    acquisition.insert(QStringLiteral("logScaled"), logScaled);

    QJsonObject root;
    root.insert(QStringLiteral("formatVersion"), 1);
    root.insert(QStringLiteral("kind"), QStringLiteral("ssoct_acquisition_settings"));
    root.insert(QStringLiteral("acquisition"), acquisition);
    root.insert(QStringLiteral("settings"), settingsFileToJson());
    root.insert(QStringLiteral("settingsIni"), settingsIniText());

    QString errorMessage;
    const QString jsonPath = sidecarPathForDataFile(filePath);
    if (!writeJsonDocument(jsonPath, root, &errorMessage)) {
        ui->textEdit->append(QStringLiteral("保存设置 JSON 失败：%1").arg(errorMessage));
        return false;
    }

    return true;
}

bool mainWidget::finalizeSavedDataFile(const QString &filePath,
                                       const QString &sampleType,
                                       bool preprocessedSpectrum,
                                       qint64 storedAlineCount,
                                       int storedBscanCount,
                                       bool backgroundRemoved,
                                       bool windowApplied,
                                       bool logScaled)
{
    if (!isOctDataFile(filePath))
        return true;

    const bool jsonSaved = writeAcquisitionJson(filePath,
                                                sampleType,
                                                preprocessedSpectrum,
                                                storedAlineCount,
                                                storedBscanCount,
                                                backgroundRemoved,
                                                windowApplied,
                                                logScaled);
    if (!jsonSaved)
        return false;

    if (!ui->CB_fourierOnSaved->isChecked())
        return true;

    ui->textEdit->append(QStringLiteral("保存后自动开始傅里叶变换。"));
    const bool fftOk = processFourierFile(filePath, true, true, true, false);
    if (!fftOk)
        ui->textEdit->append(QStringLiteral("自动傅里叶变换失败，原始数据和设置 JSON 已保留。"));
    return fftOk;
}

bool mainWidget::processFourierFile(const QString &filePath,
                                    bool applyWindow,
                                    bool removeBackground,
                                    bool applyLogScale,
                                    bool interactive)
{
    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile() || !isOctDataFile(filePath)) {
        ui->textEdit->append(QStringLiteral("请选择有效的 .2d 或 .3d 文件。"));
        return false;
    }

    FourierSourceMetadata metadata;
    metadata.scanModeText = ui->comboBox->currentText();
    metadata.scanMode = mainWidget::scanMode;
    metadata.ascanLen = m_AscanLen;
    metadata.bscanLen = m_BscanLen;
    metadata.cscanLen = m_CscanLen;
    metadata.angioRep = m_AngioRep;
    metadata.storedAlineCount = storedAlineCountForMode(metadata.scanMode,
                                                        metadata.bscanLen,
                                                        metadata.cscanLen,
                                                        metadata.angioRep);
    metadata.storedBscanCount = metadata.bscanLen > 0
        ? static_cast<int>(metadata.storedAlineCount / metadata.bscanLen)
        : 0;
    metadata.dispersionW0 = ui->doubleSpinBoxw0->value();
    metadata.dispersionA1 = ui->doubleSpinBoxa1->value();
    metadata.dispersionA2 = ui->doubleSpinBoxa2->value();

    const QString sidecarPath = sidecarPathForDataFile(filePath);
    const bool sidecarExists = QFileInfo::exists(sidecarPath);
    QString sidecarError;
    if (!readFourierSidecar(filePath, &metadata, &sidecarError)) {
        if (!sidecarExists) {
            if (!confirmFourierWithoutSidecar(this, fileInfo, metadata.ascanLen)) {
                ui->textEdit->append(QStringLiteral("傅里叶变换已取消：未找到同名 .json 文件。"));
                return false;
            }
            metadata.sampleType = QString::fromLatin1(kSampleTypeUint16Raw);
            ui->textEdit->append(QStringLiteral("未找到同名 .json 文件，用户已确认使用当前程序设置进行傅里叶变换。"));
        } else {
            ui->textEdit->append(QStringLiteral("读取同名 .json 文件失败：%1；使用当前程序设置。").arg(sidecarError));
        }
    }

    if (metadata.ascanLen <= 0 || metadata.bscanLen <= 0) {
        ui->textEdit->append(QStringLiteral("傅里叶变换失败：AscanLen 或 BscanLength 无效。"));
        return false;
    }

    if (metadata.sampleType.isEmpty())
        metadata.sampleType = inferSampleType(filePath, metadata.ascanLen, metadata.storedAlineCount);
    if (!isFloatSampleType(metadata.sampleType) && !isUint16SampleType(metadata.sampleType)) {
        ui->textEdit->append(QStringLiteral("傅里叶变换失败：无法判断文件中的数据类型。"));
        return false;
    }

    const qint64 sampleSize = isFloatSampleType(metadata.sampleType)
        ? static_cast<qint64>(sizeof(float))
        : static_cast<qint64>(sizeof(uint16_t));
    const qint64 bytesPerLine = static_cast<qint64>(metadata.ascanLen) * sampleSize;
    if (bytesPerLine <= 0 || fileInfo.size() % bytesPerLine != 0) {
        ui->textEdit->append(QStringLiteral("傅里叶变换失败：文件大小与 AscanLen 不匹配。"));
        return false;
    }

    const qint64 totalLines = fileInfo.size() / bytesPerLine;
    if (totalLines <= 0) {
        ui->textEdit->append(QStringLiteral("傅里叶变换失败：文件中没有可处理的 Aline。"));
        return false;
    }
    metadata.storedAlineCount = totalLines;
    metadata.storedBscanCount = metadata.bscanLen > 0 ? static_cast<int>(totalLines / metadata.bscanLen) : 0;

    ui->textEdit->append(QStringLiteral("扫描类型：%1；AscanLen=%2，BscanLength=%3，Aline=%4，Bscan=%5。")
                         .arg(metadata.scanModeText)
                         .arg(metadata.ascanLen)
                         .arg(metadata.bscanLen)
                         .arg(totalLines)
                         .arg(metadata.storedBscanCount));

    const QString outputPath = fftBinPathForDataFile(filePath);
    const QString outputJsonPath = fftJsonPathForDataFile(filePath);
    if (!confirmReplaceFourierOutputs(this, outputPath, outputJsonPath)) {
        ui->textEdit->append(QStringLiteral("傅里叶变换已取消：FFT 输出文件已存在。"));
        return false;
    }

    QElapsedTimer fftTimer;
    fftTimer.start();
    bool useOpenClBackend = canUseOpenClFftBackend(metadata.ascanLen,
                                                   metadata.dispersionW0,
                                                   metadata.dispersionA1,
                                                   metadata.dispersionA2);
    ui->textEdit->append(useOpenClBackend
                         ? QStringLiteral("FFT 后端：OpenCL/GPU。")
                         : QStringLiteral("FFT 后端：MKL/CPU。"));

    QFile inputFile(filePath);
    if (!inputFile.open(QIODevice::ReadOnly)) {
        ui->textEdit->append(QStringLiteral("傅里叶变换失败：无法打开输入文件 %1。").arg(filePath));
        return false;
    }

    const std::vector<float> window = makeSpectralWindow(metadata.ascanLen, applyWindow);
    std::vector<float> background;
    QString errorMessage;
    if (removeBackground) {
        if (!computeBackgroundFromMiddleBscan(inputFile,
                                              totalLines,
                                              metadata.ascanLen,
                                              metadata.bscanLen,
                                              metadata.sampleType,
                                              &background,
                                              &errorMessage)) {
            ui->textEdit->append(QStringLiteral("傅里叶变换失败：计算本底失败：%1").arg(errorMessage));
            return false;
        }
    }

    QSaveFile outputFile(outputPath);
    if (!outputFile.open(QIODevice::WriteOnly)) {
        ui->textEdit->append(QStringLiteral("傅里叶变换失败：无法打开输出文件 %1。").arg(outputPath));
        return false;
    }

    const int linesPerBlock = std::max(1, metadata.bscanLen);
    qint64 processedLines = 0;
    const qint64 progressInterval = 100000;
    qint64 nextProgress = progressInterval;
    qint64 lastProgressReported = 0;
    while (processedLines < totalLines) {
        const int currentLines = static_cast<int>(std::min<qint64>(linesPerBlock, totalLines - processedLines));
        std::vector<float> spectra;
        if (!readSpectraLines(inputFile,
                              processedLines,
                              currentLines,
                              metadata.ascanLen,
                              metadata.sampleType,
                              background,
                              window,
                              removeBackground,
                              &spectra,
                              &errorMessage)) {
            ui->textEdit->append(QStringLiteral("傅里叶变换失败：%1").arg(errorMessage));
            return false;
        }

        std::vector<float> fftOutput;
        bool transformOk = false;
        bool openClFailed = false;
        if (useOpenClBackend) {
            transformOk = transformSpectraBlockOpenCl(spectra,
                                                      metadata.ascanLen,
                                                      currentLines,
                                                      metadata.dispersionW0,
                                                      metadata.dispersionA1,
                                                      metadata.dispersionA2,
                                                      false,
                                                      &fftOutput);
            if (!transformOk) {
                openClFailed = true;
                useOpenClBackend = false;
            }
        }
        if (!transformOk) {
            transformOk = transformSpectraBlockMkl(spectra,
                                                   metadata.ascanLen,
                                                   currentLines,
                                                   metadata.dispersionW0,
                                                   metadata.dispersionA1,
                                                   metadata.dispersionA2,
                                                   false,
                                                   &fftOutput,
                                                   &errorMessage);
        }
        if (openClFailed)
            ui->textEdit->append(QStringLiteral("OpenCL/GPU FFT 失败，切换到 MKL/CPU。"));
        if (!transformOk) {
            ui->textEdit->append(QStringLiteral("傅里叶变换失败：%1").arg(errorMessage));
            return false;
        }

        processedLines += currentLines;
        while (nextProgress <= totalLines && processedLines >= nextProgress) {
            ui->textEdit_temp->append(QStringLiteral("已完成FFT: %1 / %2 条 Aline。").arg(nextProgress).arg(totalLines));
            lastProgressReported = nextProgress;
            nextProgress += progressInterval;
        }
        if (processedLines == totalLines && lastProgressReported != totalLines) {
            ui->textEdit_temp->append(QStringLiteral("已完成FFT: %1 / %2 条 Aline。").arg(totalLines).arg(totalLines));
            lastProgressReported = totalLines;
        }
        QApplication::processEvents();

        if (applyLogScale)
            applyLogScaleToFftOutput(&fftOutput);

        const qint64 outputBytes = static_cast<qint64>(fftOutput.size()) * static_cast<qint64>(sizeof(float));
        if (outputFile.write(reinterpret_cast<const char*>(fftOutput.data()), outputBytes) != outputBytes) {
            ui->textEdit->append(QStringLiteral("傅里叶变换失败：写入输出文件失败。"));
            return false;
        }
    }

    if (!outputFile.commit()) {
        ui->textEdit->append(QStringLiteral("傅里叶变换失败：保存输出文件失败。"));
        return false;
    }

    QJsonObject source;
    source.insert(QStringLiteral("file"), QDir::toNativeSeparators(filePath));
    source.insert(QStringLiteral("sidecar"), QDir::toNativeSeparators(sidecarPathForDataFile(filePath)));
    source.insert(QStringLiteral("scanModeText"), metadata.scanModeText);
    source.insert(QStringLiteral("scanMode"), metadata.scanMode);
    source.insert(QStringLiteral("ascanLen"), metadata.ascanLen);
    source.insert(QStringLiteral("bscanLen"), metadata.bscanLen);
    source.insert(QStringLiteral("cscanLen"), metadata.cscanLen);
    source.insert(QStringLiteral("storedAlineCount"), static_cast<double>(metadata.storedAlineCount));
    source.insert(QStringLiteral("storedBscanCount"), metadata.storedBscanCount);
    source.insert(QStringLiteral("sampleType"), metadata.sampleType);
    source.insert(QStringLiteral("preprocessedSpectrum"), metadata.preprocessedSpectrum);

    QJsonObject options;
    options.insert(QStringLiteral("applyWindow"), applyWindow);
    options.insert(QStringLiteral("removeBackground"), removeBackground);
    options.insert(QStringLiteral("applyLogScale"), applyLogScale);
    options.insert(QStringLiteral("fftScale"), mainWidget::FFT_SCALE);
    options.insert(QStringLiteral("outputSampleType"), QStringLiteral("float32"));
    options.insert(QStringLiteral("outputFile"), QDir::toNativeSeparators(outputPath));

    QJsonObject root;
    root.insert(QStringLiteral("formatVersion"), 1);
    root.insert(QStringLiteral("kind"), QStringLiteral("ssoct_fft_settings"));
    root.insert(QStringLiteral("source"), source);
    root.insert(QStringLiteral("fftProcessing"), options);
    root.insert(QStringLiteral("settings"), settingsFileToJson());
    root.insert(QStringLiteral("settingsIni"), settingsIniText());

    if (!writeJsonDocument(outputJsonPath, root, &errorMessage)) {
        ui->textEdit->append(QStringLiteral("FFT 设置 JSON 保存失败：%1").arg(errorMessage));
        return false;
    }

    saveFftSourcePath(filePath);
    ui->textEdit->append(QStringLiteral("FFT 处理耗时：%1 秒。")
                         .arg(static_cast<double>(fftTimer.elapsed()) / 1000.0, 0, 'f', 2));
    ui->textEdit->append(QStringLiteral("傅里叶变换完成：%1").arg(outputPath));
    if (interactive) {
        styledMessageBox(this,
                         QMessageBox::Information,
                         QStringLiteral("提示"),
                         QStringLiteral("傅里叶变换完成。"),
                         QMessageBox::Ok,
                         QMessageBox::Ok);
    }
    return true;
}

bool mainWidget::saveContinuousAcquisition()
{
    if (!isContinuousAcquisitionMode())
        return false;

    const int targetBuffers = continuousAcquisitionBufferCount();
    const long long targetSamples = continuousTargetSampleCount();
    const int singleBufferSamples = ssoctThread->singleBufferSize();
    if (targetBuffers <= 0 || targetSamples <= 0 || singleBufferSamples <= 0)
    {
        ui->textEdit->append(QStringLiteral("连续采集保存失败：采集参数无效。"));
        return false;
    }

    int curIndexInMEMbuffer = 0;
    int bufferCompleted = 0;
    int availbleIndex = -1;
    bufferRefreshIndex(bufferCompleted, curIndexInMEMbuffer, availbleIndex);
    const int bufferCountInMem = ssoctThread->m_volumeMemBuffer.size();
    if (availbleIndex == -1 || bufferCompleted < targetBuffers || bufferCountInMem < targetBuffers)
    {
        ui->textEdit->append(QStringLiteral("连续采集数据不足，无法保存完整 .3d 文件。"));
        return false;
    }

    const int bscanSampleCount = m_AscanLen * m_BscanLen;
    const bool cropRepeatCycles = (mainWidget::scanMode == 2);
    const bool cropAngioGroups = (mainWidget::scanMode == 22);
    int cycleSampleCount = 0;
    int groupSampleCount = 0;
    int sourceOffsetSamples = 0;
    int cyclesPerBuffer = 1;
    int groupsPerBuffer = 1;
    if (cropRepeatCycles)
    {
        const long long cycleSamples = static_cast<long long>(m_AscanLen) * ssoctThread->bscanCycleAlineCount();
        const long long offsetSamples = static_cast<long long>(m_AscanLen) * ssoctThread->transitionAlineCount();
        cyclesPerBuffer = std::max(1, ssoctThread->repeatCyclesPerBuffer());
        const long long requiredSamples =
            (static_cast<long long>(cyclesPerBuffer) - 1) * cycleSamples + offsetSamples + bscanSampleCount;
        if (cycleSamples <= 0 || offsetSamples < 0 || requiredSamples > singleBufferSamples
            || cycleSamples > (std::numeric_limits<int>::max)()
            || offsetSamples > (std::numeric_limits<int>::max)())
        {
            ui->textEdit->append(QStringLiteral("连续采集保存失败：2D repeat 周期裁剪参数无效。"));
            return false;
        }
        cycleSampleCount = static_cast<int>(cycleSamples);
        sourceOffsetSamples = static_cast<int>(offsetSamples);
    }
    if (cropAngioGroups)
    {
        const long long groupAlineCount = static_cast<long long>(ssoctThread->xScanAlineCount());
        const long long groupSamples = static_cast<long long>(m_AscanLen) * groupAlineCount;
        const long long cycleSamples = static_cast<long long>(m_AscanLen) * ssoctThread->bscanCycleAlineCount();
        const long long offsetSamples = static_cast<long long>(m_AscanLen) * ssoctThread->transitionAlineCount();
        groupsPerBuffer = std::max(1, ssoctThread->repeatCyclesPerBuffer());
        const long long requiredSamples =
            (static_cast<long long>(groupsPerBuffer) - 1) * groupSamples
            + (static_cast<long long>(m_AngioRep) - 1) * cycleSamples
            + offsetSamples + bscanSampleCount;
        if (groupSamples <= 0 || cycleSamples <= 0 || offsetSamples < 0
            || requiredSamples > singleBufferSamples
            || groupSamples > (std::numeric_limits<int>::max)()
            || cycleSamples > (std::numeric_limits<int>::max)()
            || offsetSamples > (std::numeric_limits<int>::max)())
        {
            ui->textEdit->append(QStringLiteral("连续采集保存失败：2D angio 扫描组裁剪参数无效。"));
            return false;
        }
        groupSampleCount = static_cast<int>(groupSamples);
        cycleSampleCount = static_cast<int>(cycleSamples);
        sourceOffsetSamples = static_cast<int>(offsetSamples);
    }

    QStringList selected_paths = save_dialog(tr("3D (*.3d)"),
                                             QStringLiteral("volume3dSavePath"),
                                             QStringLiteral("3d"));
    if (selected_paths.isEmpty())
    {
        ui->textEdit->append(QStringLiteral("保存被取消。"));
        return false;
    }

    std::ofstream file1;
    file1.open(selected_paths.at(0).toStdWString(), std::ios::binary);
    if (!file1.is_open())
    {
        ui->textEdit->append(QStringLiteral("连续采集保存失败：无法打开文件 %1。").arg(selected_paths.at(0)));
        return false;
    }

    long long samplesRemaining = targetSamples;
    bool writeOk = true;
    {
        QMutexLocker lock(&ssoctThread->mutex1);
        for (int frame = 0; frame < targetBuffers && samplesRemaining > 0; ++frame)
        {
            const int sourceIndex = ringIndexBack(availbleIndex, targetBuffers - 1 - frame, bufferCountInMem);
            if (sourceIndex < 0 || sourceIndex >= bufferCountInMem || ssoctThread->m_volumeMemBuffer[sourceIndex] == nullptr)
            {
                writeOk = false;
                break;
            }

            if (cropRepeatCycles)
            {
                for (int cycle = 0; cycle < cyclesPerBuffer && samplesRemaining > 0; ++cycle)
                {
                    const U16 *source = ssoctThread->m_volumeMemBuffer[sourceIndex]
                        + cycle * cycleSampleCount + sourceOffsetSamples;
                    file1.write(reinterpret_cast<const char*>(source), sizeof(U16) * bscanSampleCount);
                    if (!file1.good())
                    {
                        writeOk = false;
                        break;
                    }
                    samplesRemaining -= bscanSampleCount;
                }
                if (!writeOk)
                    break;
                continue;
            }

            if (cropAngioGroups)
            {
                for (int group = 0; group < groupsPerBuffer && samplesRemaining > 0; ++group)
                {
                    for (int rep = 0; rep < m_AngioRep && samplesRemaining > 0; ++rep)
                    {
                        const U16 *source = ssoctThread->m_volumeMemBuffer[sourceIndex]
                            + group * groupSampleCount
                            + rep * cycleSampleCount
                            + sourceOffsetSamples;
                        file1.write(reinterpret_cast<const char*>(source), sizeof(U16) * bscanSampleCount);
                        if (!file1.good())
                        {
                            writeOk = false;
                            break;
                        }
                        samplesRemaining -= bscanSampleCount;
                    }
                    if (!writeOk)
                        break;
                }
                if (!writeOk)
                    break;
                continue;
            }

            const int samplesThisFrame = (mainWidget::scanMode == 1)
                ? static_cast<int>(std::min<long long>(singleBufferSamples, samplesRemaining))
                : static_cast<int>(std::min<long long>(bscanSampleCount, samplesRemaining));
            file1.write(reinterpret_cast<const char*>(ssoctThread->m_volumeMemBuffer[sourceIndex]),
                        sizeof(U16) * samplesThisFrame);
            if (!file1.good())
            {
                writeOk = false;
                break;
            }
            samplesRemaining -= samplesThisFrame;
        }
    }
    file1.close();

    if (!writeOk || samplesRemaining != 0)
    {
        ui->textEdit->append(QStringLiteral("连续采集保存失败：写入 .3d 文件时数据不完整。"));
        return false;
    }

    if (mainWidget::scanMode == 1)
    {
        ui->textEdit->append(QStringLiteral("连续采集保存完成：%1 个 Aline 已写入 .3d 文件。")
                             .arg(mainWidget::ContinuousAlineCount));
    }
    else
    {
        ui->textEdit->append(QStringLiteral("连续采集保存完成：%1 个 Aline，按 %2 个 Bscan 写入 .3d 文件。")
                             .arg(mainWidget::ContinuousAlineCount)
                             .arg(continuousStoredBscanCount()));
    }

    const QString periodicityReport = analyzeContinuousAcquisitionPeriodicity(targetBuffers,
                                                                              bufferCountInMem,
                                                                              availbleIndex);
    if (!periodicityReport.isEmpty())
    {
        const QStringList reportLines = periodicityReport.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : reportLines)
            ui->textEdit->append(line);
        writeContinuousPeriodicityReport(selected_paths.at(0), periodicityReport);
    }

    const qint64 storedAlineCount = (mainWidget::scanMode == 1)
        ? static_cast<qint64>(mainWidget::ContinuousAlineCount)
        : static_cast<qint64>(continuousStoredBscanCount()) * m_BscanLen;
    const int storedBscanCount = m_BscanLen > 0
        ? static_cast<int>(storedAlineCount / m_BscanLen)
        : 0;
    return finalizeSavedDataFile(selected_paths.at(0),
                                 QString::fromLatin1(kSampleTypeUint16Raw),
                                 false,
                                 storedAlineCount,
                                 storedBscanCount,
                                 false,
                                 false,
                                 false);
}

QString mainWidget::analyzeContinuousAcquisitionPeriodicity(int targetBuffers,
                                                            int bufferCountInMem,
                                                            int newestIndex) const
{
    if (ssoctThread == nullptr || targetBuffers <= 0 || bufferCountInMem <= 0 || newestIndex < 0)
        return QString();

    const int singleBufferSamples = ssoctThread->singleBufferSize();
    if (singleBufferSamples <= 0)
        return QStringLiteral("连续采集周期诊断：逻辑缓存帧大小无效，无法分析周期。");

    const qint64 totalRawSamples = static_cast<qint64>(targetBuffers) * singleBufferSamples;
    if (totalRawSamples < 2048)
        return QStringLiteral("连续采集周期诊断：样点数过少，无法分析周期。");

    const int windowSamples = static_cast<int>(std::min<qint64>(totalRawSamples, 131072));
    QVector<qint64> windowStarts;
    windowStarts.append(0);
    if (totalRawSamples > static_cast<qint64>(windowSamples) * 2)
        windowStarts.append((totalRawSamples - windowSamples) / 2);
    if (totalRawSamples > windowSamples)
    {
        const qint64 endStart = totalRawSamples - windowSamples;
        if (windowStarts.isEmpty() || endStart - windowStarts.last() > windowSamples / 2)
            windowStarts.append(endStart);
    }

    auto copyWindowSamples = [&](qint64 startSample, int sampleCount) {
        std::vector<float> samples;
        samples.reserve(sampleCount);
        QMutexLocker lock(&ssoctThread->mutex1);
        qint64 copied = 0;
        while (copied < sampleCount)
        {
            const qint64 globalSample = startSample + copied;
            const int frame = static_cast<int>(globalSample / singleBufferSamples);
            const int offset = static_cast<int>(globalSample % singleBufferSamples);
            if (frame < 0 || frame >= targetBuffers)
                break;

            const int sourceIndex = ringIndexBack(newestIndex,
                                                  targetBuffers - 1 - frame,
                                                  bufferCountInMem);
            if (sourceIndex < 0
                || sourceIndex >= ssoctThread->m_volumeMemBuffer.size()
                || ssoctThread->m_volumeMemBuffer[sourceIndex] == nullptr)
            {
                break;
            }

            const int chunkSamples = std::min<int>(sampleCount - static_cast<int>(copied),
                                                   singleBufferSamples - offset);
            const U16 *source = ssoctThread->m_volumeMemBuffer[sourceIndex] + offset;
            for (int i = 0; i < chunkSamples; ++i)
                samples.push_back(static_cast<float>(source[i]));
            copied += chunkSamples;
        }
        return samples;
    };

    QVector<PeriodicityWindowResult> results;
    for (int i = 0; i < windowStarts.size(); ++i)
    {
        const QString label = (i == 0)
            ? QStringLiteral("起始")
            : ((i == windowStarts.size() - 1 && windowStarts[i] > 0)
               ? QStringLiteral("末尾")
               : QStringLiteral("中间"));
        const std::vector<float> samples = copyWindowSamples(windowStarts[i], windowSamples);
        results.append(analyzePeriodicityWindow(label, samples, m_AscanLen));
    }

    QStringList lines;
    lines.append(QStringLiteral("连续采集周期诊断：共 %1 个原始样点，分析 %2 个窗口，每窗口最多 %3 样点。")
                 .arg(totalRawSamples)
                 .arg(results.size())
                 .arg(windowSamples));
    bool reportSampleRateOk = false;
    const double reportSampleRateMHz = sampleRateMHzFromUi(&reportSampleRateOk);
    const double reportSamplesPerMicrosecond = reportSampleRateOk ? reportSampleRateMHz : 1000.0;

    int validCount = 0;
    double periodSum = 0.0;
    double peakSum = 0.0;
    double adjacentSum = 0.0;
    int minPeriod = std::numeric_limits<int>::max();
    int maxPeriod = 0;
    for (const PeriodicityWindowResult &result : results)
    {
        if (!result.valid)
        {
            lines.append(QStringLiteral("  %1窗口：没有找到可靠的周期候选。").arg(result.label));
            continue;
        }

        ++validCount;
        periodSum += result.bestPeriodSamples;
        peakSum += result.peakCorrelation;
        adjacentSum += result.adjacentMeanCorrelation;
        minPeriod = std::min(minPeriod, result.bestPeriodSamples);
        maxPeriod = std::max(maxPeriod, result.bestPeriodSamples);
        lines.append(QStringLiteral("  %1窗口：候选周期 %2 样点（约 %3 个 Aline，按采样率 %4 MHz 约 %5 us），自相关峰值 %6，相邻周期相关 mean=%7 std=%8 min=%9（%10 对）。")
                     .arg(result.label)
                     .arg(result.bestPeriodSamples)
                     .arg(static_cast<double>(result.bestPeriodSamples) / std::max(1, m_AscanLen), 0, 'f', 3)
                     .arg(reportSamplesPerMicrosecond, 0, 'f', 3)
                     .arg(static_cast<double>(result.bestPeriodSamples) / reportSamplesPerMicrosecond, 0, 'f', 3)
                     .arg(result.peakCorrelation, 0, 'f', 3)
                     .arg(result.adjacentMeanCorrelation, 0, 'f', 3)
                     .arg(result.adjacentStdCorrelation, 0, 'f', 3)
                     .arg(result.adjacentMinCorrelation, 0, 'f', 3)
                     .arg(result.adjacentPairCount));
    }

    if (validCount == 0)
    {
        lines.append(QStringLiteral("  判读：没有看到明确稳定周期。"));
    }
    else
    {
        const double meanPeriod = periodSum / validCount;
        const double meanPeak = peakSum / validCount;
        const double meanAdjacent = adjacentSum / validCount;
        const double driftPercent = (validCount > 1 && meanPeriod > 0.0)
            ? static_cast<double>(maxPeriod - minPeriod) * 100.0 / meanPeriod
            : 0.0;
        lines.append(QStringLiteral("  窗口间候选周期漂移：%1 样点（%2%）。")
                     .arg(validCount > 1 ? maxPeriod - minPeriod : 0)
                     .arg(driftPercent, 0, 'f', 2));

        if (meanPeak >= 0.35 && meanAdjacent >= 0.25 && driftPercent <= 2.0)
            lines.append(QStringLiteral("  判读：这批连续采集数据中可以看到较稳定的周期候选。"));
        else if (meanPeak < 0.20)
            lines.append(QStringLiteral("  判读：周期自相关峰值偏弱，当前数据中没有明确稳定周期。"));
        else if (driftPercent > 2.0)
            lines.append(QStringLiteral("  判读：不同窗口的候选周期不一致，可能存在周期漂移或触发/时钟不稳定。"));
        else
            lines.append(QStringLiteral("  判读：存在周期候选，但相邻周期相似度偏低，需要结合波形和触发接线继续确认。"));
    }

    return lines.join(QLatin1Char('\n'));
}

bool mainWidget::writeContinuousPeriodicityReport(const QString &dataFilePath,
                                                  const QString &reportText) const
{
    if (reportText.isEmpty())
        return false;

    const QString reportPath = periodicityReportPathForDataFile(dataFilePath);
    QSaveFile file(reportPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        ui->textEdit->append(QStringLiteral("连续采集周期诊断报告保存失败：无法打开 %1。").arg(reportPath));
        return false;
    }

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    stream << reportText << '\n';
    if (!file.commit())
    {
        ui->textEdit->append(QStringLiteral("连续采集周期诊断报告保存失败：%1。").arg(reportPath));
        return false;
    }

    ui->textEdit->append(QStringLiteral("连续采集周期诊断报告已保存：%1").arg(reportPath));
    return true;
}

bool mainWidget::saveclicked()
{   // 只给了 4 种保存模式的代码，是否需要增补？
    if (mainWidget::scanMode == 1)
    {   // 储存 Ascan 数据，640 个
        // 更新索引
        int curIndexInMEMbuffer = 0;
        int bufferCompleted;
        int availbleIndex = -1;
        bufferRefreshIndex(bufferCompleted, curIndexInMEMbuffer, availbleIndex);


        // 存储 Ascan 数据
        if (isContinuousAcquisitionMode())
            return saveContinuousAcquisition();

        std::vector<unsigned short> ascanData;
        if (availbleIndex != -1)
        {
            ascanData.resize(m_AscanLen);
            QMutexLocker lock(&ssoctThread->mutex1);
            memcpy(ascanData.data(), ssoctThread->m_volumeMemBuffer[availbleIndex],
                   m_AscanLen * sizeof(short));
        }
        else if (m_lastAscanForSave.size() == static_cast<std::vector<unsigned short>::size_type>(m_AscanLen))
        {
            ascanData = m_lastAscanForSave;
        }
        else
        {
            ui->textEdit->append("没有可保存的 1D 数据，请先开始扫描并等待曲线刷新。");
            return false;
        }

        std::vector<float> datainfer2(m_AscanLen);
        removeBGForAscan(datainfer2.data(), ascanData.data());

        // 选择保存路径
        QStringList selected_paths = save_dialog(tr("Text files (*.txt);;Images (*.png *.xpm *.jpg);; 2D (*.2d);; 3D (*.3d)"));
        if (selected_paths.isEmpty())
        {
            ui->textEdit->append("保存被取消。");
            return false;
        }
        const QString savePath = selected_paths.at(0);
        qDebug() << "保存路径：" << savePath;
        if (isOctDataFile(savePath))
        {
            std::ofstream file1;
            file1.open(savePath.toStdWString(), std::ios::binary);
            if (!file1.is_open())
            {
                ui->textEdit->append(QStringLiteral("保存失败，无法打开文件：%1").arg(savePath));
                return false;
            }
            file1.write(reinterpret_cast<const char*>(datainfer2.data()), sizeof(float) * m_AscanLen);
            file1.close();
        }
        else
        {
            QFile file(savePath);
            if (!file.open(QFile::WriteOnly|QFile::Text))
            {
                ui->textEdit->append(QStringLiteral("保存失败，无法打开文件：%1").arg(savePath));
                return false;
            }
            QTextStream ts(&file);
            for (int i = 0; i < m_AscanLen; i++)
                ts << datainfer2[i] << " ";
        }
        ui->textEdit->append("Ascan 储存完毕！");
        return finalizeSavedDataFile(savePath,
                                     QString::fromLatin1(kSampleTypeFloat32Spectrum),
                                     true,
                                     1,
                                     1,
                                     !mainWidget::BGReductionFlag,
                                     !mainWidget::Wflag,
                                     false);
    }
    else if (mainWidget::scanMode == 2)   // 曾经是 angio 模式，现在改回常规 Bscan 了
    {
        int curIndexInMEMbuffer = 0;
        int bufferCompleted;
        int availbleIndex = -1;
        bufferRefreshIndex(bufferCompleted, curIndexInMEMbuffer, availbleIndex);
        if (availbleIndex == -1)
        {
            ui->textEdit->append("没有可保存的 Bscan 数据。");
            return false;
        }

        if (isContinuousAcquisitionMode())
            return saveContinuousAcquisition();

        m_curDisplayDataAngio1 = new unsigned short[sizeof(short) * m_AscanLen * m_BscanLen];
        {
            QMutexLocker lock(&ssoctThread->mutex1);
            const int sourceOffsetSamples = bscanDisplaySourceOffsetSamples(ssoctThread, m_AscanLen, m_BscanLen);
            memcpy(m_curDisplayDataAngio1,
                   ssoctThread->m_volumeMemBuffer[availbleIndex] + sourceOffsetSamples,
                   sizeof(short) * m_AscanLen * m_BscanLen);
        }

        p_A3 = new float[m_AscanLen * m_BscanLen];
        removeBGForBscan(p_A3, m_curDisplayDataAngio1);

        QStringList selected_paths = save_dialog(tr("2D (*.2d);; 3D (*.3d)"));
        if (selected_paths.isEmpty())
        {
            delete[] p_A3;
            delete[] m_curDisplayDataAngio1;
            m_curDisplayDataAngio1 = nullptr;
            ui->textEdit->append("保存被取消。");
            return false;
        }
        const QString savePath = selected_paths.at(0);
        qDebug()<<savePath;
        std::ofstream file1;
        file1.open(savePath.toStdWString(), std::ios::binary);
        if (!file1.is_open())
        {
            delete[] p_A3;
            delete[] m_curDisplayDataAngio1;
            p_A3 = nullptr;
            m_curDisplayDataAngio1 = nullptr;
            ui->textEdit->append(QStringLiteral("保存失败，无法打开文件：%1").arg(savePath));
            return false;
        }
        // 直接写入原始二进制 float 数据
        file1.write((char*)p_A3, sizeof(float) * m_AscanLen * m_BscanLen);
        file1.close();
        delete[] p_A3;                          // 释放内存，这是新加的代码，如果出错就删掉
        p_A3 = nullptr;
        delete[] m_curDisplayDataAngio1;
        m_curDisplayDataAngio1 = nullptr;
        ui->textEdit->append("Bscan 储存完毕！");
        return finalizeSavedDataFile(savePath,
                                     QString::fromLatin1(kSampleTypeFloat32Spectrum),
                                     true,
                                     m_BscanLen,
                                     1,
                                     !mainWidget::BGReductionFlag,
                                     !mainWidget::Wflag,
                                     false);
    }
    else if (mainWidget::scanMode == 10)    // 2D cross scan
    {
        int curIndexInMEMbuffer = 0;
        int bufferCompleted;
        int availbleIndex = -1;
        bufferRefreshIndex(bufferCompleted, curIndexInMEMbuffer, availbleIndex);
        if (bufferCompleted < 1 || availbleIndex == -1)
        {
            ui->textEdit->append("可保存的 2D 数据不足。");
            return false;
        }

        unsigned short *firstBscan = new unsigned short[m_AscanLen * m_BscanLen];
        unsigned short *secondBscan = new unsigned short[m_AscanLen * m_BscanLen];
        if (!copyCrossScanBscans(firstBscan,
                                 secondBscan,
                                 ssoctThread,
                                 availbleIndex,
                                 m_AscanLen,
                                 m_BscanLen))
        {
            delete[] firstBscan;
            delete[] secondBscan;
            ui->textEdit->append(QStringLiteral("2D cross 保存失败：扫描过程裁剪参数无效。"));
            return false;
        }

        p_A3 = new float[m_AscanLen * 2 * m_BscanLen];
        removeBGForBscanDouble(p_A3, firstBscan, secondBscan);

        QStringList selected_paths = save_dialog(tr("2D (*.2d);; 3D (*.3d)"));
        if (selected_paths.isEmpty())
        {
            delete[] p_A3;
            delete[] firstBscan;
            delete[] secondBscan;
            p_A3 = nullptr;
            ui->textEdit->append("保存被取消。");
            return false;
        }

        const QString savePath = selected_paths.at(0);
        std::ofstream file1;
        file1.open(savePath.toStdWString(), std::ios::binary);
        if (!file1.is_open())
        {
            delete[] p_A3;
            delete[] firstBscan;
            delete[] secondBscan;
            p_A3 = nullptr;
            ui->textEdit->append(QStringLiteral("保存失败，无法打开文件：%1").arg(savePath));
            return false;
        }
        file1.write((char*)p_A3, sizeof(float) * m_AscanLen * 2 * m_BscanLen);
        file1.close();

        delete[] p_A3;
        delete[] firstBscan;
        delete[] secondBscan;
        p_A3 = nullptr;
        ui->textEdit->append("2D 十字扫描储存完毕！");
        return finalizeSavedDataFile(savePath,
                                     QString::fromLatin1(kSampleTypeFloat32Spectrum),
                                     true,
                                     static_cast<qint64>(m_BscanLen) * 2,
                                     2,
                                     !mainWidget::BGReductionFlag,
                                     !mainWidget::Wflag,
                                     false);
    }
    else if (mainWidget::scanMode == 22)
    {
        int curIndexInMEMbuffer = 0;
        int bufferCompleted;
        int availbleIndex = -1;
        bufferRefreshIndex(bufferCompleted, curIndexInMEMbuffer, availbleIndex);
        if (isContinuousAcquisitionMode())
            return saveContinuousAcquisition();

        if (bufferCompleted < 1 || availbleIndex == -1)
        {
            ui->textEdit->append("可保存的 2D angio 数据不足。");
            return false;
        }

        const int bscanSampleCount = m_AscanLen * m_BscanLen;
        unsigned short *recentBscans = new unsigned short[bscanSampleCount * m_AngioRep];
        if (!copyAngioGroupBscans(recentBscans,
                                  ssoctThread,
                                  availbleIndex,
                                  m_AscanLen,
                                  m_BscanLen,
                                  m_AngioRep))
        {
            delete[] recentBscans;
            ui->textEdit->append(QStringLiteral("2D angio 保存失败：扫描组裁剪参数无效。"));
            return false;
        }

        p_A3 = new float[bscanSampleCount * m_AngioRep];
        for (int frame = 0; frame < m_AngioRep; ++frame)
        {
            removeBGForBscan(p_A3 + frame * bscanSampleCount,
                             recentBscans + frame * bscanSampleCount);
        }

        QStringList selected_paths = save_dialog(tr("2D (*.2d)"));
        if (selected_paths.isEmpty())
        {
            delete[] p_A3;
            delete[] recentBscans;
            p_A3 = nullptr;
            ui->textEdit->append("保存被取消。");
            return false;
        }

        const QString savePath = selected_paths.at(0);
        std::ofstream file1;
        file1.open(savePath.toStdWString(), std::ios::binary);
        if (!file1.is_open())
        {
            delete[] p_A3;
            delete[] recentBscans;
            p_A3 = nullptr;
            ui->textEdit->append(QStringLiteral("保存失败，无法打开文件：%1").arg(savePath));
            return false;
        }
        file1.write((char*)p_A3, sizeof(float) * bscanSampleCount * m_AngioRep);
        file1.close();

        delete[] p_A3;
        delete[] recentBscans;
        p_A3 = nullptr;
        ui->textEdit->append(QStringLiteral("2D angio 最近 %1 个 Bscan 储存完毕！").arg(m_AngioRep));
        return finalizeSavedDataFile(savePath,
                                     QString::fromLatin1(kSampleTypeFloat32Spectrum),
                                     true,
                                     static_cast<qint64>(m_BscanLen) * m_AngioRep,
                                     m_AngioRep,
                                     !mainWidget::BGReductionFlag,
                                     !mainWidget::Wflag,
                                     false);
    }
    else if (mainWidget::scanMode == 32)
    {
        QStringList selected_paths = save_dialog(tr("3D (*.3d)"),
                                                 QStringLiteral("volume3dSavePath"),
                                                 QStringLiteral("3d"));
        if (selected_paths.isEmpty())
        {
            ui->textEdit->append("保存被取消。");
            return false;
        }
        const QString savePath = selected_paths.at(0);
        qDebug()<<savePath;
        std::ofstream file1;
        file1.open(savePath.toStdWString(), std::ios::binary);
        if (!file1.is_open())
        {
            ui->textEdit->append(QStringLiteral("3D Angio 保存失败：无法打开文件 %1。").arg(savePath));
            return false;
        }

        int curIndexInMEMbuffer = 0;
        int bufferCompleted;
        int availbleIndex = -1;
        bufferRefreshIndex(bufferCompleted, curIndexInMEMbuffer, availbleIndex);
        // U16 *curData = ssoctThread->m_volumeMemBuffer[0];
        // U16 *pTemp1 = curData;

        // 直接把 m_volumeMemBuffer 中的 3D 数据写入文件，不进行去本底操作
        // 大概率是固定 Cscan 长度和 Bscan 长度相同，然后每一个 Bscan 重复 ??? 次
        const int volumeCount = expectedStoredBscanCount();
        const int sourceBufferCount = expectedAcquisitionBufferCount();
        const bool volumeSaved = writeCurrentVolumeBscans(file1,
                                                          volumeCount,
                                                          sourceBufferCount);
        file1.close();
        if (!volumeSaved)
        {
            ui->textEdit->append(QStringLiteral("3D Angio 保存失败：Bscan 周期裁剪或写入不完整。"));
            return false;
        }
        ui->textEdit->append("3D Angio 储存完毕！");
        const bool jsonAndFftOk = finalizeSavedDataFile(savePath,
                                                        QString::fromLatin1(kSampleTypeUint16Raw),
                                                        false,
                                                        static_cast<qint64>(volumeCount) * m_BscanLen,
                                                        volumeCount,
                                                        false,
                                                        false,
                                                        false);

        if (crossSave == nullptr)
            return jsonAndFftOk;

        // 从 crossSave 中读取 2D cross-section 数据，将其重新放大为 uint16 格式后储存
        // crossSave: (A/16 - BG - plotOffset) * spectralWindow
        uint16_t* u16Data = new uint16_t[m_AscanLen * 2 * m_BscanLen];
        for (size_t i = 0; i < m_AscanLen * 2 * m_BscanLen; ++i)
        {
            float transformedValue = (crossSave[i] + mythread::plotOffset) * 16.0;
            // 确保值在uint16的范围内
            transformedValue = std::max(0.0f, std::min(transformedValue, static_cast<float>(UINT16_MAX)));
            u16Data[i] = static_cast<uint16_t>(std::round(transformedValue));
        }


        // 获取原始文件名并替换后缀为 '.2dcross', 以保存 cross-section 数据
        QString originalFileName = savePath;
        QString crossSaveFileName = QFileInfo(originalFileName).absolutePath() + "/" +
                                    QFileInfo(originalFileName).baseName() + ".2dcross";

        // 使用标准C++文件I/O将crossSave数据写入新文件
        std::ofstream file2;
        file2.open(crossSaveFileName.toStdString(), std::ios::binary);
        const bool crossSaved = file2.is_open();
        if (crossSaved)
        {
            file2.write(reinterpret_cast<const char*>(u16Data), m_AscanLen * 2 * m_BscanLen * sizeof(uint16_t));
            file2.close();
            ui->textEdit->append("2D cross-section 储存完毕 ");
        }
        else
            ui->textEdit->append("无法打开文件保存 2D cross-section 数据 ");

        delete[] u16Data;
        return crossSaved && jsonAndFftOk;
    }
    else if (mainWidget::scanMode == 3)
    {
        QStringList selected_paths = save_dialog(tr("3D (*.3d)"),
                                                 QStringLiteral("volume3dSavePath"),
                                                 QStringLiteral("3d"));
        if (selected_paths.isEmpty())
        {
            ui->textEdit->append("保存被取消。");
            return false;
        }
        const QString savePath = selected_paths.at(0);
        qDebug()<<savePath;
        std::ofstream file1;
        file1.open(savePath.toStdWString(), std::ios::binary);
        if (!file1.is_open())
        {
            ui->textEdit->append(QStringLiteral("3D 保存失败：无法打开文件 %1。").arg(savePath));
            return false;
        }

        int curIndexInMEMbuffer = 0;
        int bufferCompleted;
        int availbleIndex = -1;
        bufferRefreshIndex(bufferCompleted, curIndexInMEMbuffer, availbleIndex);

        // 直接把 m_volumeMemBuffer 中的 3D 数据写入文件，不进行去本底操作
        // 从某种意义上说，我们是不是固定了 Cscan 长度和 Bscan 长度一样为 800?
        const int volumeCount = expectedStoredBscanCount();
        const int sourceBufferCount = expectedAcquisitionBufferCount();
        const bool volumeSaved = writeCurrentVolumeBscans(file1,
                                                          volumeCount,
                                                          sourceBufferCount);

        file1.close();
        if (!volumeSaved)
        {
            ui->textEdit->append(QStringLiteral("3D 保存失败：Bscan 周期裁剪或写入不完整。"));
            return false;
        }
        ui->textEdit->append("3D 储存完毕！");
        return finalizeSavedDataFile(savePath,
                                     QString::fromLatin1(kSampleTypeUint16Raw),
                                     false,
                                     static_cast<qint64>(volumeCount) * m_BscanLen,
                                     volumeCount,
                                     false,
                                     false,
                                     false);
    }
    else if ((mainWidget::scanMode == 42 || mainWidget::scanMode == kSymphonicScanMode)
             && isContinuousAcquisitionMode())
    {
        return saveContinuousAcquisition();
    }
    delete[] m_curDisplayDataAngio1;
    m_curDisplayDataAngio1 = nullptr;
    ui->textEdit->append("当前扫描模式没有对应的保存流程。");
    return false;
}


void mainWidget::on_saveButton_clicked()
{
    int ret = styledMessageBox(this,
                               QMessageBox::Question,
                               "保存数据",
                               "是否将数据保存到外部文件中？",
                               QMessageBox::Save | QMessageBox::Cancel,
                               QMessageBox::Cancel);
    if(ret == QMessageBox::Save)
    {
        if (m_scanActive)
            stopCurrentScan(true, true, "保存前已先停止当前扫描。");
        if (saveclicked())  // 这个函数似乎只给了 1D, 2D-Repeat, 3D-Angio, 3D 这四种模式下保存数据的代码；是否需要增补？
        {
            styledMessageBox(this,
                             QMessageBox::Information,
                             "提示",
                             "保存流程结束！",
                             QMessageBox::Ok,
                             QMessageBox::Ok);
        }
    }
    else if(ret == QMessageBox::Cancel)
    {
        styledMessageBox(this,
                         QMessageBox::Warning,
                         "警告",
                         "保存被取消！",
                         QMessageBox::Ok,
                         QMessageBox::Ok);
        return;
    }
}

void mainWidget::on_button_FFT_clicked()
{
    if (m_scanActive) {
        ui->textEdit->append(QStringLiteral("正在扫描，暂不能执行离线傅里叶变换。"));
        return;
    }

    FftFileSelection selection;
    if (!selectFftFileAndOptions(this, &selection))
        return;

    processFourierFile(selection.filePath,
                       selection.applyWindow,
                       selection.removeBackground,
                       selection.applyLogScale,
                       true);
}


void mainWidget::on_textEdit_textChanged()
{
    appendTextEditDeltaToLog(ui->textEdit,
                             QStringLiteral("info.log"),
                             QStringLiteral("INFO"),
                             m_infoLogTextLength);
    ui->textEdit->moveCursor(QTextCursor::End);
}

void mainWidget::on_background_clicked()
{   // 重新计算本底 & 本底归零
    if (mainWidget::BGReductionFlag == true)
    {
        ui->background->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/background2.png);}");
        int curIndexInMEMbuffer = 0;
        int bufferCompleted;
        int availbleIndex = -1;
        bufferRefreshIndex(bufferCompleted, curIndexInMEMbuffer, availbleIndex);
        if (availbleIndex == -1)
            return;

        // 根据缓存中指标为 availbleIndex = ssoctThread->m_lastAvailableIndex 的数据，用平均数的方法计算本底
        U16 *curData = nullptr;

        std::fill(m_BG.begin(), m_BG.end(), 0.0f);
        const int backgroundBscanCount = (mainWidget::scanMode == 1) ? 1 : m_BscanLen;
        std::vector<U16> backgroundData(m_AscanLen * backgroundBscanCount);
        if (mainWidget::scanMode == 10)
        {
            std::vector<U16> secondBscan(m_AscanLen * m_BscanLen);
            if (!copyCrossScanBscans(backgroundData.data(),
                                     secondBscan.data(),
                                     ssoctThread,
                                     availbleIndex,
                                     m_AscanLen,
                                     m_BscanLen))
                return;
        }
        else if (mainWidget::scanMode == 22)
        {
            std::vector<U16> angioBscans(m_AscanLen * m_BscanLen * m_AngioRep);
            if (!copyAngioGroupBscans(angioBscans.data(),
                                      ssoctThread,
                                      availbleIndex,
                                      m_AscanLen,
                                      m_BscanLen,
                                      m_AngioRep))
                return;
            memcpy(backgroundData.data(), angioBscans.data(), sizeof(U16) * m_AscanLen * m_BscanLen);
        }
        else
        {
            QMutexLocker lock(&ssoctThread->mutex1);
            const int sourceOffsetSamples = bscanDisplaySourceOffsetSamples(ssoctThread, m_AscanLen, m_BscanLen);
            memcpy(backgroundData.data(),
                   ssoctThread->m_volumeMemBuffer[availbleIndex] + sourceOffsetSamples,
                   sizeof(U16) * m_AscanLen * backgroundBscanCount);
        }
        curData = backgroundData.data();
        for (int j = 0; j < backgroundBscanCount; j++)
        {
            for (int i = 0; i < m_AscanLen; i++)
                m_BG[i] += ((curData[j * m_AscanLen + i ] >> 4) - mythread::plotOffset);
        }
        for (int i = 0; i < m_AscanLen; ++i)
            m_BG[i] /= backgroundBscanCount;
        mainWidget::BGReductionFlag = false;
    }
    else
    {
        ui->background->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/background.png);}");
        std::fill(m_BG.begin(), m_BG.end(), 0.0f);
        mainWidget::BGReductionFlag = true;
    }
}

void mainWidget::on_addWindow_clicked()
{
    if (mainWidget::Wflag == false)
    {
        ui->addWindow->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/addWindow.png);}");
        mainWidget::Wflag = true;
        resetSpectralWindow();
        saveSettings();

    }
    else
    {
        ui->addWindow->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/removeWindow.png);}");
        mainWidget::Wflag = false;
        resetSpectralWindow();
        saveSettings();
    }
}


void mainWidget::fftBscanAngio2(float * numbers)
{
    // 1. 计算两个 Bscan 数据的 fft, 类似 fftBscan, 但是取 phy = 0 (即设 ignore_dispersion == true)
    float* numberslog = calc_FFT_Bscan(m_AscanLen, m_BscanLen, 2, numbers, true);

    plotBscan(numberslog);
    mkl_ascend(numberslog);

    // 2. 把 numbers 的原始数据以两个 Bscan 的形式拼接成 cv::Mat 矩阵，截取前半部分用于血流计算
    // 为了简化计算，进行了优化：直接在拼装的阶段就把前半部分截取好，把 imgTrans 的第一维度砍了一半
    cv::Mat imgTrans(m_AscanLen / 2, m_BscanLen * 2, CV_32F);
    for (int k = 0; k < 2; k++)
    {
        for (int j = 0; j < m_BscanLen; j++)
        {
            for (int i = 0; i < m_AscanLen / 2; i++)
            {
                imgTrans.at<float>(i, k * m_BscanLen + j) = numbers[j * m_AscanLen + i + m_AscanLen * m_BscanLen * k];
            }
        }
    }

    // 3. 调用 angioCM 函数计算血流图像，并将其显示在 labelflow 上
    cv::Mat angioOutput = cv::Mat::zeros(m_AscanLen/2, m_BscanLen, CV_32F);

    angioCM(imgTrans, angioOutput, 2);

    Mat angioOutput_8bit = norm_mat_to_8bit(angioOutput);

    QImage Qtemp = putImage(angioOutput_8bit);
    ui->labelflow->setPixmap(QPixmap::fromImage(Qtemp.scaled(ui->labelflow->size())));
}

void mainWidget::fftBscanCross(float *numbers)
{
    // 1. 计算 Bscan 数据的 fft, 类似 fftBscan
    float* numberslog = calc_FFT_Bscan(m_AscanLen, m_BscanLen, 2, numbers, false);

    //plotBscan(numberslog);
    plotBscanCross1(numberslog);

    mkl_ascend(numberslog);
    mkl_ascend(m_cosphy);
    mkl_ascend(m_sinphy);
}

void mainWidget::angioCM(Mat &dataTrans, Mat &angioOutput, int Measure)
{
    // 该程序是仿照我写的angioCM的Matlab程序来写的
     //qDebug()<<"cm1 "<<QTime::currentTime();
    int nPairs = Measure - 1;
   // 考虑dataTrans为2048 * 3200的数组；

   // 首先确定original的size
    int nA = dataTrans.cols; // 列数
    int mA = dataTrans.rows; // 行数

    Mat angioInput1 = dataTrans(Range::all(), Range(0, nA / Measure * (Measure - 1))); // 存在内存泄露
    Mat angioInput2 = dataTrans(Range::all(), Range(nA / Measure, nA));

    int xx;
    int yy;
    xx = ui->spinBox_5->text().toInt();   //添加数据
    yy = ui->spinBox_6->text().toInt();   //添加数据

    cv::Mat windowCM = cv::Mat::ones(xx, yy, CV_32F);
    cv::Mat angioOut1;

    // 二维滤波的写法
    cv::filter2D(angioInput1, angioOut1, -1, windowCM, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);

    angioOut1 = angioOut1 / (xx * yy); // 18 = 6 * 3;
    cv::Mat angioOut2;
    cv::filter2D(angioInput2, angioOut2, -1, windowCM, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
    angioOut2 = angioOut2 / (xx * yy);

    cv::Mat cmNumerator = (angioInput1 - angioOut1).mul(angioInput2 - angioOut2);
    cv::Mat cmIntensityAvel = (angioInput1 - angioOut1).mul(angioInput1 - angioOut1);
    cv::Mat cmIntensityAve2 = (angioInput2 - angioOut2).mul(angioInput2 - angioOut2);

    // angioOutput = dataTrans(Range(0, m_AscanLen), Range(0,800));
    // 再进行滤波的操作
    Mat Numeratorsum;
    cv::filter2D(cmNumerator, Numeratorsum, -1, windowCM, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);

    Mat intensityAvesum1;
    cv::filter2D(cmIntensityAvel, intensityAvesum1, -1, windowCM, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);

    Mat intensityAvesum2;
    cv::filter2D(cmIntensityAve2, intensityAvesum2, -1, windowCM, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);

    // 求和
    Mat allSum;
    Mat sqrtIn;
    cv::sqrt(intensityAvesum1.mul(intensityAvesum2), sqrtIn);
    allSum = Numeratorsum / sqrtIn;

    // 需要提前将angioOutput赋值为0;
    for (int iPair = 0; iPair < nPairs; iPair++)
        angioOutput = allSum(Range::all(), Range(nA / Measure * iPair, nA / Measure * (iPair + 1))) + angioOutput;

    // 做一些简单的处理
    angioOutput = angioOutput / nPairs;
    cv::abs(angioOutput);
    angioOutput = 1 - angioOutput;
}
