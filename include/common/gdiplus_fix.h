// gdiplus_fix.h - Centralized GDI+ header include to avoid macro conflicts

#ifndef LIBSWF_COMMON_GDIPLUS_FIX_H
#define LIBSWF_COMMON_GDIPLUS_FIX_H

#ifdef _WIN32

// Prevent min/max macro conflicts with C++ standard library
#ifndef NOMINMAX
    #define NOMINMAX
#endif

// Exclude rarely-used stuff from Windows headers
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif

// GDI+ version 1.1
#define GDIPVER 0x0110

// Required for COM/OLE and GDI+ GUID definitions
#include <windows.h>
#include <objbase.h>

// Undefine conflicting macros before including GDI+
#ifdef GetObject
    #undef GetObject
#endif
#ifdef CreateFont
    #undef CreateFont
#endif
#ifdef DrawText
    #undef DrawText
#endif

// Now include GDI+
#include <gdiplus.h>

// Link libraries (MSVC only)
#ifdef _MSC_VER
    #pragma comment(lib, "gdiplus.lib")
    #pragma comment(lib, "ole32.lib")
#endif

#endif // _WIN32

#endif // LIBSWF_COMMON_GDIPLUS_FIX_H
