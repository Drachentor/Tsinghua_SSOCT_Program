#include "mythread.h"
#include "mainwidget.h"
#include "AppVersion.h"
#include "DeviceSettings.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QMessageBox>
#include <QSettings>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <cstddef>
#include <vector>

#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795
#endif

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

namespace {
const int kReadAdCountUnit = 2048;
const int kTriggerLengthQuantumSamples = TRIG_UNIT * 2;
const int kHighThroughputReadChunkSamples = 4 * 1024 * 1024;
const int kAuxDaZeroBufferLength = 32;

struct DaBufferStats
{
    bool valid = false;
    unsigned short minValue = 0;
    unsigned short maxValue = 0;
    unsigned short firstValue = 0;
    unsigned short lastValue = 0;
};

DaBufferStats daBufferStats(const unsigned short *data, int length)
{
    DaBufferStats stats;
    if (data == nullptr || length <= 0)
        return stats;

    stats.valid = true;
    stats.firstValue = data[0];
    stats.lastValue = data[length - 1];
    stats.minValue = stats.firstValue;
    stats.maxValue = stats.firstValue;
    for (int i = 1; i < length; ++i)
    {
        stats.minValue = std::min(stats.minValue, data[i]);
        stats.maxValue = std::max(stats.maxValue, data[i]);
    }
    return stats;
}

QString daBufferStatsText(const DaBufferStats &stats)
{
    if (!stats.valid)
        return QStringLiteral("无效");

    return QStringLiteral("min=%1 max=%2 first=%3 last=%4")
        .arg(stats.minValue)
        .arg(stats.maxValue)
        .arg(stats.firstValue)
        .arg(stats.lastValue);
}

QString triggerModeName(LONG mode)
{
    switch (mode)
    {
        case TRIG_MODE_CONTINUE:
            return QStringLiteral("连续采集");
        case TRIG_MODE_POST:
            return QStringLiteral("触发采集/后触发");
        case TRIG_MODE_1TRIG:
            return QStringLiteral("单触发");
        case TRIG_MODE_FTRIG:
            return QStringLiteral("场触发");
        default:
            return QStringLiteral("未知");
    }
}

QString triggerSourceName(LONG source)
{
    switch (source)
    {
        case TRIG_SRC_EXT_RISING:
            return QStringLiteral("外正沿");
        case TRIG_SRC_EXT_FALLING:
            return QStringLiteral("外负沿");
        case TRIG_SRC_SOFT:
            return QStringLiteral("软件触发");
        case TRIG_SRC_INT_RISING:
            return QStringLiteral("内正沿");
        case TRIG_SRC_INT_FALLING:
            return QStringLiteral("内负沿");
        case TRIG_SRC_CH1_RISING:
            return QStringLiteral("CH1 正沿");
        case TRIG_SRC_CH1_FALLING:
            return QStringLiteral("CH1 负沿");
        case TRIG_SRC_CH2_RISING:
            return QStringLiteral("CH2 正沿");
        case TRIG_SRC_CH2_FALLING:
            return QStringLiteral("CH2 负沿");
        default:
            return QStringLiteral("未知");
    }
}

bool isLiveDisplayScanMode(int scanMode)
{
    return scanMode == 1 || scanMode == 2 || scanMode == 10 || scanMode == 22
        || scanMode == 42 || scanMode == 43;
}

bool isFiniteVolumeScanMode(int scanMode)
{
    return scanMode == 3 || scanMode == 32;
}

LONG alignedReadAdCount(ULONGLONG sampleCount)
{
    if (sampleCount > MAX_READ_LEN)
        sampleCount = MAX_READ_LEN;

    sampleCount = (sampleCount / kReadAdCountUnit) * kReadAdCountUnit;
    if (sampleCount < kReadAdCountUnit)
        return 0;

    return static_cast<LONG>(sampleCount);
}

LONG fullBufferReadCount(ULONGLONG sampleCount, int singleBufferSize)
{
    if (singleBufferSize <= 0)
        return 0;

    ULONGLONG maxCount = MAX_READ_LEN;
    maxCount = (maxCount / static_cast<ULONGLONG>(singleBufferSize))
        * static_cast<ULONGLONG>(singleBufferSize);
    if (maxCount < static_cast<ULONGLONG>(singleBufferSize))
        return 0;
    if (sampleCount > maxCount)
        sampleCount = maxCount;

    sampleCount = (sampleCount / static_cast<ULONGLONG>(singleBufferSize))
        * static_cast<ULONGLONG>(singleBufferSize);
    if (sampleCount < static_cast<ULONGLONG>(singleBufferSize))
        return 0;

    return static_cast<LONG>(sampleCount);
}

int adReadChunkSizeForLogicalFrame(int logicalFrameSamples)
{
    if (logicalFrameSamples <= 0)
        return 0;

    ULONGLONG desired = static_cast<ULONGLONG>(logicalFrameSamples);
    if (desired < kReadAdCountUnit)
        desired = kReadAdCountUnit;
    if (desired > MAX_READ_LEN)
        desired = MAX_READ_LEN;

    ULONGLONG aligned = ((desired + kReadAdCountUnit - 1) / kReadAdCountUnit)
        * kReadAdCountUnit;
    if (aligned > MAX_READ_LEN)
        aligned = (static_cast<ULONGLONG>(MAX_READ_LEN) / kReadAdCountUnit)
            * kReadAdCountUnit;
    if (aligned < kReadAdCountUnit || aligned > INT_MAX)
        return 0;

    return static_cast<int>(aligned);
}

QString sourceDirectoryPath()
{
    return QFileInfo(QString::fromLocal8Bit(__FILE__)).absolutePath();
}

bool vesselPathNoReturnToZero()
{
    QSettings settings(DeviceSettings::settingsFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("VesselFindingDialog"));
    const bool noReturn = settings.value(QStringLiteral("noReturn"), false).toBool();
    settings.endGroup();
    return noReturn;
}

QString scanPathFilePath(const QString &fileName)
{
    return QDir(DeviceSettings::scanPathDirectoryPath()).filePath(fileName);
}

int gcdInt(int a, int b)
{
    a = std::abs(a);
    b = std::abs(b);
    while (b != 0)
    {
        const int r = a % b;
        a = b;
        b = r;
    }
    return a == 0 ? 1 : a;
}

long long gcdLongLong(long long a, long long b)
{
    a = std::llabs(a);
    b = std::llabs(b);
    while (b != 0)
    {
        const long long r = a % b;
        a = b;
        b = r;
    }
    return a == 0 ? 1 : a;
}

int alignedCycleSampleCountForAdRead(int ascanLen, int cycleAlineCount, int *cyclesPerBuffer)
{
    if (cyclesPerBuffer != nullptr)
        *cyclesPerBuffer = 1;
    if (ascanLen <= 0 || cycleAlineCount <= 0)
        return 0;

    const long long cycleSamples = static_cast<long long>(ascanLen) * cycleAlineCount;
    const long long cycleBlock = kReadAdCountUnit / gcdLongLong(cycleSamples, kReadAdCountUnit);
    const long long readSamples = cycleSamples * cycleBlock;
    if (cycleBlock <= 0 || readSamples <= 0 || readSamples > MAX_READ_LEN || readSamples > INT_MAX)
        return 0;

    if (cyclesPerBuffer != nullptr)
        *cyclesPerBuffer = static_cast<int>(cycleBlock);
    return static_cast<int>(readSamples);
}

int alignedAlineCountForAdRead(int alineCount, int ascanLen)
{
    if (ascanLen <= 0)
        return 0;

    const int alineBlock = kReadAdCountUnit / gcdInt(ascanLen, kReadAdCountUnit);
    if (alineBlock <= 0)
        return 0;

    const int requested = std::max(alineCount, alineBlock);
    return ((requested + alineBlock - 1) / alineBlock) * alineBlock;
}

unsigned short interpolateDaValue(unsigned short startValue,
                                  unsigned short endValue,
                                  int step,
                                  int totalSteps)
{
    if (totalSteps <= 0)
        return endValue;

    const double t = static_cast<double>(step + 1) / static_cast<double>(totalSteps);
    const double value = static_cast<double>(startValue)
        + (static_cast<double>(endValue) - static_cast<double>(startValue)) * t;
    return static_cast<unsigned short>(std::max(0.0, std::min(65535.0, value + 0.5)));
}

bool readDaPathFile(const QString &filePath, QVector<unsigned short> &values, QString &errorMessage)
{
    QFile file(filePath);
    if (!file.open(QFile::ReadOnly | QFile::Text))
    {
        errorMessage = QStringLiteral("无法打开路径文件：%1。").arg(filePath);
        return false;
    }

    QTextStream stream(&file);
    int lineNumber = 0;
    while (!stream.atEnd())
    {
        ++lineNumber;
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty())
            continue;

        bool ok = false;
        const int value = line.toInt(&ok);
        if (!ok || value < 0 || value > 65535)
        {
            errorMessage = QStringLiteral("路径文件 %1 第 %2 行不是有效的 DA 数值。")
                .arg(filePath)
                .arg(lineNumber);
            return false;
        }
        values.append(static_cast<unsigned short>(value));
    }

    if (values.isEmpty())
    {
        errorMessage = QStringLiteral("路径文件为空：%1。").arg(filePath);
        return false;
    }
    return true;
}
}

// 构造函数：初始化多缓冲区
mythread::mythread(QObject *parent) : QObject(parent)
{
    m_pcieHandle = INVALID_HANDLE_VALUE;
    m_isCapturing = false;
    m_buffersCompleted = 0;
    m_curIndexInMemBuffer = 0;
    m_lastAvailableIndex = -1;
    m_bufferCount = 3200;
    m_singleBufferSize = 0;
    m_adReadChunkSize = 0;
    m_repeatCyclesPerBuffer = 1;
    m_plannedVolumeBufferCount = 0;
    m_currentSegmentBufferCount = 0;
    pDataX = NULL;
    pDataY = NULL;
    pDataRF = NULL;
    Voltage = 1500.0;
    galvoFreq = 2000.0;
    duty_cycle = 0.5;
    AscanFreq = 200319;
    len_ZeroBuffer = 50;
    len_Transition = 0;
    len_BscanCycle = 0;
    len_LinearScan = 0;
    len_XScanData = 0;
    len_YScanData = 0;
    len_RfScanData = 0;
    m_daSegmentStartCscan = 0;
    m_daSegmentCscanLen = 0;
    m_daTotalCscanLen = 0;
    m_vesselAutoMoveAlineCount = 0;
    m_vesselNoReturnToZero = false;
    m_daDelay = 0;
    m_rfDelay = 0;
    m_pwmDelay = 0;
    m_dacBackend = DacBackend::Pcie3640;

    // 初始化结构体，把内部置零
    memset(&m_pciePara, 0, sizeof(PCIe3640_PARA_INIT));
    memset(&m_pcieBuf, 0, sizeof(PCIE_BUF));
    qDebug() << "清华大学 SSOCT 系统驱动程序";
    qDebug().noquote().nospace() << "    Ver. " << currentVersion << " 清华大学 沈逸然 石叶炅\n";
}

// 析构函数：释放多缓冲区资源
mythread::~mythread()
{
    StopADCapture();
    // 释放所有缓冲区
    for (int i=0; i < m_volumeMemBuffer.size(); i++)
    {
        if (m_volumeMemBuffer[i])
        {
            VirtualFree(m_volumeMemBuffer[i], 0, MEM_RELEASE);
            m_volumeMemBuffer[i] = NULL;
        }
    }
    m_volumeMemBuffer.clear();
    delete [] pDataX;
    delete [] pDataY;
    delete [] pDataRF;
    pDataX = NULL;
    pDataY = NULL;
    pDataRF = NULL;
    m_niDac.stop();
}

