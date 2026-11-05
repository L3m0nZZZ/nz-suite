/* liveness.c —— HLS 活性探测：区分「真推流」与「循环垫片」
 *
 * 为什么需要这个：
 *   平台侧停推时，常见做法不是撤流，而是留一段**循环垫片**（frozen pad）。
 *   那种流「拉得开、能解出帧」，`--selftest` 会判通过，人耳听到的却是一段死循环。
 *   源清单备注里的人工判据是「隔 6s 连拉两次二级 playlist，看分片时间戳是否跟着墙上时钟前进」，
 *   这里把它自动化。
 *
 * 判据（两条，任一成立即在播）：
 *   1. **最新分片的滞后量** lag = 墙上时钟 - 分片时间戳，lag <= LAG_MAX(300s) → 在播；
 *   2. **分片时间戳是否在前进**：两次采样 Δts > 0 → 在播（时间轴偏移，例如 litenews
 *      alivealone302 域的分片名走服务器时钟，可能比墙钟慢几小时但推流是活的）。
 *   两条都不成立（lag 大且 Δts=0）→ 疑似垫片。
 *
 *   2026-10-23 补第 2 条的原因：只用 lag 会把 10 台真在播的台误报成垫片
 *   （实测惠民 lag=2168s、昌邑 lag=21069s，但 Δts=1~2s，playlist 正常滚动）。
 *
 * ⚠️ 两个都踩过的坑，别改回去：
 *   1. **不能**用「Δts 是否跟得上墙上时钟」当判据：litenews 正常直播的分片名按 1 秒递增、
 *      而每片约 2 秒（#EXTINF:1.997），于是 Δts ≈ 墙钟的 **0.5 倍**。
 *      按「Δts >= 0.7×墙钟」判，正常台会被全判成垫片（实测威海/夏津 Δts=2s、墙钟 4.1s）。
 *   2. **也不能**用「Δts >= 1」当门槛：有些源一个分片十几秒，wait_sec=4 内 playlist
 *      本来就不会更新，Δts=0 但滞后只有 19 秒，那是正常在播（实测陕西 snrtv）。
 *
 * 滞后量才真正分得开（实测 2026-10-10）：
 *   正常直播 lag ≈ 0～19 秒；垫片 lag = 927 秒 ～ 16 天，差着好几个数量级。
 * 报告里给出 lag / dts / wall 三个数，人工一眼能复核。
 *
 * 已知局限（心里有数，不要越界解读结论）：
 *   - 只对 m3u8 有意义。mp3/aac 直链（蜻蜓/清华那批）判不了，返回 -1。
 *   - 依赖分片名里带 unix 时间戳（见 seg_ts）。解析不出来就返回 -1，**绝不猜**。
 *   - 只看时间戳与滞后，不校验音频内容本身。
 *   - LAG_MAX=300 秒的取值依据见下面代码处的实测数据；真有 CDN 缓冲超过 5 分钟的直播会被误判，
 *     但那本来也已经不适合当守播台了。
 */
#include "nzgwin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 命令行开关：--selftest --live 时打开 */
int g_opt_livecheck = 0;

static const char *LIVE_UA =
    "Mozilla/5.0 (Linux; Android 9; TV) AppleWebKit/537.36";

/* 相对地址拼成绝对地址；顺带折叠 /./ 与 /../ */
static int url_join(const char *base, const char *rel, char *out, int cap)
{
    if (!base || !rel || !*rel || !out || cap <= 0) return 0;

    if (_strnicmp(rel, "http://", 7) == 0 || _strnicmp(rel, "https://", 8) == 0) {
        _snprintf(out, cap, "%s", rel);
        return 1;
    }

    char tmp[1024];
    if (rel[0] == '/') {
        const char *p = strstr(base, "://");
        if (!p) return 0;
        const char *host = p + 3;
        const char *slash = strchr(host, '/');
        if (!slash) { _snprintf(out, cap, "%s%s", base, rel); return 1; }
        size_t hl = (size_t)(slash - host);
        if (hl + strlen(rel) + 1 >= sizeof(tmp)) return 0;
        memcpy(tmp, host, hl);
        strcpy(tmp + hl, rel);
    } else {
        _snprintf(tmp, sizeof(tmp), "%s", base);
        char *q = strchr(tmp, '?');
        if (q) *q = '\0';
        char *slash = strrchr(tmp, '/');
        if (!slash) return 0;
        slash[1] = '\0';                    /* 保留最后一级目录 */
        if (strlen(tmp) + strlen(rel) + 1 >= sizeof(tmp)) return 0;
        strcat(tmp, rel);
    }

    /* 折叠 /./ 与 /../（单遍，够 HLS 用） */
    char norm[1024];
    size_t o = 0;
    const char *p = tmp;
    while (*p) {
        if (p[0] == '/' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            p += 2;
            continue;
        }
        if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\0')) {
            while (o > 0 && norm[o - 1] != '/') o--;
            if (o > 0) o--;
            p += 3;
            if (*p == '/') p++;
            continue;
        }
        if (o + 1 < sizeof(norm)) norm[o++] = *p;
        p++;
    }
    norm[o] = '\0';
    if (o == 0) return 0;
    _snprintf(out, cap, "%s", norm);
    return 1;
}

