/* 自愈状态机 + 看门狗 + 网络探测
 *
 * 这是整个移植里最容易出错的部分，严格按安卓版 PlayerManager 的语义复刻。
 *
 * ================================ 铁律回顾 ==================================
 * 铁律 A：判死只能在 MODE_PLAYING 下做。退避等待期（next_retry_at 未到）
 *         必须完全静默，绝不重复计数、绝不重复安排重试。
 * 铁律 B：判死间隔 > 封顶重试间隔。本实现看门狗 5s/跳、判死阈值 15s；
 *         退避封顶 30s。15s < 30s，且判死只在 PLAYING 态发生，
 *         所以不会出现「15s 判死 vs 30s 退避」的自锁（cz-player 踩过这个坑）。
 * 铁律 C：只重连当前频道当前源。代码里没有任何换台/换源分支。
 * ============================================================================
 */
#include "nzgwin.h"
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

#define WATCHDOG_PERIOD_MS 5000

/* 线程同步：播放线程可能同时在改 state */
static CRITICAL_SECTION g_state_lock;
static int g_lock_ready = 0;

static void ensure_lock(void)
{
    static LONG init = 0;
    if (InterlockedCompareExchange(&init, 1, 0) == 0)
        InitializeCriticalSection(&g_state_lock);
    g_lock_ready = 1;
}

static void lock(void)   { ensure_lock(); EnterCriticalSection(&g_state_lock); }
static void unlock(void) { if (g_lock_ready) LeaveCriticalSection(&g_state_lock); }

/* ---------------- 失败登记与退避 ----------------
 *
 * 语义与安卓版 fail() 一致：
 *   第一次失败记录 last_fail_at、计入统计、进入 RETRYING；
 *   之后每次失败只是把下一次重试按退避表往后推，不重复计统计。
 */
static void fail(const char *reason, int is_stall)
{
    lock();

    if (g_app.mode == MODE_WAITING_NETWORK) { unlock(); return; }

    if (g_app.mode == MODE_PLAYING) {
        g_app.last_fail_at = now_ms();
        g_app.stat_disconnects++;
        if (is_stall) g_app.stat_stalls++;
        log_event("断线", "%s", reason);
        g_app.mode = MODE_RETRYING;
    }

    g_app.failures++;

    /* 退避表 0/5/15/30，最后一档循环 */
    const Resilience *res = &g_app.cfg.res;
    int idx = g_app.failures - 1;
    if (idx < 0) idx = 0;
    if (idx >= res->retry_interval_count) idx = res->retry_interval_count - 1;
    int delay = res->retry_intervals[idx];

    g_app.next_retry_at = now_ms() + delay * 1000.0;
    log_event("重连", "第 %d 次失败，%s重试", g_app.failures,
              delay == 0 ? "立即" : (delay == 1 ? "1 秒后" : "若干秒后"));
    unlock();

    ui_refresh();
}

/* 恢复登记：任何时候出现「正在播放」且此前不在 PLAYING 态，就结算这次自愈 */
static void on_success(void)
{
    lock();
    if (g_app.mode == MODE_PLAYING) { unlock(); return; }

    double cost = 0;
    if (g_app.last_fail_at > 0) cost = (now_ms() - g_app.last_fail_at) / 1000.0;

    char msg[160];
    _snprintf(msg, sizeof(msg), "恢复正常，耗时 %.1f 秒，共尝试 %d 次", cost, g_app.failures);
    log_event("恢复", "%s", msg);

    g_app.failures = 0;
    g_app.soft_count = 0;
    g_app.stall_count = 0;
    g_app.last_fail_at = 0;
    g_app.next_retry_at = 0;
    g_app.mode = MODE_PLAYING;
    unlock();

    /* 恢复时把音量重新顶一次 —— 重连期间用户很可能在按音量键 */
    player_force_max_volume();
    ui_refresh();
}

