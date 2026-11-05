#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""换票机制实测：临时把 dynamic_refresh_sec 压到 90 秒，切到指定台，观察日志。

流程：停进程 → 备份 state.json → 放外部 config.json(90s) → state 指向目标台 →
      启动 → 等 150s → 抓 [换票] 日志 → 停进程 → 删外部 config → 恢复 state → 重启
全程可回滚；外部 config.json 必须删掉，否则以后升级永远被它钉住。

用法（默认取 nzg_win_versions 下**版本号最大**的归档）：
    python tools/verify_refresh.py
    python tools/verify_refresh.py "D:\\ningz\\nzg_win_versions\\v1.16.2_72台_20261103"

⚠️ 2026-10-23 修：原脚本把 EXEDIR 写死成 v1.13_69台、SHANXIAN_ID 写死 48。
   重新生成清单后端 id 变了（单县现在是 **50**），写死的 id 会切到别的台，
   测出来「没触发换票」还以为是代码坏了。现在改成从 archive 的 config 里按 region 找 id。
"""
import json, os, subprocess, sys, time, datetime

ARCHIVE_ROOT = r"D:\ningz\nzg_win_versions"
EXE_NAME = "守播广播.exe"


def pick_archive(arg=None) -> str:
    """默认取版本号最大的归档；显式传路径则用传的。

    ⚠️ 原来这里写死 DEFAULT_ARCHIVE = v1.13.2，回退还只 glob "v1.13*" ——
    等于**换票验证永远跑在 v1.13.2 上**，之后的版本从没验证过换票是否还正常。
    2026-11-03 和 soak_play.py 一起改掉（见 tools/archive_pick.py 里的说明）。
    """
    if arg:
        return arg
    from archive_pick import newest_archive
    return newest_archive(ARCHIVE_ROOT)


def channel_id(cfg_path: str, needle: str) -> int:
    """按地区或台名子串找频道 id。

    ⚠️ 原来写死 region == "单县"。单县那条 CDN 一抖（2026-11-03 整段 502），
    这个工具就彻底没法用了 —— 而换票恰恰是 7×24 部署最需要验的东西。
    现在允许换台（--channel），默认仍是单县。
    """
    d = json.load(open(cfg_path, encoding="utf-8"))
    for c in d["channels"]:
        if c.get("region") == needle:
            return int(c["id"])
    hits = [c for c in d["channels"] if needle in c.get("name", "")]
    if len(hits) == 1:
        return int(hits[0]["id"])
    if len(hits) > 1:
        sys.exit("「%s」匹配到多台，请写更完整的台名：%s"
                 % (needle, "、".join(c["name"] for c in hits[:6])))
    sys.exit("配置里找不到「%s」" % needle)


def parse_args():
    """返回 (归档路径或 None, 目标台子串)。手工解析，保持脚本无第三方依赖。"""
    argv = sys.argv[1:]
    channel = "单县"
    path = None
    i = 0
    while i < len(argv):
        if argv[i] == "--channel" and i + 1 < len(argv):
            channel = argv[i + 1]
            i += 2
        elif argv[i].startswith("--channel="):
            channel = argv[i].split("=", 1)[1]
            i += 1
        elif not argv[i].startswith("-") and path is None:
            path = argv[i]
            i += 1
        else:
            i += 1
    return path, channel


def main() -> int:
    path_arg, channel = parse_args()
    exedir = pick_archive(path_arg)
    exe = os.path.join(exedir, EXE_NAME)
    if not os.path.isfile(exe):
        sys.exit("归档里没有 exe：%s" % exe)
    log = os.path.join(exedir, "logs",
                       "log-%s.txt" % datetime.date.today().isoformat())
    cfg_src = os.path.join(exedir, "config_default.json")
    cfg_ext = os.path.join(exedir, "config.json")
    state = os.path.join(exedir, "state.json")
    sid = channel_id(cfg_src, channel)
    image = EXE_NAME

    print("归档：%s" % exedir)
    print("目标台：%s（id=%d），日志 %s" % (channel, sid, log))

    def stop():
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"],
                             capture_output=True, timeout=20).stdout.decode("gbk", "ignore")
        for line in out.splitlines():
            parts = [p.strip('"') for p in line.split('","')]
            if len(parts) >= 2 and parts[0].strip('"').lower() == image.lower():
                subprocess.run(["taskkill", "/F", "/PID", parts[1]], capture_output=True)
        time.sleep(1.5)

    def start():
        # DETACHED_PROCESS：不挂在验证脚本的控制台上，
        # 否则脚本一结束（控制台销毁）程序会被一起带走，
        # 最后一步「重启回正常配置」就成了一句空话。
        flags = 0
        if hasattr(subprocess, "DETACHED_PROCESS"):
            flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        subprocess.Popen([exe], cwd=exedir, close_fds=True, creationflags=flags)
        time.sleep(4)

    def read_log():
        try:
            with open(log, encoding="utf-8", errors="ignore") as f:
                return f.read().strip().splitlines()
        except FileNotFoundError:
            return []

    print("[1] 停进程")
    stop()

    print("[2] 备份 state.json / 写外部 config.json(90s) / state 指向%s" % channel)
    old_state = None
    if os.path.isfile(state):
        old_state = open(state, encoding="utf-8").read()
        print("    原 state: %s" % old_state.strip())
    cfg = json.load(open(cfg_src, encoding="utf-8"))
    cfg["resilience"]["dynamic_refresh_sec"] = 90
    json.dump(cfg, open(cfg_ext, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
    vol = 100
    muted = 0
    if old_state:
        try:
            st = json.loads(old_state)
            vol = st.get("volume", 100)
            muted = st.get("muted", 0)   # 保留静音：否则跑一次验证就外放 150 秒
        except Exception:
            pass
    json.dump({"current_channel_id": sid, "volume": vol, "muted": muted},
              open(state, "w", encoding="utf-8"))

    print("[3] 启动并等 150 秒")
    start()
    base = len(read_log())
    time.sleep(150)

    print("\n[4] 本段日志：")
    lines = read_log()[base:]
    hit = [l for l in lines if "换票" in l]
    for l in lines:
        if any(k in l for k in ("配置", "播放", "换票", "断线", "重连", "启动")):
            print("   " + l)
    print("\n    换票触发次数：%d" % len(hit))

    print("\n[5] 清理：停进程 → 删外部 config.json → 恢复 state.json")
    stop()
    if os.path.isfile(cfg_ext):
        os.remove(cfg_ext)
        print("    已删除 %s" % cfg_ext)
    if old_state is not None:
        open(state, "w", encoding="utf-8").write(old_state)
        print("    已恢复 state.json")

    print("[6] 重启回正常配置")
    start()
    for l in read_log()[-6:]:
        print("   " + l)

    return 0 if hit else 1


if __name__ == "__main__":
    sys.exit(main())