/* 取一行（去掉行尾 \r）；返回 0 表示到末尾 */
static int pl_next_line(const char **pp, char *line, int cap)
{
    const char *p = *pp;
    if (!p || !*p) return 0;
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len >= (size_t)cap) len = (size_t)cap - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    size_t l = strlen(line);
    if (l && line[l - 1] == '\r') line[l - 1] = '\0';
    *pp = e ? e + 1 : p + strlen(p);
    return 1;
}

/* master 清单里取子清单地址；本来就是媒体列表则返回 0 */
static int pl_child(const char *text, char *out, int cap)
{
    int seen_inf = 0;
    const char *p = text;
    char line[1024];
    while (pl_next_line(&p, line, sizeof(line))) {
        if (line[0] == '#') {
            /* ⚠️ 长度必须用 sizeof()-1（=17），写死 18 会拿 ':' 和 '\0' 比，
             * 永远不相等 → master 认不出来 → 只能判不了活。踩过。 */
            if (strncmp(line, "#EXT-X-STREAM-INF",
                        sizeof("#EXT-X-STREAM-INF") - 1) == 0) seen_inf = 1;
            continue;
        }
        if (!line[0]) continue;
        if (seen_inf) { _snprintf(out, cap, "%s", line); return 1; }
        return 0;        /* 没有 STREAM-INF，第一条非注释就是分片 → 已是媒体列表 */
    }
    return 0;
}

/* 取最后一个分片 URI（最后一条非注释非空行） */
static int pl_last_segment(const char *text, char *out, int cap)
{
    int found = 0;
    const char *p = text;
    char line[1024];
    while (pl_next_line(&p, line, sizeof(line))) {
        if (!line[0] || line[0] == '#') continue;
        _snprintf(out, cap, "%s", line);
        found = 1;
    }
    return found;
}

/* 取最后一个分片的 EXTINF 时长（秒）；取不到返回 0。
 * 用途：分片时长比采样窗口长时，两次采样之间本来就不会换片（Δts=0），
 * 只剩 lag 一条判据 → 采样窗口必须至少跨过一个完整分片。 */
static double pl_last_extinf(const char *text)
{
    double last = 0;
    const char *p = text;
    char line[1024];
    while (pl_next_line(&p, line, sizeof(line))) {
        if (strncmp(line, "#EXTINF:", sizeof("#EXTINF:") - 1) == 0) {
            double v = atof(line + sizeof("#EXTINF:") - 1);
            if (v > 0) last = v;      /* 末尾那条 EXTINF 就是最后一个分片的 */
        }
    }
    return last;
}

/* 从分片 URI 里解析 unix 时间戳（**归一化到秒**）：取「最长的数字段」（>= 9 位）。
 * 实测见过三种形态：
 *   1791595797.ts                 秒级（litenews alivealone 域）
 *   4566b957a3ae…-1790168594.ts   <hash>-<秒>（litenews b49 域）
 *   1791542115000.ts              **毫秒**级（陕西 snrtv）
 * 哈希里也有零散数字，但长度都 < 9，只有时间戳够长。
 * 取不到返回 0（上层报「无法判定」）。
 *
 * ⚠️ 毫秒必须归一化：13 位直接当地秒用，会算出 -1.7e12 这种负滞后，
 * 把正常台误判成垫片（踩过）。任何 >= 1e11 的值都不可能是合理的秒级 unix 时间戳
 * （当前约 1.8e9，1e11 秒要到公元 5138 年），所以按毫秒处理。 */
static long long seg_ts(const char *uri)
{
    if (!uri) return 0;
    char name[1024];
    _snprintf(name, sizeof(name), "%s", uri);
    char *q = strchr(name, '?');
    if (q) *q = '\0';
    char *slash = strrchr(name, '/');
    const char *base = slash ? slash + 1 : name;

    size_t l = strlen(base), i = 0;
    long long best = 0;
    size_t bestlen = 0;
    while (i < l) {
        if (base[i] >= '0' && base[i] <= '9') {
            size_t s = i;
            while (i < l && base[i] >= '0' && base[i] <= '9') i++;
            size_t n = i - s;
            if (n >= 9 && n > bestlen) {
                best = _strtoi64(base + s, NULL, 10);
                bestlen = n;
            }
        } else {
            i++;
        }
    }
    if (best >= 100000000000LL) best /= 1000;      /* 毫秒 → 秒 */
    return best;
}

