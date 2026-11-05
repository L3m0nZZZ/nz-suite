#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""litenews 100ycdn 会话签名有效期长测

对单县 / 巨野各取一次 master+子 playlist，然后**固定使用最初那一条子 playlist URL**
反复刷新（模拟 FFmpeg 的行为：打开后不再重新解析），记录每次的状态码与分片数，
直到连续失败或到达时限。用于确证「播半天后一直不动」的根因。
"""
import sys, re, time, json, datetime
sys.path.insert(0, "D:/ningz/nzg-win/tools")
import check_sources as C

TARGETS = {
    "单县": "litenews://155?name=%E5%8D%95%E5%8E%BF%E5%B9%BF%E6%92%AD%E7%94%B5%E5%8F%B0",
    "巨野": "litenews://139?name=%E5%B7%A8%E9%87%8E%E4%BA%BA%E6%B0%91%E5%B9%BF%E6%92%AD%E7%94%B5%E5%8F%B0",
}
MINUTES = int(sys.argv[1]) if len(sys.argv) > 1 else 240
OUT = "D:/ningz/_capture/litenews_session_test.jsonl"


def grab(raw):
    u, how = C.resolve_litenews(raw)
    if not u:
        return None, None, how
    st, body, _ = C.http(u)
    txt = body.decode("utf-8", "ignore")
    if "#EXT-X-STREAM-INF" in txt:
        sub = [l.strip() for l in txt.splitlines() if l.strip().startswith("http")][0]
        st2, body2, _ = C.http(sub)
        return sub, body2.decode("utf-8", "ignore"), how
    return u, txt, how


def main():
    state = {}
    for name, raw in TARGETS.items():
        url, pl, how = grab(raw)
        if not url:
            print(name, "解析失败", how)
            continue
        t0 = int(time.time())
        tt = int(re.search(r"[?&]t=(\d+)", url).group(1))
        state[name] = {"url": url, "t0": t0, "t": tt, "start": t0, "fails": 0}
        print("%s 起点 %s t=%d (now=%d, 差 %+ds)" % (
            name, datetime.datetime.now().strftime("%H:%M:%S"), tt, t0, tt - t0))
        print("   %s" % url[:140])

    end = time.time() + MINUTES * 60
    with open(OUT, "a", encoding="utf-8") as fp:
        while time.time() < end:
            for name, s in state.items():
                if s.get("dead"):
                    continue
                rec = {"name": name, "at": int(time.time()),
                       "elapsed": int(time.time() - s["start"])}
                try:
                    st, body, _ = C.http(s["url"], timeout=15)
                    txt = body.decode("utf-8", "ignore")
                    segs = [l.strip() for l in txt.splitlines() if l.strip() and not l.startswith("#")]
                    rec["code"] = st
                    rec["segs"] = len(segs)
                    rec["last"] = segs[-1].split("?")[0].rsplit("/", 1)[-1] if segs else ""
                    if st == 200 and segs:
                        s["fails"] = 0
                    else:
                        s["fails"] += 1
                except Exception as e:
                    rec["code"] = getattr(e, "code", 0) or -1
                    rec["err"] = str(e)[:100]
                    s["fails"] += 1
                rec["fails"] = s["fails"]
                fp.write(json.dumps(rec, ensure_ascii=False) + "\n")
                fp.flush()
                print("%s +%5ds code=%s segs=%s fails=%d last=%s" % (
                    name, rec["elapsed"], rec.get("code"), rec.get("segs"),
                    s["fails"], rec.get("last", "")), flush=True)
                if s["fails"] >= 3:
                    s["dead"] = True
                    print(">>> %s 判定失效，距开始 %d 秒（%.1f 分钟）" % (
                        name, rec["elapsed"], rec["elapsed"] / 60.0), flush=True)
            if all(s.get("dead") for s in state.values()):
                print(">>> 全部失效，长测结束")
                break
            time.sleep(30)


if __name__ == "__main__":
    main()
