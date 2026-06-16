#include "VesselAudioPlayer.h"

#include <QFile>
#include <QMutexLocker>

#include <pa_win_wasapi.h>

#include <algorithm>
#include <cstring>

namespace {

const double kVesselAudioSampleRate = 48000.0;
const int kVesselAudioChannels = 2;
const int kVesselAudioBytesPerFrame = 6;
const unsigned long kVesselAudioFramesPerBuffer = 256;

QString paErrorText(PaError error)
{
    return QString::fromLocal8Bit(Pa_GetErrorText(error));
}

quint16 readU16Le(const QByteArray &data, int offset)
{
    return static_cast<quint16>(static_cast<unsigned char>(data[offset]))
        | (static_cast<quint16>(static_cast<unsigned char>(data[offset + 1])) << 8);
}

quint32 readU32Le(const QByteArray &data, int offset)
{
    return static_cast<quint32>(static_cast<unsigned char>(data[offset]))
        | (static_cast<quint32>(static_cast<unsigned char>(data[offset + 1])) << 8)
        | (static_cast<quint32>(static_cast<unsigned char>(data[offset + 2])) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(data[offset + 3])) << 24);
}

} // namespace

VesselAudioPlayer &VesselAudioPlayer::instance()
{
    static VesselAudioPlayer player;
    return player;
}

VesselAudioPlayer::VesselAudioPlayer()
    : m_byteOffset(0),
      m_stream(nullptr),
      m_initialized(false)
{
}

VesselAudioPlayer::~VesselAudioPlayer()
{
    stop();
    if (m_initialized) {
        Pa_Terminate();
    }
}

bool VesselAudioPlayer::probeWasapiOutput(QString *statusMessage, QString *errorMessage)
{
    if (!ensureInitialized(errorMessage)) {
        return false;
    }

    const PaVersionInfo *versionInfo = Pa_GetVersionInfo();
    const QString versionText = (versionInfo && versionInfo->versionText)
        ? QString::fromLocal8Bit(versionInfo->versionText)
        : QStringLiteral("unknown");

    const PaHostApiIndex wasapiIndex = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
    if (wasapiIndex < 0) {
        setError(errorMessage, QStringLiteral("PortAudio 初始化成功，但未找到 WASAPI Host API。"));
        return false;
    }

    const PaHostApiInfo *wasapiInfo = Pa_GetHostApiInfo(wasapiIndex);
    if (!wasapiInfo) {
        setError(errorMessage, QStringLiteral("PortAudio 找到 WASAPI Host API，但无法读取 WASAPI 接口信息。"));
        return false;
    }
    if (wasapiInfo->defaultOutputDevice == paNoDevice) {
        setError(errorMessage, QStringLiteral("WASAPI 没有可用的默认输出设备。"));
        return false;
    }

    const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(wasapiInfo->defaultOutputDevice);
    if (!deviceInfo) {
        setError(errorMessage, QStringLiteral("WASAPI 默认输出设备无效，无法读取设备信息。"));
        return false;
    }
    if (deviceInfo->maxOutputChannels < kVesselAudioChannels) {
        setError(errorMessage,
                 QStringLiteral("WASAPI 默认输出设备通道数不足：需要 %1 通道，当前设备最多 %2 通道。")
                 .arg(kVesselAudioChannels)
                 .arg(deviceInfo->maxOutputChannels));
        return false;
    }

    PaWasapiStreamInfo wasapiStreamInfo;
    std::memset(&wasapiStreamInfo, 0, sizeof(wasapiStreamInfo));
    wasapiStreamInfo.size = sizeof(PaWasapiStreamInfo);
    wasapiStreamInfo.hostApiType = paWASAPI;
    wasapiStreamInfo.version = 1;
    wasapiStreamInfo.flags = paWinWasapiExclusive
        | paWinWasapiExplicitSampleFormat;

    PaStreamParameters outputParameters;
    std::memset(&outputParameters, 0, sizeof(outputParameters));
    outputParameters.device = wasapiInfo->defaultOutputDevice;
    outputParameters.channelCount = kVesselAudioChannels;
    outputParameters.sampleFormat = paInt24;
    outputParameters.suggestedLatency = deviceInfo->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = &wasapiStreamInfo;

    const PaError formatError = Pa_IsFormatSupported(nullptr,
                                                     &outputParameters,
                                                     kVesselAudioSampleRate);
    if (formatError != paFormatIsSupported) {
        setError(errorMessage,
                 QStringLiteral("已找到 PortAudio 和 WASAPI，但默认输出设备不支持 WASAPI exclusive 24-bit/48000Hz 双声道输出：%1。")
                 .arg(paErrorText(formatError)));
        return false;
    }

    if (statusMessage) {
        const QString deviceName = deviceInfo->name
            ? QString::fromLocal8Bit(deviceInfo->name)
            : QStringLiteral("unknown");
        *statusMessage = QStringLiteral("Symphonic 声卡接口检查通过：%1；WASAPI 默认输出设备：%2；支持 24-bit/48000Hz 双声道。")
            .arg(versionText)   // PortAudio 版本
            .arg(deviceName);
    }
    return true;
}