// 停止采集
void mythread::StopADCapture()
{
    QMutexLocker locker(&mutex1); // 使用原有锁名称mutex1
    // 互斥锁确保线程安全，防止在采集过程中被调用导致资源冲突
    m_isCapturing = false;
    m_niDac.stop();

    // 关闭设备
    if (m_pcieHandle != INVALID_HANDLE_VALUE)
    {
        PCIe3640_intDA(m_pcieHandle);
        PCIe3640_StopAD(m_pcieHandle);
        PCIe3640_UnLink(m_pcieHandle);
        m_pcieHandle = INVALID_HANDLE_VALUE;
    }

    qDebug() << "StopADCapture(): 已停止采集！";
}

bool mythread::EnsureDeviceLinked()
{
    if (m_pcieHandle != INVALID_HANDLE_VALUE)
        return true;

    memset(&m_pcieBuf, 0, sizeof(PCIE_BUF));
    m_pcieHandle = PCIe3640_Link(0, &m_pcieBuf);
    if (m_pcieHandle == INVALID_HANDLE_VALUE)
    {
        return FailDAConfig(QStringLiteral("PCIe3640 设备连接失败，请检查采集卡电源、驱动、设备号和 PCIe3640.dll。"));
    }

    qDebug() << "EnsureDeviceLinked(): PCIe3640 连接成功，设备句柄 =" << m_pcieHandle;
    return true;
}

QString mythread::LastDAError() const
{
    return m_lastDAError;
}

bool mythread::InitializePositionOutputsToZero()
{
    QMutexLocker locker(&mutex1);
    m_lastDAError.clear();
    if (!EnsureDeviceLinked())
        return false;

    return WritePositionZeroToCardLocked(QStringLiteral("启动初始化"));
}

void mythread::UsePcie3640DacBackend()
{
    QMutexLocker locker(&mutex1);
    m_dacBackend = DacBackend::Pcie3640;
    m_niDac.stop();
}

void mythread::UseNiPcie6353DacBackend(const NiPcie6353DacConfig &config)
{
    QMutexLocker locker(&mutex1);
    m_dacBackend = DacBackend::NiPcie6353;
    m_niDacConfig = config;
}

void mythread::ConfigureSymphonicTiming(int ascanFreq, int moveAlineCount, bool noReturnToZero)
{
    QMutexLocker locker(&mutex1);
    AscanFreq = ascanFreq;
    m_vesselAutoMoveAlineCount = std::max(0, moveAlineCount);
    m_vesselNoReturnToZero = noReturnToZero;
    len_LinearScan = m_BscanLen;
    len_Transition = m_vesselNoReturnToZero ? 0 : m_vesselAutoMoveAlineCount;
    len_BscanCycle = m_BscanLen
        + (m_vesselNoReturnToZero ? m_vesselAutoMoveAlineCount : 2 * m_vesselAutoMoveAlineCount);
    m_lastDAError.clear();
}

bool mythread::ConfigureSymphonicRfOnly(int ascanFreq)
{
    QMutexLocker locker(&mutex1);
    m_lastDAError.clear();
    AscanFreq = ascanFreq;
    if (AscanFreq <= 0)
    {
        return FailDAConfig(QStringLiteral("Symphonic RF 时基输出需要有效的 AscanFreq（当前 %1）。")
            .arg(AscanFreq));
    }

    GenData_init(32, 32);
    if (!GenRfSquareData())
        return false;

    if (!EnsureDeviceLinked())
        return false;

    qDebug() << "ConfigureSymphonicRfOnly(): 已准备 RF 时基，DACH1/DACH2 保持 0V。";
    return WriteDABuffersToCard();
}

bool mythread::ConfigureSymphonicDAFromBoundPath(int ascanFreq)
{
    QMutexLocker locker(&mutex1);
    m_lastDAError.clear();
    AscanFreq = ascanFreq;
    if (AscanFreq <= 0)
    {
        return FailDAConfig(QStringLiteral("Symphonic DA 测试输出需要有效的 AscanFreq（当前 %1）。")
            .arg(AscanFreq));
    }

    QVector<unsigned short> vesselPathX;
    QVector<unsigned short> vesselPathY;
    QString errorMessage;
    if (!readDaPathFile(scanPathFilePath(QStringLiteral("scanX.txt")), vesselPathX, errorMessage)
        || !readDaPathFile(scanPathFilePath(QStringLiteral("scanY.txt")), vesselPathY, errorMessage))
    {
        return FailDAConfig(errorMessage);
    }
    if (vesselPathX.isEmpty())
        return FailDAConfig(QStringLiteral("Symphonic DA 测试输出路径为空，请重新生成音频并导出路径。"));
    if (vesselPathX.size() != vesselPathY.size())
    {
        return FailDAConfig(QStringLiteral("Symphonic DA 测试输出路径文件长度不一致：scanX=%1, scanY=%2。")
            .arg(vesselPathX.size())
            .arg(vesselPathY.size()));
    }
    if (vesselPathX.size() > INT_MAX)
    {
        return FailDAConfig(QStringLiteral("Symphonic DA 测试输出路径过长：%1 点。")
            .arg(vesselPathX.size()));
    }

    GenData_init(vesselPathX.size(), vesselPathY.size());
    for (int i = 0; i < vesselPathX.size(); ++i)
    {
        pDataX[i] = vesselPathX[i];
        pDataY[i] = vesselPathY[i];
    }

    if (!GenRfSquareData())
        return false;

    if (!EnsureDeviceLinked())
        return false;

    qDebug() << "ConfigureSymphonicDAFromBoundPath(): 已载入绑定路径，点数 =" << len_XScanData;
    return WriteDABuffersToCard();
}

bool mythread::FailDAConfig(const QString &message)
{
    m_lastDAError = message;
    qDebug().noquote() << "[错误]" << message;
    return false;
}

bool mythread::WritePositionZeroToCardLocked(const QString &stage)
{
    if (m_pcieHandle == INVALID_HANDLE_VALUE)
        return FailDAConfig(QStringLiteral("%1位置 0V 初始化失败：PCIe3640 设备未连接。").arg(stage));

    const int zeroSampleCount = kAuxDaZeroBufferLength;
    unsigned short zeroBuffer[zeroSampleCount];
    std::fill(zeroBuffer, zeroBuffer + zeroSampleCount, PositionDacZeroCode);

    PCIe3640_intDA(m_pcieHandle);
    const BOOL okX = PCIe3640_SetDA(m_pcieHandle,
                                    DA_SRC_DACH1,
                                    zeroSampleCount,
                                    zeroBuffer,
                                    m_daDelay,
                                    0);
    const BOOL okY = PCIe3640_SetDA(m_pcieHandle,
                                    DA_SRC_DACH2,
                                    zeroSampleCount,
                                    zeroBuffer,
                                    m_daDelay,
                                    0);
    const BOOL okAux1 = PCIe3640_SetDA(m_pcieHandle,
                                       DA_SRC_DACH3,
                                       zeroSampleCount,
                                       zeroBuffer,
                                       m_daDelay,
                                       0);
    const BOOL okAux2 = PCIe3640_SetDA(m_pcieHandle,
                                       DA_SRC_DACH4,
                                       zeroSampleCount,
                                       zeroBuffer,
                                       m_daDelay,
                                       0);
    if (!okX || !okY || !okAux1 || !okAux2)
    {
        return FailDAConfig(QStringLiteral("%1位置 0V 初始化失败：写入 DACH1-DACH4 失败（X=%2, Y=%3, Aux1=%4, Aux2=%5）。")
            .arg(stage)
            .arg(okX)
            .arg(okY)
            .arg(okAux1)
            .arg(okAux2));
    }

    if (!PCIe3640_StartDA(m_pcieHandle, 0x01, m_pwmDelay))
        return FailDAConfig(QStringLiteral("%1位置 0V 初始化失败：启动低速 DA 输出失败。").arg(stage));

    qDebug().noquote() << QStringLiteral("%1位置 0V 初始化成功：DACH1-DACH4 已输出中点码 %2。")
                              .arg(stage)
                              .arg(PositionDacZeroCode);
    return true;
}

bool mythread::ConfigureDA(double Voltage, double galvoFreq, int AscanFreq,
                           double duty_cycle, int scanMode)
{
    return ConfigureDA(Voltage, galvoFreq, AscanFreq, duty_cycle, scanMode,
                       0, mainWidget::CscanLen, mainWidget::CscanLen);
}

bool mythread::ConfigureDA(double Voltage, double galvoFreq, int AscanFreq,
                           double duty_cycle, int scanMode,
                           int segmentStartCscan, int segmentCscanLen, int totalCscanLen)
{
    QMutexLocker locker(&mutex1);
    m_lastDAError.clear();
    this->Voltage = Voltage;
    this->galvoFreq = galvoFreq;
    this->duty_cycle = duty_cycle;
    this->AscanFreq = AscanFreq;
    m_daTotalCscanLen = totalCscanLen > 0 ? totalCscanLen : mainWidget::CscanLen;
    m_daSegmentStartCscan = 0;
    m_daSegmentCscanLen = mainWidget::CscanLen;

    if (isFiniteVolumeScanMode(scanMode))
    {
        if (m_daTotalCscanLen <= 0 || segmentStartCscan < 0 || segmentCscanLen <= 0
            || segmentStartCscan + segmentCscanLen > m_daTotalCscanLen)
        {
            return FailDAConfig(QStringLiteral("3D 分段参数无效：起始=%1，段长=%2，总 Bscan 数=%3。")
                .arg(segmentStartCscan)
                .arg(segmentCscanLen)
                .arg(m_daTotalCscanLen));
        }

        m_daSegmentStartCscan = segmentStartCscan;
        m_daSegmentCscanLen = segmentCscanLen;
    }

    if (!GenData(scanMode))
        return false;
    if (!GenRfSquareData())
        return false;

    if (m_dacBackend == DacBackend::NiPcie6353)
    {
        if (!EnsureDeviceLinked())
            return false;
        if (!WriteRfBufferToPcieCardOnly())
            return false;

        QString diagnosticMessage;
        QString errorMessage;
        if (!m_niDac.prepare(m_niDacConfig,
                             pDataX,
                             len_XScanData,
                             pDataY,
                             len_YScanData,
                             AscanFreq,
                             &diagnosticMessage,
                             &errorMessage))
        {
            return FailDAConfig(errorMessage);
        }
        emit acquisitionStatus(diagnosticMessage);
        if (isFiniteVolumeScanMode(scanMode) && len_XScanData != len_YScanData)
        {
            emit acquisitionStatus(QStringLiteral("NI AO 诊断：NI-DAQmx 不支持 USB3020 式独立段表；当前将 X 周期扩展到 Y 的 %1 点完整缓冲后使用 regeneration。")
                                       .arg(std::max(len_XScanData, len_YScanData)));
        }
        return true;
    }

    if (!EnsureDeviceLinked())
        return false;

    return WriteDABuffersToCard();
}

