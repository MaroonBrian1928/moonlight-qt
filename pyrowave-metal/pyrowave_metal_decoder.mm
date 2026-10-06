// Builds metal/pyrowave_decoder.mm under the names in pyrowave_metal_names.h
#if !__has_feature(objc_arc)
#error "The PyroWave Metal port must be built with -fobjc-arc"
#endif

#include "pyrowave_metal_names.h"
#include "../pyrowave/pyrowave/metal/pyrowave_decoder.mm"
