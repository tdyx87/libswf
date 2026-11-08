#!/usr/bin/env python3
"""参考帧采集：启动官方 Adobe Flash Player projector 播放 SWF，按时间表发送点击并截屏。

用法: python tools/fp_capture.py <swf路径> <输出目录> [--grace 2.0] [--schedule 菜单:60,...]
      [--burst fps:秒]   连拍模式：按 fps 帧率连续截图 burst_NNNN.png（供 GIF 对比），
                         点击时间表按墙钟时刻照常执行

时间表以"游戏帧号"为单位（24fps），换算为 grace + frame/24 秒的墙钟时刻：
  click<frame>:<x>:<y>  在该帧时刻向窗口客户区发送一次左键点击
  shot<frame>:<name>    在该帧时刻截取窗口客户区保存为 <name>.png

默认时间表与 swfparser --play-game 的验证脚本一致（swf2.swf 全流程）。
"""
import os
import subprocess
import sys
import time

import mss
import win32api
import win32con
import win32gui
import win32process

FPS = 24.0
STAGE_W, STAGE_H = 1280, 768

DEFAULT_SCHEDULE = [
    'shot60:menu',
    'click100:478:700',
    'shot130:select',
    'click150:225:375',
    'shot180:select2',
    'click200:1160:550',
    'shot240:bath',
    'shot320:bath2',
    'click260:94:543',
    'shot440:step3',
    'click460:650:430',
    'shot500:show',
    'click560:1169:596',
    'shot580:end',
    'shot604:end2',
]


def find_hwnd(pid, deadline):
    found = []

    def cb(hwnd, _):
        if not win32gui.IsWindowVisible(hwnd):
            return True
        _, wp = win32process.GetWindowThreadProcessId(hwnd)
        if wp == pid:
            found.append(hwnd)
        return True

    while time.time() < deadline:
        found.clear()
        win32gui.EnumWindows(cb, None)
        if found:
            return found[0]
        time.sleep(0.2)
    return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    swf, outdir = args[0], args[1]
    grace = 2.0
    if '--grace' in sys.argv:
        grace = float(sys.argv[sys.argv.index('--grace') + 1])
    schedule = DEFAULT_SCHEDULE
    if '--schedule' in sys.argv:
        schedule = sys.argv[sys.argv.index('--schedule') + 1].split(',')
    burst_fps, burst_secs = None, None
    if '--burst' in sys.argv:
        b = sys.argv[sys.argv.index('--burst') + 1].split(':')
        burst_fps, burst_secs = float(b[0]), float(b[1])
    os.makedirs(outdir, exist_ok=True)

    fp = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'flashplayer.exe')
    proc = subprocess.Popen([fp, swf])
    hwnd = find_hwnd(proc.pid, time.time() + 15)
    if not hwnd:
        print('flash player window not found')
        proc.kill()
        return 1
    try:
        win32gui.SetForegroundWindow(hwnd)
    except Exception:
        pass
    time.sleep(0.5)
    left, top, right, bottom = win32gui.GetClientRect(hwnd)
    ox, oy = win32gui.ClientToScreen(hwnd, (0, 0))
    cw, ch = right - left, bottom - top
    sx, sy = cw / STAGE_W, ch / STAGE_H
    print('window client %dx%d scale=(%.3f,%.3f) origin=(%d,%d)' % (cw, ch, sx, sy, ox, oy))

    events = []
    for item in schedule:
        if item.startswith('click'):
            f, x, y = item[5:].split(':')
            events.append((grace + int(f) / FPS, 'click', int(x), int(y), ''))
        else:
            f, name = item[4:].split(':')
            events.append((grace + int(f) / FPS, 'shot', 0, 0, name))
    events.sort(key=lambda e: e[0])

    def ensure_fg():
        """确认官方播放器窗口处于前台；尝试置前，失败则返回 False。
        防护：真实全局输入（SetCursorPos+mouse_event）会命中当时的前台窗口，
        若播放器不在前台则必须跳过，避免误点/误截其它应用窗口。"""
        try:
            if win32gui.GetForegroundWindow() == hwnd:
                return True
            win32gui.SetForegroundWindow(hwnd)
            time.sleep(0.15)
            return win32gui.GetForegroundWindow() == hwnd
        except Exception:
            return False

    t0 = time.time()
    sct = mss.mss()

    def do_click(x, y):
        if not ensure_fg():
            print('t=%.2f click SKIPPED (flash window not foreground)' % (time.time() - t0))
            return
        lx, ly = int(x * sx), int(y * sy)
        sp = (ox + lx, oy + ly)
        # 真实全局输入：官方播放器忽略 PostMessage 合成消息（点击不生效）。
        win32api.SetCursorPos(sp)
        time.sleep(0.05)
        win32api.mouse_event(win32con.MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.08)
        win32api.mouse_event(win32con.MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
        print('t=%.2f click stage=(%d,%d) screen=%s' % (time.time() - t0, x, y, sp))

    def do_shot(name):
        if not ensure_fg():
            print('t=%.2f shot SKIPPED (flash window not foreground)' % (time.time() - t0))
            return
        region = {'left': ox, 'top': oy, 'width': cw, 'height': ch}
        img = sct.grab(region)
        path = os.path.join(outdir, name + '.png')
        mss.tools.to_png(img.rgb, img.size, output=path)
        print('t=%.2f shot %s' % (time.time() - t0, path))

    if burst_fps:
        # 连拍模式：按 fps 帧率循环，每个时刻先执行到期的点击，再抓一帧
        ev_i, n = 0, int(burst_secs * burst_fps)
        for i in range(n):
            target = t0 + i / burst_fps
            now = time.time()
            if target > now:
                time.sleep(target - now)
            now = time.time()
            while ev_i < len(events) and t0 + events[ev_i][0] <= now:
                t, kind, x, y, name = events[ev_i]
                if kind == 'click':
                    do_click(x, y)
                else:
                    do_shot(name)
                ev_i += 1
            region = {'left': ox, 'top': oy, 'width': cw, 'height': ch}
            img = sct.grab(region)   # 抓屏不依赖前台焦点；窗口被遮挡时该帧内容无效
            path = os.path.join(outdir, 'burst_%04d.png' % i)
            mss.tools.to_png(img.rgb, img.size, output=path)
        while ev_i < len(events):   # 尾部剩余事件
            t, kind, x, y, name = events[ev_i]
            if kind == 'click':
                do_click(x, y)
            else:
                do_shot(name)
            ev_i += 1
        print('burst done: %d frames -> %s' % (n, outdir))
    else:
        for t, kind, x, y, name in events:
            wait = t0 + t - time.time()
            if wait > 0:
                time.sleep(wait)
            if kind == 'click':
                do_click(x, y)
            else:
                do_shot(name)
    proc.kill()
    return 0


if __name__ == '__main__':
    sys.exit(main())