bool mythread::WriteDABuffersToCard()
{
    if (m_pcieHandle == INVALID_HANDLE_VALUE || !pDataX || !pDataY || !pDataRF)
        return FailDAConfig(QStringLiteral("DA 写入前检查失败：设备未连接，或 X/Y/RF 缓冲区为空。"));
    if (len_XScanData == 0 || len_YScanData == 0 || len_RfScanData == 0)
    {
        return FailDAConfig(QStringLiteral("DA 写入前检查失败：数据长度为 0（RF=%1, X=%2, Y=%3）。")
            .arg(len_RfScanData).arg(len_XScanData).arg(len_YScanData));
    }
    if (len_XScanData > LONG_MAX || len_YScanData > LONG_MAX || len_RfScanData > LONG_MAX)
    {
        return FailDAConfig(QStringLiteral("DA 数据长度超过 PCIe3640_SetDA 的 LONG 参数范围（RF=%1, X=%2, Y=%3）。")
            .arg(len_RfScanData).arg(len_XScanData).arg(len_YScanData));
    }
    if (len_XScanData > MAX_ADD_TRIG_LEN || len_YScanData > MAX_ADD_TRIG_LEN)
    {
        int maxCscanLen = 0;
        if (len_XScanData > 0)
        {
            const qint64 repeatFactor = (mainWidget::scanMode == 32) ? m_AngioRep : 1;
            const qint64 samplesPerBscan = static_cast<qint64>(len_XScanData) * repeatFactor;
            if (samplesPerBscan > 0)
                maxCscanLen = static_cast<int>(MAX_ADD_TRIG_LEN / samplesPerBscan);
        }
        return FailDAConfig(QStringLiteral("DA 数据长度超过 PCIe3640 每路 1M 样点上限（最大 %1；当前 X=%2, Y=%3）。当前每个 Bscan 的 DA 周期长度为 %4 点，若保持当前帧率、占空比和 Angio 重复次数，Bscan 数量最多约为 %5。")
            .arg(MAX_ADD_TRIG_LEN)
            .arg(len_XScanData)
            .arg(len_YScanData)
            .arg(len_XScanData)
            .arg(maxCscanLen));
    }

    const DaBufferStats rfStats = daBufferStats(pDataRF, len_RfScanData);
    const DaBufferStats xStats = daBufferStats(pDataX, len_XScanData);
    const DaBufferStats yStats = daBufferStats(pDataY, len_YScanData);
    emit acquisitionStatus(QStringLiteral("DA 诊断：准备写入 PCIe3640，RF=%1 点（%2），X=%3 点（%4），Y=%5 点（%6）。")
                               .arg(len_RfScanData)
                               .arg(daBufferStatsText(rfStats))
                               .arg(len_XScanData)
                               .arg(daBufferStatsText(xStats))
                               .arg(len_YScanData)
                               .arg(daBufferStatsText(yStats)));

    PCIe3640_intDA(m_pcieHandle);
    unsigned short auxZeroBuffer[kAuxDaZeroBufferLength];
    std::fill(auxZeroBuffer,
              auxZeroBuffer + kAuxDaZeroBufferLength,
              PositionDacZeroCode);
    BOOL okRF = PCIe3640_SetDA(m_pcieHandle, DA_SRC_RF, (LONG)len_RfScanData,
                               pDataRF, m_rfDelay, RfDacAgc);
    BOOL okX = PCIe3640_SetDA(m_pcieHandle, DA_SRC_DACH1, (LONG)len_XScanData,
                              pDataX, m_daDelay, 0);
    BOOL okY = PCIe3640_SetDA(m_pcieHandle, DA_SRC_DACH2, (LONG)len_YScanData,
                              pDataY, m_daDelay, 0);
    BOOL okAux1 = PCIe3640_SetDA(m_pcieHandle, DA_SRC_DACH3,
                                 kAuxDaZeroBufferLength, auxZeroBuffer,
                                 m_daDelay, 0);
    BOOL okAux2 = PCIe3640_SetDA(m_pcieHandle, DA_SRC_DACH4,
                                 kAuxDaZeroBufferLength, auxZeroBuffer,
                                 m_daDelay, 0);

    if (!okRF || !okX || !okY || !okAux1 || !okAux2)
    {
        return FailDAConfig(QStringLiteral("调用 PCIe3640_SetDA 写入失败：RF=%1, X=%2, Y=%3, Aux1=%4, Aux2=%5；长度 RF=%6, X=%7, Y=%8, Aux=%9。")
            .arg(okRF).arg(okX).arg(okY).arg(okAux1).arg(okAux2)
            .arg(len_RfScanData).arg(len_XScanData).arg(len_YScanData)
            .arg(kAuxDaZeroBufferLength));
    }

    qDebug() << "WriteDABuffersToCard(): DA 数据写入成功，RF 点数 =" << len_RfScanData
             << "X 点数 =" << len_XScanData << "Y 点数 =" << len_YScanData
             << "Aux 零点数 =" << kAuxDaZeroBufferLength;
    emit acquisitionStatus(QStringLiteral("DA 诊断：PCIe3640_SetDA 写入成功（RF delay=%1, X/Y delay=%2, RF AGC=%3；RF 口=Ascan 时基，LDA1=X，LDA2=Y，DACH3/DACH4 已配置为 0V）。")
                               .arg(m_rfDelay)
                               .arg(m_daDelay)
                               .arg(RfDacAgc));
    return true;
}

bool mythread::WriteRfBufferToPcieCardOnly()
{
    if (m_pcieHandle == INVALID_HANDLE_VALUE || !pDataRF)
        return FailDAConfig(QStringLiteral("PCIe3640 RF 写入前检查失败：设备未连接，或 RF 缓冲区为空。"));
    if (len_RfScanData == 0)
        return FailDAConfig(QStringLiteral("PCIe3640 RF 写入前检查失败：RF 数据长度为 0。"));
    if (len_RfScanData > LONG_MAX)
    {
        return FailDAConfig(QStringLiteral("PCIe3640 RF 数据长度超过 PCIe3640_SetDA 的 LONG 参数范围（RF=%1）。")
            .arg(len_RfScanData));
    }

    const DaBufferStats rfStats = daBufferStats(pDataRF, len_RfScanData);
    emit acquisitionStatus(QStringLiteral("DA 诊断：准备写入 PCIe3640 RF 时基，RF=%1 点（%2）；DACH1-DACH4 保持 0V，X/Y 由 NI PCIe6353 输出。")
                               .arg(len_RfScanData)
                               .arg(daBufferStatsText(rfStats)));

    PCIe3640_intDA(m_pcieHandle);
    unsigned short auxZeroBuffer[kAuxDaZeroBufferLength];
    std::fill(auxZeroBuffer,
              auxZeroBuffer + kAuxDaZeroBufferLength,
              PositionDacZeroCode);
    const BOOL okRF = PCIe3640_SetDA(m_pcieHandle, DA_SRC_RF, (LONG)len_RfScanData,
                                     pDataRF, m_rfDelay, RfDacAgc);
    const BOOL okX = PCIe3640_SetDA(m_pcieHandle, DA_SRC_DACH1,
                                    kAuxDaZeroBufferLength, auxZeroBuffer,
                                    m_daDelay, 0);
    const BOOL okY = PCIe3640_SetDA(m_pcieHandle, DA_SRC_DACH2,
                                    kAuxDaZeroBufferLength, auxZeroBuffer,
                                    m_daDelay, 0);
    const BOOL okAux1 = PCIe3640_SetDA(m_pcieHandle, DA_SRC_DACH3,
                                       kAuxDaZeroBufferLength, auxZeroBuffer,
                                       m_daDelay, 0);
    const BOOL okAux2 = PCIe3640_SetDA(m_pcieHandle, DA_SRC_DACH4,
                                       kAuxDaZeroBufferLength, auxZeroBuffer,
                                       m_daDelay, 0);

    if (!okRF || !okX || !okY || !okAux1 || !okAux2)
    {
        return FailDAConfig(QStringLiteral("调用 PCIe3640_SetDA 写入 RF/零位失败：RF=%1, X0=%2, Y0=%3, Aux1=%4, Aux2=%5；长度 RF=%6, Zero=%7。")
            .arg(okRF).arg(okX).arg(okY).arg(okAux1).arg(okAux2)
            .arg(len_RfScanData)
            .arg(kAuxDaZeroBufferLength));
    }

    emit acquisitionStatus(QStringLiteral("DA 诊断：PCIe3640 RF 时基写入成功（RF delay=%1, RF AGC=%2；位置通道已配置为 0V）。")
                               .arg(m_rfDelay)
                               .arg(RfDacAgc));
    return true;
}

bool mythread::StartDAScan(int scanMode)
{
    Q_UNUSED(scanMode);
    QMutexLocker locker(&mutex1);
    if (m_pcieHandle == INVALID_HANDLE_VALUE)
    {
        m_lastDAError = QStringLiteral("启动 DA 失败：PCIe3640 句柄无效。");
        qDebug() << "[错误] StartDAScan(): PCIe3640 句柄无效";
        emit acquisitionStatus(QStringLiteral("DA 诊断：%1").arg(m_lastDAError));
        return false;
    }

    if (m_dacBackend == DacBackend::NiPcie6353)
    {
        QString niError;
        if (!m_niDac.start(&niError))
        {
            m_lastDAError = QStringLiteral("启动 NI PCIe6353 AO 失败：%1").arg(niError);
            emit acquisitionStatus(QStringLiteral("DA 诊断：%1").arg(m_lastDAError));
            return false;
        }
    }

    BOOL ok = PCIe3640_StartDA(m_pcieHandle, 0x01, m_pwmDelay);
    if (!ok)
    {
        if (m_dacBackend == DacBackend::NiPcie6353)
            m_niDac.stop();
        m_lastDAError = QStringLiteral("启动 DA 失败：PCIe3640_StartDA 返回失败。");
        qDebug() << "[错误] StartDAScan(): 启动 DA 失败";
        emit acquisitionStatus(QStringLiteral("DA 诊断：%1").arg(m_lastDAError));
    }
    else
    {
        m_lastDAError.clear();
        if (m_dacBackend == DacBackend::NiPcie6353)
        {
            emit acquisitionStatus(QStringLiteral("DA 诊断：NI PCIe6353 AO 已启动；PCIe3640 RF 时基已启动（stDA=0x01, pwmDelay=%1）。")
                                       .arg(m_pwmDelay));
        }
        else
        {
            emit acquisitionStatus(QStringLiteral("DA 诊断：PCIe3640_StartDA 启动成功（stDA=0x01, pwmDelay=%1）。")
                                       .arg(m_pwmDelay));
        }
    }
    return ok;
}

void mythread::StopDAScan()
{
    QMutexLocker locker(&mutex1);
    if (m_dacBackend == DacBackend::NiPcie6353)
    {
        m_niDac.stop();
        QString niError;
        if (!m_niDac.resetOutputsToZero(m_niDacConfig, &niError))
        {
            qDebug().noquote() << QStringLiteral("[警告] NI PCIe6353 AO 复位到 0V 失败：%1").arg(niError);
            emit acquisitionStatus(QStringLiteral("DA 诊断：NI PCIe6353 AO 复位到 0V 失败：%1").arg(niError));
        }
        else
        {
            emit acquisitionStatus(QStringLiteral("DA 诊断：NI PCIe6353 AO 已停止并复位到 0V。"));
        }
    }
    if (m_pcieHandle != INVALID_HANDLE_VALUE)
    {
        m_lastDAError.clear();
        WritePositionZeroToCardLocked(QStringLiteral("停止扫描后"));
    }
}

void mythread::SetVolumeSegmentAcquisitionPlan(int plannedBufferCount, int currentSegmentBufferCount)
{
    QMutexLocker locker(&mutex1);
    m_plannedVolumeBufferCount = std::max(0, plannedBufferCount);
    m_currentSegmentBufferCount = std::max(0, currentSegmentBufferCount);
}

