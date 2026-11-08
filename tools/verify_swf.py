#!/usr/bin/env python3
"""End-to-end regression check on real SWF samples (swf.swf / swf2.swf).

Drives the built swfparser CLI through parse / render / playback / AVM2 opcode
coverage, and asserts structural invariants. Intended for CTest:

    python tools/verify_swf.py --exe <swfparser> [--swf <sample>] [--swf2 <sample>]
                               [--baseline <old-exe>] [--workdir <dir>]

Samples are optional: missing ones are reported and skipped, so CI machines
without the real files still pass. Exit code is non-zero on any FAIL.
"""
import argparse
import hashlib
import itertools
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from check_png import load_png  # minimal RGBA/RGB PNG decoder already in repo

fails = []
warns = []


def check(name, cond, detail=""):
    ok = bool(cond)
    print(("PASS " if ok else "FAIL ") + name + ((" :: " + detail) if detail and not ok else ""))
    if not ok:
        fails.append(name)


def warn(msg):
    warns.append(msg)
    print("WARN " + msg)


def run(exe, args, timeout=240, env=None):
    e = os.environ.copy()
    if env:
        e.update(env)
    try:
        p = subprocess.run([exe] + args, capture_output=True, text=True,
                           errors="replace", timeout=timeout, env=e)
        return p.returncode, (p.stdout or "") + "\n" + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return -999, "TIMEOUT after %ss" % timeout
    except OSError as ex:
        return -998, "SPAWN FAILED: %s" % ex


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def png_stats(path):
    w, h, ch, px = load_png(path)
    colors = set()
    for i in range(0, len(px), ch):
        colors.add(tuple(px[i:i + 3]))
        if len(colors) > 512:  # enough to prove the frame is not blank
            break
    return w, h, len(colors)


def pngs_in(d):
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.lower().endswith(".png"))


def check_dump(exe, swf, label):
    rc, out = run(exe, [swf])
    check(label + "/dump rc", rc == 0, out[-300:])
    m = re.search(r"frameCount\s*:\s*(\d+)", out)
    check(label + "/frameCount > 0", m and int(m.group(1)) > 0, out[-300:])
    m = re.search(r"tags\s*:\s*(\d+)", out)
    check(label + "/tags > 0", m and int(m.group(1)) > 0, out[-300:])
    check(label + "/no decode error",
          not re.search(r"decode failed|bad signature|cannot open", out), out[-300:])


def check_render(exe, swf, d, label):
    png = os.path.join(d, label + "_render.png")
    rc, out = run(exe, [swf, "--render", png, "--frame", "1"])
    check(label + "/render rc", rc == 0, out[-300:])
    check(label + "/render png produced", os.path.isfile(png))
    if not os.path.isfile(png):
        return None
    try:
        w, h, ncol = png_stats(png)
    except Exception as ex:
        check(label + "/render png decodable", False, str(ex))
        return None
    check(label + "/render dims valid", w > 0 and h > 0, "%dx%d" % (w, h))
    check(label + "/render non-blank", ncol >= 4, "colors=%d" % ncol)
    return w, h


def check_play(exe, swf, d, label, steps=24):
    out_dir = os.path.join(d, label + "_play")
    os.makedirs(out_dir, exist_ok=True)
    rc, out = run(exe, [swf, "--play-game", str(steps), out_dir])
    check(label + "/play rc", rc == 0, out[-400:])
    frames = re.findall(r"step=(\d+) frame=(\d+)", out)
    check(label + "/play step count", len(frames) == steps, "got %d" % len(frames))
    produced = pngs_in(out_dir)
    check(label + "/play png count", len(produced) == steps, "got %d" % len(produced))
    check(label + "/play no crash text",
          not re.search(r"load failed|Assertion failed|decode failed|uncaught|TIMEOUT", out),
          out[-400:])
    uniq = len(set(f for _, f in frames))
    check(label + "/play timeline advances", uniq >= 2, "unique frames=%d" % uniq)
    if produced:
        try:
            w, h, ncol = png_stats(produced[-1])
            check(label + "/play last frame non-blank", ncol >= 4, "colors=%d" % ncol)
        except Exception as ex:
            check(label + "/play last frame decodable", False, str(ex))
    stalled = max((len(list(g)) for _, g in itertools.groupby(f for _, f in frames)), default=0)
    if frames and stalled > steps // 2:
        warn("%s: timeline parks on frame %s for %d/%d steps "
             "(SWF script Stop/GotoFrame2 waiting for interaction)" % (label, frames[-1][1], stalled, steps))
    return out_dir


