#include "NiPcie6353Dac.h"

#include <QLibrary>
#include <QByteArray>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#ifdef Q_OS_WIN
#define DAQMX_CALL __stdcall
#else
#define DAQMX_CALL
#endif

namespace {

using int32 = int;
using uInt32 = unsigned int;
using uInt64 = unsigned long long;
using bool32 = unsigned int;
using float64 = double;
using TaskHandle = void*;

const int32 kDaqmxValGroupByChannel = 0;
const int32 kDaqmxValRising = 10280;
const int32 kDaqmxValContSamps = 10123;
const int32 kDaqmxValVolts = 10348;
const int32 kDaqmxValAllowRegen = 10097;

using CreateTaskFn = int32 (DAQMX_CALL *)(const char[], TaskHandle *);
using ClearTaskFn = int32 (DAQMX_CALL *)(TaskHandle);
using StartTaskFn = int32 (DAQMX_CALL *)(TaskHandle);
using StopTaskFn = int32 (DAQMX_CALL *)(TaskHandle);
using CreateAOVoltageChanFn = int32 (DAQMX_CALL *)(TaskHandle, const char[], const char[],
                                                   float64, float64, int32, const char[]);
using CfgSampClkTimingFn = int32 (DAQMX_CALL *)(TaskHandle, const char[], float64,
                                                int32, int32, uInt64);
using CfgDigEdgeStartTrigFn = int32 (DAQMX_CALL *)(TaskHandle, const char[], int32);
using WriteAnalogF64Fn = int32 (DAQMX_CALL *)(TaskHandle, int32, bool32, float64,
                                               bool32, const float64[], int32 *, bool32 *);
using SetWriteRegenModeFn = int32 (DAQMX_CALL *)(TaskHandle, int32);
using GetBufOutputOnbrdBufSizeFn = int32 (DAQMX_CALL *)(TaskHandle, uInt32 *);
using SetAOUseOnlyOnBrdMemFn = int32 (DAQMX_CALL *)(TaskHandle, const char[], bool32);
using GetExtendedErrorInfoFn = int32 (DAQMX_CALL *)(char[], uInt32);
using GetSysDevNamesFn = int32 (DAQMX_CALL *)(char[], uInt32);
using GetDevProductTypeFn = int32 (DAQMX_CALL *)(const char[], char[], uInt32);
using GetDevIsSimulatedFn = int32 (DAQMX_CALL *)(const char[], bool32 *);
using GetDevSerialNumFn = int32 (DAQMX_CALL *)(const char[], uInt32 *);
using GetDevAOPhysicalChansFn = int32 (DAQMX_CALL *)(const char[], char[], uInt32);
using GetDevAOMaxRateFn = int32 (DAQMX_CALL *)(const char[], float64 *);
using GetDevAOVoltageRngsFn = int32 (DAQMX_CALL *)(const char[], float64[], uInt32);
using GetDevAIPhysicalChansFn = int32 (DAQMX_CALL *)(const char[], char[], uInt32);

bool daqmxFailed(int32 status)
{
    return status < 0;
}

template <typename Fn>
bool resolveDaqmxFunction(QLibrary &library,
                          Fn &target,
                          const char *name,
                          bool required,
                          QString *errorMessage)
{
    target = reinterpret_cast<Fn>(library.resolve(name));
    if (target)
        return true;

    if (required && errorMessage)
        *errorMessage = QStringLiteral("NI-DAQmx 函数缺失：%1。").arg(QString::fromLatin1(name));
    return !required;
}

QString daqmxExtendedError(GetExtendedErrorInfoFn getExtendedErrorInfo, int32 status)
{
    if (getExtendedErrorInfo) {
        char buffer[4096] = {0};
        if (getExtendedErrorInfo(buffer, static_cast<uInt32>(sizeof(buffer))) >= 0 && buffer[0] != '\0')
            return QString::fromLocal8Bit(buffer).trimmed();
    }
    return QStringLiteral("NI-DAQmx error %1").arg(status);
}

bool checkDaqmxStatus(int32 status,
                      const QString &stage,
                      GetExtendedErrorInfoFn getExtendedErrorInfo,
                      QString *errorMessage)
{
    if (!daqmxFailed(status))
        return true;
    if (errorMessage)
        *errorMessage = QStringLiteral("%1失败：%2")
                            .arg(stage, daqmxExtendedError(getExtendedErrorInfo, status));
    return false;
}

QStringList splitDaqmxNameList(const QString &names)
{
    QStringList result;
    const QStringList tokens = names.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        const QString trimmed = token.trimmed();
        if (!trimmed.isEmpty())
            result.append(trimmed);
    }
    return result;
}