bool mythread::RestartADForNextSegment()
{
    QMutexLocker locker(&mutex1);
    if (m_pcieHandle == INVALID_HANDLE_VALUE || m_volumeMemBuffer.isEmpty())
    {
        emit acquisitionStatus(QStringLiteral("分段扫描：采集卡未初始化，无法开始下一段。"));
        return false;
    }

    PCIe3640_StopAD(m_pcieHandle);
    if (!PCIe3640_initAD(m_pcieHandle, &m_pciePara))
    {
        emit acquisitionStatus(QStringLiteral("分段扫描：重新初始化 AD 采集参数失败。"));
        return false;
    }

    m_isCapturing = true;
    return true;
}

void mythread::FillUncompletedBuffersWithZeroVoltage(int expectedBufferCount)
{
    QMutexLocker locker(&mutex1);
    if (m_volumeMemBuffer.isEmpty() || m_singleBufferSize <= 0)
        return;

    int startIndex = m_buffersCompleted;
    if (startIndex < 0)
        startIndex = 0;
    if (startIndex > expectedBufferCount)
        startIndex = expectedBufferCount;

    int stopIndex = expectedBufferCount;
    if (stopIndex > m_volumeMemBuffer.size())
        stopIndex = m_volumeMemBuffer.size();

    for (int i = startIndex; i < stopIndex; ++i)
    {
        U16 *buf = m_volumeMemBuffer[i];
        if (!buf)
            continue;
        for (int j = 0; j < m_singleBufferSize; ++j)
            buf[j] = AdcZeroCode;
    }

    if (stopIndex > startIndex)
    {
        m_buffersCompleted = stopIndex;
        m_curIndexInMemBuffer = stopIndex % m_volumeMemBuffer.size();
        m_lastAvailableIndex = stopIndex - 1;
        qDebug() << "FillUncompletedBuffersWithZeroVoltage(): 已将未完成的"
                 << (stopIndex - startIndex) << "个缓冲区置为零电压。";
    }
}

int mythread::singleBufferSize() const
{
    return m_singleBufferSize;
}

int mythread::bufferCount() const
{
    return m_bufferCount;
}

int mythread::adReadChunkSize() const
{
    return m_adReadChunkSize;
}

int mythread::bscanCycleAlineCount() const
{
    return len_BscanCycle;
}

int mythread::transitionAlineCount() const
{
    return len_Transition;
}

int mythread::linearScanAlineCount() const
{
    return len_LinearScan;
}

int mythread::xScanAlineCount() const
{
    return len_XScanData;
}

int mythread::repeatCyclesPerBuffer() const
{
    return m_repeatCyclesPerBuffer;
}

bool mythread::CalculateLengthParams()
{
    len_BscanCycle = long(AscanFreq * 1.0 / galvoFreq + 0.5);
    // 在线性扫描区域前后各有一个过渡区域
    // 修改了算法, 保证 len_BscanCycle = len_LinearScan + 2 * len_Transition
    len_Transition = long(len_BscanCycle * 0.5 * (1 - duty_cycle));
    len_LinearScan = len_BscanCycle - 2 * len_Transition;

    qDebug() << "DAC 长度参数：";
    qDebug() << "    len_BscanCycle =" << len_BscanCycle;
    qDebug() << "    len_LinearScan =" << len_LinearScan;
    qDebug() << "    len_Transition =" << len_Transition;
    qDebug() << "    len_ZeroBuffer =" << len_ZeroBuffer;

    if (m_BscanLen > (int)len_LinearScan)
    {
        m_BscanLen = (int)len_LinearScan;
        qDebug() << "警告：m_BscanLen 过大，已自动调整为" << m_BscanLen;
    }

    return true;
}

bool mythread::GenData(int scanMode)
{
    CalculateLengthParams();
    m_vesselAutoMoveAlineCount = 0;
    m_vesselNoReturnToZero = false;

    switch(scanMode)    // 模式可以参见 mainWidget::scanMode 的定义
    {
        case 1:
            len_XScanData = 1;
            len_YScanData = 1;
            GenData_init(len_XScanData, len_YScanData);
            GenData_keep(pDataX, len_XScanData, 0, PositionDacZeroCode);
            GenData_keep(pDataY, len_YScanData, 0, PositionDacZeroCode);
            break;

        case 10:
            len_XScanData = 2 * len_LinearScan + 3 * len_Transition;
            len_YScanData = len_XScanData;
            GenData_init(len_XScanData, len_YScanData);

            GenData_fall(pDataX, len_Transition, 0);
            GenData_linScan(pDataX, len_LinearScan, len_Transition);
            GenData_return(pDataX, len_Transition, len_Transition + len_LinearScan);
            GenData_keep(pDataX, len_LinearScan + len_Transition,
                         len_LinearScan + 2 * len_Transition, PositionDacZeroCode);

            GenData_keep(pDataY, len_LinearScan + len_Transition, 0, PositionDacZeroCode);
            GenData_fall(pDataY, len_Transition, len_LinearScan + len_Transition);
            GenData_linScan(pDataY, len_LinearScan, len_LinearScan + 2 * len_Transition);
            GenData_return(pDataY, len_Transition, 2 * len_LinearScan + 2 * len_Transition);
            break;

        case 2:
            len_XScanData = len_LinearScan + 2 * len_Transition;
            len_YScanData = len_XScanData;
            GenData_init(len_XScanData, len_YScanData);

            GenData_fall(pDataX, len_Transition, 0);
            GenData_linScan(pDataX, len_LinearScan, len_Transition);
            GenData_return(pDataX, len_Transition, len_Transition + len_LinearScan);
            GenData_keep(pDataY, len_YScanData, 0, PositionDacZeroCode);
            break;

        case 22:
            len_XScanData = (len_LinearScan + 2 * len_Transition) * m_AngioRep;
            len_YScanData = len_XScanData;
            GenData_init(len_XScanData, len_YScanData);

            for (unsigned int rep = 0; rep < (unsigned int)m_AngioRep; ++rep)
            {
                int offset = rep * (len_LinearScan + 2 * len_Transition);
                GenData_fall(pDataX, len_Transition, offset);
                GenData_linScan(pDataX, len_LinearScan, offset + len_Transition);
                GenData_return(pDataX, len_Transition, offset + len_Transition + len_LinearScan);
            }
            GenData_keep(pDataY, len_YScanData, 0, PositionDacZeroCode);
            break;

        case 3:
            len_XScanData = len_LinearScan + 2 * len_Transition;
            len_YScanData = len_XScanData * m_daSegmentCscanLen;
            GenData_init(len_XScanData, len_YScanData);

            GenData_fall(pDataX, len_Transition, 0);
            GenData_linScan(pDataX, len_LinearScan, len_Transition);
            GenData_return(pDataX, len_Transition, len_Transition + len_LinearScan);
            for (unsigned int i = 0; i < (unsigned int)m_daSegmentCscanLen; ++i)
            {
                const int globalIndex = m_daSegmentStartCscan + static_cast<int>(i);
                const double normalizedY = (m_daTotalCscanLen > 1)
                    ? (2.0 * globalIndex / (m_daTotalCscanLen - 1) - 1.0)
                    : 0.0;
                const double rawValue = PositionDacZeroCode + Voltage * 65536.0 / 10000.0
                    * normalizedY + 0.5;
                const unsigned short value = static_cast<unsigned short>(
                    std::max(0.0, std::min(65535.0, rawValue)));
                GenData_keep(pDataY, len_XScanData, i * len_XScanData, value);
            }
            break;

        case 32:
            len_XScanData = len_LinearScan + 2 * len_Transition;
            len_YScanData = len_XScanData * m_AngioRep * m_daSegmentCscanLen;
            GenData_init(len_XScanData, len_YScanData);

            GenData_fall(pDataX, len_Transition, 0);
            GenData_linScan(pDataX, len_LinearScan, len_Transition);
            GenData_return(pDataX, len_Transition, len_Transition + len_LinearScan);
            for (unsigned int i = 0; i < (unsigned int)m_daSegmentCscanLen; ++i)
            {
                const int globalIndex = m_daSegmentStartCscan + static_cast<int>(i);
                const double normalizedY = (m_daTotalCscanLen > 1)
                    ? (2.0 * globalIndex / (m_daTotalCscanLen - 1) - 1.0)
                    : 0.0;
                const double rawValue = PositionDacZeroCode + Voltage * 65536.0 / 10000.0
                    * normalizedY + 0.5;
                const unsigned short value = static_cast<unsigned short>(
                    std::max(0.0, std::min(65535.0, rawValue)));
                GenData_keep(pDataY, len_XScanData * m_AngioRep, i * len_XScanData * m_AngioRep, value);
            }
            break;

        case 42:
        {
            QVector<unsigned short> vesselPathX;
            QVector<unsigned short> vesselPathY;
            QString errorMessage;
            if (!readDaPathFile(scanPathFilePath(QStringLiteral("scanX.txt")), vesselPathX, errorMessage)
                || !readDaPathFile(scanPathFilePath(QStringLiteral("scanY.txt")), vesselPathY, errorMessage))
            {
                return FailDAConfig(errorMessage);
            }
            if (vesselPathX.size() != vesselPathY.size())
            {
                return FailDAConfig(QStringLiteral("Vessel scan 路径文件长度不一致：scanX=%1, scanY=%2。")
                    .arg(vesselPathX.size())
                    .arg(vesselPathY.size()));
            }
            m_vesselNoReturnToZero = vesselPathNoReturnToZero();
            auto loadSingleCycle = [&](const QVector<unsigned short> &cycleX,
                                       const QVector<unsigned short> &cycleY) -> bool {
                if (cycleX.isEmpty() || cycleX.size() != cycleY.size())
                {
                    return FailDAConfig(QStringLiteral("Vessel scan 路径无效：X=%1，Y=%2。")
                        .arg(cycleX.size())
                        .arg(cycleY.size()));
                }

                const int cycleLen = cycleX.size();
                len_XScanData = cycleLen;
                len_YScanData = cycleLen;
                GenData_init(len_XScanData, len_YScanData);
                for (int i = 0; i < cycleLen; ++i)
                {
                    pDataX[i] = cycleX[i];
                    pDataY[i] = cycleY[i];
                }
                return true;
            };

            if (vesselPathX.size() == len_BscanCycle)
            {
                const int totalMoveAlineCount = static_cast<int>(vesselPathX.size()) - m_BscanLen;
                if (totalMoveAlineCount < 0)
                {
                    return FailDAConfig(QStringLiteral("Vessel scan 完整周期点数 (%1) 小于 Bscan 长度 (%2)，无法分离有效路径。")
                        .arg(vesselPathX.size())
                        .arg(m_BscanLen));
                }
                if (!m_vesselNoReturnToZero && totalMoveAlineCount % 2 != 0)
                {
                    return FailDAConfig(QStringLiteral("Vessel scan 返回原点模式要求完整周期点数 (%1) - Bscan 长度 (%2) 能平均分成起止两个线性运动段。")
                        .arg(vesselPathX.size())
                        .arg(m_BscanLen));
                }

                m_vesselAutoMoveAlineCount = m_vesselNoReturnToZero
                    ? totalMoveAlineCount
                    : totalMoveAlineCount / 2;
                len_LinearScan = m_BscanLen;
                len_Transition = m_vesselNoReturnToZero ? 0 : m_vesselAutoMoveAlineCount;
                len_BscanCycle = vesselPathX.size();
                if (!loadSingleCycle(vesselPathX, vesselPathY))
                    return false;
                qDebug() << "Vessel scan DA 路径：已载入完整 Bscan 周期，模式"
                         << (m_vesselNoReturnToZero ? "不返回原点" : "返回原点")
                         << "，有效路径" << m_BscanLen
                         << "点，线性运动段" << m_vesselAutoMoveAlineCount
                         << "点，单周期" << len_BscanCycle
                         << "点。";
                break;
            }
            if (vesselPathX.size() != m_BscanLen)
            {
                return FailDAConfig(QStringLiteral("Vessel scan 路径点数 (%1) 必须等于完整 Bscan 周期长度 (%2)，或只包含有效路径段时等于 Bscan 长度 (%3)。请重新导出路径或更新扫描参数。")
                    .arg(vesselPathX.size())
                    .arg(len_BscanCycle)
                    .arg(m_BscanLen));
            }

            m_vesselAutoMoveAlineCount = alignedAlineCountForAdRead(len_Transition, m_AscanLen);
            if (m_vesselAutoMoveAlineCount <= 0)
            {
                return FailDAConfig(QStringLiteral("Vessel scan 过渡段长度无效，无法生成零点到路径的运动波形。"));
            }

            const int initialMoveAlineCount = m_vesselNoReturnToZero ? 0 : m_vesselAutoMoveAlineCount;
            const int returnMoveAlineCount = m_vesselAutoMoveAlineCount;
            const int singleCycleLen = vesselPathX.size() + initialMoveAlineCount + returnMoveAlineCount;
            QVector<unsigned short> cycleX(singleCycleLen, PositionDacZeroCode);
            QVector<unsigned short> cycleY(singleCycleLen, PositionDacZeroCode);

            const unsigned short startX = vesselPathX.first();
            const unsigned short startY = vesselPathY.first();
            const unsigned short endX = vesselPathX.last();
            const unsigned short endY = vesselPathY.last();
            int pathOffset = 0;
            for (int i = 0; i < initialMoveAlineCount; ++i)
            {
                cycleX[i] = interpolateDaValue(PositionDacZeroCode, startX, i, initialMoveAlineCount);
                cycleY[i] = interpolateDaValue(PositionDacZeroCode, startY, i, initialMoveAlineCount);
            }
            pathOffset += initialMoveAlineCount;
            for (int i = 0; i < vesselPathX.size(); ++i)
            {
                const int offset = pathOffset + i;
                cycleX[offset] = vesselPathX[i];
                cycleY[offset] = vesselPathY[i];
            }
            pathOffset += vesselPathX.size();
            for (int i = 0; i < returnMoveAlineCount; ++i)
            {
                const int offset = pathOffset + i;
                const unsigned short returnTargetX = m_vesselNoReturnToZero ? startX : PositionDacZeroCode;
                const unsigned short returnTargetY = m_vesselNoReturnToZero ? startY : PositionDacZeroCode;
                cycleX[offset] = interpolateDaValue(endX, returnTargetX, i, returnMoveAlineCount);
                cycleY[offset] = interpolateDaValue(endY, returnTargetY, i, returnMoveAlineCount);
            }
            len_LinearScan = m_BscanLen;
            len_Transition = m_vesselNoReturnToZero ? 0 : m_vesselAutoMoveAlineCount;
            len_BscanCycle = singleCycleLen;
            if (!loadSingleCycle(cycleX, cycleY))
                return false;
            qDebug() << "Vessel scan DA 路径：模式" << (m_vesselNoReturnToZero ? "不返回原点" : "返回原点")
                     << "，过渡" << m_vesselAutoMoveAlineCount
                     << "点，有效路径" << vesselPathX.size()
                     << "点，单周期" << len_BscanCycle
                     << "点。";
            break;
        }

        default:
            return FailDAConfig(QStringLiteral("不支持的扫描模式：%1，无法生成 X/Y 扫描 DAC 数据。")
                .arg(scanMode));
    }

    if (scanMode != 42)
    {
        std::ofstream file1(scanPathFilePath(QStringLiteral("scanX.txt")).toStdWString());
        for (int i = 0; i < len_XScanData; ++i)
            file1 << pDataX[i] << std::endl;
        file1.close();

        std::ofstream file2(scanPathFilePath(QStringLiteral("scanY.txt")).toStdWString());
        for (int i = 0; i < len_YScanData; ++i)
            file2 << pDataY[i] << std::endl;
        file2.close();
    }

    return true;
}

