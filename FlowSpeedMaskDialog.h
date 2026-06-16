#ifndef FLOWSPEEDMASKDIALOG_H
#define FLOWSPEEDMASKDIALOG_H

#include <QDialog>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>

#include <opencv2/core.hpp>

#include <functional>

class QLabel;
class QMouseEvent;
class QPaintEvent;
class QRadioButton;
class QResizeEvent;
class QSpinBox;

class FlowSpeedMaskCanvas : public QWidget
{
public:
    enum class BrushMode
    {
        Keep,
        Erase
    };

    explicit FlowSpeedMaskCanvas(QWidget *parent = nullptr);

    void setImages(const cv::Mat &baseGray, const cv::Mat &mask);
    void setBrushMode(BrushMode mode);
    void setBrushRadius(int radius);
    void resetMask();
    void clearMask();
    void applyCrop(int top, int bottom, int left, int right);
    cv::Mat mask() const;
    int maskPixelCount() const;
    void setMaskChangedCallback(const std::function<void()> &callback);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QRect imageTargetRect() const;
    bool imagePointFromWidget(const QPoint &widgetPoint, cv::Point *imagePoint) const;
    void drawAt(const QPoint &widgetPoint);
    void notifyMaskChanged();

    QImage m_baseImage;
    cv::Mat m_mask;
    cv::Mat m_initialMask;
    BrushMode m_brushMode = BrushMode::Erase;
    int m_brushRadius = 8;
    std::function<void()> m_maskChanged;
};

class FlowSpeedMaskDialog : public QDialog
{
public:
    explicit FlowSpeedMaskDialog(const cv::Mat &baseGray,
                                 const cv::Mat &initialMask,
                                 QWidget *parent = nullptr);

    cv::Mat mask() const;

private:
    void updateStats();

    FlowSpeedMaskCanvas *m_canvas = nullptr;
    QLabel *m_statsLabel = nullptr;
    QRadioButton *m_eraseButton = nullptr;
    QRadioButton *m_keepButton = nullptr;
    QSpinBox *m_brushRadiusSpinBox = nullptr;
    QSpinBox *m_cropTopSpinBox = nullptr;
    QSpinBox *m_cropBottomSpinBox = nullptr;
    QSpinBox *m_cropLeftSpinBox = nullptr;
    QSpinBox *m_cropRightSpinBox = nullptr;
};

#endif // FLOWSPEEDMASKDIALOG_H
