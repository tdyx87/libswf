// AVM2 运行时语义单元测试：全局内建函数、事件流、显示列表。
// CTest: swfparser_avm2_runtime。
#include "swf/Abc.hpp"
#include "swf/Decoder.hpp"
#include "swf/Fvm.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace f = swf::fvm;
using f::ObjKind;
using f::Value;
using f::VKind;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

static Value global(const f::Vm& vm, const char* n) {
    const auto it = vm.globalsObj()->props.find(n);
    return it == vm.globalsObj()->props.end() ? Value::undefined() : it->second;
}

static Value call(f::Vm& vm, const std::shared_ptr<f::Object>& obj,
                  const char* n, std::vector<Value> args) {
    return vm.callMethod(obj, n, args);
}

static Value make(f::Vm& vm, const char* cls, std::vector<Value> args = {}) {
    return vm.constructObject(global(vm, cls), args);
}

// 构造一个可计数的原生监听器：$native 指向自造 ClassDef 上的方法。
static Value counterFn(f::Vm& vm, const char* name, int& counter, int& phase,
                       bool stop = false) {
    auto cd = std::make_shared<f::ClassDef>();
    f::Member m;
    m.kind = f::Member::Method;
    m.name = name;
    m.native = [&counter, &phase, stop](std::shared_ptr<f::Object>,
                                         const std::vector<Value>& a) -> Value {
        ++counter;
        if (!a.empty() && a[0].kind == VKind::Object && a[0].obj) {
            const auto p = a[0].obj->props.find("eventPhase");
            if (p != a[0].obj->props.end()) phase = p->second.toInt();
            if (stop) a[0].obj->props["$stop"] = Value::boolean(true);
        }
        return Value::undefined();
    };
    cd->instanceMembers.push_back(m);
    auto holder = vm.newObject(ObjKind::Generic, "ListenerHolder");
    holder->cls = cd;
    auto fn = vm.newObject(ObjKind::Function, "listener");
    fn->props["$native"] = Value::str(name);
    fn->props["$this"] = Value::object(holder);
    return Value::object(fn);
}

// 通用原生函数值：holder.cls 上挂一个可计数的同名方法，返回值由 body 决定。
static Value nativeFn(f::Vm& vm, const char* name,
                      std::function<Value(const std::vector<Value>&)> body,
                      int* counter = nullptr) {
    auto cd = std::make_shared<f::ClassDef>();
    f::Member m;
    m.kind = f::Member::Method;
    m.name = name;
    m.native = [body, counter](std::shared_ptr<f::Object>,
                               const std::vector<Value>& a) -> Value {
        if (counter) ++*counter;
        return body(a);
    };
    cd->instanceMembers.push_back(m);
    auto holder = vm.newObject(ObjKind::Generic, "NativeHolder");
    holder->cls = cd;
    auto fn = vm.newObject(ObjKind::Function, name);
    fn->props["$native"] = Value::str(name);
    fn->props["$this"] = Value::object(holder);
    return Value::object(fn);
}

// 用手工字节码直接驱动解释器，验证 opcode 与异常表的真实行为。
static Value runBody(f::Vm& vm, const std::vector<std::uint8_t>& code,
                     const std::vector<std::uint32_t>& handlers = {},
                     std::shared_ptr<f::Object> self = nullptr) {
    swf::abc::MethodBody b;
    b.code = code;
    b.maxStack = 8;
    b.localCount = 4;
    b.exceptionHandlers = handlers;
    return vm.exec(b, {}, self);
}

