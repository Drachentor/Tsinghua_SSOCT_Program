#include "MainWidgetUISetup.h"
#include "mainwidget.h"
#include "ui_mainwidget.h"
#include "qcolor.h"
#include <QComboBox>
#include <QDoubleValidator>
#include <QFont>
#include <QIntValidator>
#include <QListView>
#include <QRect>
#include <limits>

using namespace cv;

namespace Ui {
class mainWidget;
}

void mainWidgetUISetup(Ui::mainWidget* ui)
{

    // 设置日志文本框输出的字体和样式。
    const QFont logFont(QObject::tr("Arial"), 10);
    const QString logStyle = "QTextEdit{background-color:rgba(22,28,35,1); border:0px;color: rgb(255,255,255)}";
    ui->textEdit->setFont(logFont);
    ui->textEdit_temp->setFont(logFont);
    QFont ft;
    ft.setPointSize(20);
    // ui->SNR->setFont(ft);
    ui->spinBox->setRange(0, mainWidget::AscanLen / 2);
    ui->spinBox_2->setRange(0, mainWidget::AscanLen / 2);
    ui->spinBox_3->setRange(40, 130);
    ui->spinBox_3->setValue(60);
    ui->spinBox_4->setRange(40, 130);
    ui->spinBox_4->setValue(100);
    ui->spinBox_5->setRange(2, 20);
    ui->spinBox_5->setValue(3);
    ui->spinBox_6->setRange(2, 20);
    ui->spinBox_6->setValue(3);
    ui->SB_convert_min->setValue(10);
    ui->SB_convert_max->setValue(410);
    ui->SB_projectionDepth->setValue(200);

    ui->startButton->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/start.png);}");
    ui->startButton->setFixedSize(QSize(66, 70));
    ui->stopButton->setEnabled(FALSE);
    ui->stopButton->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/stopFinish.png);}");
    ui->stopButton->setFixedSize(QSize(63, 70));
    ui->saveButton->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/iconSave.png);}");
    ui->saveButton->setFixedSize(QSize(63, 70));
    ui->resetaxis->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/fit.png);}");
    ui->resetaxis->setFixedSize(QSize(63, 70));
    ui->background->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/background.png);}");
    ui->background->setFixedSize(QSize(63, 70));
    ui->connectButton->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/connect.png);}");
    ui->connectButton->setFixedSize(QSize(63, 70));
    ui->addWindow->setStyleSheet("QPushButton{border-image:url(:/new/prefix1/addWindow.png);}");
    ui->addWindow->setFixedSize(QSize(63, 70));

    ui->textEdit->setReadOnly(true);
    ui->textEdit_temp->setReadOnly(true);
    ui->textEdit->setStyleSheet(logStyle);
    ui->textEdit_temp->setStyleSheet(logStyle);

    ui->cosplot->setBackground(QColor(0, 0, 0));
    ui->cosplot->xAxis->setBasePen(QPen(Qt::white));
    ui->cosplot->xAxis->setTickPen(QPen(Qt::white));
    ui->cosplot->xAxis->setSubTickPen(QPen(Qt::white));
    ui->cosplot->yAxis->setBasePen(QPen(Qt::white));
    ui->cosplot->yAxis->setTickPen(QPen(Qt::white));
    ui->cosplot->yAxis->setSubTickPen(QPen(Qt::white));
    ui->cosplot->xAxis->setTickLabelColor(Qt::white);
    ui->cosplot->yAxis->setTickLabelColor(Qt::white);

    ui->fftplot->setBackground(QColor(0, 0, 0));
    ui->fftplot->xAxis->setBasePen(QPen(Qt::white));
    ui->fftplot->xAxis->setTickPen(QPen(Qt::white));
    ui->fftplot->xAxis->setSubTickPen(QPen(Qt::white));
    ui->fftplot->yAxis->setBasePen(QPen(Qt::white));
    ui->fftplot->yAxis->setTickPen(QPen(Qt::white));
    ui->fftplot->yAxis->setSubTickPen(QPen(Qt::white));
    ui->fftplot->xAxis->setTickLabelColor(Qt::white);
    ui->fftplot->yAxis->setTickLabelColor(Qt::white);

    // widget 背景色属性
    ui->tabWidget->setAttribute(Qt::WA_StyledBackground);

    // 标题栏红色背景，tab选中蓝色，未选中灰色
    ui->tabWidget->setStyleSheet("QTabWidget#tabWidget{background-color:rgba(22,28,35,1);border: 0;}\
                                     QTabBar::tab{background-color:rgba(22,28,35,1);color:rgb(219,219,219); font: 11pt; padding: 0px; height:30px;}\
                                     QTabBar::tab:first{width:54px;}\
                                     QTabBar::tab:middle{width:118px;}\
                                     QTabBar::tab:last{width:86px;}\
                                     QTabBar::tab::selected{background-color:rgb(32,40,50);color:rgb(255,255,255);font: 11pt;}\
                                     QTabWidget::tab-bar{background-color:rgb(32,40,50);border-width:0px;}");
    // 绿色背景
    ui->tab->setStyleSheet("QWidget#tab{"
                           "background-color:rgb(32,40,50);}");
    ui->tab_2->setStyleSheet("QWidget#tab_2{"
                              "background-color:rgb(32,40,50);}");
    ui->tab_3->setStyleSheet("QWidget#tab_3{"
                             "background-color:rgb(32,40,50);}");
    ui->tabWidget->setDocumentMode(true);

    // combobox样式
    /* 未下拉时，QComboBox的样式 */
    ui->comboBox->setView(new QListView());
    ui->comboBox->setStyleSheet("QComboBox {border: 1px solid gray;\
                                    border-radius:4px;\
                                    padding: 0px 0px 0px 10px;\
                                    color: rgba(55,55,55,1);\
                                    font: normal normal 13px;\
                                    background: transparent;\
                                    text-align: AlignHCenter;\
                                    color:rgb(240,240,240);\
                                    background-color: transparent;\
                                    border-image: url(:/new/prefix1/84.png);\
                                    height:25px;}\
                                QComboBox QAbstractItemView {outline: 0px solid gray;\
                                    border-radius:4px;\
                                    padding-top:10px;\
                                    padding-bottom:10px;\
                                    color:rgb(123,123,123);\
                                    border-image: url(:/new/prefix1/84.png);}\
                                QComboBox QAbstractItemView::item {min-height: 15px;}\
                                QComboBox QAbstractItemView::item:hover {color:rgb(255,255,255);}\
                                QComboBox QAbstractItemView::item:selected {color:rgb(255,255,255);\
                                border-image: url(:/new/prefix1/60.png);}");
    ui->comboBox_2->setView(new QListView());
    ui->comboBox_2->setStyleSheet("QComboBox {border: 1px solid gray;\
                                        border-radius:4px;\
                                        padding: 0px 0px 0px 10px;\
                                        color: rgba(55,55,55,1);\
                                        font: normal normal 13px;\
                                        background: transparent;\
                                        text-align: AlignHCenter;\
                                        color:rgb(240,240,240);\
                                        background-color: transparent;\
                                        border-image: url(:/new/prefix1/84.png);\
                                        height:25px;}\
                                    QComboBox QAbstractItemView {outline: 0px solid gray;\
                                        border-radius:4px;\
                                        padding-top:10px;\
                                        padding-bottom:10px;\
                                        color:rgb(123,123,123);\
                                        border-image: url(:/new/prefix1/84.png);}\
                                    QComboBox QAbstractItemView::item {min-height: 15px;}\
                                    QComboBox QAbstractItemView::item:hover {color:rgb(255,255,255);}\
                                    QComboBox QAbstractItemView::item:selected {color:rgb(255,255,255);\
                                    border-image: url(:/new/prefix1/60.png);}");
    ui->combo_triggerMode->setView(new QListView());
    ui->combo_triggerMode->setStyleSheet("QComboBox {border: 1px solid gray;\
                                        border-radius:4px;\
                                        padding: 0px 0px 0px 10px;\
                                        color: rgba(55,55,55,1);\
                                        font: normal normal 13px;\
                                        background: transparent;\
                                        text-align: AlignHCenter;\
                                        color:rgb(240,240,240);\
                                        background-color: transparent;\
                                        border-image: url(:/new/prefix1/84.png);\
                                        height:25px;}\
                                    QComboBox QAbstractItemView {outline: 0px solid gray;\
                                        border-radius:4px;\
                                        padding-top:10px;\
                                        padding-bottom:10px;\
                                        color:rgb(123,123,123);\
                                        border-image: url(:/new/prefix1/84.png);}\
                                    QComboBox QAbstractItemView::item {min-height: 15px;}\
                                    QComboBox QAbstractItemView::item:hover {color:rgb(255,255,255);}\
                                    QComboBox QAbstractItemView::item:selected {color:rgb(255,255,255);\
                                    border-image: url(:/new/prefix1/60.png);}");
    ui->combo_clockMode->setView(new QListView());
    ui->combo_clockMode->setStyleSheet(ui->combo_triggerMode->styleSheet());

    ui->label->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_2->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_3->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_4->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_5->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_6->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_7->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_8->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_9->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_10->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_11->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_12->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_13->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_14->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_15->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_16->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_17->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_18->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_19->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_20->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_21->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_22->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_23->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_24->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_25->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_26->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_27->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_28->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_29->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_30->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_31->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_32->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_33->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_34->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_35->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_sysParams->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_convert->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_convert_to->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label_projectionDepth->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);font: bold 14px;}");
    ui->label2D->setStyleSheet("background-color:black");
    ui->labelflow->setStyleSheet("background-color:black");
    ui->SNR_0->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);background-color:black;}");
    ui->SNR->setStyleSheet("QLabel{font-weight: bold;color: rgb(255,255,255);background-color:black;}");
    ui->spinBox->setStyleSheet("QSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}\
        QSpinBox:hover{background-color:rgb(35,35,35);}\
        QSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->spinBox_2->setStyleSheet("QSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}\
        QSpinBox:hover{background-color:rgb(35,35,35);}\
        QSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->spinBox_3->setStyleSheet("QSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:25;}\
        QSpinBox:hover{background-color:rgb(35,35,35);}\
        QSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->spinBox_4->setStyleSheet("QSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:25;}\
        QSpinBox:hover{background-color:rgb(35,35,35);}\
        QSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->spinBox_5->setStyleSheet("QSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:25;}\
        QSpinBox:hover{background-color:rgb(35,35,35);}\
        QSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->spinBox_6->setStyleSheet("QSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:25;}\
        QSpinBox:hover{background-color:rgb(35,35,35);}\
        QSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->SB_convert_min->setStyleSheet("QSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:25;}\
        QSpinBox:hover{background-color:rgb(35,35,35);}\
        QSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->SB_convert_max->setStyleSheet("QSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:25;}\
        QSpinBox:hover{background-color:rgb(35,35,35);}\
        QSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->SB_projectionDepth->setStyleSheet("QSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:25;}\
        QSpinBox:hover{background-color:rgb(35,35,35);}\
        QSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->doubleSpinBoxa1->setStyleSheet("QDoubleSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:25;}\
        QDoubleSpinBox:hover{background-color:rgb(35,35,35);}\
        QDoubleSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QDoubleSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QDoubleSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QDoubleSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QDoubleSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QDoubleSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->doubleSpinBoxa2->setStyleSheet("QDoubleSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:25;}\
        QDoubleSpinBox:hover{background-color:rgb(35,35,35);}\
        QDoubleSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QDoubleSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QDoubleSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QDoubleSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QDoubleSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QDoubleSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->doubleSpinBoxw0->setStyleSheet("QDoubleSpinBox {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:25;}\
        QDoubleSpinBox:hover{background-color:rgb(35,35,35);}\
        QDoubleSpinBox:up-Button{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right.png);width: 20px;height: 20px;}\
        QDoubleSpinBox:down-Button{subcontrol-origin:border;\
            subcontrol-position:left;\
            image: url(:/new/prefix1/left.png);width: 20px;height: 20px;}\
        QDoubleSpinBox:up-Button:hover{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QDoubleSpinBox:down-Button:hover{subcontrol-position:left;\
            image: url(:/new/prefix1/left_pressed.png);}\
        QDoubleSpinBox:up-Button:pressed{subcontrol-origin:border;\
            subcontrol-position:right;\
            image: url(:/new/prefix1/right_pressed.png);}\
        QDoubleSpinBox:down-Button:pressed{subcontrol-position:left;\
            image: url(:/Resource/spinbox/left_pressed.png);}");
    ui->spinBox->setAlignment(Qt::AlignHCenter);
    ui->spinBox_2->setAlignment(Qt::AlignHCenter);
    ui->spinBox_3->setAlignment(Qt::AlignHCenter);
    ui->spinBox_4->setAlignment(Qt::AlignHCenter);
    ui->spinBox_5->setAlignment(Qt::AlignHCenter);
    ui->spinBox_6->setAlignment(Qt::AlignHCenter);
    ui->SB_convert_min->setAlignment(Qt::AlignHCenter);
    ui->SB_convert_max->setAlignment(Qt::AlignHCenter);
    ui->SB_projectionDepth->setAlignment(Qt::AlignHCenter);
    ui->doubleSpinBoxa1->setAlignment(Qt::AlignHCenter);
    ui->doubleSpinBoxa2->setAlignment(Qt::AlignHCenter);
    // ui->amplitude->setAlignment(Qt::AlignHCenter);
    // ui->frameRate->setAlignment(Qt::AlignHCenter);
    // ui->Bscanlines->setAlignment(Qt::AlignHCenter);
    // ui->dutycycle->setAlignment(Qt::AlignHCenter);
    ui->Line_continuousCount->setAlignment(Qt::AlignHCenter);
    ui->Line_realtimeShowInterval->setAlignment(Qt::AlignHCenter);
    ui->LE_AngioRep->setAlignment(Qt::AlignHCenter);
    ui->LE_AngioRep->setValidator(new QIntValidator(mainWidget::MinAngioRep,
                                                     mainWidget::MaxAngioRep,
                                                     ui->LE_AngioRep));
    ui->LE_AscanDutyCycle->setAlignment(Qt::AlignLeft);
    auto *ascanDutyValidator = new QDoubleValidator(0.0, 1.0, 6, ui->LE_AscanDutyCycle);
    ascanDutyValidator->setNotation(QDoubleValidator::StandardNotation);
    ui->LE_AscanDutyCycle->setValidator(ascanDutyValidator);
    ui->SB_triggerOffsetSamples->setAlignment(Qt::AlignHCenter);
    ui->SB_triggerOffsetSamples->setRange(-4096, 1000000);
    ui->SB_triggerOffsetSamples->setSingleStep(8);
    ui->SampleRate->setAlignment(Qt::AlignLeft);
    auto *sampleRateValidator = new QDoubleValidator(1.0, 10000.0, 6, ui->SampleRate);
    sampleRateValidator->setNotation(QDoubleValidator::StandardNotation);
    ui->SampleRate->setValidator(sampleRateValidator);
    ui->LE_BscanCycleLen->setAlignment(Qt::AlignLeft);
    ui->LE_BscanCycleLen->setValidator(new QIntValidator(1,
                                                         (std::numeric_limits<int>::max)(),
                                                         ui->LE_BscanCycleLen));
    ui->LE_NIDeviceName->setAlignment(Qt::AlignLeft);
    auto populateNiSourceCombo = [](QComboBox *combo) {
        combo->clear();
        combo->addItem(QStringLiteral("Internal"));
        for (int i = 0; i <= 15; ++i)
            combo->addItem(QStringLiteral("PFI%1").arg(i));
    };
    populateNiSourceCombo(ui->combo_NISampleClockSource);
    populateNiSourceCombo(ui->combo_NIStartTriggerSource);
    ui->doubleSpinBoxw0->setAlignment(Qt::AlignHCenter);

    ui->amplitude->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->frameRate->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->Bscanlines->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->dutycycle->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->AscanFreq->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->LE_AscanLen->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->LE_AscanDutyCycle->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->SB_triggerOffsetSamples->setStyleSheet(ui->spinBox->styleSheet());
    ui->SampleRate->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->LE_BscanCycleLen->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->LE_AngioRep->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->LE_adFileOffsetFrames->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->BscanLength->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->Line_continuousCount->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->Line_realtimeShowInterval->setStyleSheet("QLineEdit {background-color:rgb(55,55,55);\
            font: 75 11pt;\
            color:rgb(240,240,240);\
            border-radius:4px; height:25px;\
            width:100;}");
    ui->LE_NIDeviceName->setStyleSheet(ui->LE_BscanCycleLen->styleSheet());
    ui->label_NIParams->setStyleSheet(ui->label_23->styleSheet());
    ui->label_PCIeParams->setStyleSheet(ui->label_NIParams->styleSheet());
    ui->label_NIDeviceName->setStyleSheet(ui->label_23->styleSheet());
    ui->label_NIAOChannels->setStyleSheet(ui->label_23->styleSheet());
    ui->label_NIAORangeVolts->setStyleSheet(ui->label_23->styleSheet());
    ui->label_NISampleClockSource->setStyleSheet(ui->label_23->styleSheet());
    ui->label_NIStartTrigger->setStyleSheet(ui->label_23->styleSheet());
    ui->label_NIAOChannelsValue->setStyleSheet("QLabel {color:rgb(240,240,240); font: 75 11pt;}");
    ui->label_NIAORangeVoltsValue->setStyleSheet("QLabel {color:rgb(240,240,240); font: 75 11pt;}");
    ui->combo_NISampleClockSource->setStyleSheet(ui->comboBox_2->styleSheet());
    ui->combo_NIStartTriggerSource->setStyleSheet(ui->comboBox_2->styleSheet());
    ui->CB_enableContinuousMode->setStyleSheet("QCheckBox {background-color:transparent;\
            font: bold 14px;\
            color:rgb(240,240,240);}\
            QCheckBox::indicator{width:16px;height:16px;}");
    ui->CB_3D_showReatimeData->setStyleSheet("QCheckBox {background-color:transparent;\
            font: bold 14px;\
            color:rgb(240,240,240);}\
            QCheckBox::indicator{width:16px;height:16px;}");
    ui->CB_fourierOnSaved->setStyleSheet("QCheckBox {background-color:transparent;\
            font: bold 14px;\
            color:rgb(240,240,240);}\
            QCheckBox::indicator{width:16px;height:16px;}");
    ui->CB_enableDAInSymphonic->setStyleSheet("QCheckBox {background-color:transparent;\
            font: bold 14px;\
            color:rgb(240,240,240);}\
            QCheckBox::indicator{width:16px;height:16px;}");
    ui->connectDA->setStyleSheet("QPushButton {background-color:rgb(55,55,55);\
                                font: 75 11pt;\
                                color:rgb(240,240,240);\
                                border-radius:4px; height:25px;\
                                width:100;\
                                font-weight: bold}\
                                QPushButton:hover{background-color:rgb(35,35,35);}");
    ui->change_sysParams->setStyleSheet("QPushButton {background-color:rgb(55,55,55);\
                                font: 75 11pt;\
                                color:rgb(240,240,240);\
                                border-radius:4px; height:25px;\
                                width:100;\
                                font-weight: bold}\
                                QPushButton:hover{background-color:rgb(35,35,35);}");
    ui->AutoDecideBscanLength->setStyleSheet("QPushButton {background-color:rgb(55,55,55);\
                                border-radius:4px;\
                                color:rgb(240,240,240)}\
                                QPushButton:hover{background-color:rgb(35,35,35);}");
    ui->AutoDecideAscanLength->setStyleSheet("QPushButton {background-color:rgb(55,55,55);\
                                border-radius:4px;\
                                color:rgb(240,240,240)}\
                                QPushButton:hover{background-color:rgb(35,35,35);}");
    ui->button_FFT->setStyleSheet("QPushButton {background-color:rgb(55,55,55);\
                                border-radius:4px;\
                                color:rgb(240,240,240)}\
                                QPushButton:hover{background-color:rgb(35,35,35);}");
    ui->V_ConvertAngioToImage->setStyleSheet("QPushButton {background-color:rgb(55,55,55);\
                                border-radius:4px;\
                                color:rgb(240,240,240)}\
                                QPushButton:hover{background-color:rgb(35,35,35);}");
    ui->V_ConvertImageToPath->setStyleSheet("QPushButton {background-color:rgb(55,55,55);\
                                border-radius:4px;\
                                color:rgb(240,240,240)}\
                                QPushButton:hover{background-color:rgb(35,35,35);}");

    // 绘图库的初始化：
    ui->cosplot->addGraph();
    //设置坐标轴标签名称
    //ui->cosplot->xAxis->setLabel("x");
    //ui->cosplot->yAxis->setLabel("y");

    //设置坐标轴显示范围,否则我们只能看到默认的范围
    ui->cosplot->xAxis->setRange(0,mainWidget::AscanLen);
    ui->cosplot->yAxis->setRange(-mythread::plotOffset,mythread::plotOffset);
    ui->cosplot->setSelectionRectMode(QCP::SelectionRectMode::srmZoom);
    ui->fftplot->addGraph();

    //设置坐标轴标签名称
    //ui->fftplot->xAxis->setLabel("x");
    //ui->fftplot->yAxis->setLabel("f");

    //设置坐标轴显示范围,否则我们只能看到默认的范围
    ui->fftplot->xAxis->setRange(1,mainWidget::AscanLen/2);
    ui->fftplot->yAxis->setRange(20,120);
    ui->fftplot->setSelectionRectMode(QCP::SelectionRectMode::srmZoom);
}
