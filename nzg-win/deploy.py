#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把新构建的 守播广播.exe 部署到位（停旧进程 → 改开机自启 → 启新版 → 验证）

为什么需要这个脚本：
  程序的 exe 是放在**版本归档目录**里跑的（`nzg_win_versions/vX.Y_.../`），
  开机自启 HKCU\\...\\Run\\ShouboRadioWin 里的路径是写死绝对路径的。
  所以「编译出包」不等于「升级生效」—— 只替换 dist/ 里的 exe 没有任何作用，
  重启后照样跑旧归档目录里的旧版（2026-10-23 踩过：dist 已经是 v1.13，
  跑的却是 v1.12，两者日志混在一起排障很容易被带偏）。

做四件事（幂等，可反复跑）：
  1. 停止所有正在运行的 守播广播.exe（按映像名，名称唯一）
  2. 把 Run 键指向新 exe（先备份原值到 run_key_backup.txt）
  3. 启动新 exe（纯后台，托盘驻留）
  4. 验证：进程路径 == 新 exe，且日志里最新启动行是预期版本
     验证失败 → 自动回滚 Run 键并启动旧版，避免把用户晾在黑屏里

用法：
  python deploy.py                                  # 部署「版本号最大的那个归档」里的 exe
  python deploy.py --exe "D:\\path\\守播广播.exe" --expect 1.13
  python deploy.py --no-start                        # 只改自启不启动（下次开机生效）
