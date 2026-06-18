#ifndef MAINWIDGET_H
#define MAINWIDGET_H

#include <QWidget>
#include <mkl.h>
#include "include/PCIe3640.h"
#include <QThread>
#include "mythread.h"
#include "qdebug.h"
#include <QTimer>
#include <QElapsedTimer>
#include <QMutex>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <fstream>
#include <vector>
#include "QColorDialog"   //颜色对话框
using namespace cv;

namespace Ui {
class mainWidget;
}

class QTextEdit;



class mainWidget : public QWidget
{
    Q_OBJECT

public:

    explicit mainWidget(QWidget *parent = nullptr);
    ~mainWidget();
    float* m_MEMbufF;
    QThread *firstThread;
    mythread *ssoctThread;

    void PrintBoardInfo();
    void PrintNiPcie6353BoardInfo();
    void PrepareProcessing1D();
    void timerEvent(QTimerEvent *event);

    // config the alazar car
    bool InitADForCapture();


    static int captureflag;
    static int scanMode;
    enum class ADTriggerMode
    {
        Internal,
        External,
        Continuous
    };
    static ADTriggerMode triggerMode;
    enum class ADClockMode
    {
        Internal,
        External
    };
    static ADClockMode clockMode;
    // value    | mode              | timer No. | interval(ms)
    // 1        | 1D scan           | 1         | 60
    // 10       | 2D cross scan     | 3         | 100
    // 2        | 2D repeat         | 2         | 8
    // 22       | 2D angio          | 3         | 700
    // 3        | 3D scan           | 2         | 100
    // 32       | 3D angio          | 2         | 100
    // 42       | Vessel scan       | 2         | 100
    // 43       | Symphonic         | 2         | 100

    static int AscanLen;
    static int BscanLen;
    static int CscanLen;
    static const int MinAngioRep = 1;
    static const int MaxAngioRep = 8;
    static const int DefaultAngioRep = 4;
    static int AngioRep;
    static int TriggerOffsetSamples;
    static bool ContinuousModeEnabled;
    static int ContinuousAlineCount;

    static bool isContinuousSupportedMode(int mode);
    static int continuousStoredBscanCount();
    static int continuousSingleBufferSampleCount();
    static int continuousBufferCount();
    static long long continuousTargetSampleCount();

    const static double FFT_SCALE;

    static bool BGReductionFlag;//0为不减本底，1为减
    static bool Wflag;//true 为不加窗，false 为加窗

    double Voltage;
    double galvoFreq;
    double duty_cycle;

    int AscanFreq;

    int timerId1;
    int timerId2;
    int timerId3;
    float* m_dataIn; // 原始的频域数据
    unsigned short*				m_curDisplayData;

    // 血流用数据
    unsigned short*				m_curDisplayDataAngio1;
    unsigned short*				m_curDisplayDataAngio2;
    float* p_A1;
    float* p_A2;
    float* p_A3;
    float* p_A4;
    float* crossSave;
    unsigned short*				m_curData;

    float* p_F2; // 存事件2lock出来的数据，用于二维成像
    std::vector<float> m_BG;
    std::vector<unsigned short> m_lastAscanForSave;
    int m_last3DRealtimeDisplayBuffer;
    int m_realtimeShowInterval;
    QElapsedTimer m_volumeScanTimer;
    bool m_volumeScanTimingActive;
    int m_volumeScanTimingMode;
    std::vector<double> spectralWindow;
    float* m_cosphy; 
    float* m_sinphy;
    MKL_Complex8* m_Cdata;

    QMutex mutex1;

    // 和 400k 的相比，减少了 yolo 相关的变量
    void bufferRefreshIndex(int &bufferCompleted, int &curIndexInMEMbuffer, int &availbleIndex);
    void removeBGForAscan(float* datainfer, unsigned short* AscanData);
    void removeBGForBscan(float* datainfer, unsigned short* BscanData);
    void removeBGForBscanDouble(float* datainfer, unsigned short* BscanDataAngio1, unsigned short* BscanDataAngio2);

