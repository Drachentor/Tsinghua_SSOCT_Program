#include "mainwidget.h"
#include "VesselProjectionProcessor.h"
#include "DeviceSettings.h"
#include <QApplication>
#include <QCoreApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QLabel>
#include <QSettings>
#include <QStringList>
#include <QTextCodec>
#include <QTextStream>
#include <QTranslator>
#include <QVBoxLayout>

namespace {

bool hasCliOption(const QStringList &arguments, const QString &name)
{
    return arguments.contains(name);
}

QString cliOptionValue(const QStringList &arguments,
                       const QString &name,
                       const QString &defaultValue = QString())
{
    const int index = arguments.indexOf(name);
    if (index < 0 || index + 1 >= arguments.size()) {
        return defaultValue;
    }
    return arguments.at(index + 1);
}

int cliIntOption(const QStringList &arguments,
                 const QString &name,
                 int defaultValue)
{
    bool ok = false;
    const QString value = cliOptionValue(arguments, name);
    const int parsed = value.toInt(&ok);
    return ok ? parsed : defaultValue;
}

void printConvertAngio3dUsage(QTextStream &out)
{
    out << "用法：Tsinghua_SSOCT.exe --convert-angio3d <file.3d> [options]\n"
        << "诊断选项：\n"
        << "  --settings <settings.ini>       读取指定 settings.ini；默认优先使用程序目录，其次当前目录。\n"
        << "  --flow-skeleton                启用自动血管骨架抑制速度图噪点。\n"
        << "  --flow-fit-correlation          额外生成 segment-wise 拟合相关系数图。\n"
        << "  --flow-crop-top <px>            速度图顶部裁剪像素。\n"
        << "  --flow-crop-bottom <px>         速度图底部裁剪像素。\n"
        << "  --flow-crop-left <px>           速度图左侧裁剪像素。\n"
        << "  --flow-crop-right <px>          速度图右侧裁剪像素。\n"
        << "  --generate-preview              同时生成多帧平均预览图。\n"
        << "  --generate-grayscale            同时生成血管灰度图。\n";
}

QString defaultSettingsPath()
{
    const QString appSettings =
        QCoreApplication::applicationDirPath() + QStringLiteral("/settings.ini");
    if (QFileInfo::exists(appSettings)) {
        return appSettings;
    }
    return QDir::current().absoluteFilePath(QStringLiteral("settings.ini"));
}

void addDeviceOptions(QComboBox *combo,
                       const QVector<DeviceSettings::DeviceOption> &devices,
                       const QString &currentDeviceId)
{
    int currentIndex = 0;
    for (const DeviceSettings::DeviceOption &device : devices) {
        combo->addItem(device.displayName, device.id);
        if (device.id == currentDeviceId)
            currentIndex = combo->count() - 1;
    }
    combo->setCurrentIndex(currentIndex);
}

void addSweptSourceOptions(QComboBox *combo, const QString &currentSourceId)
{
    int currentIndex = 0;
    for (const DeviceSettings::SweptSourceOption &source : DeviceSettings::supportedSweptSources()) {
        combo->addItem(source.displayName, source.id);
        if (source.id == currentSourceId)
            currentIndex = combo->count() - 1;
    }
    combo->setCurrentIndex(currentIndex);
}

void applySweptSourcePreset(QSettings &settings,
                            const QString &sourceId,
                            const QString &dacDeviceId,
                            const QString &adcDeviceId)
{
    const DeviceSettings::SweptSourceOption source = DeviceSettings::sweptSourceById(sourceId);
    settings.setValue(QStringLiteral("devices/selectedSweptSourceId"), source.id);

    settings.setValue(DeviceSettings::legacyMainWidgetGroup() + QStringLiteral("/AscanFreq"),
                      source.ascanFreq);
    settings.setValue(DeviceSettings::legacyMainWidgetGroup() + QStringLiteral("/AscanDutyCycle"),
                      source.ascanDutyCycle);
    settings.setValue(DeviceSettings::legacyMainWidgetGroup() + QStringLiteral("/AscanLen"),
                      source.ascanLen);

    settings.setValue(DeviceSettings::dacSettingsGroup(dacDeviceId) + QStringLiteral("/AscanFreq"),
                      source.ascanFreq);
    settings.setValue(DeviceSettings::dacSettingsGroup(dacDeviceId) + QStringLiteral("/AscanDutyCycle"),
                      source.ascanDutyCycle);
    settings.setValue(DeviceSettings::adcSettingsGroup(adcDeviceId) + QStringLiteral("/AscanLen"),
                      source.ascanLen);
}

bool showStartupDeviceDialog()
{
    QSettings settings(DeviceSettings::settingsFilePath(), QSettings::IniFormat);
    const QString currentDacDeviceId = DeviceSettings::selectedDacDeviceId(settings);
    const QString currentAdcDeviceId = DeviceSettings::selectedAdcDeviceId(settings);
    const QString currentSweptSourceId = DeviceSettings::selectedSweptSourceId(settings);

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("选择采集设备和扫频光源"));
    dialog.setModal(true);

