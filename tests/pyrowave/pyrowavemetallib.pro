# PyroWave's native Metal port, built next to tst_pyrowavemetal (metal.pro)
TEMPLATE = lib
TARGET = pyrowave-metal
QT -= gui core
CONFIG += staticlib c++17 warn_off

include($$PWD/../../pyrowave-metal/pyrowave-metal.pri)
