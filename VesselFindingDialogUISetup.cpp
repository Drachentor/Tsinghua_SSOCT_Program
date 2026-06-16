#include "VesselFindingDialogUISetup.h"

#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFont>
#include <QSpinBox>
#include <QTextEdit>

namespace {

const char *kVesselFindingDialogStyleSheet = R"(
QDialog#VesselFindingDialog {
    background-color: rgb(32,40,50);
}

QWidget#horizontalLayoutWidget_2 {
    background-color: rgb(32,40,50);
}

QWidget#widget_sel,
QWidget#widget_mask,
QWidget#widget_path {
    background-color: black;
}

QLabel {
    font-weight: bold;
    color: rgb(255,255,255);
    font: bold 14px;
    background-color: transparent;
}

QCheckBox#CB_depthDependency,
QCheckBox#CB_noReturn,
QCheckBox#CB_generateWAV {
    background-color: transparent;
    color: rgb(255,255,255);
    font: bold 14px;
    spacing: 6px;
    min-height: 25px;
}

QCheckBox#CB_depthDependency::indicator,
QCheckBox#CB_noReturn::indicator,
QCheckBox#CB_generateWAV::indicator {
    width: 15px;
    height: 15px;
    border-radius: 3px;
    border: 1px solid rgb(160,170,180);
    background-color: rgb(55,55,55);
}

QCheckBox#CB_depthDependency::indicator:hover,
QCheckBox#CB_noReturn::indicator:hover,
QCheckBox#CB_generateWAV::indicator:hover {
    background-color: rgb(35,35,35);
}

QCheckBox#CB_depthDependency::indicator:checked,
QCheckBox#CB_noReturn::indicator:checked,
QCheckBox#CB_generateWAV::indicator:checked {
    background-color: rgb(133,238,255);
    border: 1px solid rgb(133,238,255);
}

QSpinBox,
QDoubleSpinBox {
    background-color: rgb(55,55,55);
    font: 75 11pt;
    color: rgb(240,240,240);
    border-radius: 4px;
    height: 25px;
    min-width: 55px;
}

QSpinBox#SB_depthDependencyRange {
    min-width: 48px;
    max-width: 62px;
}

QSpinBox#SB_depthDependencyRange:disabled {
    color: rgb(150,150,150);
    background-color: rgb(45,45,45);
}

QSpinBox:hover,
QDoubleSpinBox:hover {
    background-color: rgb(35,35,35);
}

QSpinBox::up-button,
QDoubleSpinBox::up-button {
    subcontrol-origin: border;
    subcontrol-position: right;
    image: url(:/new/prefix1/right.png);
    width: 20px;
    height: 20px;
}

QSpinBox::down-button,
QDoubleSpinBox::down-button {
    subcontrol-origin: border;
    subcontrol-position: left;
    image: url(:/new/prefix1/left.png);
    width: 20px;
    height: 20px;
}

QSpinBox::up-button:hover,
QSpinBox::up-button:pressed,
QDoubleSpinBox::up-button:hover,
QDoubleSpinBox::up-button:pressed {
    subcontrol-origin: border;
    subcontrol-position: right;
    image: url(:/new/prefix1/right_pressed.png);
}

QSpinBox::down-button:hover,
QSpinBox::down-button:pressed,
QDoubleSpinBox::down-button:hover,
QDoubleSpinBox::down-button:pressed {
    subcontrol-origin: border;
    subcontrol-position: left;
    image: url(:/new/prefix1/left_pressed.png);
}

QPushButton {
    background-color: rgb(55,55,55);
    font: 75 11pt;
    color: rgb(240,240,240);
    border-radius: 4px;
    height: 25px;
    font-weight: bold;
}

QPushButton:hover {
    background-color: rgb(35,35,35);
}

QPushButton:pressed {
    background-color: rgb(25,25,25);
}

QPushButton#button_draw:checked,
QPushButton#button_fixSkeleton:checked {
    background-color: rgb(38,112,132);
    color: rgb(255,255,255);
    border: 1px solid rgb(133,238,255);
}

QTextEdit#textEdit_sel {
    background-color: rgba(22,28,35,1);
    border: 0px;
    color: rgb(255,255,255);
}
)";

} // namespace

void vesselFindingDialogUISetup(QDialog *dialog)
{
    if (!dialog) {
        return;
    }

    dialog->setAttribute(Qt::WA_StyledBackground);
    dialog->setStyleSheet(kVesselFindingDialogStyleSheet);

    for (QSpinBox *spinBox : dialog->findChildren<QSpinBox *>()) {
        spinBox->setAlignment(Qt::AlignHCenter);
    }

    for (QCheckBox *checkBox : dialog->findChildren<QCheckBox *>()) {
        checkBox->setCursor(Qt::PointingHandCursor);
    }

    for (QDoubleSpinBox *doubleSpinBox : dialog->findChildren<QDoubleSpinBox *>()) {
        doubleSpinBox->setAlignment(Qt::AlignHCenter);
    }

    QTextEdit *textEditSel = dialog->findChild<QTextEdit *>("textEdit_sel");
    if (textEditSel) {
        textEditSel->setFont(QFont("Arial", 10));
        textEditSel->setReadOnly(true);
    }
}
