TEMPLATE = app
TARGET = tst_pyrowavemetal
QT -= gui core
CONFIG += console c++17 warn_off
CONFIG -= app_bundle

# The Vulkan library, for the reference decoder (through MoltenVK when
# GRANITE_VULKAN_LIBRARY points at it; skipped without a Vulkan device)
include($$PWD/../../pyrowave/pyrowave.pri)

DEFINES += SDL_MAIN_HANDLED
INCLUDEPATH += \
    $$PWD/../../pyrowave-metal \
    $$PWD/../../libs/mac/include \
    $$PWD/../../libs/mac/include/SDL2 \
    $$PWD/../../moonlight-common-c/moonlight-common-c/src

SOURCES += \
    $$PWD/tst_pyrowavemetal.mm \
    $$PWD/pyrowavemetalencoder.cpp \
    $$PWD/../../app/streaming/video/pyrowave/pyrowavemetaldecoder.mm \
    $$PWD/../../app/streaming/video/pyrowave/pyrowaveframing.cpp
HEADERS += \
    $$PWD/pyrowavetestframes.h \
    $$PWD/pyrowavemetalencoder.h \
    $$PWD/../../app/streaming/video/pyrowave/pyrowaveframedecoder.h \
    $$PWD/../../app/streaming/video/pyrowave/pyrowavemetaldecoder.h \
    $$PWD/../../app/streaming/video/pyrowave/pyrowavemetaltarget.h

# The Metal port is built with ARC by pyrowavemetallib.pro, ordered before this
LIBS += -L$$OUT_PWD -lpyrowave-metal \
        -L$$PWD/../../libs/mac/lib -lSDL2 -lavutil.60 \
        -framework Metal -framework Foundation -framework IOSurface
PRE_TARGETDEPS += $$OUT_PWD/libpyrowave-metal.a
