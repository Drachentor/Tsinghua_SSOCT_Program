#include "DeviceSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QtGlobal>

namespace {

const char kFcctecPcie3640Id[] = "FCCTEC_PCIe3640";
const char kNiPcie6353Id[] = "NI_PCIe6353";
const char kThorlabsSl134051Id[] = "Thorlabs_SL_134051";

QString normalizedToken(const QString &text)
{
    QString token = text.trimmed();
    token.replace(QLatin1Char('-'), QLatin1Char('_'));
    token.replace(QLatin1Char(' '), QLatin1Char('_'));
    return token;
}

bool matchesDevice(const QString &value, const QString &id, const QString &displayName)
{
    const QString token = normalizedToken(value);
    return token.compare(id, Qt::CaseInsensitive) == 0
        || value.trimmed().compare(displayName, Qt::CaseInsensitive) == 0;
}

QString sourceDirectoryPath()
{
    return QFileInfo(QString::fromLocal8Bit(__FILE__)).absolutePath();
}

QString applicationDirectoryPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    return appDir.isEmpty() ? sourceDirectoryPath() : appDir;
}

QString parameterDirectoryForRoot(const QString &root)
{
    return QDir(root).filePath(QStringLiteral("parameters"));
}

QString selectedParametersDirectoryPath()
{
    const QString appParameters = parameterDirectoryForRoot(applicationDirectoryPath());
    if (QFileInfo::exists(appParameters))
        return appParameters;
    return parameterDirectoryForRoot(sourceDirectoryPath());
}

QString ensureDirectory(const QString &path)
{
    QDir().mkpath(path);
    return path;
}

} // namespace

