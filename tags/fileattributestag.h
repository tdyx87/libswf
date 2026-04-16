#pragma once
#include "controltag.h"
class FileAttributesTag : public ControlTag {
public:
    FileAttributesTag(uint32_t length);
    ~FileAttributesTag();
    void SetUseDirectBlit(bool value);
    void SetUseGPU(bool value);
    void SetHasMetadata(bool value);
    void SetActionScript3(bool value);
    void SetUseNetwork(bool value);

private:
    bool usedirectblit_{false};
    bool usegpu_{false};
    bool hasmetadata_{false};
    bool actionscript3_{false};
    bool usenetwork_{false};
};