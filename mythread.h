#ifndef MYTHREAD_H
#define MYTHREAD_H

#include <cstdint>
#include <QObject>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QVector>

//#pragma pack(push, 1) // 强制1字节对齐（匹配硬件寄存器布局）
#include "include/PCIe3640.h"  // 引入SDK头文件
//#pragma pack(pop)      // 恢复默认对齐（不影响其他代码）

#ifdef _WIN32
#include <Windows.h>
#endif

// 适配PCIe3640类型定义
#ifndef U8
typedef uint8_t U8;
#endif
#ifndef U16
typedef uint16_t U16;
#endif
#ifndef U32
#ifdef _WIN32
typedef unsigned long U32;
#else
typedef uint32_t U32;
#endif
#endif
#ifndef BOOL
typedef int BOOL;
#define TRUE  1
#define FALSE 0
#endif

// 前置声明
class mainWidget;

class mythread : public QObject
{
    Q_OBJECT

public:
    explicit mythread(QObject *parent = 0);
    ~mythread();

    // 保留你原有函数名
    bool InitADForCapture();
    void StopADCapture();
    bool PrintParams_AD(PCIe3640_PARA_INIT &m_pciePara);
    bool ConfigureDA(double Voltage, double galvoFreq, int AscanFreq,
                     double duty_cycle, int scanMode);
    bool ConfigureDA(double Voltage, double galvoFreq, int AscanFreq,
                     double duty_cycle, int scanMode,
                     int segmentStartCscan, int segmentCscanLen, int totalCscanLen);
    void ConfigureSymphonicTiming(int ascanFreq, int moveAlineCount, bool noReturnToZero);
    bool ConfigureSymphonicRfOnly(int ascanFreq);
    bool ConfigureSymphonicDAFromBoundPath(int ascanFreq);
    QString LastDAError() const;
    bool InitializePositionOutputsToZero();
    bool StartDAScan(int scanMode);
    void StopDAScan();
    void SetVolumeSegmentAcquisitionPlan(int plannedBufferCount, int currentSegmentBufferCount);
    bool RestartADForNextSegment();
    void FillUncompletedBuffersWithZeroVoltage(int expectedBufferCount);
    bool CalculateLengthParams();
    int singleBufferSize() const;
    int bufferCount() const;
    int adReadChunkSize() const;
    int bscanCycleAlineCount() const;
    int transitionAlineCount() const;
    int linearScanAlineCount() const;
    int xScanAlineCount() const;
    int repeatCyclesPerBuffer() const;

    // 多缓冲区相关成员（完全匹配主程序读取逻辑）
    QMutex mutex1;                          // 保留你原有锁名称（关键！）
    QVector<U16*> m_volumeMemBuffer;        // 多缓冲区数组
    int m_buffersCompleted;                 // 已采集完成的缓冲区数量
    int m_curIndexInMemBuffer;              // 当前缓冲区索引
    int m_lastAvailableIndex;               // 最后可用索引

public slots:
    // 持续采集的线程入口函数（需要在 mainWidget 中创建线程并连接到这个槽函数）
    void entry();

signals:
    void acquisitionStatus(const QString &message);
    void captureLoopFinished(int completedBuffers);

private:
    // 核心参数
    HANDLE m_pcieHandle;                    // 设备句柄，相当于设备 / 对象的 “身份证号”
    PCIe3640_PARA_INIT m_pciePara;          // 包含采集参数的结构体，见说明书 5.4 节
    PCIE_BUF m_pcieBuf;                     // 缓存信息
    bool m_isCapturing;                     // 采集状态标记，在 mainWidget 点击连接按钮之后，执行 InitADForCapture() 函数初始化采集卡并设置 m_isCapturing = true; 停止采集之后，置为 false

    // 缓冲区配置（匹配主程序）
    int m_bufferCount;                      // 缓冲区数量（默认为最大值 BscanLen * AngioRep）
    int m_singleBufferSize;                 // 单个缓冲区大小（m_AscanLen * m_BscanLen）
    int m_adReadChunkSize;                  // PCIe3640_ReadAD 单次读取大小，必须为 2048 的整数倍
    int m_repeatCyclesPerBuffer;
    int m_plannedVolumeBufferCount;
    int m_currentSegmentBufferCount;
    static const unsigned short PositionDacZeroCode = 32768;
    static const unsigned short AdcZeroCode = 8192;
    static const unsigned short RfDacHighCode = 65535;
    static const int64_t RfDacSampleRateHz = 5000000000LL;
    static const int RfDacLengthUnit = 16;
    static const int MinRfDacSamples = 8 * 1024;
    static const int MaxRfDacSamples = 128 * 1024;
    static const long RfDacAgc = 1023;

public:
    static const unsigned short plotOffset = AdcZeroCode / 16;

private:
    bool EnsureDeviceLinked();
    bool GenData(int scanMode);
    bool GenRfSquareData();
    bool WriteDABuffersToCard();
    bool WritePositionZeroToCardLocked(const QString &stage);
    bool FailDAConfig(const QString &message);
    void GenData_init(int len_XscanData, int len_YscanData);
    void GenData_fall(unsigned short* array, int length, int offset);
    void GenData_linScan(unsigned short* array, int length, int offset);
    void GenData_return(unsigned short* array, int length, int offset);
    void GenData_keep(unsigned short* array, int length, int offset, unsigned short value);

    unsigned short* pDataX;
    unsigned short* pDataY;
    unsigned short* pDataRF;

    double Voltage;
    double galvoFreq;
    double duty_cycle;
    int AscanFreq;
    int len_ZeroBuffer;
    int len_Transition;
    int len_BscanCycle;
    int len_LinearScan;
    int len_XScanData;
    int len_YScanData;
    int len_RfScanData;
    int m_daSegmentStartCscan;
    int m_daSegmentCscanLen;
    int m_daTotalCscanLen;
    int m_vesselAutoMoveAlineCount;
    bool m_vesselNoReturnToZero;
    long m_daDelay;
    long m_rfDelay;
    long m_pwmDelay;
    QString m_lastDAError;
};

#endif