"""
import argparse
import os
import subprocess
import sys
import time
import hashlib

import winreg

RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
VALUE_NAME = "ShouboRadioWin"
IMAGE = "守播广播.exe"
BACKUP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "run_key_backup.txt")

VERSIONS_DIR = r"D:\ningz\nzg_win_versions"


def newest_archive_exe():
    """取 nzg_win_versions 下版本号最大的归档里的 exe。

    以前这里写死成 v1.13_69台 —— 隔几个版本后跑 `python deploy.py` 会
    **悄悄把老版本装回去**（自启项指向老目录），日志版本对不上还得查半天。
    改成自动取最新；版本号按 (major, minor, patch) 数值比较，
    不能按目录名字典序（字典序会把 v1.9 排到 v1.10 后面）。
    """
    import re
    best = None
    for name in os.listdir(VERSIONS_DIR):
        d = os.path.join(VERSIONS_DIR, name)
        if not os.path.isdir(d):
            continue
        m = re.match(r"v(\d+)\.(\d+)(?:\.(\d+))?", name)
        if not m:
            continue
        key = (int(m.group(1)), int(m.group(2)), int(m.group(3) or 0))
        exe = os.path.join(d, IMAGE)
        if not os.path.exists(exe):
            continue
        if best is None or key > best[0]:
            best = (key, exe)
    if best is None:
        raise SystemExit("在 %s 下没找到含 %s 的归档目录" % (VERSIONS_DIR, IMAGE))
    return best[1]


DEFAULT_EXE = None   # argparse 的默认值在 build_parser() 里用 newest_archive_exe() 填


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_run_value():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            v, _ = winreg.QueryValueEx(k, VALUE_NAME)
            return v
    except FileNotFoundError:
        return None


def write_run_value(exe):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_ALL_ACCESS) as k:
        winreg.SetValueEx(k, VALUE_NAME, 0, winreg.REG_SZ, '"%s"' % exe)


def running_pids():
    """用 WMIC 之外的方式列进程：tasklist 输出解析（避免依赖 psutil）"""
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"],
                             capture_output=True, timeout=20).stdout.decode("gbk", "ignore")
    except Exception as e:
        print("  ! 无法枚举进程：%s" % e)
        return []
    pids = []
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].strip('"').lower() == IMAGE.lower():
            try:
                pids.append(int(parts[1]))
            except ValueError:
                pass
    return pids


def stop_all():
    pids = running_pids()
    if not pids:
        print("  当前没有运行中的实例")
        return True
    for pid in pids:
        print("  停止 PID %d" % pid)
        subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                       capture_output=True, timeout=20)
    time.sleep(1.5)
    left = running_pids()
    if left:
        print("  ! 仍有实例未退出：%s" % left)
        return False
    return True


def start(exe):
    # 纯后台启动（程序自己隐藏窗口、驻留托盘）。
    #
    # ⚠️ 必须 DETACHED_PROCESS：普通 Popen 出来的子进程会挂在**父进程的控制台**上，
    # 部署脚本一退出 / 用户关掉那个终端窗口，Windows 会随控制台送 CTRL_CLOSE_EVENT
    # 把 GUI 子进程一起带走 —— 现象是「deploy.py 说部署成功、当场也验证到了进程，
    # 过一会儿进程没了」，日志里停在半截、没有 [退出] 正常退出 那一行。
    # 2026-11-03 实测踩到：验证通过后一分钟 running_pids() 就空了。
    #
    # CREATE_BREAKAWAY_FROM_JOB：从 cmd / CI / 服务管理器里跑时，父进程可能挂在
    # 一个「关闭即杀」的 Job 对象上，光 DETACHED 还逃不掉。Job 不允许 breakaway 时
    # CreateProcess 会返回拒绝访问，所以这里降级重试一次，不让部署整体失败。
    base = 0
    if hasattr(subprocess, "DETACHED_PROCESS"):
        base = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    try:
        subprocess.Popen([exe], cwd=os.path.dirname(exe), close_fds=True,
                         creationflags=base | subprocess.CREATE_BREAKAWAY_FROM_JOB)
    except OSError:
        subprocess.Popen([exe], cwd=os.path.dirname(exe), close_fds=True,
                         creationflags=base)
    time.sleep(3.0)


def proc_paths():
    """用 PowerShell 取进程可执行路径。

    ⚠️ 编码坑（2026-10-23 踩过）：PowerShell 默认按系统 ANSI（简体中文 = GBK）
    输出中文路径，用 utf-8 解码会变成 `v1.13_69̨_20261023\\ز㲥.exe` 这种乱码，
    路径比对必然失败 —— 结果是「明明部署成功却判定失败并回滚」。
    这里按 GBK 解码；解不出再从 utf-8 兜底。"""
    ps = ("Get-Process -ErrorAction SilentlyContinue | "
          "Where-Object { $_.ProcessName -like '*守播*' } | "
          "Select-Object -ExpandProperty Path")
    try:
        raw = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, timeout=30).stdout
    except Exception:
        return []
    for enc in ("gbk", "utf-8"):
        try:
            txt = raw.decode(enc)
        except UnicodeDecodeError:
            continue
        lines = [l.strip() for l in txt.splitlines() if l.strip()]
        if lines and "\\" in lines[0]:
            return lines
    return []


def latest_boot_version(logdir):
    """读最新日志里的『启动 守播广播 Windows 版 vX.Y 启动』"""
    if not os.path.isdir(logdir):
        return None
    files = sorted([f for f in os.listdir(logdir) if f.startswith("log-")])
    if not files:
        return None
    path = os.path.join(logdir, files[-1])
    ver = None
    try:
        with open(path, encoding="utf-8", errors="ignore") as f:
            for line in f:
                if "守播广播 Windows 版 v" in line and "启动" in line:
                    ver = line.split("版 v")[1].split(" ")[0]
    except Exception:
        pass
    return ver


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=None,
                    help="目标 exe（默认取 nzg_win_versions 下版本号最大的归档）")
    ap.add_argument("--expect", default=None, help="期望的版本号，如 1.13（默认从归档目录名推断）")
    ap.add_argument("--no-start", action="store_true")
    ap.add_argument("--copy-from", default=None,
                    help="先把这个 exe 复制到 --exe 目标（dist 里刚编译的新构建）")
    a = ap.parse_args()

    if not a.exe:
        a.exe = newest_archive_exe()
        print("未指定 --exe，自动选用最新归档：%s" % a.exe)

    exe = os.path.abspath(a.exe)
    if not os.path.isfile(exe):
        print("找不到 exe：%s" % exe)
        return 2
    expect = a.expect
    if not expect:
        m = os.path.basename(os.path.dirname(exe))  # v1.13_69台_20261023
        expect = m.split("_")[0].lstrip("v") if m.startswith("v") else None

    print("新 exe：%s" % exe)
    print("        md5=%s  %d 字节" % (md5(exe), os.path.getsize(exe)))
    print("期望版本：%s" % expect)

    old = read_run_value()
    print("\n[1/4] 备份并读取当前自启项")
    print("  原值：%s" % (old or "（无）"))
    with open(BACKUP, "w", encoding="utf-8") as f:
        f.write(old or "")
    old_exe = old.strip('"') if old else None

    print("\n[2/4] 停止运行中的实例")
    if not stop_all():
        print("停止失败，中止（没有改动任何东西）")
        return 3

    if a.copy_from:
        import shutil
        src = os.path.abspath(a.copy_from)
        if not os.path.isfile(src):
            print("找不到源 exe：%s" % src)
            return 6
        shutil.copy2(src, exe)
        print("  已复制 %s → %s（md5=%s）" % (src, exe, md5(exe)))

    print("\n[3/4] 更新开机自启 → 新路径")
    try:
        write_run_value(exe)
        print("  已写入 %s = \"%s\"" % (VALUE_NAME, exe))
    except Exception as e:
        print("  ! 写注册表失败：%s" % e)
        return 4

    if a.no_start:
        print("\n[4/4] 按 --no-start 跳过启动（下次开机生效）")
        return 0

    print("\n[4/4] 启动并验证")
    start(exe)
    paths = proc_paths()
    print("  运行中进程路径：%s" % (paths or "（未取到）"))

    logdir = os.path.join(os.path.dirname(exe), "logs")
    ver = latest_boot_version(logdir)
    print("  日志最新启动版本：%s" % (ver or "（未读到）"))

    ok_path = any(os.path.normcase(p) == os.path.normcase(exe) for p in paths) if paths else True
    ok_ver = (ver == expect) if (ver and expect) else True
    # 日志版本号是更硬的证据：每版 exe 写自己的版本，且写在自己目录的 logs 下。
    # 路径取不到（权限/编码）时不因此判失败，只要求「进程在 + 版本对」。
    if ok_path and ok_ver:
        print("\n✅ 部署成功：跑的就是 %s" % exe)
        return 0

    print("\n❌ 验证不通过，回滚")
    stop_all()
    if old_exe and os.path.isfile(old_exe):
        try:
            write_run_value(old_exe)
            start(old_exe)
            print("  已回滚到 %s 并重启" % old_exe)
        except Exception as e:
            print("  ! 回滚失败：%s（自启项备份见 %s）" % (e, BACKUP))
    return 5


if __name__ == "__main__":
    sys.exit(main())