    void cosplot(float*);
    void fftplot(float*);
    void plotMiddleBscanAline(float*);
    void OCTplots();
    void fftBscan(float*); // 用于计算Bscan的fft等操作
    float* fftBscanprocess(float*); // 用于Bscan的fft后处理操作
    void calc_dispersion_compensation(); // 根据输入的色散补偿参数计算相位修正量 phy 及其三角函数值，存入 m_cosphy 和 m_sinphy 数组
    void apply_dispersion_compensation(MKL_Complex8* data_compensated, float* data_input, int a, int b);   // 对输入的频域数据应用色散补偿，输出补偿后的数据
    float* calc_FFT_Bscan(int AscanLength, int BscanLength, int CscanLength, float* b_input, bool ignore_dispersion); // 计算 Bscan 的 FFT 结果，输出相应的实空间数据指针
    cv::Mat normalizeBscan(float* num, int tmp1, int tmp2, int offset); // 用于将 Bscan 数据归一化到指定范围内
    cv::Mat normalizeBscanFlatten(float* num, int tmp1, int tmp2, int offset); // 用于将 Bscan 数据归一化到指定范围内
    cv::Mat norm_mat_to_8bit(cv::Mat img); // 用于将归一化后的 Mat 数据转换成 8bit 格式
    cv::Mat writeBscan(float* num);
    void DftiTransform(int NumOfTransforms, int distance, MKL_Complex8* data_compensated, MKL_Complex8* data_fourierd); // 使用 MKL 库进行 FFT 变换的封装函数
    void plotBscan(float*); // 用于绘制Bscan数据
    void fftBfocus(float*); // 用于计算Bscan 焦点的fft等操作
    // 用于绘制得到Bscan图像的表面（画出来？），计算到焦面的距离，并反馈给机械臂要移动的距离，先只动一下；
    void plotBfocus(float*);

    void fftBfocusCoarse(float*); // 用于计算Bscan 焦点的fft等操作
    // 用于黄金分割自动变焦
    void plotBfocusCoarse(float*);
    void plotBscanCross1(float*);
    void fftBscanAngio2(float*); //用于计算血流Bscan的fft等操作

    void fftBscanCross(float * );
    void angioCM(Mat& dataTrans, Mat& angioOutput, int Measure);
    QImage putImage(const Mat&); // 用于将Mat数据转换成QIMAGE格式的数据

    QStringList save_dialog(const QString &name_filter,
                            const QString &pathKey = QString(),
                            const QString &defaultSuffix = QString());

private slots:
    void on_startButton_clicked();
    void on_connectButton_clicked();
    void on_stopButton_clicked();
    void on_comboBox_activated(const QString &arg1);
    void changeScanMode(const QString &arg1);
    void on_combo_triggerMode_activated(const QString &arg1);
    void on_combo_clockMode_activated(const QString &arg1);

    //void on_DAtest_clicked();

    void on_resetaxis_clicked();
    void on_saveButton_clicked();
    void on_textEdit_textChanged();
    void on_background_clicked();

    bool saveclicked();
    void on_addWindow_clicked();
    void on_connectDA_clicked();
    void on_change_sysParams_clicked();
    void on_AutoDecideAscanLength_clicked();
    void on_AutoDecideBscanLength_clicked();
    void on_V_ConvertAngioToImage_clicked();
    void on_V_ConvertImageToPath_clicked();
    void on_button_FFT_clicked();
    void on_comboBox_2_activated(const QString &arg1);


private:
    enum class DAState
    {
        NotReady,
        Ready,
        Scanning
    };

    struct VolumeScanSegment
    {
        int cscanStart = 0;
        int cscanCount = 0;
        int sourceBufferStart = 0;
        int sourceBufferCount = 0;
        int logicalFrameCount = 0;
    };

