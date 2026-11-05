#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在 litenews 上枚举 orgid，找菏泽市台（菏泽交通 94.8 / 新闻 87.5 / 音乐 91.8）

背景：hznet 的 HLS 是冻结时移列表，只能播 30 秒，必须换源。
litenews 的 orgid 与行政区划无关、只能扫。已知菏泽片区：牡丹=219、东明=111、
单县=155、定陶=27、巨野=139、成武=131、曹县=87、郓城=135、鄄城=141。
市台若在平台上，名字会带「菏泽」。
"""
import json, sys, re
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, "D:/ningz/nzg-win/tools")
import check_sources as C

HITS = []
LOCK = None


def probe(orgid):
    try:
        st, body, _ = C.http(
            "https://app.litenews.cn/v1/app/play/radio/live?orgid=%d" % orgid,
            timeout=10)
        d = json.loads(body.decode("utf-8", "ignore"))
        if d.get("code") != 1:
            return
        for c in d.get("data") or []:
            nm = c.get("name", "")
            if "菏泽" in nm or ("牡丹" in nm and orgid != 219):
                stm = c.get("stream", "")
                HITS.append((orgid, nm, stm))
                print("  命中 orgid=%d  %s" % (orgid, nm), flush=True)
    except Exception:
        pass


def main():
    lo, hi = (int(sys.argv[1]), int(sys.argv[2])) if len(sys.argv) > 2 else (1, 350)
    print("扫描 litenews radio 端点 orgid %d-%d …" % (lo, hi))
    with ThreadPoolExecutor(max_workers=10) as ex:
        list(ex.map(probe, range(lo, hi + 1)))
    print("\n命中 %d 条" % len(HITS))
    for orgid, nm, stm in HITS:
        ok = C.probe_hls(stm) if stm.startswith("http") else ("?", "非 http", None)
        print("  orgid=%-4d %-24s %s" % (orgid, nm, ok[0]))
        print("    %s" % stm[:120])


if __name__ == "__main__":
    main()
