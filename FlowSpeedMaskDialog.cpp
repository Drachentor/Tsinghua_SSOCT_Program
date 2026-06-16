#include "FlowSpeedMaskDialog.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace {

QImage grayscaleMatToImage(const cv::Mat &input)
{
    cv::Mat gray;
    if (input.empty()) {
        gray = cv::Mat(1, 1, CV_8U, cv::Scalar(0));
    } else if (input.type() == CV_8U) {
        gray = input;
    } else if (input.type() == CV_8UC3) {
        cv::cvtColor(input, gray, cv::COLOR_BGR2GRAY);
    } else if (input.type() == CV_8UC4) {
        cv::cvtColor(input, gray, cv::COLOR_BGRA2GRAY);
    } else {
        cv::normalize(input, gray, 0, 255, cv::NORM_MINMAX, CV_8U);
    }

    if (!gray.isContinuous()) {
        gray = gray.clone();
    }
    return QImage(gray.data,
                  gray.cols,
                  gray.rows,
                  static_cast<int>(gray.step),
                  QImage::Format_Grayscale8).copy();
}

cv::Mat normalizedMask(const cv::Mat &mask, const QSize &targetSize)
{
    const cv::Size cvTargetSize(targetSize.width(), targetSize.height());
    if (cvTargetSize.width <= 0 || cvTargetSize.height <= 0) {
        return cv::Mat();
    }

    cv::Mat mask8;
    if (mask.empty()) {
        mask8 = cv::Mat::zeros(cvTargetSize, CV_8U);
    } else if (mask.type() == CV_8U) {
        mask8 = mask;
    } else {
        mask.convertTo(mask8, CV_8U);
    }
    if (mask8.size() != cvTargetSize) {
        cv::resize(mask8, mask8, cvTargetSize, 0.0, 0.0, cv::INTER_NEAREST);
    }

    cv::Mat binary;
    cv::threshold(mask8, binary, 0, 255, cv::THRESH_BINARY);
    return binary;
}

} // namespace

FlowSpeedMaskCanvas::FlowSpeedMaskCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(640, 480);
}

void FlowSpeedMaskCanvas::setImages(const cv::Mat &baseGray, const cv::Mat &mask)
{
    m_baseImage = grayscaleMatToImage(baseGray);
    m_mask = normalizedMask(mask, m_baseImage.size());
    m_initialMask = m_mask.clone();
    update();
    notifyMaskChanged();
}

void FlowSpeedMaskCanvas::setBrushMode(BrushMode mode)
{
    m_brushMode = mode;
}

void FlowSpeedMaskCanvas::setBrushRadius(int radius)
{
    m_brushRadius = std::max(1, radius);
}

void FlowSpeedMaskCanvas::resetMask()
{
    if (!m_initialMask.empty()) {
        m_mask = m_initialMask.clone();
        update();
        notifyMaskChanged();
    }
}

void FlowSpeedMaskCanvas::clearMask()
{
    if (!m_mask.empty()) {
        m_mask.setTo(0);
        update();
        notifyMaskChanged();
    }
}

void FlowSpeedMaskCanvas::applyCrop(int top, int bottom, int left, int right)
{
    if (m_mask.empty()) {
        return;
    }

    const int cropTop = std::max(0, top);
    const int cropBottom = std::max(0, bottom);
    const int cropLeft = std::max(0, left);
    const int cropRight = std::max(0, right);
    const int width = m_mask.cols - cropLeft - cropRight;
    const int height = m_mask.rows - cropTop - cropBottom;
    if (width <= 0 || height <= 0) {
        return;
    }

    cv::Mat cropMask = cv::Mat::zeros(m_mask.size(), CV_8U);
    cropMask(cv::Rect(cropLeft, cropTop, width, height)).setTo(255);
    cv::bitwise_and(m_mask, cropMask, m_mask);
    update();
    notifyMaskChanged();
}

cv::Mat FlowSpeedMaskCanvas::mask() const
{
    return m_mask.clone();
}

int FlowSpeedMaskCanvas::maskPixelCount() const
{
    return m_mask.empty() ? 0 : cv::countNonZero(m_mask);
}

void FlowSpeedMaskCanvas::setMaskChangedCallback(const std::function<void()> &callback)
{
    m_maskChanged = callback;
}

