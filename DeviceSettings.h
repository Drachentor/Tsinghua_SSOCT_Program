#ifndef DEVICESETTINGS_H
#define DEVICESETTINGS_H

#include <QString>
#include <QVector>

class QSettings;

namespace DeviceSettings {

struct DeviceOption
{
    QString id;
    QString displayName;
};

QString settingsFilePath();

QString defaultDacDeviceId();
QString defaultAdcDeviceId();
QVector<DeviceOption> supportedDacDevices();
QVector<DeviceOption> supportedAdcDevices();

QString normalizeDacDeviceId(const QString &idOrName);
QString normalizeAdcDeviceId(const QString &idOrName);
QString selectedDacDeviceId(QSettings &settings);
QString selectedAdcDeviceId(QSettings &settings);
void saveSelectedDevices(QSettings &settings,
                         const QString &dacDeviceId,
                         const QString &adcDeviceId);

QString dacDeviceDisplayName(const QString &deviceId);
QString adcDeviceDisplayName(const QString &deviceId);

QString legacyMainWidgetGroup();
QString commonSettingsGroup();
QString dacSettingsGroup(const QString &deviceId);
QString adcSettingsGroup(const QString &deviceId);

bool isFcctecPcie3640Dac(const QString &deviceId);
bool isFcctecPcie3640Adc(const QString &deviceId);

} // namespace DeviceSettings

#endif // DEVICESETTINGS_H