bool VesselAudioPlayer::startLoop(const QByteArray &pcm24StereoData, QString *errorMessage)
{
    if (pcm24StereoData.isEmpty() || pcm24StereoData.size() % kVesselAudioBytesPerFrame != 0) {
        setError(errorMessage, QStringLiteral("音频 buffer 为空或不是 24-bit 双声道整帧数据。"));
        return false;
    }

    if (!ensureInitialized(errorMessage)) {
        return false;
    }

    stop();

    {
        QMutexLocker locker(&m_mutex);
        m_pcmData = pcm24StereoData;
        m_byteOffset = 0;
    }

    const PaHostApiIndex wasapiIndex = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
    if (wasapiIndex < 0) {
        setError(errorMessage, QStringLiteral("PortAudio 未找到 WASAPI Host API。"));
        return false;
    }

    const PaHostApiInfo *wasapiInfo = Pa_GetHostApiInfo(wasapiIndex);
    if (!wasapiInfo || wasapiInfo->defaultOutputDevice == paNoDevice) {
        setError(errorMessage, QStringLiteral("WASAPI 没有可用的默认输出设备。"));
        return false;
    }

    PaWasapiStreamInfo wasapiStreamInfo;
    std::memset(&wasapiStreamInfo, 0, sizeof(wasapiStreamInfo));
    wasapiStreamInfo.size = sizeof(PaWasapiStreamInfo);
    wasapiStreamInfo.hostApiType = paWASAPI;
    wasapiStreamInfo.version = 1;
    wasapiStreamInfo.flags = paWinWasapiExclusive
        | paWinWasapiExplicitSampleFormat
        | paWinWasapiThreadPriority;
    wasapiStreamInfo.threadPriority = eThreadPriorityProAudio;

    PaStreamParameters outputParameters;
    std::memset(&outputParameters, 0, sizeof(outputParameters));
    outputParameters.device = wasapiInfo->defaultOutputDevice;
    outputParameters.channelCount = kVesselAudioChannels;
    outputParameters.sampleFormat = paInt24;
    outputParameters.suggestedLatency = 0.0;
    outputParameters.hostApiSpecificStreamInfo = &wasapiStreamInfo;

    PaError error = Pa_OpenStream(&m_stream,
                                  nullptr,
                                  &outputParameters,
                                  kVesselAudioSampleRate,
                                  kVesselAudioFramesPerBuffer,
                                  paClipOff,
                                  &VesselAudioPlayer::streamCallback,
                                  this);
    if (error != paNoError) {
        m_stream = nullptr;
        setError(errorMessage, QStringLiteral("打开 WASAPI exclusive 24-bit/48000Hz 双声道输出失败：%1。")
                 .arg(paErrorText(error)));
        return false;
    }

    error = Pa_StartStream(m_stream);
    if (error != paNoError) {
        Pa_CloseStream(m_stream);
        m_stream = nullptr;
        setError(errorMessage, QStringLiteral("启动 WASAPI 音频输出失败：%1。")
                 .arg(paErrorText(error)));
        return false;
    }

    return true;
}