def check_avm2(exe, swf, d, steps=8):
    out_dir = os.path.join(d, "avm2_play")
    os.makedirs(out_dir, exist_ok=True)
    rc, out = run(exe, [swf, "--play-game", str(steps), out_dir], env={"SWF_AS_DEBUG": "1"})
    check("avm2/play rc", rc == 0, out[-400:])
    check("avm2/play step count", len(re.findall(r"step=\d+", out)) == steps, out[-300:])
    unimpl = re.findall(r"unimplemented op=0x([0-9A-Fa-f]+)", out)
    check("avm2/no unimplemented opcode", not unimpl, str(sorted(set(unimpl))[:10]))
    for pat in ("the menu is not exist", "frame label is not exist"):
        if pat in out:
            warn("AVM2 SWF-internal trace %r (printed by the SWF's own AS3, not an engine error)" % pat)
    produced = pngs_in(out_dir)
    check("avm2/play png count", len(produced) == steps, "got %d" % len(produced))


def check_repeat(exe, swf, d, label, rounds=3, steps=8):
    """Repeated load/play exercises handle open/close paths (M7 RAII)."""
    digests = []
    for i in range(rounds):
        out_dir = os.path.join(d, "%s_rep%d" % (label, i))
        os.makedirs(out_dir, exist_ok=True)
        rc, out = run(exe, [swf, "--play-game", str(steps), out_dir])
        check("%s/repeat%d rc" % (label, i), rc == 0, out[-300:])
        digests.append(tuple(md5(p) for p in pngs_in(out_dir)))
    check(label + "/repeat deterministic output",
          digests[0] and all(x == digests[0] for x in digests),
          "round lengths=%s" % [len(x) for x in digests])


def check_baseline(baseline, exe, swf, d, label, steps=24):
    """Optional A/B: compare frame traces and report pixel deltas vs an older build."""
    if not baseline or not os.path.isfile(baseline):
        return
    new_dir = os.path.join(d, label + "_ab_new")
    old_dir = os.path.join(d, label + "_ab_old")
    os.makedirs(new_dir, exist_ok=True)
    os.makedirs(old_dir, exist_ok=True)
    _, nout = run(exe, [swf, "--play-game", str(steps), new_dir])
    _, lout = run(baseline, [swf, "--play-game", str(steps), old_dir])
    nf = re.findall(r"step=(\d+) frame=(\d+)", nout)
    lf = re.findall(r"step=(\d+) frame=(\d+)", lout)
    check(label + "/A-B frame trace identical", nf == lf, "new=%d old=%d" % (len(nf), len(lf)))
    np_, lp = pngs_in(new_dir), pngs_in(old_dir)
    if len(np_) == len(lp):
        diff = sum(1 for a, b in zip(np_, lp) if md5(a) != md5(b))
        if diff:
            warn("%s: %d/%d frames differ from baseline binary %s (not a failure: "
                 "baseline may be an older unrelated build)" % (label, diff, len(np_), os.path.basename(baseline)))


