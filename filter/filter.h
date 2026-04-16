#pragma once
#include "datatype/datatype.h"
namespace swf {

class FILTER {
public:
    FILTER();
    ~FILTER();

private:
    uint8_t filter_id_;
};

class COLORMATRIXFILTER : public FILTER {
public:
    COLORMATRIXFILTER();
    ~COLORMATRIXFILTER();

private:
    FLOAT matrix[20];
};

class CONVOLUTIONFILTER : public FILTER {
public:
    CONVOLUTIONFILTER();
    ~CONVOLUTIONFILTER();

private:
    uint8_t matrixx_;
    uint8_t matrixy_;
    FLOAT divisor_;
    FLOAT bias_;
    FLOAT *matrix_;
    RGBA defaultcolor_;
    uint8_t clamp_;
    uint8_t preserve_alpha_;
};

class BLURFILTER : public FILTER {
public:
    BLURFILTER();
    ~BLURFILTER();

private:
    FIXED blurx_;
    FIXED blury_;
    uint8_t passes_;
};

class DROPSHADOWFILTER : public FILTER {
public:
    DROPSHADOWFILTER();
    ~DROPSHADOWFILTER();

private:
    RGBA drop_shadow_color_;
    FIXED blurx_;
    FIXED blury_;
    FIXED angle_;
    FIXED distance_;
    FIXED8 strength_;
    uint8_t inner_shadow_;
    uint8_t knock_out_;
    uint8_t composite_source_;
    uint8_t passes_;
};

class GLOWFILTER : public FILTER {
public:
    GLOWFILTER();
    ~GLOWFILTER();

private:
    RGBA glow_color_;
    FIXED blurx_;
    FIXED blury_;
    FIXED8 strenth_;
    uint8_t inner_glow_;
    uint8_t knock_out_;
    uint8_t composite_source_;
    uint8_t passes_;
};

class BEVELFILTER : public FILTER {
public:
    BEVELFILTER();
    ~BEVELFILTER();

private:
    RGBA shadow_color_;
    RGBA highlight_color_;
    FIXED blurx_;
    FIXED blury_;
    FIXED angle_;
    FIXED distance_;
    FIXED8 strength_;
    uint8_t inner_shadow_;
    uint8_t knock_out_;
    uint8_t composite_source_;
    uint8_t on_top_;
    uint8_t passes_;
};

class GRADIENTGLOWFILTER : public FILTER {
public:
    GRADIENTGLOWFILTER();
    ~GRADIENTGLOWFILTER();

private:
    std::vector<RGBA> gradient_colors_;
    std::vector<uint8_t> gradient_ratio_;
    FIXED blurx_;
    FIXED blury_;
    FIXED angle_;
    FIXED distance_;
    FIXED8 strength_;
    uint8_t inner_shadow_;
    uint8_t knock_out_;
    uint8_t composite_source_;
    uint8_t on_top_;
    uint8_t passes_;
};
}  // namespace swf