int main() {
    std::printf("AVM2 runtime semantics tests\n");
    swf::Movie movie;
    swf::abc::AbcFile abc;
    f::Vm vm(abc, movie);
    vm.run();
    auto g = vm.globalsObj();

    // ---- 1. 全局内建函数 ----
    {
        check(std::fabs(call(vm, g, "parseInt", {Value::str("0x1F")}).toNumber() - 31.0) < 1e-9,
              "parseInt(\"0x1F\") == 31");
        check(call(vm, g, "parseInt", {Value::str("12abc")}).toNumber() == 12.0,
              "parseInt(\"12abc\") == 12");
        check(call(vm, g, "parseInt", {Value::str("ff"), Value::intg(16)}).toNumber() == 255.0,
              "parseInt(\"ff\", 16) == 255");
        check(std::isnan(call(vm, g, "parseInt", {Value::str("zz")}).toNumber()),
              "parseInt(\"zz\") is NaN");
        check(call(vm, g, "parseFloat", {Value::str("12.5px")}).toNumber() == 12.5,
              "parseFloat(\"12.5px\") == 12.5");
        check(call(vm, g, "isNaN", {Value::str("abc")}).toBool(), "isNaN(\"abc\") == true");
        check(!call(vm, g, "isNaN", {Value::nullv()}).toBool(), "isNaN(null) == false");
        check(call(vm, g, "isNaN", {Value::undefined()}).toBool(), "isNaN(undefined) == true");
        check(!call(vm, g, "isFinite", {Value::num(INFINITY)}).toBool(),
              "isFinite(Infinity) == false");
        check(call(vm, g, "Number", {Value::str(" 3.5 ")}).toNumber() == 3.5,
              "Number(\" 3.5 \") == 3.5");
        check(std::isnan(call(vm, g, "Number", {Value::undefined()}).toNumber()),
              "Number(undefined) is NaN");
        check(call(vm, g, "int", {Value::num(4294967297.0)}).toInt() == 1,
              "int() wraps at 32 bits");
        check(call(vm, g, "encodeURIComponent", {Value::str("a b&")}).toString() == "a%20b%26",
              "encodeURIComponent escapes space and &");
        check(call(vm, g, "decodeURIComponent", {Value::str("a%20b%26")}).toString() == "a b&",
              "decodeURIComponent round-trip");
        check(call(vm, g, "getQualifiedClassName", {Value::intg(1)}).toString() == "int",
              "getQualifiedClassName(int)");
        check(call(vm, g, "describeType", {Value::intg(1)}).toString().find("<type") == 0,
              "describeType returns an XML-ish description");
        const Value fn = global(vm, "parseInt");
        check(fn.kind == VKind::Object && fn.obj && fn.obj->kind == ObjKind::Function,
              "global functions are first-class callable values");
    }

    // ---- 2. Event ----
    {
        const Value ev = make(vm, "Event", {Value::str("click"), Value::boolean(true),
                                           Value::boolean(true)});
        check(vm.getProperty(ev.obj, "type").toString() == "click", "Event(type)");
        check(vm.getProperty(ev.obj, "bubbles").toBool(), "Event(bubbles=true)");
        check(!call(vm, ev.obj, "isDefaultPrevented", {}).toBool(),
              "isDefaultPrevented() is false initially");
        call(vm, ev.obj, "preventDefault", {});
        check(call(vm, ev.obj, "isDefaultPrevented", {}).toBool(),
              "preventDefault() works on cancelable events");
        const Value ev2 = make(vm, "Event", {Value::str("x")});
        call(vm, ev2.obj, "preventDefault", {});
        check(!call(vm, ev2.obj, "isDefaultPrevented", {}).toBool(),
              "preventDefault() ignored when cancelable=false");
        call(vm, ev2.obj, "stopPropagation", {});
        check(ev2.obj->props["$stop"].toBool(), "stopPropagation sets $stop");
        const Value cloned = call(vm, ev2.obj, "clone", {});
        check(cloned.kind == VKind::Object && cloned.obj != ev2.obj &&
              !cloned.obj->props["$stop"].toBool(),
              "clone() clears propagation flags");
        const Value ec = global(vm, "Event");
        check(vm.getProperty(ec.obj, "AT_TARGET").toInt() == 2 &&
              vm.getProperty(ec.obj, "BUBBLING_PHASE").toInt() == 3,
              "Event phase constants");
    }

    // ---- 3. EventDispatcher 三段事件流 ----
    {
        const Value parent = make(vm, "Sprite");
        const Value child = make(vm, "Sprite");
        call(vm, parent.obj, "addChild", {child});
        int childCalls = 0, parentCalls = 0, captureCalls = 0;
        int childPhase = 0, parentPhase = 0, capturePhase = 0;
        const Value fnChild = counterFn(vm, "onChild", childCalls, childPhase);
        const Value fnParent = counterFn(vm, "onParent", parentCalls, parentPhase);
        const Value fnCapture = counterFn(vm, "onCapture", captureCalls, capturePhase);
        // 同类型同监听器重复注册必须被忽略
        call(vm, child.obj, "addEventListener", {Value::str("ping"), fnChild});
        call(vm, child.obj, "addEventListener", {Value::str("ping"), fnChild});
        call(vm, parent.obj, "addEventListener", {Value::str("ping"), fnParent});
        call(vm, parent.obj, "addEventListener",
             {Value::str("ping"), fnCapture, Value::boolean(true), Value::intg(0)});
        check(child.obj->listeners["ping"].size() == 1, "duplicate listener is ignored");
        check(call(vm, parent.obj, "hasEventListener", {Value::str("ping")}).toBool(),
              "hasEventListener on parent");
        check(call(vm, child.obj, "willTrigger", {Value::str("pong")}).toBool() == false,
              "willTrigger false without any listener");

        const Value ev = make(vm, "Event", {Value::str("ping"), Value::boolean(true),
                                           Value::boolean(false)});
        const Value ok = call(vm, child.obj, "dispatchEvent", {ev});
        check(ok.toBool(), "dispatchEvent returns true when not cancelled");
        check(childCalls == 1 && parentCalls == 1 && captureCalls == 1,
              "capture, target and bubble listeners all fire once");
        check(childPhase == 2 && parentPhase == 3 && capturePhase == 1,
              "eventPhase is AT_TARGET / BUBBLING / CAPTURING");
        const auto tp = ev.obj->props.find("target");
        check(tp != ev.obj->props.end() && tp->second.kind == VKind::Object &&
              tp->second.obj == child.obj,
              "event target stays the dispatching object");

        // stopPropagation 阻止冒泡
        childCalls = parentCalls = captureCalls = 0;
        int stopPhase = 0;
        const Value stopFn = counterFn(vm, "onStop", childCalls, stopPhase, true);
        call(vm, child.obj, "removeEventListener", {Value::str("ping"), fnChild});
        call(vm, child.obj, "addEventListener", {Value::str("ping"), stopFn});
        const Value ev2 = make(vm, "Event", {Value::str("ping"), Value::boolean(true),
                                            Value::boolean(false)});
        call(vm, child.obj, "dispatchEvent", {ev2});
        check(childCalls == 1 && parentCalls == 0,
              "stopPropagation prevents bubble-phase listeners");

        // preventDefault + cancelable -> dispatchEvent 返回 false
        const Value ev3 = make(vm, "Event", {Value::str("ping"), Value::boolean(false),
                                            Value::boolean(true)});
        call(vm, ev3.obj, "preventDefault", {});
        check(!call(vm, child.obj, "dispatchEvent", {ev3}).toBool(),
              "dispatchEvent returns false for cancelled events");

        // removeEventListener 按 capture 分离
        call(vm, parent.obj, "removeEventListener", {Value::str("ping"), fnParent});
        check(call(vm, parent.obj, "hasEventListener", {Value::str("ping")}).toBool(),
              "capture listener still present after removing bubble listener");
        call(vm, parent.obj, "removeEventListener",
             {Value::str("ping"), fnCapture, Value::boolean(true)});
        check(!call(vm, parent.obj, "hasEventListener", {Value::str("ping")}).toBool(),
              "removing capture listener clears hasEventListener");
    }

    // ---- 4. 显示列表 ----
    {
        const Value p1 = make(vm, "Sprite");
        const Value p2 = make(vm, "Sprite");
        const Value a = make(vm, "Sprite");
        const Value b = make(vm, "Sprite");
        call(vm, p1.obj, "addChild", {a});
        call(vm, p1.obj, "addChild", {a});
        check(p1.obj->display->children.size() == 1, "addChild does not duplicate a child");
        call(vm, p2.obj, "addChild", {a});
        check(p1.obj->display->children.empty() &&
              a.obj->display->parent == p2.obj->display,
              "addChild detaches child from its previous parent");
        check(a.obj->display->depth == -1, "script-added children have depth -1");
        call(vm, p2.obj, "addChildAt", {b, Value::intg(0)});
        check(call(vm, p2.obj, "getChildIndex", {b}).toInt() == 0 &&
              p2.obj->display->children.size() == 2,
              "addChildAt inserts at the requested index");
        call(vm, p2.obj, "swapChildrenAt", {Value::intg(0), Value::intg(1)});
        check(call(vm, p2.obj, "getChildIndex", {b}).toInt() == 1,
              "swapChildrenAt swaps indices");
        const Value removed = call(vm, p2.obj, "removeChildAt", {Value::intg(0)});
        check(removed.kind == VKind::Object && removed.obj && removed.obj->display &&
              !removed.obj->display->parent,
              "removeChildAt returns child and clears its parent");
        call(vm, p2.obj, "removeChildren", {});
        check(p2.obj->display->children.empty(), "removeChildren clears the container");
    }

    // ---- 5. JSON ----
    {
        const Value jsonCls = global(vm, "JSON");
        const Value parsed = call(vm, jsonCls.obj, "parse",
            {Value::str("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":-2.5},\"u\":\"\\u0041\\u00e9\"}")});
        check(parsed.kind == VKind::Object && parsed.obj, "JSON.parse returns an object");
        check(vm.getProperty(parsed.obj, "a").toNumber() == 1.0, "JSON.parse scalar field");
        const Value arr = vm.getProperty(parsed.obj, "b");
        check(arr.kind == VKind::Object && arr.obj &&
              arr.obj->props["$length"].toInt() == 3 &&
              arr.obj->props["0"].toBool() && arr.obj->props["1"].isNull() &&
              arr.obj->props["2"].toString() == "x",
              "JSON.parse array with true/null/string");
        const Value nested = vm.getProperty(parsed.obj, "c");
        check(nested.kind == VKind::Object && nested.obj &&
              vm.getProperty(nested.obj, "d").toNumber() == -2.5,
              "JSON.parse nested object and negative number");
        check(vm.getProperty(parsed.obj, "u").toString() == "A\xc3\xa9",
              "JSON.parse decodes \\u escapes as UTF-8");
        check(call(vm, jsonCls.obj, "parse", {Value::str("{broken")}).isUndefined(),
              "JSON.parse rejects malformed input");
        check(call(vm, jsonCls.obj, "stringify", {Value::intg(5)}).toString() == "5",
              "JSON.stringify number");
        check(call(vm, jsonCls.obj, "stringify", {Value::str("a\"b\n")}).toString() ==
              "\"a\\\"b\\n\"",
              "JSON.stringify escapes quotes and control characters");
        const Value out = call(vm, jsonCls.obj, "stringify", {parsed});
        check(out.kind == VKind::String && out.s.find("\"a\":1") != std::string::npos &&
              out.s.find("\"b\":[true,null,\"x\"]") != std::string::npos,
              "JSON.stringify produces valid JSON for parsed objects");
        const Value back = call(vm, jsonCls.obj, "parse", {out});
        check(back.kind == VKind::Object && back.obj &&
              vm.getProperty(back.obj, "a").toNumber() == 1.0,
              "JSON.parse/stringify round-trip");
    }

    // ---- 6. Date ----
    {
        const Value d = make(vm, "Date", {Value::intg(0)});
        check(call(vm, d.obj, "getTime", {}).toNumber() == 0.0, "Date(0).getTime()");
        check(call(vm, d.obj, "getUTCFullYear", {}).toInt() == 1970,
              "Date(0).getUTCFullYear() == 1970");
        check(call(vm, d.obj, "getUTCMonth", {}).toInt() == 0 &&
              call(vm, d.obj, "getUTCDate", {}).toInt() == 1,
              "Date(0) UTC month/date");
        check(call(vm, d.obj, "setTime", {Value::num(1500.0)}).toNumber() == 1500.0 &&
              call(vm, d.obj, "getMilliseconds", {}).toInt() == 500,
              "Date.setTime / getMilliseconds");
        const Value d2 = make(vm, "Date", {Value::intg(2020), Value::intg(0), Value::intg(2)});
        check(call(vm, d2.obj, "getFullYear", {}).toInt() == 2020 &&
              call(vm, d2.obj, "getMonth", {}).toInt() == 0,
              "Date(year, month, date) constructor");
    }

    // ---- 7. Math ----
    {
        const Value math = global(vm, "Math");
        check(call(vm, math.obj, "max",
                   {Value::intg(1), Value::intg(5), Value::intg(3)}).toNumber() == 5.0,
              "Math.max is variadic");
        check(std::isinf(call(vm, math.obj, "min", {}).toNumber()),
              "Math.min() == Infinity");
        check(std::fabs(call(vm, math.obj, "atan2",
                             {Value::intg(0), Value::intg(-1)}).toNumber() -
                        3.141592653589793) < 1e-9,
              "Math.atan2");
        check(std::fabs(call(vm, math.obj, "log",
                             {Value::num(std::exp(1.0))}).toNumber() - 1.0) < 1e-9,
              "Math.log");
        check(std::fabs(vm.getProperty(math.obj, "PI").toNumber() -
                        3.141592653589793) < 1e-12,
              "Math.PI constant");
        const double r = call(vm, math.obj, "random", {}).toNumber();
        check(r >= 0.0 && r <= 1.0, "Math.random is in [0,1]");
    }

    // ---- 8. 无 classDef 的时间轴显示对象包装 ----
    {
        auto plain = vm.newObject(ObjKind::MovieClip, "PlainClip");
        int n = 0, ph = 0;
        const Value fn = counterFn(vm, "onPlain", n, ph);
        call(vm, plain, "addEventListener", {Value::str("evt"), fn});
        check(plain->listeners["evt"].size() == 1,
              "bare display wrapper resolves addEventListener via native class chain");
        const Value ev = make(vm, "Event", {Value::str("evt")});
        call(vm, plain, "dispatchEvent", {ev});
        check(n == 1, "bare display wrapper dispatchEvent fires its listener");
        const Value cm = make(vm, "ContextMenu");
        check(cm.obj->props.count("customItems") == 1, "ContextMenu ctor creates customItems");
        call(vm, cm.obj, "hideBuiltInItems", {});
        check(cm.obj->props.count("builtInItems") == 1 &&
              !vm.getProperty(cm.obj->props["builtInItems"].obj, "play").toBool(),
              "ContextMenu.hideBuiltInItems disables built-in entries");
        auto bare = vm.newObject(ObjKind::Generic, "bare");
        call(vm, bare, "hideBuiltInItems", {});
        check(bare->props.count("builtInItems") == 1,
              "bare object resolves native class methods via fallback");
        auto bareArr = vm.newObject(ObjKind::Array, "Array");
        bareArr->props["$length"] = Value::intg(0);
        const Value len = call(vm, bareArr, "push", {Value::intg(7)});
        check(len.toInt() == 1 && bareArr->props["0"].toInt() == 7,
              "bare array literal resolves Array.push via native class chain");
    }

    // ---- 9. AS3 异常：catch 入口与顶层未捕获 ----
    {
        // pushbyte 7; throw; returnvalue。catch target 指向 returnvalue，
        // catch 入口栈上只有异常值，因此 exec() 应返回被抛出的 7。
        const Value caught = runBody(vm, {0x24, 7, 0x03, 0x48}, {0, 3, 3, 0, 1});
        check(caught.toInt() == 7, "throw is caught by exception_info and resumes at target");
        const Value uncaught = runBody(vm, {0x24, 7, 0x03});
        check(uncaught.isUndefined(), "uncaught top-level exception aborts only that script");
        check(runBody(vm, {0x24, 9, 0x48}).toInt() == 9,
              "interpreter still executes normally after an exception");
    }

    // ---- 10. 时间轴重建：脚本子对象层级与脚本/时间轴变换优先级 ----
    {
        swf::Movie tlMovie;
        tlMovie.frameRate = 12;
        swf::DisplayItem it1, it2;
        it1.depth = 1; it1.characterId = 10; it1.matrix.tx = 100;
        it2.depth = 2; it2.characterId = 11; it2.matrix.tx = 200;
        swf::Frame f0, f1;
        f0.items = {it1, it2};
        f1.items = {it1, it2};
        tlMovie.frames = {f0, f1};
        swf::abc::AbcFile tlAbc;
        f::Vm tlVm(tlAbc, tlMovie);
        tlVm.run();
        auto root = std::make_shared<f::DisplayObject>();
        root->characterId = 0;
        tlVm.stage.push_back(root);
        tlVm.setClipFrame(root, 0);
        check(root->children.size() == 2, "timeline frame builds the placed children");
        auto code = std::make_shared<f::DisplayObject>();
        code->depth = -1;
        code->name = "scriptChild";
        root->children.insert(root->children.begin() + 1, code);
        tlVm.setClipFrame(root, 1);
        check(root->children.size() == 3 && root->children[1] == code,
              "script child keeps its absolute index across timeline frames");
        auto owner = root->children[0]->owner.lock();
        check(owner != nullptr, "timeline child is bound to its AS3 owner");
        if (owner) {
            tlVm.setProperty(owner, "x", Value::num(123));
            check(owner->display->scriptTransform && owner->display->x == 123.0,
                  "script write marks the transform as script-owned");
            tlVm.setClipFrame(root, 1);
            check(root->children[0]->x == 123.0,
                  "unchanged timeline placement does not overwrite script transform");
            tlMovie.frames[1].items[0].matrix.tx = 400 * 20;
            tlVm.setClipFrame(root, 1);
            check(std::fabs(root->children[0]->x - 400.0) < 1e-9,
                  "changed timeline placement takes transform ownership back");
        }
    }

    // ---- 11. 嵌套精灵的放置矩阵必须精确保真（镜像/错切）----
    // 回归：时间轴放置的矩阵此前被分解成 scaleX/scaleY/rotation 三自由度近似，
    // 且分解公式误用 RotateSkew0(r1) 当旋转角，使带镜像的矩阵（拍手动画的
    // 手掌/手臂）重建后与原矩阵不符 → 手掌被甩到腹部、手臂倾斜。
    {
        swf::Movie m;
        m.frameRate = 12;
        // 嵌套精灵 20 的第 1 帧：用拍手动画中手掌 d910 的真实矩阵（镜像，det<0）
        swf::DisplayItem inner;
        inner.depth = 1;
        inner.characterId = 21;
        inner.matrix.sx = -0.377f; inner.matrix.r0 = -0.6529f;
        inner.matrix.r1 = -0.6529f; inner.matrix.sy = 0.377f;
        inner.matrix.tx = 13154; inner.matrix.ty = 15995;   // twips
        swf::Frame subFrame;
        subFrame.items = {inner};
        auto sprite = std::make_shared<swf::Character>();
        sprite->kind = swf::Character::Kind::Sprite;
        sprite->spriteFrames = {subFrame};
        m.characters[20] = sprite;
        m.characters[21] = std::make_shared<swf::Character>();   // 占位子角色

        swf::DisplayItem top;
        top.depth = 1; top.characterId = 20; top.matrix.tx = 100;
        swf::Frame tf;
        tf.items = {top};
        m.frames = {tf};

        swf::abc::AbcFile abc;
        f::Vm v(abc, m);
        v.run();
        auto root = std::make_shared<f::DisplayObject>();
        root->characterId = 0;
        v.stage.push_back(root);
        v.setClipFrame(root, 0);

        check(root->children.size() == 1, "top-level sprite instance built");
        if (!root->children.empty()) {
            auto sp = root->children[0];
            check(sp->children.size() == 1, "nested sprite frame built its children");
            if (!sp->children.empty()) {
                auto& c = sp->children[0];
                check(c->hasFull,
                      "timeline-placed child keeps the full 2x2 matrix (hasFull)");
                bool same = std::fabs(c->fullA - inner.matrix.sx) < 1e-6 &&
                            std::fabs(c->fullB - inner.matrix.r0) < 1e-6 &&
                            std::fabs(c->fullC - inner.matrix.r1) < 1e-6 &&
                            std::fabs(c->fullD - inner.matrix.sy) < 1e-6;
                check(same, "full 2x2 equals the original PlaceObject matrix");
                check(c->scaleY < 0, "mirrored placement keeps negative scaleY");
            }
        }
    }

    // ---- 12. applicationDomain.domainMemory（Alchemy 小端读写）----
    {
        auto mem = std::make_shared<std::vector<std::uint8_t>>(16, 0);
        vm.setDomainMemoryData(mem);
        // pushbyte 42; pushbyte 4; si32(0x3C); returnvoid
        runBody(vm, {0x24, 42, 0x24, 4, 0x3C, 0x47});
        check((*mem)[4] == 42 && (*mem)[5] == 0 && (*mem)[6] == 0 && (*mem)[7] == 0,
              "si32 writes little-endian bytes into domainMemory");
        // pushbyte 4; li32(0x37); returnvalue
        check(runBody(vm, {0x24, 4, 0x37, 0x48}).toInt() == 42,
              "li32 reads the value back");
        // pushbyte 127; pushbyte 8; si8(0x3A); ...; li8(0x35)
        runBody(vm, {0x24, 127, 0x24, 8, 0x3A, 0x47});
        check(runBody(vm, {0x24, 8, 0x35, 0x48}).toInt() == 127,
              "si8/li8 round-trip on signed bytes");
    }

    // ---- 12. ByteArray ----
    {
        const Value ba = make(vm, "ByteArray");
        check(vm.getProperty(ba.obj, "length").toInt() == 0, "new ByteArray is empty");
        call(vm, ba.obj, "writeInt", {Value::intg(0x12345678)});
        call(vm, ba.obj, "writeByte", {Value::intg(255)});
        call(vm, ba.obj, "writeDouble", {Value::num(2.5)});
        check(vm.getProperty(ba.obj, "length").toInt() == 13,
              "ByteArray writes advance length and position");
        vm.setProperty(ba.obj, "position", Value::intg(0));
        check(call(vm, ba.obj, "readInt", {}).toInt() == 0x12345678,
              "ByteArray.readInt reads little-endian int");
        check(call(vm, ba.obj, "readUnsignedByte", {}).toInt() == 255,
              "ByteArray.readUnsignedByte");
        check(std::fabs(call(vm, ba.obj, "readDouble", {}).toNumber() - 2.5) < 1e-12,
              "ByteArray.readDouble");
        check(vm.getProperty(ba.obj, "bytesAvailable").toInt() == 0,
              "ByteArray.bytesAvailable");
        call(vm, ba.obj, "writeUTFBytes", {Value::str("hi")});
        vm.setProperty(ba.obj, "position", Value::intg(13));
        check(call(vm, ba.obj, "readUTFBytes", {Value::intg(2)}).toString() == "hi",
              "ByteArray UTF byte round-trip");
        const Value ad = global(vm, "applicationDomain");
        const Value cd = vm.getProperty(ad.obj, "currentDomain");
        vm.setProperty(cd.obj, "domainMemory", ba);
        check(vm.domainMemoryData() == ba.obj->bytes,
              "applicationDomain.domainMemory shares the ByteArray buffer");
    }

    // ---- 13. XML / XMLList / describeType ----
    {
        const Value x = make(vm, "XML",
            {Value::str("<root a=\"1\"><item>hi</item><item>x</item></root>")});
        check(call(vm, x.obj, "toXMLString", {}).toString().find("<item>hi</item>") !=
              std::string::npos, "XML ctor parses element children");
        check(call(vm, x.obj, "attribute", {Value::str("a")}).toString() == "1",
              "XML.attribute reads parsed attributes");
        const Value items = vm.getProperty(x.obj, "item");
        check(items.kind == VKind::Object && items.obj,
              "XML dot access resolves repeated children to an XMLList");
        check(call(vm, items.obj, "length", {}).toInt() == 2,
              "XMLList.length for repeated children");
        const Value desc = call(vm, x.obj, "descendants", {Value::str("item")});
        check(call(vm, desc.obj, "length", {}).toInt() == 2,
              "XML.descendants collects descendants by name");
        const Value dt = call(vm, g, "describeType", {Value::intg(1)});
        check(dt.kind == VKind::Object && dt.obj &&
              vm.getProperty(dt.obj, "$xmlName").toString() == "type",
              "describeType returns a real XML object");
        check(dt.toString().find("<type") == 0,
              "XML object stringifies to its markup");
    }

    // ---- 14. getdescendants opcode（含嵌套节点与 QName 常量池）----
    {
        abc.strings.push_back("item");
        const std::uint32_t strIdx = static_cast<std::uint32_t>(abc.strings.size() - 1);
        if (abc.multinames.empty()) abc.multinames.push_back(swf::abc::Multiname{});
        swf::abc::Multiname qn;
        qn.kind = swf::abc::Multiname::Kind::QName;
        qn.name = strIdx;
        abc.multinames.push_back(qn);
        const std::uint8_t mnByte =
            static_cast<std::uint8_t>(abc.multinames.size() - 1);
        const Value x = make(vm, "XML",
            {Value::str("<root><item>1</item><box><item>2</item></box></root>")});
        // getlocal0; getdescendants QName#mn; returnvalue
        const Value list = runBody(vm, {0xD0, 0x59, mnByte, 0x48}, {}, x.obj);
        check(list.kind == VKind::Object && list.obj &&
              list.obj->props["$length"].toInt() == 2,
              "getdescendants opcode collects nested matching nodes");
    }

    // ---- 15. JSON.parse reviver ----
    {
        int calls = 0;
        const Value rev = nativeFn(vm, "revive",
            [](const std::vector<Value>& a) -> Value {
                if (a.size() < 2) return Value::undefined();
                const Value& v = a[1];
                if (v.kind == VKind::Number || v.kind == VKind::Int || v.kind == VKind::UInt)
                    return Value::num(v.toNumber() * 2.0);
                return v;
            }, &calls);
        const Value parsed = call(vm, global(vm, "JSON").obj, "parse",
                                  {Value::str("{\"a\":2,\"b\":3}"), rev});
        check(parsed.kind == VKind::Object && parsed.obj &&
              vm.getProperty(parsed.obj, "a").toNumber() == 4.0 &&
              vm.getProperty(parsed.obj, "b").toNumber() == 6.0,
              "JSON.parse reviver transforms leaf values");
        check(calls >= 3, "reviver runs for each property and the root value");
    }

    // ---- 16. Date setters / formatting ----
    {
        const Value d = make(vm, "Date", {Value::intg(0)});
        check(call(vm, d.obj, "toISOString", {}).toString() == "1970-01-01T00:00:00.000Z",
              "Date.toISOString");
        check(call(vm, d.obj, "toUTCString", {}).toString().find("01 Jan 1970") !=
              std::string::npos, "Date.toUTCString");
        call(vm, d.obj, "setUTCFullYear", {Value::intg(2001)});
        check(call(vm, d.obj, "getUTCFullYear", {}).toInt() == 2001,
              "Date.setUTCFullYear");
        call(vm, d.obj, "setMilliseconds", {Value::intg(250)});
        check(call(vm, d.obj, "getMilliseconds", {}).toInt() == 250,
              "Date.setMilliseconds");
        const Value d2 = make(vm, "Date", {Value::intg(0)});
        call(vm, d2.obj, "setUTCMonth", {Value::intg(1)});
        check(call(vm, d2.obj, "getUTCMonth", {}).toInt() == 1,
              "Date.setUTCMonth");
    }

    // ---- 17. TweenLite / TweenMax ----
    {
        const Value s = make(vm, "Sprite");
        auto vars = vm.newObject(ObjKind::Generic, "Object");
        vars->props["x"] = Value::num(200);
        vars->props["alpha"] = Value::num(0.5);
        int done = 0;
        vars->props["onComplete"] = nativeFn(vm, "onDone",
            [](const std::vector<Value>&) -> Value { return Value::undefined(); }, &done);
        const Value tl = global(vm, "TweenLite");
        const Value tw = call(vm, tl.obj, "to",
                              {s, Value::num(0.25), Value::object(vars)});
        check(tw.kind == VKind::Object && tw.obj,
              "TweenLite.to returns a tween handle");
        for (int i = 0; i < 4; ++i) vm.tick();
        check(std::fabs(vm.getProperty(s.obj, "x").toNumber() - 200.0) < 1e-6,
              "TweenLite interpolates x to its target");
        check(std::fabs(s.obj->display->alpha - 0.5) < 1e-6,
              "TweenLite interpolates alpha to its target");
        check(done == 1, "TweenLite fires onComplete exactly once");

        const Value s2 = make(vm, "Sprite");
        auto vars2 = vm.newObject(ObjKind::Generic, "Object");
        vars2->props["x"] = Value::num(99);
        call(vm, tl.obj, "to", {s2, Value::num(1.0), Value::object(vars2)});
        call(vm, tl.obj, "killTweensOf", {s2});
        for (int i = 0; i < 4; ++i) vm.tick();
        check(vm.getProperty(s2.obj, "x").toNumber() == 0.0,
              "killTweensOf cancels pending tweens");

        const Value gs = global(vm, "com.greensock.TweenMax");
        check(gs.kind == VKind::Object && gs.obj && gs.obj->classDef,
              "greensock fully-qualified alias resolves to the class");
        int late = 0;
        const Value cb = nativeFn(vm, "late",
            [](const std::vector<Value>&) -> Value { return Value::undefined(); }, &late);
        call(vm, gs.obj, "delayedCall", {Value::num(0.1), cb});
        check(late == 0, "delayedCall does not fire immediately");
        for (int i = 0; i < 4; ++i) vm.tick();
        check(late == 1, "TweenMax.delayedCall fires after its delay");

        // ---- ease 必须真正参与插值 ----
        // 回归：vars.ease 此前只被列进"保留键"（不作为补间属性），但从未被调用，
        // 所有补间一律按 elapsed/duration 线性推进。换装道具的悬停放大用的是
        // gs.easing.Elastic（弹性回弹），线性化后动画特征全部丢失。
        {
            const Value s3 = make(vm, "Sprite");
            auto vars3 = vm.newObject(ObjKind::Generic, "Object");
            vars3->props["x"] = Value::num(100);
            int easeCalls = 0;
            // 恒定返回 0.5 的 ease：中间帧必须停在起止值的一半处，
            // 线性（k≈0.08）与"忽略 ease"都会让这个断言失败。
            vars3->props["ease"] = nativeFn(vm, "easeHalf",
                [](const std::vector<Value>&) -> Value { return Value::num(0.5); },
                &easeCalls);
            call(vm, tl.obj, "to", {s3, Value::num(1.0), Value::object(vars3)});
            vm.tick();
            const double mid = vm.getProperty(s3.obj, "x").toNumber();
            check(easeCalls > 0, "ease callback is invoked by the tween driver");
            check(std::fabs(mid - 50.0) < 1e-6,
                  "ease output drives the interpolated value (not linear)");
            for (int i = 0; i < 20; ++i) vm.tick();
            check(std::fabs(vm.getProperty(s3.obj, "x").toNumber() - 100.0) < 1e-6,
                  "the final frame lands exactly on the target regardless of ease");
        }

        // ---- EaseBase 实例走 getRatio(p)（GreenSock 的实际用法）----
        {
            const Value s4 = make(vm, "Sprite");
            auto vars4 = vm.newObject(ObjKind::Generic, "Object");
            vars4->props["x"] = Value::num(200);
            auto ecd = std::make_shared<f::ClassDef>();
            f::Member em;
            em.kind = f::Member::Method;
            em.name = "getRatio";
            static int ratioCalls = 0;
            em.native = [](std::shared_ptr<f::Object>,
                           const std::vector<Value>& a) -> Value {
                ++ratioCalls;
                // 平方曲线：p=0.5 -> 0.25
                const double p = a.empty() ? 0.0 : a[0].toNumber();
                return Value::num(p * p);
            };
            ecd->instanceMembers.push_back(em);
            auto easeObj = vm.newObject(ObjKind::Generic, "Elastic_easeOut");
            easeObj->cls = ecd;
            vars4->props["ease"] = Value::object(easeObj);
            ratioCalls = 0;
            call(vm, tl.obj, "to", {s4, Value::num(1.0), Value::object(vars4)});
            vm.tick();
            check(ratioCalls > 0, "EaseBase instance is driven through getRatio(p)");
            for (int i = 0; i < 20; ++i) vm.tick();
            check(std::fabs(vm.getProperty(s4.obj, "x").toNumber() - 200.0) < 1e-6,
                  "getRatio-based tween reaches its target");
        }
    }

    // ---- 换装道具的悬停光晕：new GlowFilter(...) 与 filters 赋值 ----
    // 回归：换装_448 的 onOver 做 `curOB.filters = [new GlowFilter(0xFFFFFF,1,10,10)]`
    // 再 `gotoAndPlay(2)`。此前 GlowFilter 等类根本没注册，而 constructObject 对未
    // 注册类不抛异常、只返回不带参数的空对象，构造实参全丢；且 filters 赋值没有接进
    // display->filters，渲染器看不到 —— 光晕两头都断。另注意单位：AS3 用像素，
    // Filter 内部约定是 twips，必须 ×20。
    {
        const Value gf = global(vm, "GlowFilter");
        check(gf.kind == VKind::Object && gf.obj && gf.obj->classDef,
              "GlowFilter class is registered");
        const Value filt = vm.constructObject(gf, {Value::uintv(0xFFFFFF), Value::num(1.0),
                                                   Value::num(10.0), Value::num(10.0)});
        check(filt.kind == VKind::Object && filt.obj, "new GlowFilter(...) yields an instance");
        if (filt.obj) {
            check(vm.getProperty(filt.obj, "blurX").toNumber() == 10.0,
                  "GlowFilter keeps its blurX constructor argument");
            check(vm.getProperty(filt.obj, "color").toNumber() == 16777215.0,
                  "GlowFilter keeps its color constructor argument");
            // AS3 默认值：未传 strength 时 GlowFilter 默认 2
            check(vm.getProperty(filt.obj, "strength").toNumber() == 2.0,
                  "GlowFilter applies AS3 default strength=2");
        }

        const Value sp = make(vm, "Sprite");
        auto arr = vm.newObject(ObjKind::Array, "Array");
        arr->props["$length"] = Value::intg(1);
        arr->props["0"] = filt;
        vm.setProperty(sp.obj, "filters", Value::object(arr));
        check(sp.obj->display != nullptr, "Sprite instance has a display object");
        check(sp.obj->display && sp.obj->display->filters &&
                  !sp.obj->display->filters->empty(),
              "assigning filters reaches the renderer's filter list");
        if (sp.obj->display && sp.obj->display->filters &&
            !sp.obj->display->filters->empty()) {
            const swf::Filter& f = (*sp.obj->display->filters)[0];
            check(f.kind == swf::FilterKind::Glow, "GlowFilter maps to FilterKind::Glow");
            check(f.color.r == 255 && f.color.g == 255 && f.color.b == 255,
                  "GlowFilter color 0xFFFFFF decodes to white RGB");
            check(f.blurX == 200.0f && f.blurY == 200.0f,
                  "blurX/blurY convert pixels -> twips (x20, renderer divides by 20)");
        }

        // 空数组 = 清除（onOut 里的 `curOB.filters = []` 走的就是这条路）
        auto emptyArr = vm.newObject(ObjKind::Array, "Array");
        emptyArr->props["$length"] = Value::intg(0);
        vm.setProperty(sp.obj, "filters", Value::object(emptyArr));
        check(sp.obj->display->filters == nullptr,
              "filters = [] clears the renderer's filter list");
    }

    // ---- 18. 加载 / 网络 / 媒体 / 本地存储 ----
    {
        const Value req = make(vm, "URLRequest", {Value::str("asset.swf")});
        check(vm.getProperty(req.obj, "url").toString() == "asset.swf",
              "URLRequest ctor stores url");
        const Value ul = make(vm, "URLLoader");
        call(vm, ul.obj, "load", {req});
        check(vm.getProperty(ul.obj, "data").kind == VKind::String,
              "URLLoader.load exposes data");
        const Value loader = make(vm, "Loader");
        call(vm, loader.obj, "load", {req});
        check(vm.getProperty(loader.obj, "content").kind == VKind::Object,
              "Loader.load creates content");
        const Value channel = call(vm, make(vm, "Sound").obj, "play", {});
        check(channel.kind == VKind::Object && channel.obj,
              "Sound.play returns a SoundChannel");
        const Value nc = make(vm, "NetConnection");
        check(call(vm, nc.obj, "connect", {Value::str("rtmp://x")}).toBool(),
              "NetConnection.connect");
        const Value so = call(vm, global(vm, "SharedObject").obj, "getLocal",
                              {Value::str("save")});
        check(so.kind == VKind::Object && so.obj &&
              vm.getProperty(so.obj, "data").kind == VKind::Object,
              "SharedObject.getLocal exposes a data object");
    }

    // ---- 嵌套时间轴实例的 stage 可达性（GreenSock ticker 注册的前提）----
    // 换装道具的悬停放大走 gs.TweenMax.to(mc, 1, {scaleX…, ease:Elastic.easeOut})。
    // GreenSock 11 的 addTickListener 是 `s = target.stage; if (s) s.addEventListener(
    // Event.ENTER_FRAME, tick)` —— stage 拿不到就**静默跳过**，补间被创建但永远不被
    // 推进，表现正是"悬停毫无动画、且没有任何报错"。实测日志里 enterFrame 的监听者
    // 只有预加载器的 progreso，GreenSock 的 tick 从未注册，故锁定此路径。
    // 层级按真实显示树复刻：root → main → stepMC → List → a6。
    {
        swf::Movie m;
        m.frameRate = 12;
        // 由内向外建 4 层精灵：33(a6) ← 32(List) ← 31(stepMC) ← 30(main)
        auto leaf = std::make_shared<swf::Character>();
        leaf->kind = swf::Character::Kind::Sprite;
        leaf->spriteFrames = {swf::Frame{}};
        m.characters[33] = leaf;
        for (int cid = 32; cid >= 30; --cid) {
            swf::DisplayItem child;
            child.depth = 1;
            child.characterId = static_cast<std::uint16_t>(cid + 1);
            swf::Frame fr;
            fr.items = {child};
            auto sp = std::make_shared<swf::Character>();
            sp->kind = swf::Character::Kind::Sprite;
            sp->spriteFrames = {fr};
            m.characters[cid] = sp;
        }
        swf::DisplayItem top;
        top.depth = 1; top.characterId = 30;
        swf::Frame tf;
        tf.items = {top};
        m.frames = {tf};

        swf::abc::AbcFile abc;
        f::Vm v(abc, m);
        v.run();
        auto root = std::make_shared<f::DisplayObject>();
        root->characterId = 0;
        v.stage.push_back(root);
        v.setClipFrame(root, 0);

        // 走到最深层 a6
        std::shared_ptr<f::DisplayObject> cur = root;
        for (int lvl = 0; lvl < 4 && !cur->children.empty(); ++lvl)
            cur = cur->children[0];
        check(cur != root && cur->characterId == 33, "4 层嵌套实例全部建成");

        // 诊断：从最深层沿 parent 上溯，必须能到达 stage 根（stage getter 依赖它）
        {
            std::shared_ptr<f::DisplayObject> up = cur;
            int hops = 0;
            while (up->parent) { up = up->parent; ++hops; }
            check(hops == 4 && up.get() == root.get(),
                  "4 层嵌套实例的 parent 链上溯到舞台根");
        }
        // 用公开 API 验证 stage getter 的祖先上溯：给最深层实例挂一个 AS3 包装对象。
        // 必须像运行时那样把 cls 指到 MovieClip 的 ClassDef，否则 stage/root 这些
        // 继承来的 getter 在成员链上查不到（那是测试构造缺陷，不是引擎行为）。
        auto wrap = v.newObject(ObjKind::MovieClip, "a6");
        wrap->display = cur;
        const Value mc = global(v, "MovieClip");
        if (mc.kind == VKind::Object && mc.obj && mc.obj->classDef)
            wrap->cls = mc.obj->classDef;
        const Value st = v.getProperty(wrap, "stage");
        check(st.kind == VKind::Object && st.obj && st.obj->display,
              "嵌套时间轴实例的 stage 非空（GreenSock 才能注册 ENTER_FRAME ticker）");
        const Value rt = v.getProperty(wrap, "root");
        check(rt.kind == VKind::Object && rt.obj, "同一实例的 root 非空");
        if (st.kind == VKind::Object && st.obj) {
            // GreenSock 拿到 stage 后立刻 addEventListener(Event.ENTER_FRAME, tick)。
            // 这里真的注册一个并跑几帧，确认 tick 会被调用（否则补间永远不推进）。
            int ticks = 0;
            const Value fn = nativeFn(v, "gsTick",
                [&ticks](const std::vector<Value>&) -> Value { ++ticks; return Value::undefined(); });
            call(v, st.obj, "addEventListener", {Value::str("enterFrame"), fn});
            for (int i = 0; i < 3; ++i) v.tick();
            check(ticks >= 3, "Stage 上的 ENTER_FRAME 监听每帧都被派发");
        }
    }

    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
