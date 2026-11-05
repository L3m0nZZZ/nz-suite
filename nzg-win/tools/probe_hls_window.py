#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""HLS 窗口/分片耗时探针 —— 解释 "skipping N segments ahead, expired from playlists"

背景：FFmpeg hls demuxer 的 live_start_index 默认 -3：
    seq_no = start_seq_no + max(n_segments + (-3), 0)
playlist 窗口 <= 3 片时，起播点被 clamp 到 0 —— 直接站在最老分片上，
每次 reload 都会越界，于是持续 skip。若分片下载耗时又接近分片时长，
就永远追不上直播头，表现为「一直 skip + 没声音」。

本脚本对一个 HLS URL 采集：
  n_segments / EXTINF（min,max,avg）/ TARGETDURATION / MEDIA-SEQUENCE
  以及**冷连接下载单个分片的耗时**（模拟 http_persistent=0 每片新连接）
并给风险判定。

用法：
  python tools/probe_hls_window.py                 # 扫内置配置里全部 HLS 台
  python tools/probe_hls_window.py <url>           # 只测一个
  python tools/probe_hls_window.py --limit 10      # 只测前 10 个
"""
import sys, os, re, ssl, time, json, io, socket, urllib.request, urllib.error
from urllib.parse import urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE
UA = "Mozilla/5.0 (Linux; Android 9; TV) AppleWebKit/537.36"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def http(url, timeout=15, limit=None, headers=None):
    h = {"User-Agent": UA}
    if headers:
        h.update(headers)
    req = urllib.request.Request(url, headers=h)
    with urllib.request.urlopen(req, timeout=timeout, context=CTX) as r:
        return r.status, (r.read(limit) if limit else r.read()), dict(r.headers)


def fetch_media_playlist(url, depth=0):
    """返回 (media_url, text)，自动穿 master playlist"""
    st, body, _ = http(url, timeout=15)
    txt = body.decode("utf-8", "ignore")
    if st != 200:
        raise RuntimeError("HTTP %s" % st)
    if "#EXT-X-STREAM-INF" in txt and depth < 2:
        subs = [l.strip() for l in txt.splitlines() if l.strip().startswith("http")]
        if subs:
            return fetch_media_playlist(subs[0], depth + 1)
    return url, txt


def parse(txt):
    lines = [l.strip() for l in txt.splitlines() if l.strip()]
    segs, durs, cur = [], [], None
    for l in lines:
        if l.startswith("#EXTINF:"):
            m = re.match(r"#EXTINF:\s*([\d.]+)", l)
            cur = float(m.group(1)) if m else None
        elif not l.startswith("#"):
            segs.append(l)
            durs.append(cur)
    seq = re.search(r"#EXT-X-MEDIA-SEQUENCE:(\d+)", txt)
    tgt = re.search(r"#EXT-X-TARGETDURATION:(\d+)", txt)
    end = "#EXT-X-ENDLIST" in txt
    return {
        "n": len(segs),
        "segs": segs,
        "durs": durs,
        "seq": int(seq.group(1)) if seq else None,
        "tgt": int(tgt.group(1)) if tgt else None,
        "endlist": end,
    }


def cold_segment_time(media_url, seg_name):
    """冷连接（新建 TCP+TLS）下载该分片到 EOF 的耗时"""
    seg_url = seg_name if seg_name.startswith("http") else media_url.rsplit("/", 1)[0] + "/" + seg_name
    t0 = time.time()
    try:
        st, data, _ = http(seg_url, timeout=25)
        dt = time.time() - t0
        return dt, st, len(data)
    except Exception as e:
        return time.time() - t0, -1, str(e)


def analyze(url, label="", do_timing=True):
    try:
        mus, txt = fetch_media_playlist(url)
    except Exception as e:
        return {"label": label, "url": url, "error": "取 playlist 失败 %s" % e}

    p = parse(txt)
    durs = [d for d in p["durs"] if d]
    r = {
        "label": label,
        "url": mus,
        "n": p["n"],
        "seq": p["seq"],
        "tgt": p["tgt"],
        "endlist": p["endlist"],
        "dmin": min(durs) if durs else None,
        "dmax": max(durs) if durs else None,
        "davg": (sum(durs) / len(durs)) if durs else None,
        "window_sec": (sum(durs)) if durs else None,
    }
    # live_start_index=-3 落地位置
    r["start_off"] = max(p["n"] - 3, 0)

    if do_timing and p["segs"]:
        # 测「最新一片」与「最老一片」，最新那片最能反映真实追流压力
        dt_new, st_new, sz_new = cold_segment_time(mus, p["segs"][-1])
        r["t_new"] = dt_new
        r["sz_new"] = sz_new if isinstance(sz_new, int) else sz_new
        r["st_new"] = st_new
        if len(p["segs"]) > 1:
            dt_old, st_old, sz_old = cold_segment_time(mus, p["segs"][0])
            r["t_old"] = dt_old
            r["st_old"] = st_old

    # 风险判定
    flags = []
    if p["n"] and p["n"] <= 3:
        flags.append("窗口<=3片(起播贴最老边，必skip)")
    if p["endlist"]:
        flags.append("VOD(带ENDLIST，不是直播)")
    dn = r.get("t_new")
    seg_dur = r.get("dmax") or r.get("tgt") or 0
    if dn and seg_dur and dn >= seg_dur * 0.6:
        flags.append("单片下载%.1fs>=时长%.1fs*0.6(追不上)" % (dn, seg_dur))
    if seg_dur and seg_dur >= 8:
        flags.append("长分片%.1fs(一次skip丢掉这么多音频)" % seg_dur)
    r["flags"] = flags
    return r


def load_targets():
    path = os.path.join(ROOT, "config_default.json")
    d = json.load(io.open(path, encoding="utf-8"))
    return d["channels"]


def main():
    args = [a for a in sys.argv[1:]]
    limit = None
    if "--limit" in args:
        i = args.index("--limit")
        limit = int(args[i + 1])
        del args[i:i + 2]
    no_timing = "--no-timing" in args
    if no_timing:
        args.remove("--no-timing")

    if args and args[0].startswith("http"):
        r = analyze(args[0], "直接指定", do_timing=not no_timing)
        print(json.dumps(r, ensure_ascii=False, indent=2))
        return

    # 扫配置
    import check_sources as cs

    chs = load_targets()
    if limit:
        chs = chs[:limit]
    rows = []
    for c in chs:
        raw = c.get("primary_url", "")
        try:
            u, how = cs.resolve(c)
        except Exception as e:
            continue
        if not u:
            continue
        # 只关心 HLS（解析后是否 m3u8，靠实际拉取判断）
        if not (".m3u8" in u or raw.startswith(("litenews://", "hntv://", "page://"))):
            continue
        label = "id=%s %s/%s" % (c.get("id"), c.get("region"), c.get("name"))
        r = analyze(u, label, do_timing=not no_timing)
        rows.append(r)
        if "error" in r:
            print("[ERR ] %-28s %s" % (label, r["error"]))
            continue
        print("[%s] %-28s n=%-3d tgt=%-3s seg=%-5s~%-5s win=%-6s 起播偏移=%d 新片%.2fs %s"
              % ("危险" if r["flags"] else "正常", label, r["n"], r["tgt"],
                 ("%.1f" % r["dmin"]) if r["dmin"] else "?",
                 ("%.1f" % r["dmax"]) if r["dmax"] else "?",
                 ("%.0f" % r["window_sec"]) if r["window_sec"] else "?",
                 r["start_off"], r.get("t_new", -1),
                 " | ".join(r["flags"])))

    print()
    bad = [r for r in rows if r.get("flags")]
    print("汇总：HLS 台 %d 个，高危 %d 个" % (len(rows), len(bad)))
    for r in bad:
        print("  %-28s %s" % (r["label"], " | ".join(r["flags"])))


if __name__ == "__main__":
    main()
