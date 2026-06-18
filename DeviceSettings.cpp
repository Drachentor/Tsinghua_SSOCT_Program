#include "DeviceSettings.h"

#include <QFileInfo>
#include <QSettings>
#include <QtGlobal>

namespace {

const char kFcctecPcie3640Id[] = "FCCTEC_PCIe3640";
const char kNiPcie6353Id[] = "NI_PCIe6353";

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

} // namespace

namespace DeviceSettings {

QString settingsFilePath()
{
    return QFileInfo(QString::fromLocal8Bit(__FILE__)).absolutePath()
        + QStringLiteral("/settings.ini");
}

QString defaultDacDeviceId()
{
    return QString::fromLatin1(kFcctecPcie3640Id);
}

QString defaultAdcDeviceId()
{
    return QString::fromLatin1(kFcctecPcie3640Id);
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