    bool readScanParametersFromUi();
    bool readSysParametersFromUi();
    bool readAngioRepFromUi(bool appendMessage);
    double sampleRateMHzFromUi(bool *ok = nullptr) const;
    int maxAscanLenFromUi(bool *ok = nullptr) const;
    bool clampAscanLenToLimit(bool appendMessage);
    void updateBscanCycleLengthFromFrequencies();
    void updateFrameRateFromBscanCycleLength();
    void updateAscanLenDependentUiState();
    void resetSpectralWindow();
    void updateControlState();
    void killActiveTimers();
    bool isVolumeScanMode() const;
    bool isContinuousAcquisitionMode() const;
    int continuousAcquisitionBufferCount() const;
    int expectedStoredBscanCount() const;
    int expectedAcquisitionBufferCount() const;
    bool hasSegmentedVolumeScan() const;
    int currentVolumeSegmentEndBuffer() const;
    bool configureVolumeSegmentationFromCurrentParams();
    bool prepareVolumeSegmentDA(int segmentIndex);
    void onAcquisitionLoopFinished(int completedBuffers);
    bool startNextVolumeSegment();
    bool writeCurrentVolumeBscans(std::ofstream &file, int volumeCount, int sourceBufferCount);
    bool saveContinuousAcquisition();
    QString analyzeContinuousAcquisitionPeriodicity(int targetBuffers,
                                                    int bufferCountInMem,
                                                    int newestIndex) const;
    bool writeContinuousPeriodicityReport(const QString &dataFilePath,
                                          const QString &reportText) const;
    bool finalizeSavedDataFile(const QString &filePath,
                               const QString &sampleType,
                               bool preprocessedSpectrum,
                               qint64 storedAlineCount,
                               int storedBscanCount,
                               bool backgroundRemoved,
                               bool windowApplied,
                               bool logScaled);
    bool writeAcquisitionJson(const QString &filePath,
                              const QString &sampleType,
                              bool preprocessedSpectrum,
                              qint64 storedAlineCount,
                              int storedBscanCount,
                              bool backgroundRemoved,
                              bool windowApplied,
                              bool logScaled);
    bool processFourierFile(const QString &filePath,
                            bool applyWindow,
                            bool removeBackground,
                            bool applyLogScale,
                            bool interactive);
    void stopCurrentScan(bool fillUnfinishedVolume,
                         bool releaseAD,
                         const QString &reason = QString(),
                         bool forceStopDA = false);
    bool prepareDAFromUi();
    bool symphonicDaOutputEnabled() const;
    void stopPreparedOrRunningDAIfNeeded(bool forceStopDA = false);
    void loadSettings();
    void saveSettings() const;
    bool selectedDacUsesPcie3640() const;
    bool selectedDacUsesNiPcie6353() const;
    bool selectedAdcUsesPcie3640() const;
    QString selectedDacDeviceName() const;
    QString selectedAdcDeviceName() const;
    NiPcie6353DacConfig niDacConfigFromUi() const;
    void applySelectedDacBackendToThread();
    void applyFastAxisMode(const QString &arg1, bool appendMessage);
    void initializeLogFiles();
    void appendTextEditDeltaToLog(QTextEdit *textEdit,
                                  const QString &fileName,
                                  const QString &level,
                                  int &lastTextLength);
    void appendInfoLogLine(const QString &message);

    Ui::mainWidget *ui;
    DAState m_daState;
    bool m_adReady;
    bool m_scanActive;
    std::vector<VolumeScanSegment> m_volumeScanSegments;
    int m_activeVolumeSegmentIndex;
    int m_infoLogTextLength;
    int m_captureLogTextLength;
    bool m_logFilesInitialized;
    QString m_selectedDacDeviceId;
    QString m_selectedAdcDeviceId;
};

#endif // MAINWIDGET_H