    auto *layout = new QVBoxLayout(&dialog);
    auto *message = new QLabel(QStringLiteral("请选择本次运行使用的 DAC、ADC 设备和扫频光源。"), &dialog);
    message->setWordWrap(true);
    layout->addWidget(message);

    auto *form = new QFormLayout;
    auto *dacCombo = new QComboBox(&dialog);
    auto *adcCombo = new QComboBox(&dialog);
    auto *sweptSourceCombo = new QComboBox(&dialog);
    addDeviceOptions(dacCombo, DeviceSettings::supportedDacDevices(), currentDacDeviceId);
    addDeviceOptions(adcCombo, DeviceSettings::supportedAdcDevices(), currentAdcDeviceId);
    addSweptSourceOptions(sweptSourceCombo, currentSweptSourceId);
    form->addRow(QStringLiteral("DAC 设备"), dacCombo);
    form->addRow(QStringLiteral("ADC 设备"), adcCombo);
    form->addRow(QStringLiteral("扫频光源"), sweptSourceCombo);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal,
                                         &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return false;

    DeviceSettings::saveSelectedDevices(settings,
                                         dacCombo->currentData().toString(),
                                         adcCombo->currentData().toString());
    applySweptSourcePreset(settings,
                           sweptSourceCombo->currentData().toString(),
                           dacCombo->currentData().toString(),
                           adcCombo->currentData().toString());
    settings.sync();
    return true;
}