void FlowSpeedMaskCanvas::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.fillRect(rect(), QColor(20, 20, 20));

    const QRect target = imageTargetRect();
    if (target.isEmpty() || m_baseImage.isNull()) {
        return;
    }

    painter.drawImage(target, m_baseImage);
    if (!m_mask.empty()) {
        QImage overlay(m_mask.cols, m_mask.rows, QImage::Format_ARGB32);
        overlay.fill(Qt::transparent);
        for (int y = 0; y < m_mask.rows; ++y) {
            const uint8_t *maskRow = m_mask.ptr<uint8_t>(y);
            QRgb *overlayRow = reinterpret_cast<QRgb *>(overlay.scanLine(y));
            for (int x = 0; x < m_mask.cols; ++x) {
                if (maskRow[x] != 0) {
                    overlayRow[x] = qRgba(30, 220, 80, 95);
                }
            }
        }
        painter.drawImage(target, overlay);
    }

    painter.setPen(QPen(QColor(220, 220, 220), 1));
    painter.drawRect(target.adjusted(0, 0, -1, -1));
}

void FlowSpeedMaskCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        drawAt(event->pos());
    }
}

void FlowSpeedMaskCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
        drawAt(event->pos());
    }
}

void FlowSpeedMaskCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}

QRect FlowSpeedMaskCanvas::imageTargetRect() const
{
    if (m_baseImage.isNull() || width() <= 0 || height() <= 0) {
        return QRect();
    }

    const double scale = std::min(static_cast<double>(width()) / m_baseImage.width(),
                                  static_cast<double>(height()) / m_baseImage.height());
    const int targetWidth = std::max(1, static_cast<int>(m_baseImage.width() * scale));
    const int targetHeight = std::max(1, static_cast<int>(m_baseImage.height() * scale));
    return QRect((width() - targetWidth) / 2,
                 (height() - targetHeight) / 2,
                 targetWidth,
                 targetHeight);
}

bool FlowSpeedMaskCanvas::imagePointFromWidget(const QPoint &widgetPoint, cv::Point *imagePoint) const
{
    if (imagePoint == nullptr || m_baseImage.isNull()) {
        return false;
    }

    const QRect target = imageTargetRect();
    if (!target.contains(widgetPoint)) {
        return false;
    }

    const double x = static_cast<double>(widgetPoint.x() - target.left()) *
        static_cast<double>(m_baseImage.width()) / static_cast<double>(target.width());
    const double y = static_cast<double>(widgetPoint.y() - target.top()) *
        static_cast<double>(m_baseImage.height()) / static_cast<double>(target.height());
    imagePoint->x = std::min(m_baseImage.width() - 1, std::max(0, static_cast<int>(x)));
    imagePoint->y = std::min(m_baseImage.height() - 1, std::max(0, static_cast<int>(y)));
    return true;
}

void FlowSpeedMaskCanvas::drawAt(const QPoint &widgetPoint)
{
    if (m_mask.empty()) {
        return;
    }

    cv::Point imagePoint;
    if (!imagePointFromWidget(widgetPoint, &imagePoint)) {
        return;
    }

    const int value = m_brushMode == BrushMode::Keep ? 255 : 0;
    cv::circle(m_mask,
               imagePoint,
               std::max(1, m_brushRadius),
               cv::Scalar(value),
               cv::FILLED,
               cv::LINE_8);
    update();
    notifyMaskChanged();
}

void FlowSpeedMaskCanvas::notifyMaskChanged()
{
    if (m_maskChanged) {
        m_maskChanged();
    }
}

