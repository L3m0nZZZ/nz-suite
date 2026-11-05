# -*- coding: utf-8 -*-
"""查指定 orgid 在 litenews 上的真实频道名。

什么时候用：
  - CSV 里的 `name=` 对不上（`check_litenews_names.py` 报 MISS），先来这里看接口到底叫什么；
  - 表格里写「未找到 / PENDING」，但你怀疑平台其实有台 —— 这里能确认 radio/tv 端点有没有数据。

用法： python probe_orgid.py 159 15 125
"""
import json
import ssl
import sys
import urllib.request

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
UA = {"User-Agent": "Mozilla/5.0 (Linux; Android 9; TV) AppleWebKit/537.36"}


def fetch(orgid, kind):
    u = "https://app.litenews.cn/v1/app/play/%s/live?orgid=%s" % (kind, orgid)
    req = urllib.request.Request(u, headers=UA)
    with urllib.request.urlopen(req, timeout=15, context=ctx) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


for arg in sys.argv[1:]:
    print("### orgid=%s" % arg)
    for kind in ("radio", "tv"):
        try:
            d = fetch(arg, kind)
        except Exception as e:
            print("   %-5s 请求失败: %s" % (kind, e))
            continue
        names = [c.get("name", "") for c in (d.get("data") or [])]
        print("   %-5s code=%s  %s" % (kind, d.get("code"),
                                        " | ".join(names) if names else "(空)"))
    print("   提示：端点有名字 != 流能播，还要手动取一次 stream 看是不是 404")
