#-------------------------------------------------
#
# Project created by QtCreator 2021-11-12T10:46:25
#
#-------------------------------------------------


QT       += core gui
QMAKE_PROJECT_DEPTH = 0

# 配置绘图库
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets printsupport
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = Tsinghua_SSOCT
TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
# DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

CONFIG += c++11
msvc {
    QMAKE_CXXFLAGS += /utf-8
    QMAKE_CXXFLAGS += /FI\"$$PWD/msvc_qt5_checked_iterator_compat.h\"
}
# 若出现大量 "编码无法保存" 的错误，则需要把受到影响的文件用 UTF-8 进行保存！

SOURCES += \
        FlowSpeedCalculation.cpp \
        FlowSpeedMaskDialog.cpp \
        VesselAudioPlayer.cpp \
        VesselFindingDialog.cpp \
        main.cpp \
        mainwidget.cpp \
        mythread.cpp \
        qcustomplot.cpp \
        MainWidgetUISetup.cpp \
        VesselFindingDialogUISetup.cpp \
        VesselFlowShared.cpp \
        VesselSegmenter.cpp \
        VesselProjectionProcessor.cpp

HEADERS += \
        AppVersion.h \
        FlowSpeedCalculation.h \
        FlowSpeedMaskDialog.h \
        VesselAudioPlayer.h \
        VesselFindingDialog.h \
        include/AlazarApi.h \
        include/AlazarCmd.h \
        include/AlazarDSP.h \
        include/AlazarError.h \
        include/AlazarRC.h \
        include/PCIe3628.h \
        include/PCIe3640.h \
        include/Usb3020.h \
        mainwidget.h \
        mythread.h \
        qcustomplot.h \
        MainWidgetUISetup.h \
        VesselFindingDialogUISetup.h \
        VesselFlowShared.h \
        VesselSegmenter.h \
        VesselProjectionProcessor.h \
        VesselProjectionCuda.h

FORMS += \
        VesselFindingDialog.ui \
        mainwidget.ui

# 配置硬件 SDK：商家头文件统一放在 include，库和运行时 DLL 统一放在 lib。
INCLUDEPATH += $$PWD/include
DEPENDPATH += $$PWD/include
LIBS += -L$$PWD/lib -lPCIe3640


# 配置opencv库
INCLUDEPATH +=D:\libsdk\opencv\build\include
              D:\libsdk\opencv\build\include\opencv2
CONFIG(debug, debug|release) {
    LIBS +=D:\libsdk\opencv\build\x64\vc15\lib\opencv_world454d.lib
} else {
    LIBS +=D:\libsdk\opencv\build\x64\vc15\lib\opencv_world454.lib
}

# 配置mkl库
INCLUDEPATH += D:\libsdk\mkl\include
LIBS +=D:\libsdk\mkl\lib\intel64_win\mkl_intel_lp64.lib
LIBS +=D:\libsdk\mkl\lib\intel64_win\mkl_intel_thread.lib
LIBS +=D:\libsdk\mkl\lib\intel64_win\mkl_core.lib
LIBS +=D:\libsdk\mkl\lib\intel64_win\libiomp5md.lib
# 配置alazar采集卡；之后可以删掉

# 配置 CUDA。若当前机器没有安装 CUDA Toolkit，则跳过 CUDA 后端，保留 OpenCL/CPU 路径。
CUDA_PATH = $$(CUDA_PATH)
!isEmpty(CUDA_PATH):exists($$CUDA_PATH/include/cuda_runtime.h) {
    DEFINES += VESSEL_USE_CUDA
    INCLUDEPATH += $$CUDA_PATH/include
    LIBS += -L$$CUDA_PATH/lib/x64 -lcudart -lcufft
    win32-msvc {
        QMAKE_LFLAGS += /DELAYLOAD:cufft64_12.dll
        LIBS += delayimp.lib
    }

    CUDA_SOURCES += VesselProjectionCuda.cu
    cuda.input = CUDA_SOURCES
    cuda.output = ${QMAKE_VAR_OBJECTS_DIR}${QMAKE_FILE_BASE}.obj
    CONFIG(debug, debug|release) {
        cuda.commands = \"$$CUDA_PATH/bin/nvcc.exe\" -c -std=c++14 -Xcompiler \"/MDd /Zi /EHsc /utf-8\" -I\"$$PWD\" -I\"$$CUDA_PATH/include\" -o ${QMAKE_FILE_OUT} ${QMAKE_FILE_NAME}
    } else {
        cuda.commands = \"$$CUDA_PATH/bin/nvcc.exe\" -c -std=c++14 -Xcompiler \"/MD /O2 /DNDEBUG /EHsc /utf-8\" -I\"$$PWD\" -I\"$$CUDA_PATH/include\" -o ${QMAKE_FILE_OUT} ${QMAKE_FILE_NAME}
    }
    cuda.dependency_type = TYPE_C
    cuda.variable_out = OBJECTS
    QMAKE_EXTRA_COMPILERS += cuda
}

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target


INCLUDEPATH += $$PWD/.
DEPENDPATH += $$PWD/.


RESOURCES += \
    qrc.qrc

# PortAudio: in-process WASAPI output for looping vessel scan drive signals.
PORTAUDIO_ROOT = C:/Windows/portaudio
INCLUDEPATH += $$PORTAUDIO_ROOT/include \
               $$PORTAUDIO_ROOT/src/common \
               $$PORTAUDIO_ROOT/src/os/win
DEFINES += PA_USE_WASAPI=1 \
           PA_USE_WMME=0 \
           PA_USE_DS=0 \
           PA_USE_ASIO=0 \
           PA_USE_WDMKS=0 \
           PA_USE_SKELETON=0 \
           _CRT_SECURE_NO_WARNINGS
SOURCES += \
    $$PORTAUDIO_ROOT/src/common/pa_allocation.c \
    $$PORTAUDIO_ROOT/src/common/pa_converters.c \
    $$PORTAUDIO_ROOT/src/common/pa_cpuload.c \
    $$PORTAUDIO_ROOT/src/common/pa_debugprint.c \
    $$PORTAUDIO_ROOT/src/common/pa_dither.c \
    $$PORTAUDIO_ROOT/src/common/pa_front.c \
    $$PORTAUDIO_ROOT/src/common/pa_process.c \
    $$PORTAUDIO_ROOT/src/common/pa_ringbuffer.c \
    $$PORTAUDIO_ROOT/src/common/pa_stream.c \
    $$PORTAUDIO_ROOT/src/common/pa_trace.c \
    $$PORTAUDIO_ROOT/src/hostapi/wasapi/pa_win_wasapi.c \
    $$PORTAUDIO_ROOT/src/os/win/pa_win_coinitialize.c \
    $$PORTAUDIO_ROOT/src/os/win/pa_win_hostapis.c \
    $$PORTAUDIO_ROOT/src/os/win/pa_win_util.c \
    $$PORTAUDIO_ROOT/src/os/win/pa_win_waveformat.c \
    $$PORTAUDIO_ROOT/src/os/win/pa_win_wdmks_utils.c \
    $$PORTAUDIO_ROOT/src/os/win/pa_x86_plain_converters.c
LIBS += ole32.lib uuid.lib winmm.lib
