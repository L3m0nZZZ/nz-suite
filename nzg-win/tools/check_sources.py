#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""nzg 全台源活性检查（安卓 29 台 + win 内置 46 台 合并去重）

判定口径（对齐 nzg-win selftest）：
  HLS : playlist 能取到 + 最后分片 HTTP 200 + 分片名里的 unix 秒距墙钟 <= 300s
  MP3 : 能取到 >8KB 且含帧同步/ID3
  ADTS: 0xFFF 帧头
状态：OK / LAG(分片时间戳旧) / BAD(取不到) / UNRESOLVED(解析失败)
"""
import json, re, ssl, sys, time, base64, hashlib, urllib.request, urllib.error
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import urlparse, parse_qs, unquote

CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE

UA = "Mozilla/5.0 (Linux; Android 9; TV) AppleWebKit/537.36"
UA_MP3 = "okhttp/4.9.0"
HNTV_SALT = "6ca114a836ac7d73"
HNTV_CLIENTS = {
    "shenqiu": "09a9ab11660d4aa5a4a0fb6a7809f1e6",
    "taikang": "71a0b3560f4e422ab7e5260ea14f1a8f",
    "xihua":   "069b05fa076f4a9b9a97fea2eac9c97d",
    "luyi":    "ecddfc8273dd42508b2d5d427437d067",
    "fugou":   "558feebc64524f988fa12d9ecaddf81e",
}


def http(url, headers=None, timeout=12, limit=None):
    h = {"User-Agent": UA}
    if headers:
        h.update(headers)
    req = urllib.request.Request(url, headers=h)
    with urllib.request.urlopen(req, timeout=timeout, context=CTX) as r:
        if limit:
            return r.status, r.read(limit), dict(r.headers)
        return r.status, r.read(), dict(r.headers)


# ---------------- 解析 ----------------
def resolve_litenews(raw):
    m = re.match(r"litenews://([^?]+)\?(.*)$", raw)
    if not m:
        return None, "litenews 格式异常"
    orgid, qs = m.group(1), m.group(2)
    q = parse_qs(qs)
    want = unquote(q.get("name", [""])[0])
    kind = q.get("kind", [""])[0]
    endpoints = [kind] if kind in ("radio", "tv") else ["radio", "tv"]
    cands = []
    for ep in endpoints:
        api = "https://app.litenews.cn/v1/app/play/%s/live?orgid=%s" % (ep, orgid)
        try:
            _, body, _ = http(api, timeout=15)
            d = json.loads(body.decode("utf-8", "ignore"))
        except Exception as e:
            return None, "litenews %s 接口失败 %s" % (ep, e)
        if d.get("code") != 1:
            continue
        for c in d.get("data") or []:
            nm, st = c.get("name", ""), c.get("stream", "")
            if not st.startswith("h"):
                continue
            cands.append((nm, st, ep))
            if want and (want in nm or nm in want or nm.startswith(want)):
                return st, "litenews/%s「%s」" % (ep, nm)
    if cands:
        return cands[0][1], "litenews/%s 名称兜底「%s」want=%s" % (cands[0][2], cands[0][0], want)
    return None, "litenews 无候选 want=%s" % want


def resolve_hntv(raw):
    m = re.match(r"hntv://([^?]+)(\?(.*))?$", raw)
    key = m.group(1)
    cls = "1"
    if m.group(3):
        q = parse_qs(m.group(3))
        cls = q.get("cls", ["1"])[0]
    client = HNTV_CLIENTS.get(key, key)
    ts = str(int(time.time()))
    sign = hashlib.sha256((HNTV_SALT + ts).encode()).hexdigest()
    hdr = {"timestamp": ts, "sign": sign, "clientId": client, "User-Agent": UA_MP3}
    try:
        _, body, _ = http("https://xy-gw.hntv.tv/program/getAuth/live/class/%s" % cls, hdr, timeout=15)
    except Exception as e:
        return None, "hntv 网关失败 %s" % e
    s = body.decode("utf-8", "ignore").replace("\\/", "/")
    mm = re.search(r'"streams"\s*:\s*\[\s*"([^"]+)"', s)
    if mm:
        return mm.group(1), "hntv 网关"
    return None, "hntv 无 streams"


def resolve(ch):
    raw = ch.get("primary_url", "")
    if raw.startswith("litenews://"):
        return resolve_litenews(raw)
    if raw.startswith("hntv://"):
        return resolve_hntv(raw)
    if raw.startswith("page://"):
        try:
            _, body, _ = http(raw[7:], timeout=15)
            mm = re.search(r'https?://[^\s"\'<>]+?\.m3u8[^\s"\'<>]*', body.decode("utf-8", "ignore"))
            if mm:
                return mm.group(0), "page"
        except Exception as e:
            return None, "page 失败 %s" % e
        return None, "page 无 m3u8"
    return raw, "直链"


# ---------------- 活性 ----------------
def ts_from_segment(name):
    """从分片名取 unix 秒：1791595797.ts / hash-1790168594.ts / 毫秒戳"""
    base = name.split("?")[0].rsplit("/", 1)[-1]
    nums = re.findall(r"(\d{10,16})", base)
    if not nums:
        return None
    n = int(nums[-1])
    if n > 10 ** 12:      # 毫秒
        n //= 1000
    if 1_400_000_000 < n < 2_100_000_000:
        return n
    return None


def probe_hls(url, depth=0, verify=True, attempt=0):
    st, body, hdrs = http(url, timeout=15)
    if st != 200:
        return "BAD", "playlist HTTP %s" % st, None
    txt = body.decode("utf-8", "ignore")
    if "#EXTM3U" not in txt:
        return "BAD", "非 m3u8（%d 字节）" % len(body), None

    # master playlist：取第一个子 playlist 继续（litenews 就是这层套 100ycdn）
    if "#EXT-X-STREAM-INF" in txt and depth < 2:
        subs = [l.strip() for l in txt.splitlines() if l.strip().startswith("http")]
        if not subs:
            return "BAD", "master playlist 无子地址", None
        status, detail, extra = probe_hls(subs[0], depth + 1, verify, attempt)
        if status == "LAG" and attempt == 0:
            # ⚠️ 2026-10-23 踩过：100ycdn 的 master 会被 CDN 缓存几十秒，
            # 拿到的是**上一个会话**的子列表 URL；那条会话的 playlist 缓存条目
            # 本来就停在旧 seq（实测昌乐 seq=3，而实时已经 seq=46，落后约 2 分钟），
            # 于是 3 秒内"不推进"→ 报假 LAG。
            # 重取一次 master 换新会话再判：真停更的台重取后照样不推进。
            status, detail, extra = probe_hls(url, depth, verify, attempt + 1)
            detail += "（旧会话，重取后复测）"
        return status, detail, extra

    segs = [l.strip() for l in txt.splitlines() if l.strip() and not l.startswith("#")]
    seq = re.search(r"#EXT-X-MEDIA-SEQUENCE:(\d+)", txt)
    tgt = re.search(r"#EXT-X-TARGETDURATION:(\d+)", txt)
    if not segs:
        return "BAD", "playlist 无分片", None

    # 末尾分片可能刚生成/尚未落盘（404），依次往前退
    base = url.rsplit("/", 1)[0] + "/"
    st2, data, seg = None, b"", ""
    for cand in reversed(segs[-3:]):
        seg_url = cand if cand.startswith("http") else base + cand
        try:
            st2, data, _ = http(seg_url, timeout=15, limit=8192)
        except urllib.error.HTTPError as e:
            st2, data = e.code, b""
            continue
        except Exception:
            st2, data = -1, b""
            continue
        if st2 == 200 and len(data) >= 100:   # 广播分片可低至数百字节
            seg = cand
            break
    if not seg:
        return "BAD", "分片全部取不到（末日 HTTP %s）" % st2, None

    # 权威判据：等 target_duration+1 秒再拉一次，看 playlist 是否前进。
    # 分片名里的数字是「服务器时间轴」，不是墙钟（惠民/新泰 lag 十几小时其实在播），
    # 所以只认 seq / 末片名是否变化。
    if verify:
        td = int(tgt.group(1)) if tgt else 4
        time.sleep(min(max(td + 1, 3), 8))
        try:
            _, body2, _ = http(url, timeout=15)
            txt2 = body2.decode("utf-8", "ignore")
            seq2 = re.search(r"#EXT-X-MEDIA-SEQUENCE:(\d+)", txt2)
            segs2 = [l.strip() for l in txt2.splitlines() if l.strip() and not l.startswith("#")]
            moved = False
            if seq and seq2 and seq.group(1) != seq2.group(1):
                moved = True
            elif segs2 and segs2[-1] != segs[-1]:
                moved = True
            if not moved:
                return "LAG", "playlist 未推进（%ds 内 seq=%s 不变化，停更）" % (
                    td + 1, seq.group(1) if seq else "?"), None
        except Exception as e:
            return "LAG", "二次采样失败 %s" % e, None

    return "OK", "在播（seq=%s 分片数=%d 目标=%ss %dB，已确认推进）" % (
        seq.group(1) if seq else "?", len(segs),
        tgt.group(1) if tgt else "?", len(data)), None


def probe_direct(url):
    path = urlparse(url).path.lower()
    st, data, hdrs = http(url, timeout=12, limit=65536)
    if st != 200:
        return "BAD", "HTTP %s" % st, None
    if data[:7] == b"#EXTM3U" or ".m3u8" in path:
        return probe_hls(url)
    if len(data) < 2000:
        return "BAD", "仅 %d 字节" % len(data), None
    ctype = (hdrs.get("Content-Type") or "").lower()
    # 帧同步可能在任意偏移（qtfm 常带前置元数据），扫前 64KB
    ok_mp3 = data[:3] == b"ID3" or any(
        data[i] == 0xFF and (data[i + 1] & 0xE0) == 0xE0 for i in range(0, min(len(data) - 1, 16384)))
    if ok_mp3:
        return "OK", "音频帧正常（%s %dB）" % (ctype or "?", len(data)), None
    return "BAD", "无音频帧（%s）" % (ctype or "?"), None


def check(ch):
    name = ch.get("name", "?")
    try:
        url, how = resolve(ch)
    except Exception as e:
        return {"name": name, "status": "UNRESOLVED", "detail": "解析异常 %s" % e, "url": ""}
    if not url:
        return {"name": name, "status": "UNRESOLVED", "detail": how, "url": ""}
    try:
        if ".m3u8" in url or "m3u8" in url:
            stt, det, lag = probe_hls(url)
        else:
            stt, det, lag = probe_direct(url)
    except urllib.error.HTTPError as e:
        stt, det, lag = "BAD", "HTTP %s" % e.code, None
    except Exception as e:
        stt, det, lag = "BAD", "%s: %s" % (type(e).__name__, e), None
    return {"name": name, "status": stt, "detail": "%s | %s" % (how, det),
            "url": url, "lag": lag, "region": ch.get("region", "")}


def main():
    files = sys.argv[1:] or ["D:/ningz/nzg/channels.json", "D:/ningz/nzg-win/config_default.json"]
    chans, seen = [], set()
    for f in files:
        d = json.load(open(f, encoding="utf-8"))
        for c in d.get("channels", []):
            k = c.get("name", "")
            if k in seen:
                continue
            seen.add(k)
            chans.append(c)
    print("共 %d 台（去重后），开始探测…\n" % len(chans))
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=8) as ex:
        res = list(ex.map(check, chans))
    order = {"BAD": 0, "UNRESOLVED": 1, "LAG": 2, "OK": 3}
    res.sort(key=lambda r: (order.get(r["status"], 9), r.get("region", ""), r["name"]))
    for r in res:
        print("[%-10s] %-32s %s" % (r["status"], r["name"], r["detail"]))
        if r["status"] != "OK" and r.get("url"):
            print("             URL: %s" % r["url"][:150])
    print("\n耗时 %.1fs   OK=%d LAG=%d BAD=%d UNRESOLVED=%d" % (
        time.time() - t0,
        sum(1 for r in res if r["status"] == "OK"),
        sum(1 for r in res if r["status"] == "LAG"),
        sum(1 for r in res if r["status"] == "BAD"),
        sum(1 for r in res if r["status"] == "UNRESOLVED")))
    json.dump(res, open("D:/ningz/_capture/nzg_check_result.json", "w", encoding="utf-8"),
              ensure_ascii=False, indent=2)


if __name__ == "__main__":
    main()