int runConvertAngio3dCli(const QStringList &arguments)
{
    QTextStream out(stdout);
    out.setCodec("UTF-8");
    QTextStream err(stderr);
    err.setCodec("UTF-8");

    const int convertIndex = arguments.indexOf(QStringLiteral("--convert-angio3d"));
    if (convertIndex < 0 || convertIndex + 1 >= arguments.size() ||
        arguments.at(convertIndex + 1).startsWith(QStringLiteral("--")) ||
        hasCliOption(arguments, QStringLiteral("--help"))) {
        printConvertAngio3dUsage(out);
        return convertIndex < 0 ? 0 : 2;
    }

    const QString filePath = QDir::fromNativeSeparators(arguments.at(convertIndex + 1));
    if (!QFileInfo::exists(filePath)) {
        err << "文件不存在：" << filePath << "\n";
        return 2;
    }

    const QString settingsPath = cliOptionValue(arguments,
                                                QStringLiteral("--settings"),
                                                defaultSettingsPath());
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("mainWidget"));

    VesselProjectionParams params;
    params.ascanLen = cliIntOption(arguments,
                                   QStringLiteral("--ascan-len"),
                                   settings.value(QStringLiteral("AscanLen"), 640).toInt());
    params.bscanLen = cliIntOption(arguments,
                                   QStringLiteral("--bscan-len"),
                                   settings.value(QStringLiteral("BscanLength"), 800).toInt());
    params.cscanLen = cliIntOption(arguments,
                                   QStringLiteral("--cscan-len"),
                                   settings.value(QStringLiteral("Bscanlines"), 800).toInt());
    params.angioRep = cliIntOption(arguments,
                                   QStringLiteral("--angio-rep"),
                                   settings.value(QStringLiteral("AngioRep"), 4).toInt());
    params.adFileOffsetFrames = cliIntOption(
        arguments,
        QStringLiteral("--ad-file-offset-frames"),
        settings.value(QStringLiteral("adFileOffsetFrames"), 0).toInt());
    params.cropZStart = cliIntOption(arguments,
                                     QStringLiteral("--crop-z-start"),
                                     settings.value(QStringLiteral("convertMin"), 10).toInt());
    params.cropZEnd = cliIntOption(arguments,
                                   QStringLiteral("--crop-z-end"),
                                   settings.value(QStringLiteral("convertMax"), 410).toInt());
    params.previewDepth = params.cropZEnd;
    params.projectionDepth = cliIntOption(
        arguments,
        QStringLiteral("--projection-depth"),
        settings.value(QStringLiteral("projectionDepth"), 200).toInt());
    settings.endGroup();

    params.generatePreviewImage = hasCliOption(arguments, QStringLiteral("--generate-preview"));
    params.generateGrayscaleImage = hasCliOption(arguments, QStringLiteral("--generate-grayscale"));
    params.generatePixelWiseFlowSpeedImage =
        !hasCliOption(arguments, QStringLiteral("--no-flow-pixelwise"));
    params.generateAveragedFlowSpeedImage =
        !hasCliOption(arguments, QStringLiteral("--no-flow-avg"));
    params.generateSegmentWiseFlowSpeedImage =
        !hasCliOption(arguments, QStringLiteral("--no-flow-segmented"));
    params.generateFlowSpeedFitCorrelationImage =
        hasCliOption(arguments, QStringLiteral("--flow-fit-correlation"));
    params.useFlowSpeedSkeletonDenoise =
        hasCliOption(arguments, QStringLiteral("--flow-skeleton"));
    params.useFlowSpeedManualMask =
        hasCliOption(arguments, QStringLiteral("--flow-manual"));
    params.flowSpeedCropTop = cliIntOption(arguments, QStringLiteral("--flow-crop-top"), 0);
    params.flowSpeedCropBottom = cliIntOption(arguments, QStringLiteral("--flow-crop-bottom"), 0);
    params.flowSpeedCropLeft = cliIntOption(arguments, QStringLiteral("--flow-crop-left"), 0);
    params.flowSpeedCropRight = cliIntOption(arguments, QStringLiteral("--flow-crop-right"), 0);

    out << "命令行转换 3D Angio 文件：" << filePath << "\n"
        << "参数：AscanLen=" << params.ascanLen
        << ", BscanLength=" << params.bscanLen
        << ", CscanLen=" << params.cscanLen
        << ", AngioRep=" << params.angioRep
        << ", cropZ=" << params.cropZStart << ".." << params.cropZEnd
        << ", projectionDepth=" << params.projectionDepth << "\n"
        << "settings.ini：" << settingsPath << "\n";

    QString errorMessage;
    const bool ok = convertAngio3dToColorProjection(
        filePath,
        params,
        [&out](const QString &message) {
            out << message << "\n";
            out.flush();
        },
        [&out](const QString &message) {
            out << message << "\n";
            out.flush();
        },
        [&out](const VesselProjectionFileSizeInfo &sizeInfo) {
            out << "3D Angio 文件大小和程序要求不匹配，命令行诊断模式将使用推断的 Cscan 长度："
                << sizeInfo.inferredCscanLen << "。\n";
            return true;
        },
        &errorMessage);
    if (!ok) {
        err << "转换失败：" << errorMessage << "\n";
        return 1;
    }

    out << "转换完成。\n";
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));   // 防止文本出现乱码
    QApplication a(argc, argv);

    // 设置全局翻译
    QTranslator qtBaseTranslator;
    const QString externalQtBaseTranslation =
        QApplication::applicationDirPath() + QStringLiteral("/translations/qtbase_zh_CN.qm");
    if (qtBaseTranslator.load(externalQtBaseTranslation) ||
        qtBaseTranslator.load(QStringLiteral(":/translations/qtbase_zh_CN.qm"))) {
        a.installTranslator(&qtBaseTranslator);
    }

    QFont font("Arial");          // 设置全局字体
    font.setPointSize(10);        // 设置全局字号（可改）
    a.setFont(font);              // 应用到整个程序

    const QStringList arguments = a.arguments();
    if (hasCliOption(arguments, QStringLiteral("--convert-angio3d")) ||
        hasCliOption(arguments, QStringLiteral("--help"))) {
        return runConvertAngio3dCli(arguments);
    }

    if (!showStartupDeviceDialog())
        return 0;

    mainWidget w;
    w.setStyleSheet("mainWidget { background-color: black; }"
                    "QLabel {background-color: rgb(25,40,50);}");
    w.show();

    QFile styleFile(":\\sty\\style.qss");
        if(styleFile.open(QIODevice::ReadOnly))
        {
            qDebug("main(): 打开样式表成功！");
            QString setStyleSheet(styleFile.readAll());
            a.setStyleSheet(setStyleSheet);
            styleFile.close();
        }
        else
        {
            qDebug("main(): 打开样式表失败！");
        }

    return a.exec();
}
