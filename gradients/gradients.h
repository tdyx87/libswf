#pragma once
#include "datatype/datatype.h"
namespace swf {
struct GRADRECORD {
    uint8_t ratio;
    union {
        RGB color1;
        RGBA color2;
    } COLOR;
};
struct GRADIENT {
    uint8_t spread_mode;
    uint8_t interpolation_mode;
    uint8_t num_gradients;
    GRADRECORD *records;
};

struct FOCALGRADIENT {
    GRADIENT gradient;
    FIXED8 focal_point;
};

};  // namespace swf