FlowSpeedMaskDialog::FlowSpeedMaskDialog(const cv::Mat &baseGray,
                                         const cv::Mat &initialMask,
                                         QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("手动编辑速度图掩膜"));
    resize(900, 720);

    m_canvas = new FlowSpeedMaskCanvas(this);
    m_canvas->setImages(baseGray, initialMask);

    m_statsLabel = new QLabel(this);
    m_eraseButton = new QRadioButton(QStringLiteral("擦除"), this);
    m_keepButton = new QRadioButton(QStringLiteral("保留"), this);
    m_eraseButton->setChecked(true);

    m_brushRadiusSpinBox = new QSpinBox(this);
    m_brushRadiusSpinBox->setRange(1, 80);
    m_brushRadiusSpinBox->setValue(8);
    m_brushRadiusSpinBox->setMaximumWidth(80);

    m_cropTopSpinBox = new QSpinBox(this);
    m_cropBottomSpinBox = new QSpinBox(this);
    m_cropLeftSpinBox = new QSpinBox(this);
    m_cropRightSpinBox = new QSpinBox(this);
    for (QSpinBox *spinBox : {m_cropTopSpinBox, m_cropBottomSpinBox,
                              m_cropLeftSpinBox, m_cropRightSpinBox}) {
        spinBox->setRange(0, 100000);
        spinBox->setValue(0);
        spinBox->setMaximumWidth(80);
    }

    QPushButton *applyCropButton = new QPushButton(QStringLiteral("应用裁剪"), this);
    QPushButton *resetButton = new QPushButton(QStringLiteral("恢复自动掩膜"), this);
    QPushButton *clearButton = new QPushButton(QStringLiteral("清空掩膜"), this);
    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                                     QDialogButtonBox::Cancel,
                                                     this);

    QHBoxLayout *modeLayout = new QHBoxLayout;
    modeLayout->addWidget(new QLabel(QStringLiteral("画笔"), this));
    modeLayout->addWidget(m_eraseButton);
    modeLayout->addWidget(m_keepButton);
    modeLayout->addWidget(new QLabel(QStringLiteral("半径"), this));
    modeLayout->addWidget(m_brushRadiusSpinBox);
    modeLayout->addStretch(1);
    modeLayout->addWidget(m_statsLabel);

    QGridLayout *cropLayout = new QGridLayout;
    cropLayout->addWidget(new QLabel(QStringLiteral("裁剪"), this), 0, 0);
    cropLayout->addWidget(new QLabel(QStringLiteral("上"), this), 0, 1);
    cropLayout->addWidget(m_cropTopSpinBox, 0, 2);
    cropLayout->addWidget(new QLabel(QStringLiteral("下"), this), 0, 3);
    cropLayout->addWidget(m_cropBottomSpinBox, 0, 4);
    cropLayout->addWidget(new QLabel(QStringLiteral("左"), this), 0, 5);
    cropLayout->addWidget(m_cropLeftSpinBox, 0, 6);
    cropLayout->addWidget(new QLabel(QStringLiteral("右"), this), 0, 7);
    cropLayout->addWidget(m_cropRightSpinBox, 0, 8);
    cropLayout->addWidget(applyCropButton, 0, 9);
    cropLayout->addWidget(resetButton, 0, 10);
    cropLayout->addWidget(clearButton, 0, 11);
    cropLayout->setColumnStretch(12, 1);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(modeLayout);
    mainLayout->addWidget(m_canvas, 1);
    mainLayout->addLayout(cropLayout);
    mainLayout->addWidget(buttons);

    connect(m_eraseButton, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            m_canvas->setBrushMode(FlowSpeedMaskCanvas::BrushMode::Erase);
        }
    });
    connect(m_keepButton, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            m_canvas->setBrushMode(FlowSpeedMaskCanvas::BrushMode::Keep);
        }
    });
    connect(m_brushRadiusSpinBox,
            QOverload<int>::of(&QSpinBox::valueChanged),
            m_canvas,
            &FlowSpeedMaskCanvas::setBrushRadius);
    connect(applyCropButton, &QPushButton::clicked, this, [this]() {
        m_canvas->applyCrop(m_cropTopSpinBox->value(),
                            m_cropBottomSpinBox->value(),
                            m_cropLeftSpinBox->value(),
                            m_cropRightSpinBox->value());
    });
    connect(resetButton, &QPushButton::clicked, m_canvas, &FlowSpeedMaskCanvas::resetMask);
    connect(clearButton, &QPushButton::clicked, m_canvas, &FlowSpeedMaskCanvas::clearMask);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_canvas->setMaskChangedCallback([this]() {
        updateStats();
    });
    updateStats();
}

cv::Mat FlowSpeedMaskDialog::mask() const
{
    return m_canvas ? m_canvas->mask() : cv::Mat();
}

void FlowSpeedMaskDialog::updateStats()
{
    if (m_statsLabel == nullptr || m_canvas == nullptr) {
        return;
    }

    const cv::Mat currentMask = m_canvas->mask();
    const int pixels = m_canvas->maskPixelCount();
    const int total = currentMask.empty() ? 0 : currentMask.rows * currentMask.cols;
    const double percent = total > 0 ? 100.0 * static_cast<double>(pixels) /
        static_cast<double>(total) : 0.0;
    m_statsLabel->setText(QStringLiteral("掩膜：%1 像素（%2%）")
                          .arg(pixels)
                          .arg(percent, 0, 'f', 2));
}
