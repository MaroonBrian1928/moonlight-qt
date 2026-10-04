TEMPLATE = subdirs
CONFIG += ordered

framing.file = $$PWD/framing.pro
SUBDIRS += framing

# The round trip compiles the vendored codec and needs a Vulkan GPU at runtime.
# Its CPU decode is the macOS client's system-memory fallback.
if(win32:contains(QT_ARCH, x86_64)|macx) {
    roundtrip.file = $$PWD/roundtrip.pro
    SUBDIRS += roundtrip
}

win32:contains(QT_ARCH, x86_64) {
    # The client's D3D11 surface pool and Vulkan decoder, without a host
    d3d11.file = $$PWD/d3d11.pro
    SUBDIRS += d3d11
}
