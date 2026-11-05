#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""长跑验证（soak）：让播放器真跑 N 小时，统计重建/换票/静音空档，给出 PASS/FAIL。

为什么需要它：
  「播半天后一直不动」这类故障，selftest 抓不到（它每台只跑十几秒），
  唯一可信的验证是**连续播几小时**。以前只能靠耳朵听，这个脚本把它变成一条命令。

做法：
  把 exe 复制到独立临时目录跑（**不碰正式归档的 state.json / logs**），
  外部 config.json 只改音量（默认 0 = 静音，夜里跑不吵人），
  其它全部用 exe 内置配置；结束时解析日志给出统计。

用法：
    python tools/soak_play.py --minutes 240                 # 4 小时，静音，单县
    python tools/soak_play.py --minutes 60 --channel 巨野
    python tools/soak_play.py --minutes 30 --volume 50      # 出声（会真的响）
    python tools/soak_play.py --minutes 200 --expect-refresh   # 验证 3 小时换票

⚠️ 跑之前先退出正式实例，测试期间也别启动它 —— 单实例互斥会让两边互相唤醒。
⚠️ 单实例检查在自检分派之后，但普通启动仍会抢互斥量，所以必须独占。
"""
import argparse, datetime, json, os, re, shutil, subprocess, sys, time

EXE_NAME = "守播广播.exe"
ARCHIVE_ROOT = r"D:\ningz\nzg_win_versions"
WORK_ROOT = r"D:\ningz\_capture\soak"

RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_VALUE = "ShouboRadioWin"


def snapshot_run_key():
    """记下开机自启项原来的值（没有就返回 None）。失败不影响长跑。"""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            v, _ = winreg.QueryValueEx(k, RUN_VALUE)
            return v
    except Exception:
        return None


def restore_run_key(saved):
    """把自启项改回去。程序在临时目录里跑完会把它改成 temp 路径，必须还原。"""
    if not saved:
        return
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0,
                            winreg.KEY_ALL_ACCESS) as k:
            cur, _ = winreg.QueryValueEx(k, RUN_VALUE)
            if cur == saved:
                return
            winreg.SetValueEx(k, RUN_VALUE, 0, winreg.REG_SZ, saved)
            print("已还原开机自启项 → %s" % saved)
    except Exception as e:
        print("⚠️ 还原开机自启项失败（%s），请手动检查：%s\\%s" % (e, RUN_KEY, RUN_VALUE))


def pick_archive(arg):
    if arg:
        return arg
    # ⚠️ 原来这里优先返回写死的 DEFAULT_ARCHIVE（v1.13.2）——只要那个目录还在，
    # 不带 --archive 的长跑就**永远在测 v1.13.2**，新版本根本没被验证过。
    # 现在统一走 archive_pick（按版本号数值取最大）。
    from archive_pick import newest_archive
    return newest_archive(ARCHIVE_ROOT)


def channel_id(cfg_path, needle):
    d = json.load(open(cfg_path, encoding="utf-8"))
    hits = [c for c in d["channels"]
            if needle in c.get("name", "") or needle == c.get("region", "")]
    if not hits:
        sys.exit("找不到台：%s" % needle)
    return int(hits[0]["id"]), hits[0]["name"]


def stop_all():
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"],
                         capture_output=True, timeout=20).stdout.decode("gbk", "ignore")
    killed = 0
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].strip('"').lower() == EXE_NAME.lower():
            subprocess.run(["taskkill", "/F", "/PID", parts[1]], capture_output=True)
            killed += 1
    if killed:
        time.sleep(1.5)
    return killed


def parse(log_path):
    """返回事件列表 [(ts, kind, line)]，kind ∈ 启动/播放/换票/重建/断线/重连/失败"""
    ev = []
    if not os.path.isfile(log_path):
        return ev
    pat = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\]\s*\[([^\]]+)\]\s*(.*)$")
    for ln in open(log_path, encoding="utf-8", errors="replace"):
        m = pat.match(ln.rstrip("\n"))
        if not m:
            continue
        ts, cat, msg = m.group(1), m.group(2), m.group(3)
        kind = None
        if cat == "启动" and "启动" in msg:
            kind = "启动"
        elif cat == "播放" and msg.startswith("开始播放"):
            kind = "播放"
        elif cat == "换票":
            kind = "换票"
        elif cat == "重启":
            kind = "重建"
        elif cat in ("断线", "重连") or "失败" in msg:
            kind = cat if cat in ("断线", "重连") else "失败"
        if kind:
            ev.append((datetime.datetime.strptime(ts, "%Y-%m-%d %H:%M:%S"), kind, msg))
    return ev


def silence_windows(ev):
    """重建 → 下一次「开始播放」之间的静音时长（秒）"""
    out, pending = [], None
    for ts, kind, msg in ev:
        if kind == "重建":
            pending = ts
        elif kind == "播放" and pending:
            out.append((pending, (ts - pending).total_seconds()))
            pending = None
    return out


def count_skips(log_path):
    """统计 HLS「分片过期跳过」丢掉的分片数（2026-11-03 加）。

    两个版本的日志写法不同，都要吃下：
      v1.14.1 及更早：`[FFmpeg] 警告：... skipping 3 segments ahead, expired from playlists`
      v1.15 起（60 秒节流）：`[FFmpeg] 警告：HLS 分片过期跳过（近 60 秒累计 2 次；...）`
    返回 (总跳过分片数, 触发次数)。
    """
    total, hits = 0, 0
    if not os.path.isfile(log_path):
        return 0, 0
    for ln in open(log_path, encoding="utf-8", errors="replace"):
        if "expired from playlists" in ln:
            m = re.search(r"skipping\s+(\d+)\s+segments", ln)
            if m:
                total += int(m.group(1))
                hits += 1
        elif "HLS 分片过期跳过" in ln:
            m = re.search(r"累计\s+(\d+)\s+次", ln)
            if m:
                total += int(m.group(1))
                hits += 1
    return total, hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=240)
    ap.add_argument("--channel", default="单县", help="台名或地区子串，默认单县")
    ap.add_argument("--volume", type=int, default=0, help="启动音量 0-100，默认 0（静音）")
    ap.add_argument("--archive", default=None)
    ap.add_argument("--expect-refresh", action="store_true",
                    help="预期期间至少触发一次换票（跑 ≥ 3.1 小时时用）")
    ap.add_argument("--clean", action="store_true",
                    help="跑完删掉临时目录（默认**保留**，方便回头查日志）")
    a = ap.parse_args()

    arch = pick_archive(a.archive)
    src_exe = os.path.join(arch, EXE_NAME)
    src_cfg = os.path.join(arch, "config_default.json")
    if not os.path.isfile(src_cfg):
        sys.exit("归档里没有 config_default.json")

    cid, cname = channel_id(src_cfg, a.channel)
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M")
    work = os.path.join(WORK_ROOT, stamp)
    os.makedirs(work, exist_ok=True)
    exe = os.path.join(work, EXE_NAME)
    shutil.copy2(src_exe, exe)

    cfg = json.load(open(src_cfg, encoding="utf-8"))
    cfg["audio"]["startup_volume"] = a.volume
    cfg["audio"]["remember_volume"] = False
    json.dump(cfg, open(os.path.join(work, "config.json"), "w", encoding="utf-8"),
              ensure_ascii=False, indent=2)
    json.dump({"current_channel_id": cid, "volume": a.volume},
              open(os.path.join(work, "state.json"), "w", encoding="utf-8"))

    print("归档：%s" % arch)
    print("隔离目录：%s" % work)
    print("被测 exe：%s  (%d 字节)" % (src_exe, os.path.getsize(src_exe)))
    print("台：%s（id=%d）  音量：%d%%  时长：%.1f 分钟" % (cname, cid, a.volume, a.minutes))
    print("换票周期：%s 秒（内置配置）" % cfg["resilience"].get("dynamic_refresh_sec"))
    print()

    n = stop_all()
    if n:
        print("已停止 %d 个正在运行的实例" % n)

    log = os.path.join(work, "logs")
    # 程序启动时会把自己的路径写进开机自启项（设计如此：路径跟随 exe）。
    # 长跑跑的是临时目录里的副本 ⇒ 自启项会被改成 temp 路径，temp 一删就指向空气，
    # 下次开机播放器根本起不来（2026-11-03 踩到）。所以跑前快照、跑完还原。
    run_key_saved = snapshot_run_key()
    proc = subprocess.Popen([exe], cwd=work, close_fds=True)
    t0 = time.time()
    total = a.minutes * 60
    print("已启动，开始计时（%s）。中途 Ctrl-C 也可出报告。"
          % datetime.datetime.now().strftime("%H:%M:%S"))

    try:
        while True:
            time.sleep(30)
            el = time.time() - t0
            today = datetime.date.today().isoformat()
            ev = parse(os.path.join(log, "log-%s.txt" % today))
            c = {k: sum(1 for e in ev if e[1] == k) for k in ("播放", "换票", "重建", "断线", "重连", "失败")}
            print("  %5.1f 分钟  播放=%d 重建=%d 换票=%d 断线=%d 失败=%d" % (
                el / 60, c["播放"], c["重建"], c["换票"], c["断线"], c["失败"]), flush=True)
            if el >= total:
                break
            if proc.poll() is not None:
                print("  ⚠️ 进程已退出（code=%s），提前结束" % proc.returncode)
                break
    except KeyboardInterrupt:
        print("\n被中断，照常出报告")

    elapsed = time.time() - t0
    stop_all()
    restore_run_key(run_key_saved)

    today = datetime.date.today().isoformat()
    ev = parse(os.path.join(log, "log-%s.txt" % today))
    sil = silence_windows(ev)
    cnt = {k: sum(1 for e in ev if e[1] == k) for k in ("播放", "换票", "重建", "断线", "重连", "失败")}

    print("\n================ 长跑报告 ================")
    # 版本号从日志的「启动」行里读，证明**这份报告测的是哪一版**，
    # 不靠传入路径猜（曾因 DEFAULT_ARCHIVE 写死 v1.13.2，长跑一直在测老版本）。
    ver = "(日志里没抓到)"
    try:
        with open(os.path.join(log, "log-%s.txt" % today), encoding="utf-8", errors="ignore") as f:
            for line in f:
                mm = re.search(r"Windows 版 (v[\d.]+) 启动", line)
                if mm:
                    ver = mm.group(1)
                    break
    except Exception:
        pass
    print("被测版本    ：%s" % ver)
    print("时长        ：%.1f 分钟（%.2f 小时）" % (elapsed / 60, elapsed / 3600))
    print("台          ：%s" % cname)
    print("开始播放    ：%d 次" % cnt["播放"])
    print("播放器重建  ：%d 次" % cnt["重建"])
    print("换票        ：%d 次" % cnt["换票"])
    print("断线/重连   ：%d / %d" % (cnt["断线"], cnt["重连"]))
    print("失败记录    ：%d 条" % cnt["失败"])
    if sil:
        print("静音空档    ：%d 段，合计 %.0f 秒（最长 %.0f 秒）" % (
            len(sil), sum(s for _, s in sil), max(s for _, s in sil)))
        for ts, s in sil[:10]:
            print("              %s 起 %.0f 秒" % (ts.strftime("%H:%M:%S"), s))
    else:
        print("静音空档    ：无")

    # HLS 分片过期跳过（v1.15 加）。按 2 秒/片折算丢掉的音频时长。
    # 这不是故障指标，只是「源窗口太短 + 网络抖」的量化；健康长跑应接近 0。
    logfile = os.path.join(log, "log-%s.txt" % today)
    sk_seg, sk_hits = count_skips(logfile)
    print("分片过期跳过：%d 片（触发 %d 次，约丢 %.0f 秒音频）"
          % (sk_seg, sk_hits, sk_seg * 2.0))

    # 判定：健康的长跑应当 0 重建；若开了 --expect-refresh，换票次数应 ≈ 时长/周期
    ok = cnt["重建"] == 0 and cnt["失败"] == 0
    if a.expect_refresh:
        ok = ok and cnt["换票"] >= 1
    print("结论        ：%s" % ("PASS 全程无重建" if ok else "FAIL 见上面的重建/失败明细"))
    print("日志        ：%s" % os.path.join(log, "log-%s.txt" % today))
    print("=========================================")

    if a.clean:
        shutil.rmtree(work, ignore_errors=True)
        print("临时目录已删除")
    else:
        print("临时目录已保留（要删加 --clean）")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