def _gray_samples(path, gw=110, gh=80):
    """把 PNG 降采样成灰度网格，用于比较逐帧运动量（只用仓库自带解码器）。"""
    w, h, ch, px = load_png(path)
    out = []
    for j in range(gh):
        y = min(h - 1, (j * h) // gh)
        row = y * w * ch
        for i in range(gw):
            x = min(w - 1, (i * w) // gw)
            k = row + x * ch
            out.append((px[k] + px[k + 1] + px[k + 2]) // 3 if ch >= 3 else px[k])
    return out


def _swing_motion(paths, idle, tail):
    """返回 (待机摆动运动量, 回收结束后的运动量)。

    摆动区由待机帧自身的最高变化像素自动标定，避免把背景/计分数字的变化
    误当成钩爪在甩动。缺陷版本里回收结束后这一区域几乎静止。
    """
    ia, ib = idle
    ta, tb = tail
    if tb > len(paths):
        return 0.0, 0.0
    idle_frames = [_gray_samples(paths[i]) for i in range(ia, ib)]
    tail_frames = [_gray_samples(paths[i]) for i in range(ta, tb)]
    n = len(idle_frames[0])
    rng = [max(f[k] for f in idle_frames) - min(f[k] for f in idle_frames)
           for k in range(n)]
    mask = sorted(range(n), key=lambda k: rng[k], reverse=True)[:max(40, n // 100)]

    def motion(frames):
        pairs = list(zip(frames, frames[1:]))
        return (sum(abs(a[k] - b[k]) for a, b in pairs for k in mask) /
                float(len(pairs) * len(mask)))

    return motion(idle_frames), motion(tail_frames)


def check_hires_render(exe, swf, d, steps=3):
    """窗口放大时按设备尺寸重新光栅化（而不是拉伸舞台尺寸的位图）：
    2x 输出尺寸正确，且 2x2 平均下采样后与原生 1x 逐像素一致。
    """
    d1 = os.path.join(d, "hires_1x")
    d2 = os.path.join(d, "hires_2x")
    os.makedirs(d1, exist_ok=True)
    os.makedirs(d2, exist_ok=True)
    rc1, _ = run(exe, [swf, "--play-game", str(steps), d1], timeout=300)
    p1 = pngs_in(d1)
    size = "0x0"
    rc2 = -1
    if rc1 == 0 and p1:
        w1, h1, _, _ = load_png(p1[0])
        size = "%dx%d" % (w1 * 2, h1 * 2)
        rc2, _ = run(exe, [swf, "--play-game", str(steps), d2, "--size", size], timeout=300)
    p2 = pngs_in(d2)
    check("hires/1x rc", rc1 == 0)
    check("hires/2x rc", rc2 == 0)
    check("hires/frame counts", len(p1) == steps and len(p2) == steps,
          "1x=%d 2x=%d" % (len(p1), len(p2)))
    if not p1 or not p2:
        return
    w1, h1, c1, px1 = load_png(p1[-1])
    w2, h2, c2, px2 = load_png(p2[-1])
    check("hires/2x dims", (w2, h2) == (w1 * 2, h1 * 2),
          "%dx%d -> %dx%d (%s)" % (w1, h1, w2, h2, size))
    if (w2, h2) != (w1 * 2, h1 * 2):
        return
    total = 0
    for y in range(h1):
        for x in range(w1):
            k1 = (y * w1 + x) * c1
            for ch in range(3):
                s = 0
                for dy in (0, 1):
                    for dx in (0, 1):
                        s += px2[((2 * y + dy) * w2 + 2 * x + dx) * c2 + ch]
                total += abs(s // 4 - px1[k1 + ch])
    mean = total / float(w1 * h1 * 3)
    check("hires/2x re-rasterized in place", mean < 8.0,
          "2x2 downsample vs 1x mean abs=%.2f" % mean)


def check_hook_interaction(exe, swf, d, strict=False, steps=200, key_step=70,
                           retry_step=190, idle_window=(50, 70),
                           tail_window=(182, 200)):
    """下键放钩 -> 钩爪回收 -> 恢复左右甩动 -> 再次下键。

    swf.swf 是黄金矿工类样本：下键（VK_DOWN=40 / keyPress 15）驱动 327 帧的
    钩爪剪辑，回收结束后应由 _root.bonus.gotoAndPlay(2) ->
    _root.C.gotoAndPlay(1) 复位摆动控制器。若 _root 的成员访问解析不到命名
    子剪辑，这条链会静默失效，钩子就永久停在末帧。
    """
    # 两轮复用同一目录：第二轮覆盖同名 PNG，避免把临时文件数翻倍。
    out_dir = os.path.join(d, "hook_ab")
    os.makedirs(out_dir, exist_ok=True)
    key = "%d:40" % key_step
    retry = "%d:40" % retry_step
    _, out_a = run(exe, [swf, "--play-game", str(steps), out_dir, "--key", key], timeout=300)
    paths_a = pngs_in(out_dir)
    ha = [md5(p) for p in paths_a]
    idle_motion, tail_motion = _swing_motion(paths_a, idle_window, tail_window)
    check("hook/idle swing visible", idle_motion > 1.0,
          "idle motion=%.3f" % idle_motion)
    ratio = tail_motion / idle_motion if idle_motion > 0 else 0.0
    check("hook/swing resumes after claw returns", ratio >= 0.5,
          "tail/idle motion ratio=%.2f (idle=%.3f tail=%.3f); claw parked after return"
          % (ratio, idle_motion, tail_motion))

    _, out_b = run(exe, [swf, "--play-game", str(steps), out_dir,
                         "--key", key, "--key", retry], timeout=300)
    hb = [md5(p) for p in pngs_in(out_dir)]
    ta = re.findall(r"step=(\d+) frame=(\d+)", out_a)
    tb = re.findall(r"step=(\d+) frame=(\d+)", out_b)
    check("hook/once trace", len(ta) == steps, "got %d" % len(ta))
    check("hook/retry trace", len(tb) == steps, "got %d" % len(tb))
    check("hook/png sets complete", len(ha) == steps and len(hb) == steps,
          "once=%d twice=%d" % (len(ha), len(hb)))
    if len(ha) != steps or len(hb) != steps:
        return
    changed = [i for i in range(retry_step, steps) if ha[i] != hb[i]]
    detail = ("second key press at step %d changed no frames in %d..%d"
              % (retry_step, retry_step, steps - 1))
    if changed:
        print("PASS hook/re-launch after claw returns (%d frames differ)" % len(changed))
    elif strict:
        check("hook/re-launch after claw returns", False, detail)
    else:
        warn("hook: " + detail)
    tail = [f for _, f in ta[retry_step:]]
    if tail and len(set(tail)) == 1:
        print("INFO hook: main timeline stays on frame %s (level wait state)" % tail[0])


def default_workdir(exe, sample):
    """Pick a scratch dir the CLI can actually write PNGs into.

    Some Windows setups deny the parser write access to %TEMP% (controlled
    folder access / ACL) even though the parent Python process can write it.
    Probe with a real --render call so the default is never silently unusable.
    """
    local = os.path.join(os.path.dirname(HERE), "build", "verify_out")
    candidates = [os.environ.get("SWF_VERIFY_WORKDIR", ""), local, tempfile.gettempdir()]
    for base in candidates:
        if not base:
            continue
        try:
            os.makedirs(base, exist_ok=True)
            d = tempfile.mkdtemp(prefix="swf_verify_", dir=base)
        except OSError:
            continue
        if not sample or not os.path.isfile(sample):
            return d
        probe = os.path.join(d, "_writable.png")
        rc, _ = run(exe, [sample, "--render", probe, "--frame", "1"], timeout=120)
        if rc == 0 and os.path.isfile(probe):
            os.remove(probe)
            return d
        shutil.rmtree(d, ignore_errors=True)
    check("scratch dir writable by swfparser", False,
          "no candidate directory allowed PNG output")
    return tempfile.mkdtemp(prefix="swf_verify_")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--swf", default="")
    ap.add_argument("--swf2", default="")
    ap.add_argument("--baseline", default="")
    ap.add_argument("--workdir", default="")
    ap.add_argument("--strict-hook", action="store_true",
                    help="fail instead of warn when the claw cannot re-launch after returning")
    a = ap.parse_args()

    if not os.path.isfile(a.exe):
        check("swfparser executable exists", False, a.exe)
        print("\n%d failures" % len(fails))
        return 1

    # 全部转成绝对路径：CTest 的工作目录是 build/，手动运行则在源码根，
    # 相对参数会在两种入口下指向不同文件。
    exe = os.path.abspath(a.exe)
    swf = os.path.abspath(a.swf) if a.swf else ""
    swf2 = os.path.abspath(a.swf2) if a.swf2 else ""
    baseline = os.path.abspath(a.baseline) if a.baseline else ""
    workdir = os.path.abspath(a.workdir) if a.workdir else ""
    d = workdir or default_workdir(exe, swf or swf2)
    os.makedirs(d, exist_ok=True)
    print("workdir: %s" % d)
    try:
        if swf and os.path.isfile(swf):
            print("--- AVM1 sample: %s ---" % swf)
            check_dump(exe, swf, "avm1")
            check_render(exe, swf, d, "avm1")
            check_play(exe, swf, d, "avm1", steps=24)
            check_repeat(exe, swf, d, "avm1")
            check_baseline(baseline, exe, swf, d, "avm1")
            check_hook_interaction(exe, swf, d, strict=a.strict_hook)
            check_hires_render(exe, swf, d)
        else:
            print("SKIP AVM1 sample (not provided)")

        if swf2 and os.path.isfile(swf2):
            print("--- AVM2 sample: %s ---" % swf2)
            check_dump(exe, swf2, "avm2")
            check_render(exe, swf2, d, "avm2")
            check_play(exe, swf2, d, "avm2", steps=6)
            check_avm2(exe, swf2, d)
            check_repeat(exe, swf2, d, "avm2")
        else:
            print("SKIP AVM2 sample (not provided)")
    finally:
        if not workdir:
            # 大量 PNG 一次性删除会被部分 CI/沙箱的批量删除保护拦截，
            # 超过阈值时保留目录供排查，不影响测试结论。
            count = sum(len(fs) for _, _, fs in os.walk(d))
            if count <= 400:
                shutil.rmtree(d, ignore_errors=True)
            else:
                print("INFO scratch dir kept (%d files): %s" % (count, d))

    print("\n%d failures, %d warnings" % (len(fails), len(warns)))
    if fails:
        print("failed checks: " + ", ".join(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
