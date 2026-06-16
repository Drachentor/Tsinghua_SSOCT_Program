#ifndef VESSELFINDINGDIALOG_H
#define VESSELFINDINGDIALOG_H

#include <QPoint>
#include <QDialog>
#include <QString>
#include <QVector>
#include <opencv2/core/core.hpp>

namespace Ui {
class VesselFindingDialog;
}

class VesselImageCanvas;

class VesselFindingDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VesselFindingDialog(QWidget *parent = nullptr);
    explicit VesselFindingDialog(double voltage, QWidget *parent = nullptr);
    explicit VesselFindingDialog(double voltage, int ascanFreq, QWidget *parent = nullptr);
    explicit VesselFindingDialog(double voltage,
                                 int ascanFreq,
                                 int bscanLen,
                                 int bscanCycleLen,
                                 QWidget *parent = nullptr);
    ~VesselFindingDialog();

private slots:
    void on_button_fixSkeleton_clicked();
    void on_button_draw_clicked();
    void on_button_sel_redo_clicked();
    void on_button_sel_confirm_clicked();
    void on_button_mask_redo_clicked();
    void on_button_route_redo_clicked();
    void on_button_route_save_clicked();
    void on_button_show_reselect_clicked();
    void on_button_show_cancel_clicked();
    void on_button_show_confirm_clicked();

private:
    struct MaskParams;
    struct RouteParams;
    struct VesselResult;

    void setupCanvases();
    void setupTextsAndDefaults();
    void loadImageFromDialog();
    bool loadImage(const QString &filePath);
    bool rebuildMaskAndSkeleton(bool rebuildExistingRoute = false);
    bool rebuildRoute();
    void resetMaskParams();
    void resetRouteParams();
    void resetEndpointWeightParams();
    MaskParams maskParams() const;
    RouteParams routeParams() const;
    void showMaskViews();
    void showSelectionView();
    void showRouteView();
    void setSelectionActive(bool active);
    void setInteractionMode(int mode);
    void restoreFullSkeletonForSelection(const QString &reason, int nextMode);
    void handleSeedClick(const QPoint &imagePoint);
    void handleSkeletonFixClick(const QPoint &imagePoint);
    void handleDrawnPath(const QVector<QPoint> &path);
    void clearManualPath();
    void updatePathStats(const QVector<QPoint> &path);
    bool moveSeedToNearestSkeleton(const QPoint &preferredPoint, int maxDistance);
    void appendImageSizeLog();
    void updateStats();
    void appendLog(const QString &message);
    bool exportPathFile();
    bool exportWavPathFile(const QVector<QPoint> &path, const QString &dirPath);
    QVector<QPoint> orderedMainVesselPath() const;
    void loadSettings();
    void saveSettings(bool includeEndpointWeights = true) const;
    bool ensureDepthIndexMap();

    Ui::VesselFindingDialog *ui;
    VesselImageCanvas *m_selectionCanvas;
    VesselImageCanvas *m_maskCanvas;
    VesselImageCanvas *m_routeCanvas;

    QString m_inputImagePath;
    cv::Mat m_rgbImage;
    cv::Mat m_grayImage;
    cv::Mat m_initialVesselMask;
    cv::Mat m_fullVesselSkeleton;
    cv::Mat m_vesselSkeleton;
    cv::Mat m_depthRemovedSkeleton;
    cv::Mat m_mainVesselSkeleton;
    cv::Mat m_diameterMap;
    cv::Mat m_depthIndexMap;
    QString m_depthProjectionSource;
    QVector<QPoint> m_smoothedMainVesselPath;
    QVector<QPoint> m_manualVesselPath;

    int m_interactionMode;
    bool m_hasSeed;
    QPoint m_seedPoint;
    double m_maxDiameter;
    double m_meanDiameter;
    int m_mainVesselLen;
    double m_voltage;
    int m_ascanFreq;
    int m_bscanLen;
    int m_bscanCycleLen;
};

#endif // VESSELFINDINGDIALOG_H