bool mythread::GenRfSquareData()
{
    if (AscanFreq <= 0)
    {
        return FailDAConfig(QStringLiteral("Ascan 频率无效，无法生成射频 DAC 方波（AscanFreq=%1）。")
            .arg(AscanFreq));
    }

    const double exactSamplesPerAscan = double(RfDacSampleRateHz) / double(AscanFreq);
    if (exactSamplesPerAscan > double(MaxRfDacSamples) + RfDacLengthUnit / 2.0)
    {
        return FailDAConfig(QStringLiteral("Ascan 频率过低，射频 DAC 一个 A-scan 周期需要约 %1 点，超过最大 %2 点。")
            .arg(exactSamplesPerAscan, 0, 'f', 1)
            .arg(MaxRfDacSamples));
    }

    int samplesPerAscan = int((exactSamplesPerAscan + RfDacLengthUnit / 2.0) / RfDacLengthUnit)
        * RfDacLengthUnit;
    if (samplesPerAscan < MinRfDacSamples)
    {
        return FailDAConfig(QStringLiteral("Ascan 频率过高，射频 DAC 一个 A-scan 周期只有 %1 点，低于最小 %2 点；当前 AscanFreq=%3 Hz。")
            .arg(samplesPerAscan)
            .arg(MinRfDacSamples)
            .arg(AscanFreq));
    }
    if (samplesPerAscan > MaxRfDacSamples)
    {
        return FailDAConfig(QStringLiteral("射频 DAC 一个 A-scan 周期为 %1 点，超过最大 %2 点。")
            .arg(samplesPerAscan)
            .arg(MaxRfDacSamples));
    }

    delete [] pDataRF;
    pDataRF = NULL;
    len_RfScanData = samplesPerAscan;
    pDataRF = new unsigned short[len_RfScanData];

    const int highSamples = samplesPerAscan / 2;
    for (int i = 0; i < len_RfScanData; ++i)
    {
        const int phase = i % samplesPerAscan;
        pDataRF[i] = (phase < highSamples) ? RfDacHighCode : 0;    // RF DAC 低电平应为 0，不是位置 DA 的 0V 中点码。
    }

    const double actualAscanFreq = double(RfDacSampleRateHz) / double(samplesPerAscan);
    qDebug() << "GenRfSquareData(): 射频 DAC 方波已生成，目标 Ascan 频率 =" << AscanFreq
             << "Hz，缓冲区样点 =" << len_RfScanData
             << "单周期样点 =" << samplesPerAscan
             << "实际频率 =" << actualAscanFreq << "Hz，占空比 = 1/2";
    return true;
}

void mythread::GenData_init(int len_XscanData, int len_YscanData)
{
    len_XScanData = len_XscanData;
    len_YScanData = len_YscanData;

    delete [] pDataX;
    delete [] pDataY;

    pDataX = new unsigned short[len_XscanData];
    pDataY = new unsigned short[len_YscanData];

    for (int i = 0; i < len_XscanData; ++i)
        pDataX[i] = PositionDacZeroCode;
    for (int i = 0; i < len_YscanData; ++i)
        pDataY[i] = PositionDacZeroCode;
}

void mythread::GenData_fall(unsigned short* array, int length, int offset)
{
    if (length == 0)
        return;
    if (length == 1)
    {
        array[offset] = PositionDacZeroCode;
        return;
    }

    for (int i = 0; i < length; ++i)
    {
        const double rawValue = PositionDacZeroCode - Voltage * 65536.0 / 10000.0
            * double(i) / (length - 1) + 0.5;
        array[i + offset] = static_cast<unsigned short>(
            std::max(0.0, std::min(65535.0, rawValue)));
    }
}

void mythread::GenData_linScan(unsigned short* array, int length, int offset)
{
    if (length == 0)
        return;
    if (length == 1)
    {
        array[offset] = PositionDacZeroCode;
        return;
    }

    for (int i = 0; i < length; ++i)
    {
        const double rawValue = PositionDacZeroCode + Voltage * 65536.0 / 10000.0
            * (2.0 * i / (length - 1) - 1) + 0.5;
        array[i + offset] = static_cast<unsigned short>(
            std::max(0.0, std::min(65535.0, rawValue)));
    }
}

void mythread::GenData_return(unsigned short* array, int length, int offset)
{
    if (length == 0)
        return;
    if (length == 1)
    {
        array[offset] = PositionDacZeroCode;
        return;
    }

    for (int i = 0; i < length; ++i)
    {
        const double rawValue = PositionDacZeroCode + Voltage * 65536.0 / 10000.0
            * (1 - double(i) / (length - 1)) + 0.5;
        array[i + offset] = static_cast<unsigned short>(
            std::max(0.0, std::min(65535.0, rawValue)));
    }
}

void mythread::GenData_keep(unsigned short* array, int length, int offset, unsigned short value)
{
    for (int i = 0; i < length; ++i)
        array[i + offset] = value;
}