bool containsDaqmxDeviceName(const QStringList &devices, const QString &deviceName)
{
    for (const QString &device : devices) {
        if (device.compare(deviceName, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QString formatRate(double hertz)
{
    if (hertz >= 1000000.0)
        return QStringLiteral("%1 MHz").arg(hertz / 1000000.0, 0, 'f', 3);
    if (hertz >= 1000.0)
        return QStringLiteral("%1 kHz").arg(hertz / 1000.0, 0, 'f', 3);
    return QStringLiteral("%1 Hz").arg(hertz, 0, 'f', 0);
}

QString formatVoltageRanges(const std::vector<float64> &ranges)
{
    QStringList texts;
    for (size_t i = 0; i + 1 < ranges.size(); i += 2) {
        const double minV = ranges[i];
        const double maxV = ranges[i + 1];
        if (std::fabs(minV + maxV) < 0.001) {
            texts.append(QStringLiteral("±%1 V")
                         .arg(std::max(std::fabs(minV), std::fabs(maxV)), 0, 'f', 2));
        } else {
            texts.append(QStringLiteral("%1 到 %2 V")
                         .arg(minV, 0, 'f', 2)
                         .arg(maxV, 0, 'f', 2));
        }
    }
    texts.removeDuplicates();
    return texts.join(QStringLiteral("，"));
}

template <typename Fn>
bool queryDaqmxDeviceText(Fn function,
                          const QString &deviceName,
                          const QString &stage,
                          GetExtendedErrorInfoFn getExtendedErrorInfo,
                          QString *text,
                          QString *errorMessage)
{
    char buffer[4096] = {0};
    const QByteArray deviceBytes = deviceName.toLocal8Bit();
    const int32 status = function(deviceBytes.constData(),
                                  buffer,
                                  static_cast<uInt32>(sizeof(buffer)));
    if (!checkDaqmxStatus(status, stage, getExtendedErrorInfo, errorMessage))
        return false;
    if (text)
        *text = QString::fromLocal8Bit(buffer).trimmed();
    return true;
}

QString joinedPhysicalChannels(const NiPcie6353DacConfig &config)
{
    const QString trimmedChannels = config.aoChannels.trimmed();
    const QString device = config.deviceName.trimmed().isEmpty()
        ? QStringLiteral("Dev1")
        : config.deviceName.trimmed();
    QStringList physicalChannels;
    const QStringList tokens = trimmedChannels.split(QLatin1Char(','),
                                                     Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        const QString trimmedToken = token.trimmed();
        if (trimmedToken.isEmpty())
            continue;
        physicalChannels.append(trimmedToken.contains(QLatin1Char('/'))
                                ? trimmedToken
                                : device + QLatin1Char('/') + trimmedToken);
    }
    if (physicalChannels.isEmpty())
        physicalChannels.append(device + QStringLiteral("/ao0:1"));
    return physicalChannels.join(QStringLiteral(","));
}

QStringList expandOneTokenChannels(const QString &token)
{
    const int slashIndex = token.lastIndexOf(QLatin1Char('/'));
    const QString prefix = (slashIndex >= 0) ? token.left(slashIndex + 1) : QString();
    const QString channelToken = (slashIndex >= 0) ? token.mid(slashIndex + 1) : token;
    if (!channelToken.startsWith(QStringLiteral("ao"), Qt::CaseInsensitive))
        return {token};

    const QString rangeText = channelToken.mid(2);
    const int colonIndex = rangeText.indexOf(QLatin1Char(':'));
    if (colonIndex < 0)
        return {token};

    bool firstOk = false;
    bool lastOk = false;
    const int first = rangeText.left(colonIndex).toInt(&firstOk);
    const int last = rangeText.mid(colonIndex + 1).toInt(&lastOk);
    if (!firstOk || !lastOk || last < first)
        return {token};

    QStringList expanded;
    for (int channel = first; channel <= last; ++channel)
        expanded.append(prefix + QStringLiteral("ao%1").arg(channel));
    return expanded;
}

QStringList expandedAoChannels(const QString &physicalChannels)
{
    QStringList expanded;
    const QStringList tokens = physicalChannels.split(QLatin1Char(','),
                                                      Qt::SkipEmptyParts);
    for (const QString &token : tokens)
        expanded.append(expandOneTokenChannels(token.trimmed()));
    return expanded;
}

int countAoChannels(const QString &physicalChannels)
{
    return std::max(1, expandedAoChannels(physicalChannels).size());
}

double daCodeToVolts(unsigned short code)
{
    const double centered = static_cast<double>(code) - 32768.0;
    return centered * (10.0 / 65536.0);
}

double clampedVolts(double value, double range)
{
    return std::max(-range, std::min(range, value));
}

} // namespace

struct NiPcie6353Dac::Impl
{
    QLibrary library;
    bool loaded = false;
    TaskHandle task = nullptr;
    NiPcie6353DacConfig preparedConfig;
    int preparedSamplesPerChannel = 0;

    CreateTaskFn createTask = nullptr;
    ClearTaskFn clearTask = nullptr;
    StartTaskFn startTask = nullptr;
    StopTaskFn stopTask = nullptr;
    CreateAOVoltageChanFn createAOVoltageChan = nullptr;
    CfgSampClkTimingFn cfgSampClkTiming = nullptr;
    CfgDigEdgeStartTrigFn cfgDigEdgeStartTrig = nullptr;
    WriteAnalogF64Fn writeAnalogF64 = nullptr;
    SetWriteRegenModeFn setWriteRegenMode = nullptr;
    GetBufOutputOnbrdBufSizeFn getBufOutputOnbrdBufSize = nullptr;
    SetAOUseOnlyOnBrdMemFn setAOUseOnlyOnBrdMem = nullptr;
    GetExtendedErrorInfoFn getExtendedErrorInfo = nullptr;

    template <typename Fn>
    bool resolve(Fn &target, const char *name, QString *errorMessage)
    {
        target = reinterpret_cast<Fn>(library.resolve(name));
        if (target)
            return true;
        if (errorMessage)
            *errorMessage = QStringLiteral("NI-DAQmx 函数缺失：%1。").arg(QString::fromLatin1(name));
        return false;
    }

    bool ensureLoaded(QString *errorMessage)
    {
        if (loaded)
            return true;

        library.setFileName(QStringLiteral("nicaiu"));
        if (!library.load()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("无法加载 nicaiu.dll，请确认已安装 NI-DAQmx 运行库：%1")
                                    .arg(library.errorString());
            }
            return false;
        }

        if (!resolve(createTask, "DAQmxCreateTask", errorMessage)
            || !resolve(clearTask, "DAQmxClearTask", errorMessage)
            || !resolve(startTask, "DAQmxStartTask", errorMessage)
            || !resolve(stopTask, "DAQmxStopTask", errorMessage)
            || !resolve(createAOVoltageChan, "DAQmxCreateAOVoltageChan", errorMessage)
            || !resolve(cfgSampClkTiming, "DAQmxCfgSampClkTiming", errorMessage)
            || !resolve(cfgDigEdgeStartTrig, "DAQmxCfgDigEdgeStartTrig", errorMessage)
            || !resolve(writeAnalogF64, "DAQmxWriteAnalogF64", errorMessage)
            || !resolve(setWriteRegenMode, "DAQmxSetWriteRegenMode", errorMessage)
            || !resolve(getBufOutputOnbrdBufSize, "DAQmxGetBufOutputOnbrdBufSize", errorMessage)
            || !resolve(setAOUseOnlyOnBrdMem, "DAQmxSetAOUseOnlyOnBrdMem", errorMessage)
            || !resolve(getExtendedErrorInfo, "DAQmxGetExtendedErrorInfo", errorMessage)) {
            library.unload();
            return false;
        }

        loaded = true;
        return true;
    }

    QString extendedError(int32 status) const
    {
        if (getExtendedErrorInfo) {
            char buffer[4096] = {0};
            if (getExtendedErrorInfo(buffer, sizeof(buffer)) >= 0 && buffer[0] != '\0')
                return QString::fromLocal8Bit(buffer);
        }
        return QStringLiteral("NI-DAQmx error %1").arg(status);
    }

    bool check(int32 status, const QString &stage, QString *errorMessage) const
    {
        if (!daqmxFailed(status))
            return true;
        if (errorMessage)
            *errorMessage = QStringLiteral("%1失败：%2").arg(stage, extendedError(status));
        return false;
    }

    void clearPreparedTask()
    {
        if (!task)
            return;
        if (stopTask)
            stopTask(task);
        if (clearTask)
            clearTask(task);
        task = nullptr;
        preparedSamplesPerChannel = 0;
    }
};

NiPcie6353Dac::NiPcie6353Dac()
    : d(new Impl)
{
}

NiPcie6353Dac::~NiPcie6353Dac()
{
    stop();
    delete d;
}

bool NiPcie6353Dac::probeDevice(const NiPcie6353DacConfig &config,
                                 QStringList *infoLines,
                                 QString *errorMessage)
{
    if (infoLines)
        infoLines->clear();

    QLibrary library;
    library.setFileName(QStringLiteral("nicaiu"));
    if (!library.load()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法加载 nicaiu.dll，请确认已安装 NI-DAQmx 运行库：%1")
                                .arg(library.errorString());
        }
        return false;
    }

    GetExtendedErrorInfoFn getExtendedErrorInfo = nullptr;
    GetSysDevNamesFn getSysDevNames = nullptr;
    GetDevProductTypeFn getDevProductType = nullptr;
    GetDevIsSimulatedFn getDevIsSimulated = nullptr;
    GetDevSerialNumFn getDevSerialNum = nullptr;
    GetDevAOPhysicalChansFn getDevAOPhysicalChans = nullptr;
    GetDevAOMaxRateFn getDevAOMaxRate = nullptr;
    GetDevAOVoltageRngsFn getDevAOVoltageRngs = nullptr;
    GetDevAIPhysicalChansFn getDevAIPhysicalChans = nullptr;

    resolveDaqmxFunction(library, getExtendedErrorInfo, "DAQmxGetExtendedErrorInfo", false, nullptr);
    if (!resolveDaqmxFunction(library, getSysDevNames, "DAQmxGetSysDevNames", true, errorMessage)
        || !resolveDaqmxFunction(library, getDevProductType, "DAQmxGetDevProductType", true, errorMessage)) {
        return false;
    }
    resolveDaqmxFunction(library, getDevIsSimulated, "DAQmxGetDevIsSimulated", false, nullptr);
    resolveDaqmxFunction(library, getDevSerialNum, "DAQmxGetDevSerialNum", false, nullptr);
    resolveDaqmxFunction(library, getDevAOPhysicalChans, "DAQmxGetDevAOPhysicalChans", false, nullptr);
    resolveDaqmxFunction(library, getDevAOMaxRate, "DAQmxGetDevAOMaxRate", false, nullptr);
    resolveDaqmxFunction(library, getDevAOVoltageRngs, "DAQmxGetDevAOVoltageRngs", false, nullptr);
    resolveDaqmxFunction(library, getDevAIPhysicalChans, "DAQmxGetDevAIPhysicalChans", false, nullptr);

    char sysDevBuffer[4096] = {0};
    int32 status = getSysDevNames(sysDevBuffer, static_cast<uInt32>(sizeof(sysDevBuffer)));
    if (!checkDaqmxStatus(status,
                          QStringLiteral("DAQmxGetSysDevNames"),
                          getExtendedErrorInfo,
                          errorMessage)) {
        return false;
    }

    const QString sysDevNames = QString::fromLocal8Bit(sysDevBuffer).trimmed();
    const QStringList sysDevices = splitDaqmxNameList(sysDevNames);
    const QString deviceName = config.deviceName.trimmed().isEmpty()
        ? QStringLiteral("Dev1")
        : config.deviceName.trimmed();
    if (!containsDaqmxDeviceName(sysDevices, deviceName)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("DAQmx 系统中未找到设备 \"%1\"。当前识别到的 NI 设备：%2。")
                                .arg(deviceName,
                                     sysDevNames.isEmpty() ? QStringLiteral("无") : sysDevNames);
        }
        return false;
    }

    QString productType;
    if (!queryDaqmxDeviceText(getDevProductType,
                              deviceName,
                              QStringLiteral("DAQmxGetDevProductType"),
                              getExtendedErrorInfo,
                              &productType,
                              errorMessage)) {
        return false;
    }

    bool simulatedKnown = false;
    bool32 isSimulated = 0;
    if (getDevIsSimulated) {
        const QByteArray deviceBytes = deviceName.toLocal8Bit();
        status = getDevIsSimulated(deviceBytes.constData(), &isSimulated);
        if (!checkDaqmxStatus(status,
                              QStringLiteral("DAQmxGetDevIsSimulated"),
                              getExtendedErrorInfo,
                              errorMessage)) {
            return false;
        }
        simulatedKnown = true;
        if (isSimulated != 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("DAQmx 设备 \"%1\" 是模拟设备，不是真实插入的 PCIe6353 硬件。")
                                    .arg(deviceName);
            }
            return false;
        }
    }

    QString normalizedProduct = productType;
    normalizedProduct.remove(QLatin1Char('-'));
    normalizedProduct.remove(QLatin1Char(' '));
    if (!normalizedProduct.contains(QStringLiteral("PCIe6353"), Qt::CaseInsensitive)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("DAQmx 设备 \"%1\" 的型号为 \"%2\"，不是 PCIe-6353。")
                                .arg(deviceName, productType);
        }
        return false;
    }

    QStringList lines;
    lines.append(QStringLiteral("NI PCIe6353 板卡信息："));
    lines.append(QStringLiteral("DAQmx 设备名：%1").arg(deviceName));
    lines.append(QStringLiteral("产品型号：%1").arg(productType));
    lines.append(QStringLiteral("当前 AO 配置：%1，输出范围=±%2 V")
                 .arg(joinedPhysicalChannels(config))
                 .arg(config.outputRangeVolts > 0.0 ? config.outputRangeVolts : 5.0, 0, 'f', 2));
    lines.append(QStringLiteral("NI-DAQmx 系统设备：%1").arg(sysDevNames));
    if (simulatedKnown)
        lines.append(QStringLiteral("模拟设备：否"));

    if (getDevSerialNum) {
        const QByteArray deviceBytes = deviceName.toLocal8Bit();
        uInt32 serialNumber = 0;
        status = getDevSerialNum(deviceBytes.constData(), &serialNumber);
        if (!daqmxFailed(status)) {
            lines.append(QStringLiteral("序列号：0x%1")
                         .arg(QString::number(serialNumber, 16).toUpper()));
        } else {
            lines.append(QStringLiteral("序列号：读取失败（%1）")
                         .arg(daqmxExtendedError(getExtendedErrorInfo, status)));
        }
    }

    if (getDevAOPhysicalChans) {
        QString aoChannels;
        QString optionalError;
        if (queryDaqmxDeviceText(getDevAOPhysicalChans,
                                 deviceName,
                                 QStringLiteral("DAQmxGetDevAOPhysicalChans"),
                                 getExtendedErrorInfo,
                                 &aoChannels,
                                 &optionalError)) {
            lines.append(QStringLiteral("AO 物理通道：%1").arg(aoChannels));
        } else {
            lines.append(QStringLiteral("AO 物理通道：读取失败（%1）").arg(optionalError));
        }
    }

    if (getDevAOMaxRate) {
        const QByteArray deviceBytes = deviceName.toLocal8Bit();
        float64 aoMaxRate = 0.0;
        status = getDevAOMaxRate(deviceBytes.constData(), &aoMaxRate);
        if (!daqmxFailed(status))
            lines.append(QStringLiteral("AO 最大更新率：%1").arg(formatRate(aoMaxRate)));
        else
            lines.append(QStringLiteral("AO 最大更新率：读取失败（%1）")
                         .arg(daqmxExtendedError(getExtendedErrorInfo, status)));
    }

    if (getDevAOVoltageRngs) {
        const QByteArray deviceBytes = deviceName.toLocal8Bit();
        const int32 requiredSize = getDevAOVoltageRngs(deviceBytes.constData(), nullptr, 0);
        if (daqmxFailed(requiredSize)) {
            lines.append(QStringLiteral("AO 电压范围：读取失败（%1）")
                         .arg(daqmxExtendedError(getExtendedErrorInfo, requiredSize)));
        } else if (requiredSize > 0) {
            std::vector<float64> voltageRanges(static_cast<size_t>(requiredSize), 0.0);
            status = getDevAOVoltageRngs(deviceBytes.constData(),
                                         voltageRanges.data(),
                                         static_cast<uInt32>(voltageRanges.size()));
            if (!daqmxFailed(status)) {
                const QString rangeText = formatVoltageRanges(voltageRanges);
                if (!rangeText.isEmpty())
                    lines.append(QStringLiteral("AO 电压范围：%1").arg(rangeText));
            } else {
                lines.append(QStringLiteral("AO 电压范围：读取失败（%1）")
                             .arg(daqmxExtendedError(getExtendedErrorInfo, status)));
            }
        }
    }

    if (getDevAIPhysicalChans) {
        QString aiChannels;
        QString optionalError;
        if (queryDaqmxDeviceText(getDevAIPhysicalChans,
                                 deviceName,
                                 QStringLiteral("DAQmxGetDevAIPhysicalChans"),
                                 getExtendedErrorInfo,
                                 &aiChannels,
                                 &optionalError)) {
            lines.append(QStringLiteral("AI 物理通道：%1").arg(aiChannels));
        } else {
            lines.append(QStringLiteral("AI 物理通道：读取失败（%1）").arg(optionalError));
        }
    }

    if (infoLines)
        *infoLines = lines;
    return true;
}