namespace DeviceSettings {

QString parametersDirectoryPath()
{
    return ensureDirectory(selectedParametersDirectoryPath());
}

QString calibrationDirectoryPath()
{
    return ensureDirectory(QDir(parametersDirectoryPath()).filePath(QStringLiteral("calibration")));
}

QString scanPathDirectoryPath()
{
    return ensureDirectory(QDir(parametersDirectoryPath()).filePath(QStringLiteral("scan_path")));
}

QString scanPathAudioDirectoryPath()
{
    return ensureDirectory(QDir(parametersDirectoryPath()).filePath(QStringLiteral("scan_path_audio")));
}

QString settingsFilePath()
{
    const QString path = QDir(parametersDirectoryPath()).filePath(QStringLiteral("settings.ini"));
    const QString legacyPath = QDir(sourceDirectoryPath()).filePath(QStringLiteral("settings.ini"));
    if (!QFileInfo::exists(path) && QFileInfo::exists(legacyPath))
        QFile::copy(legacyPath, path);
    return path;
}

QString defaultDacDeviceId()
{
    return QString::fromLatin1(kFcctecPcie3640Id);
}

QString defaultAdcDeviceId()
{
    return QString::fromLatin1(kFcctecPcie3640Id);
}

QString defaultSweptSourceId()
{
    return QString::fromLatin1(kThorlabsSl134051Id);
}

QVector<DeviceOption> supportedDacDevices()
{
    QVector<DeviceOption> devices;
    devices.append(DeviceOption{QString::fromLatin1(kFcctecPcie3640Id),
                                QStringLiteral("FCCTEC PCIe3640")});
    devices.append(DeviceOption{QString::fromLatin1(kNiPcie6353Id),
                                QStringLiteral("NI PCIe6353")});
    return devices;
}

QVector<DeviceOption> supportedAdcDevices()
{
    QVector<DeviceOption> devices;
    devices.append(DeviceOption{QString::fromLatin1(kFcctecPcie3640Id),
                                QStringLiteral("FCCTEC PCIe3640")});
    return devices;
}

QVector<SweptSourceOption> supportedSweptSources()
{
    QVector<SweptSourceOption> sources;
    sources.append(SweptSourceOption{QString::fromLatin1(kThorlabsSl134051Id),
                                     QStringLiteral("Thorlabs SL-134051"),
                                     400000,
                                     1600,
                                     QStringLiteral("0.55")});
    return sources;
}

QString normalizeDacDeviceId(const QString &idOrName)
{
    if (matchesDevice(idOrName,
                      QString::fromLatin1(kNiPcie6353Id),
                      QStringLiteral("NI PCIe6353"))) {
        return QString::fromLatin1(kNiPcie6353Id);
    }
    return defaultDacDeviceId();
}

QString normalizeAdcDeviceId(const QString &idOrName)
{
    Q_UNUSED(idOrName);
    return defaultAdcDeviceId();
}

QString normalizeSweptSourceId(const QString &idOrName)
{
    for (const SweptSourceOption &source : supportedSweptSources()) {
        if (matchesDevice(idOrName, source.id, source.displayName))
            return source.id;
    }
    return defaultSweptSourceId();
}

QString selectedDacDeviceId(QSettings &settings)
{
    const QString stored = settings.value(QStringLiteral("devices/selectedDacId"),
                                          defaultDacDeviceId()).toString();
    return normalizeDacDeviceId(stored);
}

QString selectedAdcDeviceId(QSettings &settings)
{
    const QString stored = settings.value(QStringLiteral("devices/selectedAdcId"),
                                          defaultAdcDeviceId()).toString();
    return normalizeAdcDeviceId(stored);
}

QString selectedSweptSourceId(QSettings &settings)
{
    const QString stored = settings.value(QStringLiteral("devices/selectedSweptSourceId"),
                                          defaultSweptSourceId()).toString();
    return normalizeSweptSourceId(stored);
}

void saveSelectedDevices(QSettings &settings,
                         const QString &dacDeviceId,
                         const QString &adcDeviceId)
{
    settings.setValue(QStringLiteral("devices/selectedDacId"),
                      normalizeDacDeviceId(dacDeviceId));
    settings.setValue(QStringLiteral("devices/selectedAdcId"),
                      normalizeAdcDeviceId(adcDeviceId));
}

QString dacDeviceDisplayName(const QString &deviceId)
{
    const QString normalized = normalizeDacDeviceId(deviceId);
    for (const DeviceOption &device : supportedDacDevices()) {
        if (device.id == normalized)
            return device.displayName;
    }
    return QStringLiteral("FCCTEC PCIe3640");
}

QString adcDeviceDisplayName(const QString &deviceId)
{
    Q_UNUSED(deviceId);
    return QStringLiteral("FCCTEC PCIe3640");
}

QString sweptSourceDisplayName(const QString &sourceId)
{
    const QString normalized = normalizeSweptSourceId(sourceId);
    for (const SweptSourceOption &source : supportedSweptSources()) {
        if (source.id == normalized)
            return source.displayName;
    }
    return QStringLiteral("Thorlabs SL-134051");
}

SweptSourceOption sweptSourceById(const QString &sourceId)
{
    const QString normalized = normalizeSweptSourceId(sourceId);
    for (const SweptSourceOption &source : supportedSweptSources()) {
        if (source.id == normalized)
            return source;
    }
    return supportedSweptSources().first();
}

QString legacyMainWidgetGroup()
{
    return QStringLiteral("mainWidget");
}

QString commonSettingsGroup()
{
    return QStringLiteral("mainWidget/Common");
}

QString dacSettingsGroup(const QString &deviceId)
{
    return QStringLiteral("mainWidget/DAC/%1").arg(normalizeDacDeviceId(deviceId));
}

QString adcSettingsGroup(const QString &deviceId)
{
    return QStringLiteral("mainWidget/ADC/%1").arg(normalizeAdcDeviceId(deviceId));
}

bool isFcctecPcie3640Dac(const QString &deviceId)
{
    return normalizeDacDeviceId(deviceId) == QString::fromLatin1(kFcctecPcie3640Id);
}

bool isFcctecPcie3640Adc(const QString &deviceId)
{
    return normalizeAdcDeviceId(deviceId) == QString::fromLatin1(kFcctecPcie3640Id);
}

bool isNiPcie6353Dac(const QString &deviceId)
{
    return normalizeDacDeviceId(deviceId) == QString::fromLatin1(kNiPcie6353Id);
}

} // namespace DeviceSettings
