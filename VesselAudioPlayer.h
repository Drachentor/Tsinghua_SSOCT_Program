#ifndef VESSELAUDIOPLAYER_H
#define VESSELAUDIOPLAYER_H

#include <QByteArray>
#include <QMutex>
#include <QString>

#include <portaudio.h>

class VesselAudioPlayer
{
public:
    static VesselAudioPlayer &instance();

    bool probeWasapiOutput(QString *statusMessage = nullptr, QString *errorMessage = nullptr);
    bool startLoop(const QByteArray &pcm24StereoData, QString *errorMessage = nullptr);
    bool startLoopFromWav(const QString &wavPath, QString *errorMessage = nullptr);
    void stop();
    bool isPlaying() const;

private:
    VesselAudioPlayer();
    ~VesselAudioPlayer();
    VesselAudioPlayer(const VesselAudioPlayer &) = delete;
    VesselAudioPlayer &operator=(const VesselAudioPlayer &) = delete;

    static int streamCallback(const void *inputBuffer,
                              void *outputBuffer,
                              unsigned long framesPerBuffer,
                              const PaStreamCallbackTimeInfo *timeInfo,
                              PaStreamCallbackFlags statusFlags,
                              void *userData);
    int fillOutput(void *outputBuffer, unsigned long framesPerBuffer);
    bool ensureInitialized(QString *errorMessage);
    void setError(QString *errorMessage, const QString &message) const;

    mutable QMutex m_mutex;
    QByteArray m_pcmData;
    qsizetype m_byteOffset;
    PaStream *m_stream;
    bool m_initialized;
};

#endif // VESSELAUDIOPLAYER_H
