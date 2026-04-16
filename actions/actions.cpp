#include "actions.h"

namespace swf {
ACTIONRECORD::ACTIONRECORD(uint8_t actioncode, uint16_t length)
    : action_code_(actioncode), length_(length)
{
}

ACTIONRECORD::~ACTIONRECORD() {}
ActionGotoFrame::ActionGotoFrame(uint16_t length) : ACTIONRECORD(0x81, length)
{
}
ActionGotoFrame::~ActionGotoFrame() {}
void ActionGotoFrame::SetFrame(uint16_t value)
{
    frame_index_ = value;
}
ActionGetURL::ActionGetURL(uint16_t length) : ACTIONRECORD(0x83, length) {}
ActionGetURL::~ActionGetURL() {}
void ActionGetURL::SetUrlString(const std::string& value)
{
    target_url_string_ = value;
}
void ActionGetURL::SetTargetString(const std::string& value)
{
    target_string_ = value;
}
ActionNextFrame::ActionNextFrame(uint16_t length) : ACTIONRECORD(0x04, length)
{
}
ActionNextFrame::~ActionNextFrame() {}
ActionPreviousFrame::ActionPreviousFrame(uint16_t length)
    : ACTIONRECORD(0x05, length)
{
}
ActionPreviousFrame::~ActionPreviousFrame() {}
ActionPlay::ActionPlay(uint16_t length) : ACTIONRECORD(0x06, length) {}
ActionPlay::~ActionPlay() {}
}  // namespace swf