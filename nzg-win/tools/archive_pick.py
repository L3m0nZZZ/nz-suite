#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""选归档目录：nzg_win_versions 下**版本号最大**的那个。

为什么单独抽出来（2026-11-03）：
  soak_play.py 和 verify_refresh.py 原来各自写死一个 DEFAULT_ARCHIVE
  （都是 v1.13.2_71台_20261023）并优先返回它。只要那个目录还在，
  不带参数跑就**永远在测老版本** —— 那天连跑两轮长跑，一轮 FAIL 一轮 PASS，
  后来才发现全是 v1.13.2 的数据，新版本一次都没测到。这类"测了等于没测"
  比测试失败危险得多，因为报告上是绿的。

  两份重复的选版本逻辑本身就是病因，所以抽成一个模块，别再各写一份。

排序规则：按 (major, minor, patch) 数值比较。
  ⚠️ 不能用目录名字典序：'v1.9_...' > 'v1.10_...'（'9' > '1'）。
"""
import os
import re

ARCHIVE_ROOT = r"D:\ningz\nzg_win_versions"
EXE_NAME = "守播广播.exe"


def _ver_key(name):
    m = re.match(r"v(\d+)\.(\d+)(?:\.(\d+))?", name)
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3) or 0))


def list_archives(root=ARCHIVE_ROOT):
    """返回 [(version_key, dir_path)]，按版本号升序。只保留含 exe 的目录。"""
    out = []
    if not os.path.isdir(root):
        return out
    for name in os.listdir(root):
        d = os.path.join(root, name)
        if not os.path.isdir(d):
            continue
        k = _ver_key(name)
        if k is None:
            continue
        if not os.path.isfile(os.path.join(d, EXE_NAME)):
            continue
        out.append((k, d))
    out.sort()
    return out


def newest_archive(root=ARCHIVE_ROOT):
    """版本号最大的归档目录；找不到就抛 SystemExit（带可读原因）。"""
    archs = list_archives(root)
    if not archs:
        raise SystemExit("在 %s 下没找到含 %s 的归档目录" % (root, EXE_NAME))
    return archs[-1][1]