bool NiPcie6353Dac::prepare(const NiPcie6353DacConfig &config,
                            const unsigned short *xData,
                            int xLength,
                            const unsigned short *yData,
                            int yLength,
                            int sampleRateHz,
                            QString *diagnosticMessage,
                            QString *errorMessage)
{
    if (!xData || !yData || xLength <= 0 || yLength <= 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("NI AO 准备失败：X/Y 波形为空。");
        return false;
    }
    if (sampleRateHz <= 0 || sampleRateHz > 1250000) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("NI AO 采样率无效或超过 PCIe-6353 多通道上限：%1 Hz。")
                                .arg(sampleRateHz);
        }
        return false;
    }

    const double range = (config.outputRangeVolts > 0.0)
        ? config.outputRangeVolts
        : 5.0;
    const QString physicalChannels = joinedPhysicalChannels(config);
    const int channelCount = countAoChannels(physicalChannels);
    if (channelCount < 2) {
        if (errorMessage)
            *errorMessage = QStringLiteral("NI AO 至少需要两路通道用于 X/Y，目前为：%1。")
                                .arg(physicalChannels);
        return false;
    }

    const int samplesPerChannel = std::max(xLength, yLength);
    if ((samplesPerChannel % xLength) != 0 || (samplesPerChannel % yLength) != 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("NI AO 当前要求 X/Y 波形长度可整周期重复：X=%1，Y=%2。")
                                .arg(xLength)
                                .arg(yLength);
        }
        return false;
    }
    if (samplesPerChannel > (std::numeric_limits<int32>::max)()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("NI AO 样点数超过 DAQmxWriteAnalogF64 范围。");
        return false;
    }

    if (!d->ensureLoaded(errorMessage))
        return false;
    d->clearPreparedTask();

    std::vector<float64> output(static_cast<size_t>(channelCount) * samplesPerChannel, 0.0);
    for (int i = 0; i < samplesPerChannel; ++i) {
        output[i] = clampedVolts(daCodeToVolts(xData[i % xLength]), range);
        output[samplesPerChannel + i] = clampedVolts(daCodeToVolts(yData[i % yLength]), range);
    }

    TaskHandle task = nullptr;
    int32 status = d->createTask("SSOCT_NI_PCIe6353_AO", &task);
    if (!d->check(status, QStringLiteral("DAQmxCreateTask"), errorMessage))
        return false;

    auto clearOnFailure = [&]() {
        if (task) {
            d->stopTask(task);
            d->clearTask(task);
            task = nullptr;
        }
    };

    const QByteArray channelBytes = physicalChannels.toLocal8Bit();
    status = d->createAOVoltageChan(task,
                                    channelBytes.constData(),
                                    "",
                                    -range,
                                    range,
                                    kDaqmxValVolts,
                                    nullptr);
    if (!d->check(status, QStringLiteral("DAQmxCreateAOVoltageChan"), errorMessage)) {
        clearOnFailure();
        return false;
    }

    const QByteArray clockBytes = config.sampleClockSource.trimmed().toLocal8Bit();
    status = d->cfgSampClkTiming(task,
                                 clockBytes.constData(),
                                 static_cast<float64>(sampleRateHz),
                                 kDaqmxValRising,
                                 kDaqmxValContSamps,
                                 static_cast<uInt64>(samplesPerChannel));
    if (!d->check(status, QStringLiteral("DAQmxCfgSampClkTiming"), errorMessage)) {
        clearOnFailure();
        return false;
    }

    const QString triggerSource = config.startTriggerSource.trimmed();
    if (!triggerSource.isEmpty()) {
        const QByteArray triggerBytes = triggerSource.toLocal8Bit();
        status = d->cfgDigEdgeStartTrig(task, triggerBytes.constData(), kDaqmxValRising);
        if (!d->check(status, QStringLiteral("DAQmxCfgDigEdgeStartTrig"), errorMessage)) {
            clearOnFailure();
            return false;
        }
    }

    status = d->setWriteRegenMode(task, kDaqmxValAllowRegen);
    if (!d->check(status, QStringLiteral("DAQmxSetWriteRegenMode"), errorMessage)) {
        clearOnFailure();
        return false;
    }

    uInt32 onboardSamplesPerChannel = 0;
    status = d->getBufOutputOnbrdBufSize(task, &onboardSamplesPerChannel);
    if (!d->check(status, QStringLiteral("DAQmxGetBufOutputOnbrdBufSize"), errorMessage)) {
        clearOnFailure();
        return false;
    }
    const bool onboardSizeKnown = onboardSamplesPerChannel > 0;
    const bool fitsOnboardFifo = onboardSizeKnown
        && static_cast<uInt64>(samplesPerChannel) <= static_cast<uInt64>(onboardSamplesPerChannel);
    const bool autoFifoOnly =
        config.fifoRegenerationMode == NiPcie6353FifoRegenerationMode::AutoIfFits;
    bool usingFifoOnly = false;
    QString regenerationNote;
    if (autoFifoOnly) {
        if (!onboardSizeKnown) {
            regenerationNote = QStringLiteral("（DAQmx 未返回板载 FIFO 容量，已退回 standard）");
        } else if (!fitsOnboardFifo) {
            regenerationNote = QStringLiteral("（每通道 %1 点超过板载 FIFO %2 点，已退回 standard）")
                .arg(samplesPerChannel)
                .arg(onboardSamplesPerChannel);
        } else {
            usingFifoOnly = true;
        }
    }

    if (usingFifoOnly) {
        const QStringList channels = expandedAoChannels(physicalChannels);
        for (const QString &channel : channels) {
            const QByteArray oneChannelBytes = channel.toLocal8Bit();
            status = d->setAOUseOnlyOnBrdMem(task, oneChannelBytes.constData(), 1);
            if (!d->check(status,
                          QStringLiteral("DAQmxSetAOUseOnlyOnBrdMem(%1)").arg(channel),
                          errorMessage)) {
                clearOnFailure();
                return false;
            }
        }
    }

    int32 written = 0;
    status = d->writeAnalogF64(task,
                               static_cast<int32>(samplesPerChannel),
                               0,
                               30.0,
                               kDaqmxValGroupByChannel,
                               output.data(),
                               &written,
                               nullptr);
    if (!d->check(status, QStringLiteral("DAQmxWriteAnalogF64"), errorMessage)) {
        clearOnFailure();
        return false;
    }
    if (written != samplesPerChannel) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("NI AO 写入样点数不完整：期望 %1，实际 %2。")
                                .arg(samplesPerChannel)
                                .arg(written);
        }
        clearOnFailure();
        return false;
    }

    d->task = task;
    d->preparedConfig = config;
    d->preparedSamplesPerChannel = samplesPerChannel;

    if (diagnosticMessage) {
        const QString regenerationText = usingFifoOnly
            ? QStringLiteral("FIFO-only")
            : QStringLiteral("standard");
        *diagnosticMessage = QStringLiteral("NI AO 已准备：通道=%1，采样率=%2 Hz，每通道=%3 点，X 模板=%4 点，Y 模板=%5 点，输出范围=±%6 V，regeneration=%7%8，板载 FIFO≈%9 点/通道%10%11。")
            .arg(physicalChannels)
            .arg(sampleRateHz)
            .arg(samplesPerChannel)
            .arg(xLength)
            .arg(yLength)
            .arg(range, 0, 'f', 2)
            .arg(regenerationText)
            .arg(regenerationNote)
            .arg(onboardSamplesPerChannel)
            .arg(triggerSource.isEmpty()
                 ? QString()
                 : QStringLiteral("，StartTrigger=%1").arg(triggerSource))
            .arg(config.sampleClockSource.trimmed().isEmpty()
                 ? QString()
                 : QStringLiteral("，SampleClock=%1").arg(config.sampleClockSource.trimmed()));
    }
    return true;
}

