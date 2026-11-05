"""把 _capture/nzg_*.csv（用户维护的可用源清单）转成 Win 版内置频道配置。

为什么单独维护：这张表是本轮实际攻破并验证过的源清单，
比安卓版内置的 29 台更新更全。Win 版直接吃这张表，避免手工誊抄出错。

处理规则：
  - **不再按「验证状态」过滤**（2026-10-10 起）：表格里标着「失效 / 待测」的行，
    只要有可用地址就照样收进来 —— 官方推流随时可能恢复（临邑/莒县就是这么回来的），
    收进来由 `--selftest` 和播放器的自愈去判定生死，比在表格里一刀切更划算。
    无地址的行（青州那种 PENDING、临沂财富932 那种空）仍然跳过并报告。
  - 地址既不是 http(s) 也不是 litenews:// 的行一律跳过并报告
  - litenews:// 的 name 参数**优先沿用 CSV 里已有的 name=**，
    因为那是逐台对着接口核实过的；只有当 URL 里没有 name= 时才用
    「频道名称(平台真名)」列补一个（见 _maybe_lite 的注释）
  - 平台信息与备注写进 note 字段（Win 版 UI 不展示，但保留可追溯）

用法： python gen_config_from_csv.py [csv路径]
      不给参数就自动取 _capture 下最新的 nzg_*.csv
"""
import csv
import glob
import io
import json
import os
import sys
import time
from collections import Counter
from urllib.parse import quote


CAPTURE_DIR = r"D:\ningz\_capture"
DST = r"D:\ningz\nzg-win\config_default.json"

# CSV 里压根没给地址、但我们事后在接口上找到源的台：
# 「用户清单原名」→ (orgid, 接口上的真实台名, 平台)
#
# 2026-10-10 复查记录（**当前是空的**，留这段是为了以后平台恢复了能一行加回来）：
#   青州市人民广播电台 —— orgid=125 的 radio 端点确实有数据、台名就叫「综合广播」，
#   表格记的「master 连续两次 404」是真的：2026-10-10 再取 stream 直接 HTTP 404，
#   平台侧根本没推这路流。所以**没有**放进补源表（补了也只是让列表里多一个点不响的台，
#   自愈还会无限重试）。等平台恢复推流时，在这里加一行
#   "青州市人民广播电台": ("125", "综合广播", "litenews") 即可。
SOURCE_BACKFILL = {}

# litenews name 的手工纠正表（key 为 CSV 里的原值）。
# 只收「CSV 写错了、而接口侧是另一个确定值」的情况，正常对上的一律不动。
# 已知三类：
#   1) 昌邑的「邑」是 E9 82 91，CSV 里被误录成 E9 98 91（阑），
#      前两字节一样、拿子串匹配永远命不中，还看不出错在哪。
#   2) 莒县写成「莘县」（两字形近，orgid=159）。
#   3) 潍城区写成「滨城区」（两字同音近形，orgid=15）。
LITE_NAME_FIX = {
    "昌阑广播电视台": "昌邑广播电视台",
    "昌阑": "昌邑",
    "莘县广播电视台调频广播": "莒县广播电视台调频广播",
    "滨城区电台": "潍城区电台",
}


def pick_csv(argv):
    """挑 CSV：给了参数就用参数，否则取 _capture 下最新的 nzg_*.csv。"""
    if len(argv) > 1:
        return argv[1]
    cands = glob.glob(os.path.join(CAPTURE_DIR, "nzg_*.csv"))
    if not cands:
        raise SystemExit("在 %s 下找不到 nzg_*.csv" % CAPTURE_DIR)
    return max(cands, key=os.path.getmtime)