/* ---------------- 重连分级 ----------------
 *
 * 安卓版分级：软重连 N 次 → 硬重连 M 次 → 休眠等待（网络探测）。
 * Win 上没有「软/硬播放器实例」之分（waveOut 会话随线程重建），
 * 但分级语义要保留：前 N 次只做「快速重启播放线程」，
 * 后面再失败才进入更重的流程，最后进休眠等待网络。
 */

static void enter_waiting_network(void)
{
    lock();
    if (g_app.mode == MODE_WAITING_NETWORK) { unlock(); return; }
    g_app.mode = MODE_WAITING_NETWORK;
    unlock();
    log_event("等待", "连续重试失败，进入休眠等待（每 %d 秒探测网络）",
              g_app.cfg.res.network_probe_sec);
    ui_refresh();
}

/* 看门狗到点后执行一次重连尝试 */
static void attempt_retry(void)
{
    lock();
    int failures = g_app.failures;
    int soft = g_app.soft_count;
    const Resilience *res = &g_app.cfg.res;
    int soft_max = res->soft_retry_max;
    int hard_max = res->hard_retry_max;
    g_app.next_retry_at = 0;
    unlock();

    if (!net_probe()) {
        enter_waiting_network();
        return;
    }

    if (soft < soft_max) {
        lock(); g_app.soft_count++; unlock();
        log_event("重连", "软重连（第 %d 次，保留状态）", soft + 1);
    } else if (failures <= soft_max + hard_max) {
        lock(); g_app.stall_count++; unlock();
        log_event("重连", "硬重连（重建播放会话）");
    } else {
        enter_waiting_network();
        return;
    }

    /* 分级只影响日志与计数，实际动作一致：
     * 释放当前播放线程并重新拉流（只针对当前频道当前源） */
    player_hard_rebuild("重连触发");
}

/* ---------------- 网络探测 ----------------
 *
 * 用 WinHTTP 请求一个轻量目标判断连通性。
 * 不用 ICMP（Win7 需要额外权限，且很多机器禁 ping）。
 */