// 打印采集参数
bool mythread::PrintParams_AD(PCIe3640_PARA_INIT &m_pciePara)
{
    switch (mainWidget::clockMode)
    {
        case mainWidget::ADClockMode::External:
            qDebug() << "    ClockMode = External (板外10MHz参考时钟，需由硬件J1/J2选择)";
            break;
        case mainWidget::ADClockMode::Internal:
        default:
            qDebug() << "    ClockMode = Internal (板上10MHz参考时钟)";
            break;
    }

    // 时钟分频
    qDebug() << "    lClkDiv = " << m_pciePara.lClkDiv << " (分频倍率)";
    if (m_pciePara.lClkDiv != 1 && m_pciePara.lClkDiv != 2 && m_pciePara.lClkDiv != 4 && m_pciePara.lClkDiv != 8 && m_pciePara.lClkDiv != 16 && m_pciePara.lClkDiv != 32 && m_pciePara.lClkDiv != 64 && m_pciePara.lClkDiv != 128 && m_pciePara.lClkDiv != 256)
    {
        qDebug() << "[错误] 分频倍率无效！必须是1,2,4,...,256";
        return false;
    }

    // 通道使能 1=CH1，2=CH1+CH2
    switch(m_pciePara.lChCnt)
    {
        case 1: qDebug() << "    lChCnt = " << m_pciePara.lChCnt << " (1CH 使能通道 CH1)"; break;
        case 2: qDebug() << "    lChCnt = " << m_pciePara.lChCnt << " (2CH 通道使能 CH1 + CH2)"; break;
        default: qDebug() << "    lChCnt = " << m_pciePara.lChCnt << " (未知通道配置)"; return false;
    }

    // 触发模式
    switch(m_pciePara.TriggerMode)
    {
        case TRIG_MODE_CONTINUE: qDebug() << "    TriggerMode = " << m_pciePara.TriggerMode << " (连续采集)"; break;
        case TRIG_MODE_POST: qDebug() << "    TriggerMode = " << m_pciePara.TriggerMode << " (触发采集/后触发)"; break;
        case TRIG_MODE_1TRIG: qDebug() << "    TriggerMode = " << m_pciePara.TriggerMode << " (单触发)"; break;
        case TRIG_MODE_FTRIG: qDebug() << "    TriggerMode = " << m_pciePara.TriggerMode << " (场触发/预留)"; break;
        default: qDebug() << "    TriggerMode = " << m_pciePara.TriggerMode << " (未知触发模式)"; return false;
    }

    // 触发源
    switch (m_pciePara.TriggerSource)
    {
        case TRIG_SRC_EXT_RISING: qDebug() << "    TriggerSource = " << m_pciePara.TriggerSource << " (外正沿)"; break;
        case TRIG_SRC_EXT_FALLING: qDebug() << "    TriggerSource = " << m_pciePara.TriggerSource << " (外负沿)"; break;
        case TRIG_SRC_SOFT: qDebug() << "    TriggerSource = " << m_pciePara.TriggerSource << " (软件触发)"; break;
        case TRIG_SRC_INT_RISING: qDebug() << "    TriggerSource = " << m_pciePara.TriggerSource << " (内正沿)"; break;
        case TRIG_SRC_INT_FALLING: qDebug() << "    TriggerSource = " << m_pciePara.TriggerSource << " (内负沿)"; break;
        case TRIG_SRC_CH1_RISING: qDebug() << "    TriggerSource = " << m_pciePara.TriggerSource << " (CH1正沿)"; break;
        case TRIG_SRC_CH1_FALLING: qDebug() << "    TriggerSource = " << m_pciePara.TriggerSource << " (CH1负沿)"; break;
        case TRIG_SRC_CH2_RISING: qDebug() << "    TriggerSource = " << m_pciePara.TriggerSource << " (CH2正沿)"; break;
        case TRIG_SRC_CH2_FALLING: qDebug() << "    TriggerSource = " << m_pciePara.TriggerSource << " (CH2负沿)"; break;
        default: qDebug() << "    TriggerSource = " << m_pciePara.TriggerSource << " (未知触发源)"; return false;
    }

    // 前触发
    qDebug() << "    TriggerPre = " << m_pciePara.TriggerPre << " (*8 样点)";
    if(m_pciePara.TriggerPre * 8 * m_pciePara.lClkDiv > 4096)
    {
        qDebug() << "[错误] TriggerPre 过大！";
        return false;
    }

    // 触发延时
    if(m_pciePara.TriggerPre == 0)
        qDebug() << "    TriggerDelay = " << m_pciePara.TriggerDelay << " (*8 样点)";
    else
        qDebug() << "    TriggerDelay = " << m_pciePara.TriggerDelay << " (*8 样点，采用前触发，本参数无效)";

    // 触发长度
    qDebug() << "    TriggerLength = " << m_pciePara.TriggerLength << " (*8 样点)";
    if(m_pciePara.TriggerLength % 2 != 0)
    {
        qDebug() << "[错误] TriggerLength 必须为偶数！当前值 = " << m_pciePara.TriggerLength;
        return false;
    }

    // 触发门限
    qDebug() << "    TriggerLevel = " << m_pciePara.TriggerLevel;

    // 数据源选择 0=AD，1=计数器
    switch(m_pciePara.lSelDataSrc)
    {
        case 0: qDebug() << "    lSelDataSrc = " << m_pciePara.lSelDataSrc << " (AD数据源)"; break;
        case 1: qDebug() << "    lSelDataSrc = " << m_pciePara.lSelDataSrc << " (计数器数据源)"; break;
        default: qDebug() << "    lSelDataSrc = " << m_pciePara.lSelDataSrc << " (未知数据源)"; return false;
    }

    // AD输出码制 0=直接二进制，1=补码
    switch(m_pciePara.lADFmt)
    {
        case 0: qDebug() << "    lADFmt = " << m_pciePara.lADFmt << " (直接二进制)"; break;
        case 1: qDebug() << "    lADFmt = " << m_pciePara.lADFmt << " (二进制补码)"; break;
        default: qDebug() << "    lADFmt = " << m_pciePara.lADFmt << " (未知码制)"; return false;
    }

    // 场触发行个数（预留）
    qDebug() << "    lLineNum = " << m_pciePara.lLineNum << " (*8ns, 外触发最小高电平持续时间)";

    return true;
}

// 初始化采集（创建多缓冲区，匹配主程序）
bool mythread::InitADForCapture()
{
    QMutexLocker locker(&mutex1);
    qDebug() << "InitADForCapture(): 正在初始化采集卡...";

    // ==================== 阶段1：测试内存分配 ====================
    // VirtualAlloc 在 MEM_COMMIT 模式下，会真实地分配物理内存（但不一定立刻分配，很多系统采用"按需置零"策略，真正第一次写入的时候才给你一个实际的物理页）
    // malloc 和 new 是更高层的抽象，底层通常也是调用类似 VirtualAlloc 的系统调用，但它们在上面加了一个堆管理器（heap manager） ，负责把大块虚拟内存切成小块分给你，同时处理碎片、空闲列表等等。

    qDebug() << "[阶段1] 缓冲区测试...";
    long long logicalBufferSamples = static_cast<long long>(m_AscanLen) * m_BscanLen;
    m_buffersCompleted = 0;
    m_curIndexInMemBuffer = 0;
    m_lastAvailableIndex = -1;
    qDebug() << "    Ascan 长度: " << m_AscanLen;
    qDebug() << "    Bscan 长度: " << m_BscanLen;
    m_repeatCyclesPerBuffer = 1;
    if (mainWidget::scanMode == 1)
        logicalBufferSamples = m_AscanLen;
    else if ((mainWidget::scanMode == 2 || mainWidget::scanMode == 3
              || mainWidget::scanMode == 32
              || mainWidget::scanMode == 42
              || mainWidget::scanMode == 43)
             && len_BscanCycle > 0)
    {
        logicalBufferSamples = static_cast<long long>(m_AscanLen) * len_BscanCycle;
    }
    else if (mainWidget::scanMode == 10 && len_XScanData > 0)
    {
        logicalBufferSamples = static_cast<long long>(m_AscanLen) * len_XScanData;
    }
    else if (mainWidget::scanMode == 22 && len_XScanData > 0)
    {
        logicalBufferSamples = static_cast<long long>(m_AscanLen) * len_XScanData;
    }

    if (logicalBufferSamples <= 0 || logicalBufferSamples > INT_MAX)
    {
        emit acquisitionStatus(QStringLiteral("采集诊断：逻辑缓存帧大小无效（%1 样点）。")
                                   .arg(static_cast<qlonglong>(logicalBufferSamples)));
        return false;
    }

    m_singleBufferSize = static_cast<int>(logicalBufferSamples);
    m_adReadChunkSize = adReadChunkSizeForLogicalFrame(m_singleBufferSize);
    if (isLiveDisplayScanMode(mainWidget::scanMode))
        m_adReadChunkSize = std::max(m_adReadChunkSize,
                                     adReadChunkSizeForLogicalFrame(kHighThroughputReadChunkSamples));
    if (m_adReadChunkSize <= 0)
    {
        emit acquisitionStatus(QStringLiteral("采集诊断：硬件读取块大小无效，逻辑缓存帧为 %1 样点。")
                                   .arg(m_singleBufferSize));
        return false;
    }

    qDebug() << "    逻辑缓存帧大小: " << m_singleBufferSize << " 样点";
    qDebug() << "    硬件单次读取大小: " << m_adReadChunkSize << " 样点";

    void* testMem = VirtualAlloc(0, m_singleBufferSize * sizeof(U16), MEM_COMMIT, PAGE_READWRITE);
    if (testMem == NULL)
    {
        qDebug() << "[错误] 内存分配失败！错误码 = " << GetLastError();
        return false;
    }
    else
    {
        qDebug() << "    VirtualAlloc 测试成功，地址： " << testMem;
        VirtualFree(testMem, 0, MEM_RELEASE);
    }


    // ==================== 阶段2：创建多缓冲区 ====================
    if (mainWidget::ContinuousModeEnabled && mainWidget::isContinuousSupportedMode(mainWidget::scanMode))
    {
        if (mainWidget::scanMode == 2 || mainWidget::scanMode == 22)
        {
            const int targetBscanCount = mainWidget::continuousStoredBscanCount();
            const int logicalBscansPerBuffer =
                (mainWidget::scanMode == 22)
                ? m_AngioRep * m_repeatCyclesPerBuffer
                : m_repeatCyclesPerBuffer;
            m_bufferCount = (targetBscanCount + logicalBscansPerBuffer - 1) / logicalBscansPerBuffer;
        }
        else
        {
            m_bufferCount = mainWidget::continuousBufferCount();
        }
    }
    else if (mainWidget::scanMode == 3 || mainWidget::scanMode == 32)
    {
        const int targetBscanCount = (mainWidget::scanMode == 32)
            ? m_CscanLen * m_AngioRep
            : m_CscanLen;
        if (m_plannedVolumeBufferCount > 0)
            m_bufferCount = m_plannedVolumeBufferCount;
        else
            m_bufferCount = (targetBscanCount + m_repeatCyclesPerBuffer - 1) / m_repeatCyclesPerBuffer;

        if (m_currentSegmentBufferCount <= 0 || m_currentSegmentBufferCount > m_bufferCount)
            m_currentSegmentBufferCount = m_bufferCount;
    }
    else if (isLiveDisplayScanMode(mainWidget::scanMode))
        m_bufferCount = 8;
    else
        m_bufferCount = m_CscanLen * m_AngioRep; // 暂时按照最多的 3D Angio 模式来配置
    if (m_bufferCount <= 0)
    {
        qDebug() << "[错误] 连续采集缓冲区数量无效：" << m_bufferCount;
        return false;
    }
    qDebug() << "[阶段2] 创建 " << m_bufferCount << " 个采集缓冲区...";
    // 释放原有缓冲区
    for (int i=0; i<m_volumeMemBuffer.size(); i++)
    {
        if (m_volumeMemBuffer[i]) VirtualFree(m_volumeMemBuffer[i], 0, MEM_RELEASE);
    }
    m_volumeMemBuffer.clear();

    // 创建新缓冲区
    for (int i=0; i<m_bufferCount; i++)
    {
        U16* buf = (U16*)VirtualAlloc(0, m_singleBufferSize * sizeof(U16), MEM_COMMIT, PAGE_READWRITE);
        if (!buf)
        {
            qDebug() << "[错误] 缓冲区 " << i << " 的内存分配失败！";
            return false;
        }
        memset(buf, 0, m_singleBufferSize * sizeof(U16));
        m_volumeMemBuffer.append(buf);  // 删掉了 qDebug(), 否则会输出几千条 debug 信息
    }

    // ==================== 阶段3：（在电脑上）设置采集参数 ====================
    qDebug() << "[阶段3] 设置采集参数...";
    memset(&m_pciePara, 0, sizeof(PCIe3640_PARA_INIT));

    // 基础参数
    m_pciePara.lSelDataSrc = 0;        // 0=AD数据源（说明书5.4节）
    m_pciePara.lChCnt = 1;             // 1=仅CH1（说明书5.4节）
    m_pciePara.lClkDiv = 1;            // 1分频；当前主界面采样率参数按 1.25 GHz 基准填写
    m_pciePara.lADFmt = 0;             // 0=直接二进制码（说明书5.4节）

    // 触发模式和触发源由 UI 的 combo_triggerMode 统一选择。
    switch (mainWidget::triggerMode)
    {
        case mainWidget::ADTriggerMode::External:
            m_pciePara.TriggerMode = TRIG_MODE_POST;
            m_pciePara.TriggerSource = TRIG_SRC_EXT_RISING;
            break;
        case mainWidget::ADTriggerMode::Continuous:
            m_pciePara.TriggerMode = TRIG_MODE_CONTINUE;
            m_pciePara.TriggerSource = TRIG_SRC_INT_RISING;
            break;
        case mainWidget::ADTriggerMode::Internal:
        default:
            m_pciePara.TriggerMode = TRIG_MODE_POST;
            m_pciePara.TriggerSource = TRIG_SRC_INT_RISING;
            break;
    }
    int triggerPostSamples = 2048 * TRIG_UNIT;
    if (m_pciePara.TriggerMode == TRIG_MODE_CONTINUE)
    {
        triggerPostSamples = 2048 * TRIG_UNIT;       // 连续采集模式下该参数不参与分帧
    }
    else
    {
        const int compatibleTriggerSamples =
            (m_AscanLen / kTriggerLengthQuantumSamples) * kTriggerLengthQuantumSamples;
        if (compatibleTriggerSamples <= 0 || compatibleTriggerSamples != m_AscanLen)
        {
            emit acquisitionStatus(QStringLiteral("采集诊断：当前 AscanLen=%1 不能精确写入硬件 TriggerLength，硬件触发需要 16 样点整数倍。")
                                       .arg(m_AscanLen));
            return false;
        }
        triggerPostSamples = compatibleTriggerSamples;
    }
    const int triggerOffsetUnits = mainWidget::TriggerOffsetSamples / TRIG_UNIT;
    if (m_pciePara.TriggerMode != TRIG_MODE_CONTINUE && triggerOffsetUnits < 0)
    {
        m_pciePara.TriggerPre = -triggerOffsetUnits;  // 前触发非 0 时 TriggerDelay 无效
        m_pciePara.TriggerDelay = 0;
        triggerPostSamples -= m_pciePara.TriggerPre * TRIG_UNIT;
        if (triggerPostSamples <= 0 || ((triggerPostSamples / TRIG_UNIT) % 2) != 0)
        {
            emit acquisitionStatus(QStringLiteral("采集诊断：触发偏移 %1 样点与 AscanLen=%2 不兼容，无法形成有效 TriggerLength。")
                                       .arg(mainWidget::TriggerOffsetSamples)
                                       .arg(m_AscanLen));
            return false;
        }
    }
    else if (m_pciePara.TriggerMode != TRIG_MODE_CONTINUE)
    {
        m_pciePara.TriggerPre = 0;
        m_pciePara.TriggerDelay = triggerOffsetUnits;
    }
    else
    {
        m_pciePara.TriggerPre = 0;
        m_pciePara.TriggerDelay = 0;
    }
    m_pciePara.TriggerLength = triggerPostSamples / TRIG_UNIT;
    m_pciePara.TriggerLevel = 3000;                   // 外触发无效，占位
    m_pciePara.lLineNum = 5;                          // 40ns

    // 打印参数日志
    PrintParams_AD(m_pciePara);
    emit acquisitionStatus(QStringLiteral("采集诊断：AD 参数 TriggerMode=%1(%2)，TriggerSource=%3(%4)，TriggerLength=%5（%6 样点），TriggerPre=%7，TriggerDelay=%8。")
                               .arg(m_pciePara.TriggerMode)
                               .arg(triggerModeName(m_pciePara.TriggerMode))
                               .arg(m_pciePara.TriggerSource)
                               .arg(triggerSourceName(m_pciePara.TriggerSource))
                               .arg(m_pciePara.TriggerLength)
                               .arg(m_pciePara.TriggerLength * TRIG_UNIT)
                               .arg(m_pciePara.TriggerPre)
                               .arg(m_pciePara.TriggerDelay));

    // ==================== 阶段4：连接设备 ====================
    qDebug() << "[阶段4] 连接 PCIe3640 采集卡...";
    if (!EnsureDeviceLinked())
    {
        qDebug() << "[错误] 设备连接失败！可能原因： ";
        qDebug() << "  1. 采集卡未插好/电源未开 ";
        qDebug() << "  2. 驱动未正确安装（设备管理器检查） ";
        qDebug() << "  3. 设备号错误（多卡时需修改为1/2） ";
        qDebug() << "  4. SDK版本与硬件不匹配 ";
        return false;
    }
    qDebug() << "    连接成功！设备句柄 = " << m_pcieHandle;
    const BOOL stopBeforeInit = PCIe3640_StopAD(m_pcieHandle);
    qDebug() << "    InitAD 前 StopAD 清理返回 =" << stopBeforeInit;
    emit acquisitionStatus(QStringLiteral("采集诊断：InitAD 前 StopAD 清理 FIFO 返回 %1。")
                               .arg(stopBeforeInit ? QStringLiteral("成功") : QStringLiteral("失败/未在采集")));

    // 验证PCIe缓冲区
    if (m_pcieBuf.pADBufA == NULL)
    {
        qDebug() << "[错误] PCIe缓存A地址为空！SDK初始化失败 ";
        PCIe3640_UnLink(m_pcieHandle);
        m_pcieHandle = INVALID_HANDLE_VALUE;
        return false;
    }
    qDebug() << "    PCIe缓存A地址 = " << m_pcieBuf.pADBufA;
    qDebug() << "    PCIe缓存B地址 = " << m_pcieBuf.pADBufB;

    // ==================== 阶段5：传输采集参数 ====================
    qDebug() << "[阶段5] 传输采集参数...";
    BOOL initResult = PCIe3640_initAD(m_pcieHandle, &m_pciePara);
    if (!initResult)
    {
        qDebug() << "[错误] 传输采集参数失败！";
        // 清理资源
        PCIe3640_UnLink(m_pcieHandle);
        m_pcieHandle = INVALID_HANDLE_VALUE;
        return false;
    }

    m_isCapturing = true;
    qDebug() << "InitADForCapture(): 采集卡初始化完成！";
    return true;
}