bool VesselAudioPlayer::startLoopFromWav(const QString &wavPath, QString *errorMessage)
{
    QFile file(wavPath);
    if (!file.open(QFile::ReadOnly)) {
        setError(errorMessage, QStringLiteral("无法打开音频文件：%1。").arg(wavPath));
        return false;
    }

    const QByteArray wavData = file.readAll();
    if (wavData.size() < 44
        || wavData.mid(0, 4) != "RIFF"
        || wavData.mid(8, 4) != "WAVE") {
        setError(errorMessage, QStringLiteral("不是有效的 RIFF/WAVE 文件：%1。").arg(wavPath));
        return false;
    }

    bool foundFormat = false;
    bool foundData = false;
    quint16 audioFormat = 0;
    quint16 channelCount = 0;
    quint32 sampleRate = 0;
    quint16 blockAlign = 0;
    quint16 bitsPerSample = 0;
    QByteArray pcmData;

    int offset = 12;
    while (offset + 8 <= wavData.size()) {
        const QByteArray chunkId = wavData.mid(offset, 4);
        const quint32 chunkSize = readU32Le(wavData, offset + 4);
        const int chunkDataOffset = offset + 8;
        if (chunkDataOffset + static_cast<int>(chunkSize) > wavData.size()) {
            setError(errorMessage, QStringLiteral("WAV chunk 长度无效：%1。").arg(wavPath));
            return false;
        }

        if (chunkId == "fmt ") {
            if (chunkSize < 16) {
                setError(errorMessage, QStringLiteral("WAV fmt chunk 过短：%1。").arg(wavPath));
                return false;
            }
            audioFormat = readU16Le(wavData, chunkDataOffset);
            channelCount = readU16Le(wavData, chunkDataOffset + 2);
            sampleRate = readU32Le(wavData, chunkDataOffset + 4);
            blockAlign = readU16Le(wavData, chunkDataOffset + 12);
            bitsPerSample = readU16Le(wavData, chunkDataOffset + 14);
            foundFormat = true;
        } else if (chunkId == "data") {
            pcmData = wavData.mid(chunkDataOffset, static_cast<int>(chunkSize));
            foundData = true;
        }

        offset = chunkDataOffset + static_cast<int>(chunkSize);
        if (offset % 2 != 0) {
            ++offset;
        }
    }

    if (!foundFormat || !foundData) {
        setError(errorMessage, QStringLiteral("WAV 文件缺少 fmt 或 data chunk：%1。").arg(wavPath));
        return false;
    }
    if (audioFormat != 1
        || channelCount != kVesselAudioChannels
        || sampleRate != static_cast<quint32>(kVesselAudioSampleRate)
        || bitsPerSample != 24
        || blockAlign != kVesselAudioBytesPerFrame) {
        setError(errorMessage,
                 QStringLiteral("WAV 格式必须为 PCM 24-bit, 48000 Hz, 双声道；当前 format=%1, channels=%2, sampleRate=%3, bits=%4, blockAlign=%5。")
                 .arg(audioFormat)
                 .arg(channelCount)
                 .arg(sampleRate)
                 .arg(bitsPerSample)
                 .arg(blockAlign));
        return false;
    }
    if (pcmData.isEmpty() || pcmData.size() % kVesselAudioBytesPerFrame != 0) {
        setError(errorMessage, QStringLiteral("WAV data chunk 为空或不是完整双声道帧：%1。").arg(wavPath));
        return false;
    }

    return startLoop(pcmData, errorMessage);
}

void VesselAudioPlayer::stop()
{
    PaStream *stream = m_stream;
    m_stream = nullptr;
    if (stream) {
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
    }

    QMutexLocker locker(&m_mutex);
    m_pcmData.clear();
    m_byteOffset = 0;
}

bool VesselAudioPlayer::isPlaying() const
{
    return m_stream != nullptr;
}

int VesselAudioPlayer::streamCallback(const void *inputBuffer,
                                      void *outputBuffer,
                                      unsigned long framesPerBuffer,
                                      const PaStreamCallbackTimeInfo *timeInfo,
                                      PaStreamCallbackFlags statusFlags,
                                      void *userData)
{
    Q_UNUSED(inputBuffer);
    Q_UNUSED(timeInfo);
    Q_UNUSED(statusFlags);
    return static_cast<VesselAudioPlayer *>(userData)->fillOutput(outputBuffer, framesPerBuffer);
}

int VesselAudioPlayer::fillOutput(void *outputBuffer, unsigned long framesPerBuffer)
{
    char *outputBytes = static_cast<char *>(outputBuffer);
    qsizetype bytesToWrite = static_cast<qsizetype>(framesPerBuffer) * kVesselAudioBytesPerFrame;
    QMutexLocker locker(&m_mutex);

    if (m_pcmData.isEmpty()) {
        std::memset(outputBytes, 0, static_cast<size_t>(bytesToWrite));
        return paContinue;
    }

    while (bytesToWrite > 0) {
        const qsizetype bytesAvailable = m_pcmData.size() - m_byteOffset;
        const qsizetype chunkBytes = std::min(bytesToWrite, bytesAvailable);
        std::memcpy(outputBytes, m_pcmData.constData() + m_byteOffset, static_cast<size_t>(chunkBytes));
        outputBytes += chunkBytes;
        bytesToWrite -= chunkBytes;
        m_byteOffset += chunkBytes;
        if (m_byteOffset >= m_pcmData.size()) {
            m_byteOffset = 0;
        }
    }

    return paContinue;
}

bool VesselAudioPlayer::ensureInitialized(QString *errorMessage)
{
    if (m_initialized) {
        return true;
    }

    const PaError error = Pa_Initialize();
    if (error != paNoError) {
        setError(errorMessage, QStringLiteral("初始化 PortAudio 失败：%1。").arg(paErrorText(error)));
        return false;
    }
    m_initialized = true;
    return true;
}

void VesselAudioPlayer::setError(QString *errorMessage, const QString &message) const
{
    if (errorMessage) {
        *errorMessage = message;
    }
}
