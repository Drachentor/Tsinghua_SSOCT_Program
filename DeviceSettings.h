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

struct SweptSourceOption
{
    QString id;
    QString displayName;
    int ascanFreq;
    int ascanLen;
    QString ascanDutyCycle;
};

QString settingsFilePath();
QString parametersDirectoryPath();
QString calibrationDirectoryPath();
QString scanPathDirectoryPath();
QString scanPathAudioDirectoryPath();

QString defaultDacDeviceId();
QString defaultAdcDeviceId();
QString defaultSweptSourceId();
QVector<DeviceOption> supportedDacDevices();
QVector<DeviceOption> supportedAdcDevices();
QVector<SweptSourceOption> supportedSweptSources();

QString normalizeDacDeviceId(const QString &idOrName);
QString normalizeAdcDeviceId(const QString &idOrName);
QString normalizeSweptSourceId(const QString &idOrName);
QString selectedDacDeviceId(QSettings &settings);
QString selectedAdcDeviceId(QSettings &settings);
QString selectedSweptSourceId(QSettings &settings);
void saveSelectedDevices(QSettings &settings,
                         const QString &dacDeviceId,
                         const QString &adcDeviceId);

QString dacDeviceDisplayName(const QString &deviceId);
QString adcDeviceDisplayName(const QString &deviceId);
QString sweptSourceDisplayName(const QString &sourceId);
SweptSourceOption sweptSourceById(const QString &sourceId);

QString legacyMainWidgetGroup();
QString commonSettingsGroup();
QString dacSettingsGroup(const QString &deviceId);
QString adcSettingsGroup(const QString &deviceId);

bool isFcctecPcie3640Dac(const QString &deviceId);
bool isFcctecPcie3640Adc(const QString &deviceId);
bool isNiPcie6353Dac(const QString &deviceId);

} // namespace DeviceSettings

#endif // DEVICESETTINGS_H
