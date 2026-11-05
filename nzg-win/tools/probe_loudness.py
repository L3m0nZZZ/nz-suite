#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""全台「实际响度」体检 —— 找出在推静音流的台

背景：nzg-win 报「没声音」，但日志里只有 HLS 的 skipping segments 警告、
没有任何断线/重连记录。这种情况程序侧是「一切正常在写 PCM」，
最可能就是源本身推的是静音流（有 AAC 帧、内容是 -inf dB）。

做法：用 ffmpeg volumedetect 对每台拉 6 秒，读 mean/max volume。
  max_volume < -50 dB  → 静音（源问题）
  max_volume < -35 dB  → 偏小，可疑
  否则正常

用法：
  python tools/probe_loudness.py               # 全部 HLS 台
  python tools/probe_loudness.py --id 51       # 只测某台
  python tools/probe_loudness.py --workers 8   # 并发数
"""
import sys, os, io, json, re, subprocess, time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

import check_sources as cs

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FFMPEG = r"D:\ningz\.buildtools\ffmpeg\ffmpeg.exe"
DUR = 6


def loudness(url, seconds=DUR, timeout=45):
    cmd = [FFMPEG, "-hide_banner", "-nostats", "-v", "info",
           "-rw_timeout", "15000000",
           "-i", url, "-t", str(seconds),
           "-af", "volumedetect", "-f", "null", "-"]
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout,
                           creationflags=0x08000000)  # CREATE_NO_WINDOW
    except subprocess.TimeoutExpired:
        return None, None, "超时 %ds" % timeout
    txt = (p.stdout + p.stderr).decode("utf-8", "ignore")
    mean = re.findall(r"mean_volume:\s*(-?[\d.]+|-inf) dB", txt)
    mx = re.findall(r"max_volume:\s*(-?[\d.]+|-inf) dB", txt)
    codec = re.search(r"Audio:\s*(\w+)", txt)
    rate = re.search(r"Audio:\s*\w+.*?(\d+) Hz", txt)
    ch = re.search(r"Audio:\s*\w+.*?(\d+) Hz,\s*(\w+)", txt)
    info = "%s %sHz %s" % (codec.group(1) if codec else "?",
                           rate.group(1) if rate else "?",
                           ch.group(2) if ch else "?")
    if not mx:
        # 没解出音频：把最后一条 error 抓出来
        err = [l for l in txt.splitlines() if "error" in l.lower() or "Error" in l]
        return None, info, (err[-1][:110] if err else "无音频输出")
    tof = lambda s: (-999.0 if s == "-inf" else float(s))
    return tof(mx[-1]), info, None


def judge(mx):
    if mx is None:
        return "ERR"
    if mx <= -50:
        return "静音"
    if mx <= -35:
        return "偏小"
    return "正常"


def main():
    args = sys.argv[1:]
    only = None
    workers = 6
    if "--id" in args:
        only = int(args[args.index("--id") + 1])
    if "--workers" in args:
        workers = int(args[args.index("--workers") + 1])

    cfg = json.load(io.open(os.path.join(ROOT, "config_default.json"), encoding="utf-8"))
    chs = cfg["channels"]
    if only:
        chs = [c for c in chs if c.get("id") == only]

    jobs = []
    for c in chs:
        raw = c.get("primary_url", "")
        # 直链 mp3/aac 也测，反正便宜；只跳过明显非音频
        try:
            u, _ = cs.resolve(c)
        except Exception:
            u = None
        if u:
            jobs.append((c, u))

    print("待测 %d 台，并发 %d，每台取 %ds\n" % (len(jobs), workers, DUR))
    results = []

    def work(item):
        c, u = item
        mx, info, err = loudness(u)
        return c, mx, info, err

    with ThreadPoolExecutor(max_workers=workers) as ex:
        for c, mx, info, err in ex.map(work, jobs):
            v = judge(mx)
            tag = {"正常": "  ", "偏小": "!!", "静音": "XX", "ERR": "??"}[v]
            results.append((v, c, mx, info, err))
            if err:
                print("[%s] id=%-3s %-24s %s" % (tag, c.get("id"), c.get("name"), err))
            else:
                print("[%s] id=%-3s %-24s max=%7.1f dB  %s" %
                      (tag, c.get("id"), c.get("name"), mx, info))

    print("\n==== 汇总 ====")
    from collections import Counter
    print(Counter(r[0] for r in results))
    bad = [r for r in results if r[0] in ("静音", "偏小", "ERR")]
    if bad:
        print("\n需要关注：")
        for v, c, mx, info, err in bad:
            print("  %-4s id=%-3s %-24s max=%s %s" %
                  (v, c.get("id"), c.get("name"),
                   ("%.1f dB" % mx) if mx is not None else "-", err or ""))


if __name__ == "__main__":
    main()
