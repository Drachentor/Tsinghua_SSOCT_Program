#include <QObject>

namespace Ui {
class mainWidget;
}

#ifndef MAIN_WIDGET_UI_SETUP_H
#define MAIN_WIDGET_UI_SETUP_H

// 把 UI 的冗长设置全都搬到这里
// 如果出现问题，则直接把 .cpp 的代码复制回去就行
void mainWidgetUISetup(Ui::mainWidget* ui);

#endif