bool NiPcie6353Dac::start(QString *errorMessage)
{
    if (!d->task) {
        if (errorMessage)
            *errorMessage = QStringLiteral("NI AO 尚未准备。");
        return false;
    }
    const int32 status = d->startTask(d->task);
    return d->check(status, QStringLiteral("DAQmxStartTask"), errorMessage);
}

void NiPcie6353Dac::stop()
{
    d->clearPreparedTask();
}

bool NiPcie6353Dac::resetOutputsToZero(const NiPcie6353DacConfig &config, QString *errorMessage)
{
    if (!d->ensureLoaded(errorMessage))
        return false;

    TaskHandle task = nullptr;
    int32 status = d->createTask("SSOCT_NI_PCIe6353_AO_Zero", &task);
    if (!d->check(status, QStringLiteral("DAQmxCreateTask"), errorMessage))
        return false;

    auto clearOnExit = [&]() {
        if (task) {
            d->stopTask(task);
            d->clearTask(task);
            task = nullptr;
        }
    };

    const double range = (config.outputRangeVolts > 0.0)
        ? config.outputRangeVolts
        : 5.0;
    const QString physicalChannels = joinedPhysicalChannels(config);
    const QByteArray channelBytes = physicalChannels.toLocal8Bit();
    status = d->createAOVoltageChan(task,
                                    channelBytes.constData(),
                                    "",
                                    -range,
                                    range,
                                    kDaqmxValVolts,
                                    nullptr);
    if (!d->check(status, QStringLiteral("DAQmxCreateAOVoltageChan"), errorMessage)) {
        clearOnExit();
        return false;
    }

    std::vector<float64> zero(static_cast<size_t>(countAoChannels(physicalChannels)), 0.0);
    int32 written = 0;
    status = d->writeAnalogF64(task,
                               1,
                               1,
                               10.0,
                               kDaqmxValGroupByChannel,
                               zero.data(),
                               &written,
                               nullptr);
    if (!d->check(status, QStringLiteral("DAQmxWriteAnalogF64(0V)"), errorMessage)) {
        clearOnExit();
        return false;
    }

    clearOnExit();
    return true;
}

bool NiPcie6353Dac::isPrepared() const
{
    return d->task != nullptr;
}