// 这是采集线程的主循环，负责从 PCIe 采集卡读取一块样本（大小为 m_singleBufferSize），并把数据不断循环写入程序内的多缓冲区 m_volumeMemBuffer，直到外部指令改变了 m_isCapturing, 出错或超时。
/*
    为什么不能一直持有锁？
    回答：entry() 是跑在独立采集线程里的，但它访问的那些成员变量—— m_pcieHandle、m_volumeMemBuffer、m_isCapturing、m_lastAvailableIndex 等等——同时也可能被主线程（负责启动/停止采集的那个）读写。
    采集循环里有两个耗时操作：
        等待 FIFO 数据量满足要求（轮询 PCIe3640_GetBufCnt，可能要循环很多次）
        调用 PCIe3640_ReadAD 读取硬件数据（超时设置了 10000 ms）
    这两步都可能阻塞很长时间。如果在这期间持有锁，主线程想停止采集（修改 m_isCapturing = false）就会被卡住，整个程序就死了。
    所以设计策略是：只在真正需要访问共享变量的短暂时刻持有锁，其余耗时操作期间释放锁。
*/
void mythread::entry()
{
    // 初始化检查，由于 entry() 是在独立线程中调用的，与开启或结束采集的线程可能存在并发，因此需要使用互斥锁来保护共享资源（设备句柄、缓冲区状态、采集状态等），确保线程安全
    QMutexLocker locker(&mutex1);
    if (m_pcieHandle == INVALID_HANDLE_VALUE)
    {
        qDebug() << "[错误] entry(): 句柄无效！";
        emit acquisitionStatus("采集诊断：采集线程未启动，设备句柄无效。");
        return;
    }
    if(m_volumeMemBuffer.isEmpty())
    {
        qDebug() << "[错误] entry(): 多缓冲区为空！";
        emit acquisitionStatus("采集诊断：采集线程未启动，内存缓冲区为空。");
        return;
    }
    if(!m_isCapturing)
    {
        qDebug() << "[错误] entry(): 采集卡未初始化！";
        emit acquisitionStatus("采集诊断：采集线程未启动，采集卡未初始化。");
        return;
    }
    const bool liveDisplayMode = isLiveDisplayScanMode(mainWidget::scanMode)
        && !mainWidget::ContinuousModeEnabled;
    const int continuousBufferLimit = (mainWidget::ContinuousModeEnabled
                                       && mainWidget::isContinuousSupportedMode(mainWidget::scanMode))
        ? m_bufferCount
        : 0;
    const int finiteVolumeBufferLimit = (!mainWidget::ContinuousModeEnabled
                                         && isFiniteVolumeScanMode(mainWidget::scanMode))
        ? ((m_currentSegmentBufferCount > 0)
           ? std::min(m_bufferCount, m_buffersCompleted + m_currentSegmentBufferCount)
           : m_bufferCount)
        : 0;
    const int acquisitionBufferLimit = (continuousBufferLimit > 0)
        ? continuousBufferLimit
        : finiteVolumeBufferLimit;
    const bool vesselAutoContinuousMode = (continuousBufferLimit > 0
                                           && (mainWidget::scanMode == 42 || mainWidget::scanMode == 43)
                                           && m_vesselAutoMoveAlineCount > 0);
    const ULONGLONG vesselAutoMoveSamples = vesselAutoContinuousMode
        ? static_cast<ULONGLONG>(m_vesselAutoMoveAlineCount) * static_cast<ULONGLONG>(m_AscanLen)
        : 0;
    const ULONGLONG vesselAutoInitialSkipSamples = (vesselAutoContinuousMode && !m_vesselNoReturnToZero)
        ? vesselAutoMoveSamples : 0;
    const ULONGLONG vesselAutoCycleSkipSamples = vesselAutoContinuousMode
        ? vesselAutoMoveSamples * (m_vesselNoReturnToZero ? 1 : 2) : 0;
    ULONGLONG vesselAutoSkipBeforeRead = vesselAutoInitialSkipSamples;
    const LONG activeTriggerMode = m_pciePara.TriggerMode;
    const LONG activeTriggerSource = m_pciePara.TriggerSource;
    locker.unlock();

    QString startMessage = QStringLiteral("采集诊断：采集线程已启动，AD 触发=%1/%2，硬件单次读取 %3 样点，逻辑缓存帧 %4 样点。")
                               .arg(triggerModeName(activeTriggerMode))
                               .arg(triggerSourceName(activeTriggerSource))
                               .arg(m_adReadChunkSize)
                               .arg(m_singleBufferSize);
    if (liveDisplayMode)
        startMessage += QStringLiteral(" 实时显示模式会持续拼接读取块，并显示最新逻辑帧。");
    if (continuousBufferLimit > 0)
        startMessage += QStringLiteral(" 连续采集目标为 %1 个逻辑缓存帧。").arg(continuousBufferLimit);
    else if (finiteVolumeBufferLimit > 0)
        startMessage += QStringLiteral(" 有限体积采集目标为 %1 个逻辑缓存帧。").arg(finiteVolumeBufferLimit);
    if (m_singleBufferSize % kReadAdCountUnit != 0)
        startMessage += QStringLiteral(" 读取块会在电脑端拼接后再写入逻辑缓存。");
    emit acquisitionStatus(startMessage);

    // 记录采集开始时间，用于超时判断
    // QTime startTime = QTime::currentTime();
    QElapsedTimer startTime;
    startTime.start();
    QElapsedTimer statusTimer;
    statusTimer.start();
    const int totalTimeoutMs = 5000;
    std::vector<U16> pendingSamples;
    pendingSamples.reserve(static_cast<size_t>(m_adReadChunkSize) + static_cast<size_t>(m_singleBufferSize));
    size_t pendingOffset = 0;

    auto compactPendingSamples = [&]() {
        if (pendingOffset == 0)
            return;
        if (pendingOffset >= pendingSamples.size())
        {
            pendingSamples.clear();
            pendingOffset = 0;
            return;
        }
        if (pendingOffset >= static_cast<size_t>(m_adReadChunkSize)
            || pendingOffset * 2 >= pendingSamples.size())
        {
            pendingSamples.erase(pendingSamples.begin(),
                                 pendingSamples.begin()
                                     + static_cast<std::vector<U16>::difference_type>(pendingOffset));
            pendingOffset = 0;
        }
    };

    auto appendPendingSamples = [&](const U16 *samples, int sampleCount) {
        if (samples == nullptr || sampleCount <= 0)
            return;
        compactPendingSamples();
        pendingSamples.insert(pendingSamples.end(), samples, samples + sampleCount);
    };

    auto emitLogicalFrames = [&](ULONGLONG fifoSamplesBeforeRead,
                                 int &completedNow,
                                 bool &shouldReportStatus,
                                 U16 &minValue,
                                 U16 &maxValue,
                                 U16 &firstValue,
                                 double &meanValue) -> int {
        int emittedFrames = 0;
        const size_t logicalFrameSamples = static_cast<size_t>(m_singleBufferSize);
        while (m_isCapturing
               && pendingSamples.size() >= pendingOffset
               && pendingSamples.size() - pendingOffset >= logicalFrameSamples)
        {
            locker.relock();
            if (m_volumeMemBuffer.isEmpty() || m_bufferCount <= 0)
            {
                locker.unlock();
                emit acquisitionStatus(QStringLiteral("采集诊断：逻辑缓存不可用，无法写入拼接后的数据。"));
                return -1;
            }

            const int tmpBufferIdx = m_curIndexInMemBuffer;
            U16 *savedData = m_volumeMemBuffer[tmpBufferIdx];
            if (savedData == nullptr)
            {
                locker.unlock();
                emit acquisitionStatus(QStringLiteral("采集诊断：逻辑缓存指针为空，无法写入拼接后的数据。"));
                return -1;
            }

            memcpy(savedData,
                   pendingSamples.data() + pendingOffset,
                   static_cast<size_t>(m_singleBufferSize) * sizeof(U16));
            pendingOffset += logicalFrameSamples;

            m_lastAvailableIndex = tmpBufferIdx;
            m_buffersCompleted++;
            if (m_buffersCompleted % 10000 == 0)
                qDebug() << "entry(): 已完成 " << m_buffersCompleted << " 个逻辑缓存帧！当前可用索引 = " << m_lastAvailableIndex;

            completedNow = m_buffersCompleted;
            if (acquisitionBufferLimit > 0 && m_buffersCompleted >= acquisitionBufferLimit)
                m_isCapturing = false;

            const bool reportThisFrame =
                !shouldReportStatus && (completedNow == 1 || statusTimer.elapsed() >= 1000);
            if (reportThisFrame)
            {
                int sampleCount = m_singleBufferSize;
                if (sampleCount > 2048)
                    sampleCount = 2048;

                firstValue = savedData[0];
                minValue = firstValue;
                maxValue = firstValue;
                double sumValue = 0.0;
                for (int i = 0; i < sampleCount; ++i)
                {
                    const U16 value = savedData[i];
                    if (value < minValue)
                        minValue = value;
                    if (value > maxValue)
                        maxValue = value;
                    sumValue += value;
                }
                meanValue = sumValue / sampleCount;
                shouldReportStatus = true;
            }

            m_curIndexInMemBuffer = (m_curIndexInMemBuffer + 1) % m_bufferCount;
            locker.unlock();
            ++emittedFrames;

            if (vesselAutoContinuousMode && continuousBufferLimit > 0 && completedNow < continuousBufferLimit)
                vesselAutoSkipBeforeRead = vesselAutoCycleSkipSamples;
        }

        compactPendingSamples();
        Q_UNUSED(fifoSamplesBeforeRead);
        return emittedFrames;
    };

    auto discardAdSamples = [&](ULONGLONG samplesToDiscard, const QString &stage) -> bool {
        ULONGLONG remaining = samplesToDiscard;
        while (remaining > 0 && m_isCapturing)
        {
            const LONG discardCount = alignedReadAdCount(remaining);
            if (discardCount <= 0)
            {
                const LONG readCount = kReadAdCountUnit;
                while (m_isCapturing)
                {
                    const ULONGLONG available = (ULONGLONG)PCIe3640_GetBufCnt(m_pcieHandle) * FIFO_UNIT;
                    if (available >= static_cast<ULONGLONG>(readCount))
                        break;

                    QThread::msleep(10);
                    if (startTime.elapsed() > totalTimeoutMs)
                    {
                        emit acquisitionStatus(QStringLiteral("采集诊断：%1 等待超时，5 秒内只有 %2 样点，未达到尾段跳过需要的 %3 样点。")
                                                   .arg(stage)
                                                   .arg(available)
                                                   .arg(readCount));
                        return false;
                    }
                }

                if (!m_isCapturing)
                    return false;

                if (!PCIe3640_ReadAD(m_pcieHandle, 0, readCount, 10000))
                {
                    emit acquisitionStatus(QStringLiteral("采集诊断：%1 跳过尾段数据失败。").arg(stage));
                    return false;
                }

                U16* pRealADData = (U16*)m_pcieBuf.pADBufA;
                if (pRealADData == nullptr)
                {
                    emit acquisitionStatus(QStringLiteral("采集诊断：%1 跳过尾段后 AD 缓存地址为空。").arg(stage));
                    return false;
                }

                const int skippedTailSamples = static_cast<int>(remaining);
                if (skippedTailSamples < readCount)
                    appendPendingSamples(pRealADData + skippedTailSamples,
                                         readCount - skippedTailSamples);
                remaining = 0;
                startTime.restart();
                break;
            }

            while (m_isCapturing)
            {
                const ULONGLONG available = (ULONGLONG)PCIe3640_GetBufCnt(m_pcieHandle) * FIFO_UNIT;
                if (available >= static_cast<ULONGLONG>(discardCount))
                    break;

                QThread::msleep(10);
                if (startTime.elapsed() > totalTimeoutMs)
                {
                    emit acquisitionStatus(QStringLiteral("采集诊断：%1 等待超时，5 秒内只有 %2 样点，未达到需要跳过的 %3 样点。")
                                               .arg(stage)
                                               .arg(available)
                                               .arg(discardCount));
                    return false;
                }
            }

            if (!m_isCapturing)
                return false;

            if (!PCIe3640_ReadAD(m_pcieHandle, 0, discardCount, 10000))
            {
                emit acquisitionStatus(QStringLiteral("采集诊断：%1 跳过过渡段数据失败。").arg(stage));
                return false;
            }

            remaining -= discardCount;
            startTime.restart();
        }
        return remaining == 0;
    };

    // 核心采集循环：填充多缓冲区
    while (m_isCapturing)
    {
        if (vesselAutoContinuousMode && vesselAutoSkipBeforeRead > 0)
        {
            const QString skipStage = (mainWidget::scanMode == 43)
                ? QStringLiteral("Symphonic 线性运动段")
                : QStringLiteral("Vessel scan 过渡段");
            if (!discardAdSamples(vesselAutoSkipBeforeRead, skipStage))
                break;
            vesselAutoSkipBeforeRead = 0;
        }

        // 1. 等待FIFO数据量满足要求
        // FIFO 指的是 First In First Out（先进先出），是卡里面的一个硬件缓冲区，采集卡会把采集到的数据先放到这个 FIFO 里。我们需要等 FIFO 里有足够的数据（样点数）后，才能调用 PCIe3640_ReadAD 从 FIFO 里读取数据到我们的内存缓冲区。

        // 读取数据计数。如果硬件读取块还没有凑够，就等待 10ms, 之后直接开始下一次 while 循环 (continue). 由于采集的线程和主线程是并发的，所以不会影响主线程。
        ULONGLONG bufCnt = (ULONGLONG)PCIe3640_GetBufCnt(m_pcieHandle) * FIFO_UNIT;
        int completedNow = 0;
        U16 minValue = 0;
        U16 maxValue = 0;
        U16 firstValue = 0;
        double meanValue = 0.0;
        bool shouldReportStatus = false;
        ULONG triggerCount = 0;
        if (emitLogicalFrames(bufCnt,
                              completedNow,
                              shouldReportStatus,
                              minValue,
                              maxValue,
                              firstValue,
                              meanValue) < 0)
            break;
        if (!m_isCapturing)
            break;

        if (bufCnt < static_cast<ULONGLONG>(m_adReadChunkSize))
        {
            QThread::msleep(10);
            if (startTime.elapsed() > totalTimeoutMs)
            {
                qDebug() << "[错误] entry(): FIFO等待超时！等待时间超过 " << totalTimeoutMs << " ms, 当前FIFO样点数 = " << bufCnt;
                emit acquisitionStatus(QStringLiteral("采集诊断：FIFO 等待超时，5 秒内只有 %1 样点，未达到硬件单次读取需要的 %2 样点。")
                                           .arg(bufCnt)
                                           .arg(m_adReadChunkSize));
                break;
            }
            continue;
        }

        // 2. 读取单通道数据，把数据写到采集卡的缓冲区 A 中（第二个参量 0）
        // 期间不持锁，因为它们只访问硬件，不碰共享的成员变量，而且耗时不可预测。
        BOOL ret = PCIe3640_ReadAD(m_pcieHandle, 0, m_adReadChunkSize, 10000); // 超时10000ms
        if (!ret)
        {
            qDebug() << "[错误] entry(): 读取AD数据失败！";
            emit acquisitionStatus("采集诊断：PCIe3640_ReadAD 读取失败。");
            break;
        }
        startTime.restart();

        // 3. 获取真实硬件缓存地址、填充当前缓冲区
        // 数据拷贝前，重新加锁，确保在访问和修改共享变量（m_curIndexInMemBuffer、m_lastAvailableIndex、m_buffersCompleted）时线程安全。拷贝完成后立即解锁，减少锁的持有时间，避免阻塞主线程。
        U16* pRealADData = (U16*)m_pcieBuf.pADBufA;
        if (pRealADData == nullptr)
        {
            qDebug() << "[错误] entry(): AD缓存地址为空！";
            emit acquisitionStatus("采集诊断：读取成功标志已返回，但 AD 缓存地址为空。");
            break;
        }

        appendPendingSamples(pRealADData, m_adReadChunkSize);
        const int emittedFrames = emitLogicalFrames(bufCnt,
                                                    completedNow,
                                                    shouldReportStatus,
                                                    minValue,
                                                    maxValue,
                                                    firstValue,
                                                    meanValue);
        if (emittedFrames < 0)
            break;

        if (shouldReportStatus)
        {
            triggerCount = PCIe3640_GetTrigCnt(m_pcieHandle);
            QString statusMessage = QStringLiteral("采集诊断：已生成 %1 个逻辑缓存帧，触发计数=%2，FIFO=%3 样点，硬件读取=%4 样点，待拼接=%5 样点，原始码 min=%6 max=%7 mean=%8，首点=%9。")
                                        .arg(completedNow)
                                        .arg(triggerCount)
                                        .arg(bufCnt)
                                        .arg(m_adReadChunkSize)
                                        .arg(static_cast<qulonglong>(pendingSamples.size() - pendingOffset))
                                        .arg(minValue)
                                        .arg(maxValue)
                                        .arg(meanValue, 0, 'f', 1)
                                        .arg(firstValue);
            emit acquisitionStatus(statusMessage);
            statusTimer.restart();
        }

    }

    // 采集完成清理；因为需要修改 m_isCapturing, 所以需要加锁保护。
    locker.relock();
    m_isCapturing = false;
    const int completedBuffers = m_buffersCompleted;
    locker.unlock();
    qDebug() << "entry(): 采集完成！总共生成的逻辑缓存帧数：" << completedBuffers;
    emit acquisitionStatus(QStringLiteral("采集诊断：采集线程结束，总共生成 %1 个逻辑缓存帧。").arg(completedBuffers));
    emit captureLoopFinished(completedBuffers);
}
