#pragma once
#include "controltag.h"
#include "datatype/datatype.h"
#include <vector>
class DefineSceneAndFrameLabelDataTag : public ControlTag {
public:
    DefineSceneAndFrameLabelDataTag(uint32_t length);
    ~DefineSceneAndFrameLabelDataTag();
    void AddScene(const std::string& value);
    void AddFrameLabel(const std::string& value);

private:
    std::vector<std::string> vec_scene_;
    std::vector<std::string> vec_framelabel_;
};