int net_probe(void)
{
    /* 优先探测一个必定在线的地址；不通就认为离线。
     * 选的是静态资源，失败不会产生副作用。
     * 用 HTTP 而非 ICMP：Win7 的 ping 需要额外权限，且很多机器禁 ping。 */
    HINTERNET s = WinHttpOpen(L"shoubo-nzg/1.0",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 1;   /* 探测不了不等于离线，别把正常网络误判掉 */
    http_enable_modern_tls(s);   /* 以后若改成探 https 接口，这里同样需要 */
    WinHttpSetTimeouts(s, 4000, 4000, 4000, 5000);

    int online = 0;
    const WCHAR *hosts[] = { L"www.baidu.com", L"www.qq.com" };
    for (int i = 0; i < 2 && !online; i++) {
        HINTERNET c = WinHttpConnect(s, hosts[i], INTERNET_DEFAULT_HTTP_PORT, 0);
        if (!c) continue;
        HINTERNET r = WinHttpOpenRequest(c, L"HEAD", L"/", NULL,
                                         WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (r) {
            if (WinHttpSendRequest(r, L"", 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(r, NULL)) {
                /* ⚠️ size 必须 = sizeof(status)。
                 * 传 0 会让 WinHttpQueryHeaders 直接失败、status 留在 0，
                 * 于是 online 恒为 0 —— 表现就是「断流后进入等待网络状态，
                 * 网络其实早通了却永远醒不过来」，把播放器永久卡死。
                 * 踩过：与 resolve.c 里同一个写法错误。 */
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(r,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &size, WINHTTP_NO_HEADER_INDEX);
                /* 3xx 也算通：说明网络链路是活的 */
                online = (status >= 200 && status < 500);
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return online;
}

/* ---------------- 看门狗主循环（主线程定时器调用） ---------------- */

void player_tick_watchdog(void)
{
    if (g_app.mode == MODE_WAITING_NETWORK) {
        /* 休眠等待：探测网络，通了就立刻重连，不等满整轮 */
        if (net_probe()) {
            log_event("恢复", "网络探测成功，立即重连当前频道");
            g_app.mode = MODE_RETRYING;
            g_app.failures = 0;
            g_app.soft_count = 0;
            g_app.stall_count = 0;
            ui_refresh();
            player_hard_rebuild("网络恢复");
        }
        return;
    }

    if (g_app.mode == MODE_RETRYING) {
        /* 退避等待期：铁律 A，这里必须完全静默，只在到点时才动作 */
        double due = g_app.next_retry_at;
        if (due > 0 && now_ms() >= due) {
            attempt_retry();
        } else {
            return;   /* 还没到点，什么都不做 */
        }
        return;
    }

    /* ---- 只有 PLAYING 态才允许判死（铁律 A） ---- */
    if (g_app.mode != MODE_PLAYING) return;

    if (g_app.resolving) return;      /* 正在解析动态源，不算断流 */

    /* 线程已死但状态机还停在 PLAYING：立即接管。
     * ⚠️ 2026-10-13 实机「几天后不出声」死局的根因之一：
     * 重建后新线程两条源都没打开、从未拿到首帧就退出 ——
     * g_last_audio_at 停在 0，旧的「首帧未到不判死」分支永远放行，
     * 状态机卡在 PLAYING 且无人播放，零日志永久无声。 */
    if (!player_thread_alive()) {
        fail("播放线程已退出", 0);
        return;
    }

    /* ---- 动态签名源：到期前主动换票 ----
     *
     * litenews 解析出来的是一张有时效的 100ycdn 会话票（wsSession /
     * wsIPSercert / wsBindIP），hntv 是 wsSecret + wsTime。票过期后
     * CDN 往往不是干脆 403，而是把连接挂住不发数据 —— 客户端表现为
     * 「播了半天后一直不动」，且 av_read_frame 可能长时间不返回。
     * 与其等它断（必带 15 秒静音空档，还可能踩到卡死路径），
     * 不如在到期前主动重建一次会话，重新解析换新票。
     *
     * 这里不判「是否正在出声」：正因出声正常才要提前换，断了的场景
     * 由下面的静音判死负责。 */
    if (g_app.cfg.res.dynamic_refresh_sec > 0 && player_dynamic_source()) {
        double age = g_app.resolve_at > 0 ? (now_ms() - g_app.resolve_at) / 1000.0 : 0;
        if (g_app.resolve_at > 0 && age > g_app.cfg.res.dynamic_refresh_sec) {
            log_event("换票", "动态签名源已用 %.0f 分钟，主动重新解析（阈值 %d 秒）",
                      age / 60.0, g_app.cfg.res.dynamic_refresh_sec);
            player_hard_rebuild("动态源定时换票");
            return;
        }
    }

    int silent_sec = player_seconds_since_audio();
    int threshold = g_app.cfg.res.audio_stall_sec;
    if (threshold < 5) threshold = 5;   /* 阈值不得低于看门狗周期 */

    if (silent_sec < 0) {
        /* 首帧未到：给 60 秒宽限（resolve + 双源 open 最坏约 45 秒），
         * 超时说明线程活着但持续拿不到数据 —— 判死重建。 */
        if (player_spawn_elapsed() > 60.0)
            fail("首帧 60 秒未到（打开流持续失败）", 0);
        return;
    }
    if (silent_sec > threshold) {
        char msg[128];
        _snprintf(msg, sizeof(msg), "音频停滞 %d 秒（判定卡死）", silent_sec);
        fail(msg, 1);
        return;
    }

    /* 正在正常出声音 → 若之前处于非 PLAYING 则结算恢复 */
    if (silent_sec == 0 && (g_app.failures > 0 || g_app.soft_count > 0))
        on_success();
}