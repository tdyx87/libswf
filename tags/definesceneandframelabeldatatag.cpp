#include "definesceneandframelabeldatatag.h"

DefineSceneAndFrameLabelDataTag::DefineSceneAndFrameLabelDataTag(
    uint32_t length)
    : ControlTag(TagType::TAG_DEFINESCENEANDFRAMELABELDATA, length)
{
}

DefineSceneAndFrameLabelDataTag::~DefineSceneAndFrameLabelDataTag() {}

void DefineSceneAndFrameLabelDataTag::AddScene(const std::string& value)
{
    vec_scene_.push_back(value);
}

void DefineSceneAndFrameLabelDataTag::AddFrameLabel(const std::string& value)
{
    vec_framelabel_.push_back(value);
}
