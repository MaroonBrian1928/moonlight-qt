# PyroWave's native Metal port for the macOS client (see pyrowave-metal.pri)

QT -= core gui

TARGET = pyrowave-metal
TEMPLATE = lib

# Build a static library
CONFIG += staticlib c++17

# Third-party code: keep its warnings out of our build logs
CONFIG += warn_off

# Include global qmake defs
include(../globaldefs.pri)

include(pyrowave-metal.pri)