int probe_hls_liveness(const char *url, int wait_sec)
{
    if (!url || !*url) return -1;
    if (!strstr(url, "m3u8")) return -1;      /* 非 HLS 判不了 */
    if (wait_sec < 3)  wait_sec = 3;
    if (wait_sec > 20) wait_sec = 20;

    long long t1 = 0, t2 = 0, wall1 = 0, wall2 = 0;

    for (int round = 0; round < 2; round++) {
        char *text = NULL;
        if (!http_get(url, &text, LIVE_UA, NULL, NULL)) {
            log_event("活性", "无法判定：拉主清单失败（%s）", round ? "第二次" : "第一次");
            return -1;
        }

        char child[1024] = {0};
        int has_child = pl_child(text, child, sizeof(child));
        if (has_child) {
            char joined[1024];
            int okj = url_join(url, child, joined, sizeof(joined));
            free(text);
            if (!okj) {
                log_event("活性", "无法判定：子清单地址拼接失败");
                return -1;
            }
            if (!http_get(joined, &text, LIVE_UA, NULL, NULL)) {
                log_event("活性", "无法判定：拉子清单失败（可能签名过期，源是签名 URL）");
                return -1;
            }
        }

        char seg[1024] = {0};
        int okseg = pl_last_segment(text, seg, sizeof(seg));
        double extinf = pl_last_extinf(text);
        free(text);
        if (!okseg) {
            log_event("活性", "无法判定：清单里没有分片行%s",
                      has_child ? "（子清单）" : "（主清单）");
            return -1;
        }

        long long ts = seg_ts(seg);
        if (round == 0) {
            if (ts <= 0) {
                log_event("活性", "无法判定：分片名里没有 unix 时间戳（%s）", seg);
                return -1;
            }
            t1 = ts;
            wall1 = unix_now();
            /* 采样窗口自适应：分片 9.6 秒一片、窗口只有 4 秒时，Δts 必然是 0，
             * 而 litenews 的时间轴本身可能落后墙钟几百秒（邹平实测 lag≈390s），
             * 于是「lag>300 且 Δts=0」→ 真在播的台被误判成垫片（2026-11-02 踩到）。
             * 窗口至少跨过一个完整分片，Δts 才有意义。 */
            int wait = wait_sec;
            if (extinf > 0) {
                int need = (int)(extinf * 1.2) + 1;
                if (need > wait) wait = need;
                if (wait > 20) wait = 20;
            }
            log_event("活性", "采样窗口 %ds（分片 %.1fs）", wait, extinf);
            Sleep((DWORD)wait * 1000);
        } else {
            t2 = ts;
            wall2 = unix_now();
        }
    }

    if (t2 <= 0) return -1;                    /* 第二轮解析不出，放弃 */

    /* 最新分片比墙上时钟落后多少秒 */
    long long lag = wall2 - t2;
    long long dts = t2 - t1;
    long long wall = wall2 - wall1;
    if (wall < 1) wall = 1;

    /* 滞后阈值。实测 2026-10-10（45 台全量）：
     *   正常台   lag = 0 ～ 140 秒（CDN 缓冲差异，郓城 FM89.5 实测 140 秒仍在推）
     *   垫片台   lag = 927 秒（莒县）～ 16.5 天（临邑），中间没有过渡带
     * 取 300 秒：既容忍真直播的 CDN 缓冲，又能把垫片干净分开。
     * 调到 120 秒会把郓城这种真在播的台误判成垫片（实测踩过）。
     * 参考：超过 300 秒的「直播」听感上已经是延时广播，不适合当守播台。 */
    const long long LAG_MAX = 300;

    /* 第二判据（2026-10-23 补）：**分片在前进就说明在播**。
     *
     * 只认 lag 会把一批真在播的台误判成垫片：litenews 的 alivealone302 域
     * 分片名用的是「服务器时间轴」，可能比墙钟慢几十分钟到十几小时
     * （实测惠民 lag=2168s、昌邑 lag=21069s），但两次采样分片时间戳照样
     * 前进 1~2 秒 —— 那是活的推流，不是冻结垫片。
     *
     * 真正的循环垫片特征是「分片名不动」（Δts=0），lag 才会一路涨到
     * 927 秒 ~ 16 天。所以判据改成：lag 正常 → 在播；lag 大但 Δts>0
     * → 仍在播（时间轴偏移，报告里注明）；lag 大且 Δts=0 → 垫片。
     *
     * 注意 dts>0 的门槛必须是 >0 而不是跟墙钟比（见文件头坑 1：
     * litenews 分片 2 秒一片但名字只 +1 秒，Δts≈0.5×墙钟是正常的）。 */
    int live = (lag <= LAG_MAX) || (dts > 0);

    if (live && lag > LAG_MAX) {
        log_event("活性", "在播（时间轴偏移）：滞后 %llds 但分片在前进（Δts=%llds，墙上 %llds）",
                  lag, dts, wall);
        return 1;
    }

    /* Δts 只作参考信息，**不参与判定**：
     * 有些源一个分片就十几秒，wait_sec=4 之内 playlist 不会更新，Δts 自然是 0，
     * 但只要滞后仍在正常范围，它就是在播的（实测陕西 snrtv：Δts=0、滞后 19 秒 = 正常）。 */
    if (live)
        log_event("活性", "在播：滞后 %llds（阈值 %llds），本轮分片时间戳变化 %llds（墙上 %llds）",
                  lag, LAG_MAX, dts, wall);
    else
        log_event("活性", "疑似垫片：最新分片滞后 %llds（超过 %llds），分片时间戳变化 %llds",
                  lag, LAG_MAX, dts);
    return live;
}
