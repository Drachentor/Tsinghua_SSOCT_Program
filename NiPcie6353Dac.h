#ifndef NIPCIE6353DAC_H
#define NIPCIE6353DAC_H

#include <QString>
#include <QStringList>

enum class NiPcie6353FifoRegenerationMode
{
    Standard,
    AutoIfFits
};

struct NiPcie6353DacConfig
{
    QString deviceName = QStringLiteral("Dev1");
    QString aoChannels = QStringLiteral("ao0:1");
    double outputRangeVolts = 5.0;
    QString sampleClockSource;
    QString startTriggerSource;
    NiPcie6353FifoRegenerationMode fifoRegenerationMode = NiPcie6353FifoRegenerationMode::Standard;
};

class NiPcie6353Dac
{
public:
    NiPcie6353Dac();
    ~NiPcie6353Dac();

    static bool probeDevice(const NiPcie6353DacConfig &config,
                            QStringList *infoLines,
                            QString *errorMessage);

    bool prepare(const NiPcie6353DacConfig &config,
                 const unsigned short *xData,
                 int xLength,
                 const unsigned short *yData,
                 int yLength,
                 int sampleRateHz,
                 QString *diagnosticMessage,
                 QString *errorMessage);
    bool start(QString *errorMessage);
    void stop();
    bool resetOutputsToZero(const NiPcie6353DacConfig &config, QString *errorMessage);
    bool isPrepared() const;

private:
    struct Impl;
    Impl *d;
};

#endif // NIPCIE6353DAC_H
