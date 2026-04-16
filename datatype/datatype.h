#pragma once
#include <stdint.h>
#include <vector>
namespace swf {
using TWIPS = uint32_t;
using UI8 = uint8_t;
using SI8 = int8_t;
using UI16 = uint16_t;
using SI16 = int16_t;
using UI32 = uint32_t;
using SI32 = int32_t;
using FIXED = float;
using FIXED8 = float;
using FLOAT16 = float;
using FLOAT = float;
using DOUBLE = double;
using EncodedU32 = int32_t;
using SI8N = std::vector<SI8>;
using SI16N = std::vector<SI16>;
using UI8N = std::vector<UI8>;
using UI16N = std::vector<UI16>;
using UI32N = std::vector<UI32>;
using UI64N = std::vector<uint64_t>;
using UI24N = std::vector<UI32>;
struct RECT {
    SI32 xmin;
    SI32 xmax;
    SI32 ymin;
    SI32 ymax;
};

struct MATRIX {
    bool hasscale;
    FIXED scalex;
    FIXED scaley;
    bool hasrotate;
    FIXED rotateskew0;
    FIXED rotateskew1;
    SI32 translatex;
    SI32 translatey;
};
struct CXFORM {
    bool hasaddterms;
    bool hasmultterms;
    SI16 redmultterm;
    SI16 greenmultterm;
    SI16 bluemultterm;
    SI16 redaddterm;
    SI16 greenaddterm;
    SI16 blueaddterm;
};

struct CXFORMWITHALPHA {
    bool hasaddterms;
    bool hasmultterms;
    SI16 redmultterm;
    SI16 greenmultterm;
    SI16 bluemultterm;
    SI16 alphamultterm;
    SI16 redaddterm;
    SI16 greenaddterm;
    SI16 blueaddterm;
    SI16 alphaaddterm;
};

struct RGB {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct RGBA {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

#define HASCLIPACTIONS 0x01
#define HASCLIPDEPTH 0x02
#define HASNAME 0x04
#define HASRATIO 0x08
#define HASCOLORTRANSFORM 0x10
#define HASMATRIX 0x20
#define HASCHARACTER 0x40
#define HASMOVE 0x80
#define HASOPAQUEBACKGROUND 0x100
#define HASVISIBLE 0x200
#define HASIMAGE 0x400
#define HASCLASSNAME 0x800
#define HASCACHEASBITMAP 0x1000
#define HASBLENDMODE 0x2000
#define HASFILTERLIST 0x4000

}  // namespace swf