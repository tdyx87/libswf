#pragma once
#include "datatype/datatype.h"
#include "gradients/gradients.h"
class FILESTYLE {
public:
    FILESTYLE();
    ~FILESTYLE();

private:
    uint8_t type_;
    swf::RGB color1_;
    swf::RGBA color2_;
    swf::MATRIX gradient_matrix_;
    swf::GRADIENT gradient1_;
    swf::GRADIENT gradient2_;
    uint16_t bitmapid_;
    swf::MATRIX bitmap_matrix_;
};
template <class T>
class LINESTYLE {
public:
    LINESTYLE();
    ~LINESTYLE();

private:
    uint16_t width_;
    T color;
};

struct LINESTYLE2 {
    uint8_t width;
    uint8_t start_cap_style;
    uint8_t join_style;
    uint8_t has_fill_flag;
    uint8_t no_hscale_flag;
    uint8_t no_vscale_flag;
    uint8_t pixel_Ihinting_flag;
    uint8_t no_close;
    uint8_t end_cap_style;
    uint16_t miter_limit_factor;
    swf::RGBA color;
    FILESTYLE fill_type;
};
