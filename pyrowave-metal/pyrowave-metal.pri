# PyroWave's native Metal port (Apple7+ GPUs), vendored with the Vulkan library
# in pyrowave/pyrowave/metal and built under the names in pyrowave_metal_names.h
# so both link into the macOS client. It needs ARC, which the client's
# Objective-C++ does not use, so it is always built as a library of its own:
# pyrowave-metal.pro for the client, tests/pyrowave/pyrowavemetallib.pro for the
# tests. The shaders are embedded MSL source compiled at runtime.

# Every source here is the Metal port, so ARC goes on the C++ flags as well
# (clang ignores it for plain C++). The .mm wrappers fail to build without it.
QMAKE_OBJECTIVE_CFLAGS += -fobjc-arc
QMAKE_CXXFLAGS += -fobjc-arc -fvisibility=hidden -fvisibility-inlines-hidden

SOURCES += \
    $$PWD/pyrowave_metal_common.mm \
    $$PWD/pyrowave_metal_decoder.mm \
    $$PWD/pyrowave_metal_encoder.mm \
    $$PWD/pyrowave_metal_bitstream.cpp

HEADERS += \
    $$PWD/pyrowave_metal_names.h \
    $$PWD/../pyrowave/pyrowave/metal/pyrowave_metal.h
