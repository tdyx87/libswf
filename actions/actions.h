#pragma once
#include "datatype/datatype.h"
namespace swf {

class ACTIONRECORD {
public:
    ACTIONRECORD(uint8_t actioncode, uint16_t length);
    ~ACTIONRECORD();

protected:
    uint8_t action_code_;
    uint16_t length_{0};
};

class ActionGotoFrame : public ACTIONRECORD {
public:
    ActionGotoFrame(uint16_t length);
    ~ActionGotoFrame();
    void SetFrame(uint16_t value);

private:
    uint16_t frame_index_;
};

class ActionGetURL : public ACTIONRECORD {
public:
    ActionGetURL(uint16_t length);
    ~ActionGetURL();
    void SetUrlString(const std::string& value);
    void SetTargetString(const std::string& value);

private:
    std::string target_url_string_;
    std::string target_string_;
};

class ActionNextFrame : public ACTIONRECORD {
public:
    ActionNextFrame(uint16_t length);
    ~ActionNextFrame();
};

class ActionPreviousFrame : public ACTIONRECORD {
public:
    ActionPreviousFrame(uint16_t length);
    ~ActionPreviousFrame();
};

class ActionPlay : public ACTIONRECORD {
public:
    ActionPlay(uint16_t length);
    ~ActionPlay();
};

class ActionStop : public ACTIONRECORD {
public:
    ActionStop(uint16_t length);
    ~ActionStop();
};

class ActionToggleQuality : public ACTIONRECORD {
public:
    ActionToggleQuality(uint16_t length);
    ~ActionToggleQuality();
};
class ActionStopSounds : public ACTIONRECORD {
public:
    ActionStopSounds(uint16_t length);
    ~ActionStopSounds();
};

class ActionWaitForFrame : public ACTIONRECORD {
public:
    ActionWaitForFrame(uint16_t length);
    ActionWaitForFrame();
    void SetFrame(uint16_t value);
    void SetSkipCount(uint8_t value);

private:
    uint16_t frame_;
    uint8_t skip_count_;
};

class ActionSetTarget : public ACTIONRECORD {
public:
    ActionSetTarget(uint16_t length);
    ~ActionSetTarget();
    void SetTargetName(const std::string& value);

private:
    std::string target_name_;
};

class ActionGoToLabel : public ACTIONRECORD {
public:
    ActionGoToLabel(uint16_t length);
    ~ActionGoToLabel();
    void SetLabel(const std::string& value);

private:
    std::string label_;
};

class ActionPush : public ACTIONRECORD {
    struct str {
        uint32_t size;
        char* data;
    };
    struct Value {
        union {
            str String;
            FLOAT Float;
            uint8_t registerNumber;
            uint8_t Boolean;
            DOUBLE Double;
            uint32_t Integer;
            uint8_t Constant8;
            uint16_t Constant16;
        } v;
    };

public:
    ActionPush(uint16_t length);
    ~ActionPush();
    void SetString(const std::string& str);
    void SetFloat(FLOAT value);
    void SetRegisterNumber(uint8_t value);
    void SetBoolean(uint8_t value);
    void SetDouble(DOUBLE value);
    void SetInteger(uint32_t value);
    void SetConstant8(uint8_t value);
    void SetConstant16(uint16_t value);

private:
    uint8_t type_;
    Value value_;
};

class ActionPop : public ACTIONRECORD {
public:
    ActionPop(uint16_t length);
    ~ActionPop();
};

class ActionAdd : public ACTIONRECORD {
public:
    ActionAdd(uint16_t);
    ~ActionAdd();
};

class ActionSubtract : public ACTIONRECORD {
public:
    ActionSubtract(uint16_t length);
    ~ActionSubtract();
};

class ActionMultiply : public ACTIONRECORD {
public:
    ActionMultiply(uint16_t length);
    ~ActionMultiply();
};

class ActionDivide : public ACTIONRECORD {
public:
    ActionDivide(uint16_t length);
    ~ActionDivide();
};

class ActionEquals : public ACTIONRECORD {
public:
    ActionEquals(uint8_t length);
    ~ActionEquals();
};

class ActionLess : public ACTIONRECORD {
public:
    ActionLess(uint16_t length);
    ~ActionLess();
};

class ActionAnd : public ACTIONRECORD {
public:
    ActionAnd(uint16_t length);
    ~ActionAnd();
};

class ActionOr : public ACTIONRECORD {
public:
    ActionOr(uint16_t length);
    ~ActionOr();
};

class ActionNot : public ACTIONRECORD {
public:
    ActionNot(uint16_t length);
    ~ActionNot();
    void SetResult(bool res);

private:
    bool result_;
};

class ActionStringEquals : public ACTIONRECORD {
public:
    ActionStringEquals(uint16_t length);
    ~ActionStringEquals();
};

class ActionStringLength : public ACTIONRECORD {
public:
    ActionStringLength(uint16_t length);
    ~ActionStringLength();
};

class ActionStringAdd : public ACTIONRECORD {
public:
    ActionStringAdd(uint16_t length);
    ~ActionStringAdd();
};

class ActionStringExtract : public ACTIONRECORD {
public:
    ActionStringExtract(uint16_t length);
    ~ActionStringExtract();
};

class ActionStringLess : public ACTIONRECORD {
public:
    ActionStringLess(uint16_t length);
    ~ActionStringLess();
};

class ActionMBStringLength : public ACTIONRECORD {
public:
    ActionMBStringLength(uint16_t length);
    ~ActionMBStringLength();
};

class ActionMBStringExtract : public ACTIONRECORD {
public:
    ActionMBStringExtract(uint16_t length);
    ~ActionMBStringExtract();
};

class ActionToInteger : public ACTIONRECORD {
public:
    ActionToInteger(uint16_t length);
    ~ActionToInteger();
};

class ActionCharToAscii : public ACTIONRECORD {
public:
    ActionCharToAscii(uint16_t length);
    ~ActionCharToAscii();
};

class ActionAsciiToChar : public ACTIONRECORD {
public:
    ActionAsciiToChar(uint16_t length);
    ~ActionAsciiToChar();
};

class ActionMBCharToAscii : public ACTIONRECORD {
public:
    ActionMBCharToAscii(uint16_t length);
    ~ActionMBCharToAscii();
};

class ActionMBAsciiToChar : public ACTIONRECORD {
public:
    ActionMBAsciiToChar(uint16_t length);
    ~ActionMBAsciiToChar();
};

class ActionJump : public ACTIONRECORD {
public:
    ActionJump(uint16_t length);
    ~ActionJump();
    void SetBranchOffset(int16_t value);

private:
    int16_t branch_offset_;
};

class ActionIf : public ACTIONRECORD {
public:
    ActionIf(uint16_t length);
    ~ActionIf();
    void SetBranchOffset(int16_t value);

private:
    int16_t branch_offset_;
};

class ActionCall : public ACTIONRECORD {
public:
    ActionCall(uint16_t length);
    ~ActionCall();
};

class ActionGetVariable : public ACTIONRECORD {
public:
    ActionGetVariable(uint16_t length);
    ~ActionGetVariable();
};

class ActionSetVariable : public ACTIONRECORD {
public:
    ActionSetVariable(uint16_t length);
    ~ActionSetVariable();
};

class ActionGetURL2 : public ACTIONRECORD {
public:
    ActionGetURL2(uint16_t length);
    ~ActionGetURL2();
    void SetSendVarsMethod(uint8_t value);
    void SetLoadTargetFlag(uint8_t value);
    void SetLoadVariablesFlag(uint8_t value);

private:
    uint8_t send_vars_method_;
    uint8_t load_target_flag_;
    uint8_t load_variables_flag_;
};

class ActionGotoFrame2 : public ACTIONRECORD {
public:
    ActionGotoFrame2(uint16_t length);
    ~ActionGotoFrame2();
    void SetSceneBiasFlag(uint8_t value);
    void SetPlayFlag(uint8_t value);
    void SetSceneBias(uint16_t value);

private:
    uint8_t scene_bias_flag_;
    uint8_t play_flag_;
    uint16_t scene_bias_;
};

class ActionSetTarget2 : public ACTIONRECORD {
public:
    ActionSetTarget2(uint16_t length);
    ~ActionSetTarget2();
};

class ActionGetProperty : public ACTIONRECORD {
public:
    ActionGetProperty(uint16_t length);
    ~ActionGetProperty();
};

class ActionSetProperty : public ACTIONRECORD {
public:
    ActionSetProperty(uint16_t length);
    ~ActionSetProperty();
};

class ActionCloneSprite : public ACTIONRECORD {
public:
    ActionCloneSprite(uint16_t length);
    ~ActionCloneSprite();
};

class ActionRemoveSprite : public ACTIONRECORD {
public:
    ActionRemoveSprite(uint16_t length);
    ~ActionRemoveSprite();
};

class ActionStartDrag : public ACTIONRECORD {
public:
    ActionStartDrag(uint16_t length);
    ~ActionStartDrag();
};

class ActionEndDrag : public ACTIONRECORD {
public:
    ActionEndDrag(uint16_t length);
    ~ActionEndDrag();
};

class ActionWaitForFrame2 : public ACTIONRECORD {
public:
    ActionWaitForFrame2(uint16_t length);
    ~ActionWaitForFrame2();
    void SetSkipCount(uint8_t value);

private:
    uint8_t skip_count_;
};

class ActionTrace : public ACTIONRECORD {
public:
    ActionTrace(uint16_t length);
    ~ActionTrace();
};

class ActionGetTime : public ACTIONRECORD {
public:
    ActionGetTime(uint16_t length);
    ~ActionGetTime();
};

class ActionRandomNumber : public ACTIONRECORD {
public:
    ActionRandomNumber(uint16_t length);
    ~ActionRandomNumber();
};

class ActionCallFunction : public ACTIONRECORD {
public:
    ActionCallFunction(uint16_t length);
    ~ActionCallFunction();
};

class ActionCallMethod : public ACTIONRECORD {
public:
    ActionCallMethod(uint16_t length);
    ~ActionCallMethod();
};

class ActionConstantPool : public ACTIONRECORD {
public:
    ActionConstantPool(uint16_t length);
    ~ActionConstantPool();
    void AddConstant(const std::string& value);

private:
    std::vector<std::string> constant_pool_;
};

class ActionDefineFunction : public ACTIONRECORD {
public:
    ActionDefineFunction(uint16_t length);
    ~ActionDefineFunction();
    void SetFunctionName(const std::string& value);
    void AddParam(const std::string& value);

private:
    std::string function_name_;
    std::vector<std::string> vec_param_;
    uint16_t code_size_;
};

class ActionDefineLocal : public ACTIONRECORD {
public:
    ActionDefineLocal(uint16_t length);
    ~ActionDefineLocal();
};

class ActionDefineLocal2 : public ACTIONRECORD {
public:
    ActionDefineLocal2(uint16_t length);
    ~ActionDefineLocal2();
};

class ActionDelete : public ACTIONRECORD {
public:
    ActionDelete(uint16_t length);
    ~ActionDelete();
};

class ActionDelete2 : public ACTIONRECORD {
public:
    ActionDelete2(uint16_t length);
    ~ActionDelete2();
};

class ActionEnumerate : public ACTIONRECORD {
public:
    ActionEnumerate(uint16_t length);
    ~ActionEnumerate();
};

class ActionEquals2 : public ACTIONRECORD {
public:
    ActionEquals2(uint16_t length);
    ~ActionEquals2();
};

class ActionGetMember : public ACTIONRECORD {
public:
    ActionGetMember(uint16_t length);
    ~ActionGetMember();
};

class ActionInitArray : public ACTIONRECORD {
public:
    ActionInitArray(uint16_t length);
    ~ActionInitArray();
};

class ActionInitObject : public ACTIONRECORD {
public:
    ActionInitObject(uint16_t length);
    ~ActionInitObject();
};

class ActionNewMethod : public ACTIONRECORD {
public:
    ActionNewMethod(uint16_t length);
    ~ActionNewMethod();
};

class ActionNewObject : public ACTIONRECORD {
public:
    ActionNewObject(uint16_t length);
    ~ActionNewObject();
};

class ActionSetMember : public ACTIONRECORD {
public:
    ActionSetMember(uint16_t length);
    ~ActionSetMember();
};

class ActionTargetPath : public ACTIONRECORD {
public:
    ActionTargetPath(uint16_t length);
    ~ActionTargetPath();
};

class ActionWith : public ACTIONRECORD {
public:
    ActionWith(uint16_t length);
    ~ActionWith();
    void SetSize(uint16_t value);

private:
    uint16_t size_;
};

class ActionToNumber : public ACTIONRECORD {
public:
    ActionToNumber(uint16_t length);
    ~ActionToNumber();
};

class ActionToString : public ACTIONRECORD {
public:
    ActionToString(uint16_t length);
    ~ActionToString();
};

class ActionTypeOf : public ACTIONRECORD {
public:
    ActionTypeOf(uint16_t length);
    ~ActionTypeOf();
};

class ActionAdd2 : public ACTIONRECORD {
public:
    ActionAdd2(uint16_t length);
    ~ActionAdd2();
};

class ActionLess2 : public ACTIONRECORD {
public:
    ActionLess2(uint16_t length);
    ~ActionLess2();
};

class ActionModulo : public ACTIONRECORD {
public:
    ActionModulo(uint16_t length);
    ~ActionModulo();
};

class ActionBitAnd : public ACTIONRECORD {
public:
    ActionBitAnd(uint16_t length);
    ~ActionBitAnd();
};

class ActionBitLShift : public ACTIONRECORD {
public:
    ActionBitLShift(uint16_t length);
    ~ActionBitLShift();
};

class ActionBitOr : public ACTIONRECORD {
public:
    ActionBitOr(uint16_t length);
    ~ActionBitOr();
};

class ActionBitRShift : public ACTIONRECORD {
public:
    ActionBitRShift(uint16_t length);
    ~ActionBitRShift();
};

class ActionBitURShift : public ACTIONRECORD {
public:
    ActionBitURShift(uint16_t length);
    ~ActionBitURShift();
};

class ActionBitXor : public ACTIONRECORD {
public:
    ActionBitXor(uint16_t length);
    ~ActionBitXor();
};

class ActionDecrement : public ACTIONRECORD {
public:
    ActionDecrement(uint16_t length);
    ~ActionDecrement();
};

class ActionIncrement : public ACTIONRECORD {
public:
    ActionIncrement(uint16_t length);
    ~ActionIncrement();
};

class ActionPushDuplicate : public ACTIONRECORD {
public:
    ActionPushDuplicate(uint16_t length);
    ~ActionPushDuplicate();
};

class ActionReturn : public ACTIONRECORD {
public:
    ActionReturn(uint16_t length);
    ~ActionReturn();
};

class ActionStackSwap : public ACTIONRECORD {
public:
    ActionStackSwap(uint16_t length);
    ~ActionStackSwap();
};

class ActionStoreRegister : public ACTIONRECORD {
public:
    ActionStoreRegister(uint16_t length);
    ~ActionStoreRegister();
};

class ActionInstanceOf : public ACTIONRECORD {
public:
    ActionInstanceOf(uint16_t length);
    ~ActionInstanceOf();
};

class ActionEnumerate2 : public ACTIONRECORD {
public:
    ActionEnumerate2(uint16_t length);
    ~ActionEnumerate2();
};

class ActionStrictEquals : public ACTIONRECORD {
public:
    ActionStrictEquals(uint16_t length);
    ~ActionStrictEquals();
};

class ActionGreater : public ACTIONRECORD {
public:
    ActionGreater(uint16_t length);
    ~ActionGreater();
};
class ActionStringGreater : public ACTIONRECORD {
public:
    ActionStringGreater(uint16_t length);
    ~ActionStringGreater();
};

class ActionDefineFunction2 : public ACTIONRECORD {
    struct REGISTERPARAM {
        uint8_t reg;
        std::string paramName;
    };

public:
    ActionDefineFunction2(uint16_t length);
    ~ActionDefineFunction2();
    void SetFunctionName(const std::string& value);
    void AddParameter(uint8_t reg, const std::string& name);
    void SetRegisterCount(uint8_t value);
    void SetPreloadParentFlag(uint8_t value);
    void SetPreloadRootFlag(uint8_t value);
    void SetSuppressSuperFlag(uint8_t value);
    void SetPreloadSuperFlag(uint8_t value);
    void SetSuppressArgumentsFlag(uint8_t value);
    void SetPreloadArgumentsFlag(uint8_t value);
    void SetSuppressThisFlag(uint8_t value);
    void SetPreloadThisFlag(uint8_t value);
    void SetPreloadGlobalFlag(uint8_t value);
    void SetcodeSize(uint16_t value);

private:
    std::string function_name_;
    uint16_t num_params_;
    uint8_t register_count_;
    uint8_t preload_praent_flag_;
    uint8_t preload_root_flag_;
    uint8_t suppress_super_flag_;
    uint8_t preload_super_flag_;
    uint8_t suppress_arguments_flag_;
    uint8_t preload_arguments_flag_;
    uint8_t suppress_this_flag_;
    uint8_t preload_this_flag_;
    uint8_t preload_global_flag_;
    std::vector<REGISTERPARAM> vec_parameters_;
    uint16_t code_size_;
};

class ActionExtends : public ACTIONRECORD {
public:
    ActionExtends(uint16_t length);
    ~ActionExtends();
};

class ActionCastOp : public ACTIONRECORD {
public:
    ActionCastOp(uint16_t length);
    ~ActionCastOp();
};

class ActionImplementsOp : public ACTIONRECORD {
public:
    ActionImplementsOp(uint16_t length);
    ~ActionImplementsOp();
};

class ActionTry : public ACTIONRECORD {
public:
    ActionTry(uint16_t length);
    ~ActionTry();
    void SetCatchInRegisterFlag(uint8_t value);
    void SetFinallyBlockFlag(uint8_t value);
    void SetCatchBlockFlag(uint8_t value);
    void SetTrySize(uint16_t value);
    void SetCatchSize(uint16_t value);
    void SetFinallySize(uint16_t value);
    void SetCatchName(const std::string& name);
    void SetCatchRegister(uint8_t value);
    void SetTryBody(const std::vector<uint8_t>& value);
    void SetCatchBody(const std::vector<uint8_t>& value);
    void SetFinallyBody(const std::vector<uint8_t>& value);

private:
    uint8_t catch_in_register_flag_;
    uint8_t finally_block_flag_;
    uint8_t catch_block_flag_;
    uint16_t try_size_;
    uint16_t catch_size_;
    uint16_t finally_size_;
    std::string catch_name_;
    uint8_t catch_register_;
    std::vector<uint8_t> try_body_;
    std::vector<uint8_t> catch_body_;
    std::vector<uint8_t> finally_body_;
};

class ActionThrow : public ACTIONRECORD {
public:
    ActionThrow(uint16_t length);
    ~ActionThrow();
};

enum CLIPEVENTFLAGS {
    ClipEventKeyUp = 0x01,
    ClipEventKeyDown = 0x02,
    ClipEventMouseUp = 0x04,
    ClipEventMouseDown = 0x08,
    ClipEventMouseMove = 0x10,
    ClipEventUnload = 0x20,
    ClipEventEnterFrame = 0x40,
    ClipEventLoad = 0x80,
    ClipEventDragOver = 0x100,
    ClipEventRollOut = 0x200,
    ClipEventRollOver = 0x400,
    ClipEventReleaseOutside = 0x800,
    ClipEventRelease = 0x1000,
    ClipEventPress = 0x2000,
    ClipEventInitialize = 0x4000,
    ClipEventData = 0x8000,
    ClipEventConstruct = 0x10000,
    ClipEventKeyPress = 0x20000,
    ClipEventDragOut = 0x40000
};

struct CLIPACTIONRECORD {
    CLIPEVENTFLAGS eventflags;
    uint8_t keycode;
    std::vector<ACTIONRECORD*> actions;
};

class CLIPACTIONS {
public:
    CLIPACTIONS();
    ~CLIPACTIONS();

private:
    CLIPEVENTFLAGS all_event_flags_;
    std::vector<CLIPACTIONRECORD> clip_action_records_;
};

};  // namespace swf