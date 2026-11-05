# -*- coding: utf-8 -*-
"""把 config_default.json 里所有 litenews:// 台的 name 与接口真实频道名核对，
并给出修正建议。用法： python check_litenews_names.py [config_default.json]"""
import json, ssl, sys, urllib.request
from urllib.parse import unquote

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
UA = {"User-Agent": "Mozilla/5.0 (Linux; Android 9; TV) AppleWebKit/537.36"}

path = sys.argv[1] if len(sys.argv) > 1 else "config_default.json"
cfg = json.load(open(path, encoding="utf-8"))

_cache = {}


def fetch(orgid, kind):
    k = (orgid, kind)
    if k in _cache:
        return _cache[k]
    u = "https://app.litenews.cn/v1/app/play/%s/live?orgid=%s" % (kind, orgid)
    try:
        with urllib.request.urlopen(urllib.request.Request(u, headers=UA),
                                    timeout=15, context=ctx) as r:
            d = json.loads(r.read().decode("utf-8", "replace"))
        names = [c.get("name", "") for c in (d.get("data") or [])]
    except Exception as e:
        names = []
        print("  !! orgid=%s %s 请求失败: %s" % (orgid, kind, e))
    _cache[k] = names
    return names


def parse(raw):
    """返回 (orgid, name, kind)"""
    body = raw[len("litenews://"):]
    orgid, _, qs = body.partition("?")
    name, kind = "", ""
    for part in qs.split("&"):
        if part.startswith("name="):
            name = unquote(part[5:])
        elif part.startswith("kind="):
            kind = part[5:]
    return orgid, name, kind


chans = cfg.get("channels", [])
lite = [(c.get("name", ""), c.get("primary_url") or c.get("url", ""))
        for c in chans if "litenews://" in (c.get("primary_url") or c.get("url", ""))]

print("配置 %s：共 %d 台，其中 litenews %d 台\n" % (path, len(chans), len(lite)))
bad = 0
for label, raw in lite:
    orgid, want, kind = parse(raw)
    kinds = [kind] if kind in ("radio", "tv") else ["radio", "tv"]
    found = None
    tried = []
    for k in kinds:
        names = fetch(orgid, k)
        tried.append((k, names))
        for n in names:
            if want and want in n:
                found = (k, n)
                break
        if found:
            break
    if found:
        print("[OK  ] %-28s orgid=%-4s want=%-22s -> %s [%s]"
              % (label, orgid, want, found[1], found[0]))
    else:
        bad += 1
        print("[MISS] %-28s orgid=%-4s want=%s" % (label, orgid, want))
        for k, names in tried:
            print("         %-5s: %s" % (k, " | ".join(names) if names else "(空)"))
print("\n匹配不上 %d 台" % bad)