def main():
    src = pick_csv(sys.argv)
    print("源表：%s" % src)
    raw = io.open(src, encoding="utf-8-sig").read()
    rows = list(csv.DictReader(io.StringIO(raw)))

    channels = []
    skipped = []
    revived = []          # 状态≠可用但照样收进来的行
    backfilled = []       # 表格没给地址、由 SOURCE_BACKFILL 补上的行
    cid = 0

    for r in rows:
        name = (r["用户清单原名"] or "").strip()
        plat = (r["频道名称(平台真名)"] or "").strip()
        url = (r["推荐地址(直链或动态scheme)"] or "").strip()
        status = (r["验证状态"] or "").strip()

        # 地址能不能用（http(s) / litenews / hntv）
        # hntv:// 是河南大象系（resolve_hntv 走 xy-gw 网关签名），
        # 2026-10-23 起纳入：沈丘/扶沟/鹿邑三台同步自安卓版。
        usable = (url.startswith("http") or url.startswith("litenews://")
                  or url.startswith("hntv://"))

        # 表格里没给可用地址，但我们在接口上找到过源的，直接补上
        if not usable and name in SOURCE_BACKFILL:
            orgid, api_name, _ = SOURCE_BACKFILL[name]
            url = "litenews://%s?name=%s" % (orgid, quote(api_name, safe=""))
            usable = True
            plat = plat or api_name
            backfilled.append((name, url))

        if not usable:
            skipped.append((name, f"无可用地址（状态={status or '空'}，原地址={url[:30]!r}）"))
            continue

        # 状态≠可用也收：官方推流随时可能恢复，交给自检/自愈去判活
        if status != "可用":
            revived.append((name, status or "空"))

        # 显示名用「用户清单原名 + 频率」，跟安卓版习惯一致
        freq = (r["频率/频道号"] or "").strip()
        disp = name if not freq else f"{name}{freq}"

        cid += 1
        channels.append({
            "id": cid,
            "name": disp,
            # litenews 的 name 已在 _maybe_lite 里处理（优先沿用 CSV 的 name=）
            "primary_url": _maybe_lite(url, plat),
            # qtfm 的兜底：https 主源在 Win7 上会因 schannel 没有 TLS 1.2 而挂
            # （0x80090302），backup 用同主机的 http（无 TLS），Win7 自动落上去。
            # backup 只在主源打不开时才试，Win10/11 永远走 https，不受影响。
            "backup_url": _qtfm_backup(url),
            "category": (r["省/大组"] or "").strip() or "其他",
            "region": (r["地区"] or "").strip(),
            "player": "auto",
            "note": f"{plat}／{r['平台']}",
        })

    if not channels:
        print("没有可用频道，中止", file=sys.stderr)
        return 1

    cfg = {
        "channels": channels,
        "current_channel_id": 1,
        "favorites": [],
        "version": 1,
        "updated_at": time.strftime("%Y-%m-%d"),
        "note": "由 %s 生成，%d 台可用" % (os.path.basename(src), len(channels)),
        "resilience": {
            "buffer_stall_sec": 10,
            "frame_stall_sec": 15,
            "soft_retry_max": 3,
            "hard_retry_max": 3,
            "retry_intervals_sec": [0, 5, 15, 30],
            "network_probe_sec": 30,
            # 动态签名源（litenews/hntv）主动换票周期；0=关闭。
            # 2026-10-23 六小时长测：同一签名 URL 连拉 1428 次全 200、playlist 一路推进
            # → 有效期实测 ≥ 6 小时。取 3 小时（2 倍余量），换票实测掉 6 秒音频，
            # 别调回 20 分钟（每天 72 次断音纯属自损）。
            "dynamic_refresh_sec": 10800,
        },
        "audio": {
            "force_max": True,
            "boost_mb": 1200,
            "max_boost_mb": 1500,
            # 每次启动的初始音量（不记住上次）；v1.12 起默认 100（用户要求启动即满音量）；想记住就把 remember_volume 置 true
            "startup_volume": 100,
            "remember_volume": False,
        },
        "lock": {"channel_lock": False},
        "log": {"dir": "logs", "keep_days": 7},
    }

    with io.open(DST, "w", encoding="utf-8", newline="\n") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)

    schemes = Counter(c["primary_url"].split("://")[0] if "://" in c["primary_url"] else "?" for c in channels)
    provs = Counter(c["category"] for c in channels)

    print("已生成 %s" % DST)
    print("收进 %d 台，跳过 %d 台（无地址）" % (len(channels), len(skipped)))
    if backfilled:
        print("")
        print("补源（表格里没地址，我们自己在接口上找到的）：")
        for name, u in backfilled:
            print("  + %s -> %s" % (name, u))
    if revived:
        print("")
        print("状态≠可用但照样收进来（交给自检/自愈判活）：")
        for name, st in revived:
            print("  ~ %s（表格状态=%s）" % (name, st))
    print("协议分布：%s" % dict(schemes))
    print("省份分布：%s" % dict(provs))
    for name, why in skipped:
        print("  跳过：%s（%s）" % (name, why))
    return 0


def _qtfm_backup(url):
    """给 qtfm（蜻蜓）的台补一个「无 TLS」的兜底源。

    踩过（2026-10-10 真机 Win7）：FFmpeg 走 Schannel，[tls] Creating security
    context failed (0x80090302) = SEC_E_UNSUPPORTED_FUNCTION —— 那台 Win7 的
    schannel 没启用 TLS 1.2（注册表默认关闭），而 qtfm 的 CDN 已收紧到只收
    TLS 1.2（openssl 实测 -tls1/-tls1_1 都被拒）。结果 **19 台 qtfm 全挂**，
    连镜像 lhttp-hw 也一样挂（同一个 TLS 问题，换 CDN 没用）。

    但 **纯 http 是通的**（http://lhttp.qtfm.cn/... 实测 200 audio/mpeg、无重定向）。
    所以 backup 用「同主机 http」而不是「镜像 https」：
      - Win10/11：https 直接连，backup 永远用不上
      - Win7：https 必挂 → 自动回退 http → 通
    广播流是公开内容、无凭据，http 可接受。

    彻底的系统级修法（可选，能同时救 FFmpeg 和其他软件）是在 Win7 上启用
    schannel 的 TLS 1.2 客户端（见 README 的 Win7 章节），装完 https 也能用。
    """
    if url.startswith("https://lhttp.qtfm.cn/"):
        return url.replace("https://", "http://", 1)
    if url.startswith("https://lhttp-hw.qtfm.cn/"):
        return url.replace("https://", "http://", 1)
    return ""


def _maybe_lite(url, plat):
    """给 litenews:// 的 name 参数兜底。

    ⚠️ 优先沿用 CSV URL 里**已有的** name=，不要用「平台真名」列覆盖它。
    踩过：最初这里无条件用 plat 覆盖，而 plat 是平台展示名（「潍坊新闻广播」），
    litenews 接口返回的却是频道后缀（「新闻广播」）—— 一覆盖就整批匹配不上，
    18 台 litenews 全挂。CSV 里的 name= 是逐台对着接口核实过的，更权威。"""
    if not url.startswith("litenews://"):
        return url
    head, sep, tail = url.partition("?name=")
    if sep:
        # URL 里已有 name=：只做错字纠正，其余原样保留
        name_part, amp, extra = tail.partition("&")
        from urllib.parse import unquote
        fixed = LITE_NAME_FIX.get(unquote(name_part), unquote(name_part))
        return head + sep + quote(fixed, safe="") + amp + extra
    if plat:
        return url + ("&" if "?" in url else "?") + "name=" + quote(plat, safe="")
    return url


if __name__ == "__main__":
    sys.exit(main())