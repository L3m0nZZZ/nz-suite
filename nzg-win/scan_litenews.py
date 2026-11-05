# -*- coding: utf-8 -*-
"""并发盲扫 litenews 的 orgid，用来「找新台」或确认某路真的不存在。

两个典型用法：
  1. 确认某路没救：表格里标「未找到」的台，扫一遍全量 orgid，关键词零命中 → 可以彻底放下
     （例：2026-10-10 扫 orgid 1~700，关键词「财富/932/私家车/临沂」零命中 → 临沂财富932 确认放弃）。
  2. 找新台：改下面的 KEYS，扫一遍看命中哪些 orgid，再用 probe_orgid.py 逐个确认。

用法： python scan_litenews.py [起始orgid] [结束orgid] [并发数]
"""
import json
import ssl
import sys
import threading
import urllib.request
from concurrent.futures import ThreadPoolExecutor

START = int(sys.argv[1]) if len(sys.argv) > 1 else 1
END = int(sys.argv[2]) if len(sys.argv) > 2 else 700
WORKERS = int(sys.argv[3]) if len(sys.argv) > 3 else 12

# 命中就打报告的关键词（按要查的东西改）
KEYS = ["财富", "932", "私家车"]

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
UA = {"User-Agent": "Mozilla/5.0 (Linux; Android 9; TV) AppleWebKit/537.36"}
lock = threading.Lock()
hits = []
scanned = [0]


def one(orgid):
    u = "https://app.litenews.cn/v1/app/play/radio/live?orgid=%d" % orgid
    req = urllib.request.Request(u, headers=UA)
    with urllib.request.urlopen(req, timeout=8, context=ctx) as r:
        return [c.get("name", "") for c in
                (json.loads(r.read().decode("utf-8", "replace")).get("data") or [])]


def worker(ids):
    for orgid in ids:
        try:
            names = one(orgid)
        except Exception:
            names = []
        with lock:
            scanned[0] += 1
            for n in names:
                if any(k in n for k in KEYS):
                    hits.append((orgid, n))
                    print("* orgid=%-4d %s" % (orgid, n))
            if scanned[0] % 200 == 0:
                print("   ...已扫 %d/%d" % (scanned[0], END - START + 1))


ids = list(range(START, END + 1))
with ThreadPoolExecutor(max_workers=WORKERS) as ex:
    list(ex.map(worker, [ids[i::WORKERS] for i in range(WORKERS)]))

print("\n扫完 orgid %d~%d（%d 个），命中 %d 条：" % (START, END, scanned[0], len(hits)))
for o, n in sorted(set(hits)):
    print("   orgid=%-4d %s" % (o, n))
if not hits:
    print("   零命中 —— 这几路在 litenews radio 端确实不存在，可以放下了")
