/* 播放内核 + 自愈状态机
 *
 * ============================ 移植铁律（踩过的坑都在这） ============================
 * 1) 判死只能在「应该正在播出」的状态做。退避等待期必须完全静默，
 *    否则看门狗会在等重连的时候又判一次死，退避永远走不到头（自锁）。
 * 2) 判死间隔必须大于封顶重试间隔。退避封顶是 30s，若看门狗 15s 就判死，
 *    会形成 判死→退避→等待中又被判死 的循环，重连代码永远执行不到。
 *    本实现：看门狗 5s 一跳，判死阈值 15s，退避封顶 30s，满足。
 * 3) 重建播放器前必须检查 started 标志（释放后的迟到回调不许再重建）。
 * 4) 自动逻辑只重连「当前频道当前源」，不存在任何换台/换源逻辑。
 *    切台只有用户主动这一个入口。
 * ===================================================================================
 *
 * 音频输出：waveOut + 4 槽环形缓冲。
 *   不用 WAVEHDR 回调（回调在中断上下文，容易出并发 bug），
 *   而是每轮扫描 WHDR_DONE 回收槽位。槽满就等一小会，
 *   等不到就丢掉这一帧 —— 保证永不阻塞读流、永不无限延迟。
 *   比「单缓冲 + waveOutReset」轻得多，也不会造成声音断续。
 */

#include "nzgwin.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* WASAPI 端点音量（2026-11-03）：waveOut 那套在 Vista+ 上够不着设备主音量，
 * 详见 player_force_max_volume() 上方注释。Win7 起就有，不影响最低系统。 */
#include <mmdeviceapi.h>
#include <endpointvolume.h>
/* 读默认端点的友好名（v1.16.2）：WAVE_MAPPER 只能问出「Microsoft 声音映射器」，
 * 问不出声音实际落到哪块声卡。IPropertyStore / PROPVARIANT 的声明在这两个头里。 */
#include <propsys.h>
#include <propidl.h>

/* 输出格式：44100Hz / 16bit / 立体声 */
#define OUT_RATE     44100
#define OUT_CHANNELS 2
#define OUT_BYTES_PER_SEC (OUT_RATE * OUT_CHANNELS * 2)   /* 176400 B/s */

/* 每槽约 0.5 秒；4 槽合计约 2 秒，足够抗抖动 */
#define SLOT_BYTES   (OUT_BYTES_PER_SEC / 2)
#define WO_SLOTS     4

/* ---------------- 播放线程态 ---------------- */

static HANDLE       g_thread = NULL;
static volatile LONG g_thread_stop = 0;
/* 会话代际：spawn 每次重建时 +1，线程启动时快照自己的代际。
 * ⚠️ 2026-10-13 实机反馈「连跑几天后不出声」的根因修复：
 * stop_play_thread 只等旧线程 6 秒，旧线程卡在 FFmpeg open/reconnect 里时
 * 被迫放弃 —— 此前 g_thread_stop 在这里被复位 0，旧线程从 FFmpeg 出来一看
 * 「没人叫我停」就继续播，**永远持有自己的 waveOut 设备不释放**。
 * 半死流反复触发重连 → 僵尸线程逐个累积 → 几天后 waveOut 设备资源耗尽 →
 * 新会话 waveOutOpen 永远失败 → 永久无声（状态机还在转但设备开不出来）。
 * 现在改由代际判生死：stop 不复位标志，spawn 在新线程启动前才复位；
 * 僵尸的 my_gen 与当前 gen 不匹配，从 FFmpeg 一出来立即退出、释放设备。 */
static volatile LONG g_session_gen = 0;
/* 某台的主源打不开、镜像接管成功后，记住这个台：本会话内再进这个台直接走镜像，
 * 免得每次重连都先在坏源上白等 10 秒超时。用户切到别的台再回来仍生效；
 * 若镜像哪天也失败、主源能通，则自动解除（见 play_thread 的回退逻辑）。 */
static long long g_backup_sticky_id = 0;
static volatile LONG g_started = 0;
static HWAVEOUT     g_wo = NULL;
/* 端点主音量句柄改走 WASAPI（2026-11-03 替换掉原来的 waveOut g_main_out）。 */
static IMMDeviceEnumerator *g_mmdev  = NULL;
static IAudioEndpointVolume *g_epvol = NULL;
static int          g_com_ready  = 0;
static int          g_force_first = 1;   /* 首次读到的端点音量要进日志 */

/* 本次会话实际打开的输出设备名（waveout_open 填，心跳日志读）。
 * 排查「程序正常但不响」时，这一行能直接分辨「设备被切到 HDMI」这类现场事故。 */
static char         g_audio_dev_name[256] = "(未打开)";
/* 已成功提交给 waveOut 的音频块数 —— 心跳日志用它证明「程序真的在写 PCM」。
 * 这个数在涨而用户听不见 ⇒ 问题在系统输出侧，不在解码侧。 */
static volatile LONG g_frames_written = 0;

/* v1.16.2：默认播放端点的**真名**。
 * WAVE_MAPPER 只会报「Microsoft 声音映射器」，等于什么都没说；而
 * 「默认设备被切到 HDMI / 虚拟声卡」是「看着在播但没声」的第二大原因
 * （第一大是端点音量被拉到 0）。有了这个名字，远程看日志就能直接判死。 */
static char         g_default_ep_name[192] = "(未取到)";

/* v1.16：最近一次读到的**系统输出端点主音量**（0~100；-1 = 未知/WASAPI 不可用）。
 * 单独存一份是为了让心跳能打出来 —— 机房那台机器只能靠日志远程判「到底是程序没声
 * 还是系统音量被拉到 0」，而这件事 waveOut 侧永远看不到。 */
static int          g_ep_vol_pct = -1;

/* HLS「分片过期跳过」警告的节流状态（见 ff_log_cb 里的说明） */
static volatile LONG g_expired_cnt = 0;
static double        g_expired_last_log = 0;

/* ---------------- FFmpeg 日志接入（排障用，2026-10-10 加） ----------------
 *
 * 为什么要接：Win7 真机上「打开流失败」只给了我们自己的 averr 数字，
 * 而 FFmpeg 内部（尤其走 Schannel 的 https 协议）会把**真正的失败原因**
 * 用 av_log(ERROR) 打出来，比如：
 *   - "schannel: next InitializeSecurityContext failed: ..." （握手/证书层）
 *   - "HTTP error 403 Forbidden"                            （服务端拒绝）
 *   - "Connection timed out"                                （网络层）
 * 之前设了 AV_LOG_QUIET 全吞了，等于把最有价值的一层证据扔掉了。
 *
 * 现在装一个回调，把 WARNING 及以上转进我们自己的日志（[FFmpeg] 前缀）。
 * 级别刻意压在 WARNING 而不是 INFO/DEBUG：HLS 播放时每个分片都会打一行
 * "Opening '...'"（DEBUG），mp3 的 "Skipping junk" 是 INFO —— 全收会把
 * 守播盒子的日志刷爆。握手失败、HTTP 错误都是 ERROR，一条不漏。
 *
 * 注意：装了回调之后 av_log 默认的 stderr 输出就没了 —— 反而是好事，
 * GUI 程序本来就没有控制台；selftest 的报告走的是我们自己写的 selftest.txt。
 */
static void ff_log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > AV_LOG_WARNING) return;      /* 比 WARNING 更轻的不要 */

    char line[1024];
    int print_prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &print_prefix);

    /* 去掉行尾换行，多行的压成一行 */
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' '))
        line[--l] = '\0';
    for (size_t i = 0; i < l; i++)
        if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';
    if (!line[0]) return;

    /* HLS 分片过期警告节流（2026-11-03 加）。
     *
     * `skipping N segments ahead, expired from playlists` 对 litenews 那批
     * playlist 窗口只有 4~6 秒的台是**常态**（全 36 台如此，见 make_open_opts
     * 里 live_start_index 的实测说明）：客户端稍微落后一片就必然触发，
     * 一天能刷几百行。后果不只是日志变大 —— 用户直接把这条当成「没声音的
     * 原因」来报障，把真正有用的断线/握手信息挤出了视野。
     *
     * 改成 60 秒最多一行、并带上这段时间的累计次数：既不刷屏，也不丢可观测性
     * （真异常时计数会飙高，一眼能看出来）。 */
    if (strstr(line, "expired from playlists")) {
        InterlockedIncrement(&g_expired_cnt);
        double now = now_ms();
        if (g_expired_last_log > 0 && now - g_expired_last_log < 60000.0) return;
        long n = InterlockedExchange(&g_expired_cnt, 0);
        g_expired_last_log = now;
        log_event("FFmpeg", "警告：HLS 分片过期跳过（近 60 秒累计 %ld 次；"
                            "源 playlist 窗口仅数秒，属正常抖动，非故障）", n);
        return;
    }

    const char *lv = (level <= AV_LOG_ERROR) ? "错误" : "警告";
    log_event("FFmpeg", "%s：%s", lv, line);
}

void fflog_install(void)
{
    av_log_set_callback(ff_log_cb);
    av_log_set_level(AV_LOG_WARNING);
}

/* waveOut 会话：设备 + 4 槽缓冲，**整份属于单个播放线程**。
 *
 * ⚠️ 2026-10-12 由全局改为线程局部。原因：stop_play_thread 只等旧线程 6 秒，
 * 而 avformat_open_input 超时 10 秒、FFmpeg reconnect 还可能无限循环 ——
 * 旧线程（僵尸）退出时会 waveout_close 全局设备，恰好关掉/乱写**新线程**
 * 刚建好的会话，表现为「半死流场景下反复无声、要多绕好几轮重连才收敛」。
 * 现在每个会话的设备/槽位都在自己线程的 WoCtx 里，僵尸线程死活碰不到
 * 新会话的资源；全局 g_wo 仅作 UI 线程音量键的「当前活跃设备」句柄。
 */
typedef struct {
    HWAVEOUT  wo;
    WAVEHDR   hdr[WO_SLOTS];
    uint8_t  *slot[WO_SLOTS];
    int       used[WO_SLOTS];
} WoCtx;

/* 用于检测「音频断流」：最近一次成功提交 PCM 的时刻 */
static volatile double g_last_audio_at = 0;

/* 播放线程存活标志 + 本会话线程启动时刻。
 * ⚠️ 2026-10-13 实机日志复盘出的第二个洞（与僵尸竞态并列）：
 * 重建后若新线程**从未拿到首帧就退出**（重建窗口恰逢断网/源挂、两条源都
 * open 失败），g_last_audio_at 停在 0 —— 看门狗的「首帧未到不判死」分支
 * 会永远放行，状态机停在 PLAYING 但没有任何线程在播，零日志永久无声
 * （部署机 10-12 20:48 ~ 10-13 08:30 实测复现）。
 * 现在线程退出前清 alive 标志，看门狗发现 PLAYING 态却无线程时立即 fail
 * 接管；「首帧未到」另给 60 秒宽限（覆盖 resolve+双源 open 最坏 ~45 秒），
 * 超时照样判死重建。 */
static volatile LONG   g_thread_alive = 0;
static volatile double g_spawn_at = 0;

static const Channel *cur_channel(void)
{
    if (g_app.cur_index < 0 || g_app.cur_index >= g_app.cfg.channel_count) return NULL;
    return &g_app.cfg.channels[g_app.cur_index];
}

/* 频道自定义头拼成 FFmpeg 的 headers 串。
 * 头部变化必须重建实例（FFmpeg 的 headers 在 avformat_open_input 时固定），
 * 与安卓版「头部不同必须重建 DataSource.Factory」是同一个坑。 */
static void build_headers(const Channel *ch, char *out, int cap)
{
    size_t used = 0;
    out[0] = '\0';
    if (!ch) return;
    for (int i = 0; i < ch->headers.header_count; i++) {
        const char *n = ch->headers.header_name[i];
        const char *v = ch->headers.header_value[i];
        if (_stricmp(n, "User-Agent") == 0) continue;  /* 走 user_agent 选项 */
        int w = _snprintf(out + used, (size_t)(cap - (int)used), "%s: %s\r\n", n, v);
        if (w > 0) used += (size_t)w;
        if ((int)used >= cap - 8) break;
    }
}

/* ---------------- waveOut 输出 ----------------
 * 以下 close/open/recycle/free_slot 全部操作调用方线程自己的 WoCtx，
 * 不碰任何全局状态 —— 这是「僵尸线程不污染新会话」的关键。 */

static void waveout_close(WoCtx *c)
{
    if (!c || !c->wo) return;
    waveOutReset(c->wo);
    for (int i = 0; i < WO_SLOTS; i++) {
        if (c->used[i]) {
            waveOutUnprepareHeader(c->wo, &c->hdr[i], sizeof(WAVEHDR));
            c->used[i] = 0;
        }
        if (c->slot[i]) { free(c->slot[i]); c->slot[i] = NULL; }
    }
    waveOutClose(c->wo);
    /* 只有当前活跃会话才摘全局音量句柄；僵尸线程的旧设备不动它 */
    if (g_wo == c->wo) g_wo = NULL;
    c->wo = NULL;
}

static void apply_ctx_volume(WoCtx *c)
{
    if (!c || !c->wo) return;
    int v = g_app.muted ? 0 : g_app.volume;
    double gain = v / 100.0;
    if (gain > 1.0) gain = 1.0;
    if (gain < 0.0) gain = 0.0;
    DWORD vol = (DWORD)(gain * 65535.0);
    waveOutSetVolume(c->wo, (vol << 16) | vol);
}

static int waveout_open(WoCtx *c)
{
    if (!c) return 0;
    if (c->wo) return 1;   /* 本线程已打开，直接复用 */

    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = OUT_CHANNELS;
    wf.nSamplesPerSec  = OUT_RATE;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (WORD)(OUT_CHANNELS * 16 / 8);
    wf.nAvgBytesPerSec = OUT_RATE * wf.nBlockAlign;

    MMRESULT r = waveOutOpen(&c->wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        log_event("音频", "waveOut 打开失败（%u）", r);
        c->wo = NULL;
        return 0;
    }

    for (int i = 0; i < WO_SLOTS; i++) {
        c->slot[i] = (uint8_t *)malloc(SLOT_BYTES);
        if (!c->slot[i]) {
            log_event("音频", "音频缓冲分配失败");
            waveout_close(c);
            return 0;
        }
        memset(&c->hdr[i], 0, sizeof(WAVEHDR));
        c->used[i] = 0;
    }
    /* 成功后把全局句柄指向本会话 —— 音量键（UI 线程）由此作用于当前设备 */
    g_wo = c->wo;
    /* 把真实输出设备名打进日志（2026-11-03 加）。
     * 动机：接到「程序一切正常但不响」的报障时，最需要分辨的就是
     * 「waveOut 到底打开了哪块设备」—— 接了显示器/电视的机器上，
     * 默认播放设备被切到 HDMI 是高频现场事故，而这个状态在程序侧
     * 完全无感知（写 PCM 照常成功、看门狗照常放行）。
     * 有这一行，下次看日志就能直接排除或确认设备问题。 */
    WAVEOUTCAPSW caps;
    memset(&caps, 0, sizeof(caps));
    if (waveOutGetDevCapsW(WAVE_MAPPER, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
        char devname[256];
        wide_to_utf8_or_die(devname, sizeof(devname), caps.szPname);
        _snprintf(g_audio_dev_name, sizeof(g_audio_dev_name), "%s", devname);
    } else {
        _snprintf(g_audio_dev_name, sizeof(g_audio_dev_name), "(设备名读取失败)");
    }
    log_event("音频", "waveOut 已就绪（%dHz/%dch，%d 槽 × %d 字节）",
              OUT_RATE, OUT_CHANNELS, WO_SLOTS, SLOT_BYTES);
    log_event("音频", "输出设备：WAVE_MAPPER → %s", g_audio_dev_name);

    /* ⚠️ 上面那行拿到的永远是「Microsoft 声音映射器」——WAVE_MAPPER 是虚拟
     * 映射器，问不出它实际落在哪块声卡上。而「接显示器的机器默认输出被切到
     * HDMI」正是「程序正常但不响」的高发场景，所以再把本机设备清单整个列出来：
     * 机器上有几块声卡、有没有 HDMI 输出，一眼可见。
     * 只在本次进程里打一次（重建会话时会重复走这里，不必重复枚举）。
     * 枚举失败就跳过，绝不因为它影响播放。 */
    static int devlist_logged = 0;
    if (!devlist_logged) {
        devlist_logged = 1;
        UINT ndev = waveOutGetNumDevs();
        char devlist[600];
        devlist[0] = '\0';
        UINT shown = 0;
        for (UINT i = 0; i < ndev && shown < 8; i++) {
            WAVEOUTCAPSW dc;
            memset(&dc, 0, sizeof(dc));
            if (waveOutGetDevCapsW(i, &dc, sizeof(dc)) != MMSYSERR_NOERROR) continue;
            char nm[128];
            wide_to_utf8_or_die(nm, sizeof(nm), dc.szPname);
            if (!nm[0]) continue;
            if (devlist[0]) strncat(devlist, " | ", sizeof(devlist) - strlen(devlist) - 1);
            strncat(devlist, nm, sizeof(devlist) - strlen(devlist) - 1);
            shown++;
        }
        log_event("音频", "本机输出设备共 %u 个：%s", ndev,
                  devlist[0] ? devlist : "(枚举不到，只能走 WAVE_MAPPER)");
    }
    return 1;
}

/* 回收已播完的槽 */
static void waveout_recycle(WoCtx *c)
{
    if (!c || !c->wo) return;
    for (int i = 0; i < WO_SLOTS; i++) {
        if (c->used[i] && (c->hdr[i].dwFlags & WHDR_DONE)) {
            waveOutUnprepareHeader(c->wo, &c->hdr[i], sizeof(WAVEHDR));
            c->used[i] = 0;
        }
    }
}

static int waveout_free_slot(WoCtx *c)
{
    for (int i = 0; i < WO_SLOTS; i++) {
        if (!c->used[i]) return i;
    }
    return -1;
}

/* 音量：最终输出比例 = 软件音量。
 *
 * ⚠️ 这里原来还乘了一个「数字增益」boost（毫巴），看着像安卓版的 AudioBooster：
 *      gain = v/100 * 10^(boost_mb/2000)   然后 `if (gain > 1.0) gain = 1.0`
 *  boost_mb=1200 时 10^(1200/2000) ≈ 3.98，于是 **音量只要超过 26% 就被钳到 100%** ——
 *  70%、80%、100% 听起来一模一样，音量键形同虚设（用户要求「启动后自动 70% 音量」
 *  之所以必须做，正是因为这个：实际输出恒为满音量）。
 *
 *  根因是物理的：`waveOutSetVolume` 的上限就是 100%，往设备音量上乘增益，
 *  只会让 26% 以上的档位全部饱和。要真正的数字增益只能做在 **PCM 域**（乘采样并限幅），
 *  那是另一件事、风险更高，本次不做。
 *
 *  现在软件音量 = 最终输出比例：线性、可预期、音量键真正有效。
 *  boost_mb 字段保留（配置兼容、与安卓版字段对齐），但不再参与设备音量计算。
 */
/* 音量键（UI 线程）入口：作用于「当前活跃会话」的设备。
 * g_wo 由 waveout_open 成功时指到最新会话；僵尸线程退出不碰它。
 * 若当前没有活跃会话（g_wo 为 NULL 或悬空——僵尸线程关闭自己设备后
 * 句柄失效），waveOutSetVolume 只是返回错误码，无副作用。 */
static void apply_waveout_volume(void)
{
    if (!g_wo) return;

    int v = g_app.muted ? 0 : g_app.volume;
    double gain = v / 100.0;

    if (gain > 1.0) gain = 1.0;
    if (gain < 0.0) gain = 0.0;

    DWORD vol = (DWORD)(gain * 65535.0);
    waveOutSetVolume(g_wo, (vol << 16) | vol);
}

/* ---------------- 音量（对外） ---------------- */

void player_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    int was = g_app.volume;
    int was_muted = g_app.muted;
    g_app.muted = 0;
    g_app.volume = v;
    apply_waveout_volume();
    /* 只在值真的变了才记一行：音量键长按会连发，不节流会把日志刷满。
     * （原来这里完全不记日志，导致「声音忽然变小」在事后无从追查。） */
    if (v != was || was_muted)
        log_event("音量", "%s %d%%", was_muted ? "取消静音，音量" : "音量调至", v);
    ui_refresh();
    config_save_state();
}

void player_toggle_mute(void)
{
    g_app.muted = !g_app.muted;
    apply_waveout_volume();
    ui_refresh();
    config_save_state();
    log_event("音量", g_app.muted ? "静音" : "取消静音");
}

/* 距离最近一次提交音频过了多少秒。
 * 返回 -1 表示「还没出过声音」（首帧未到），此时不能判死。
 * 这是渲染层检测的核心：进程活着、socket 也通，但实际没有音频数据 ——
 * 那种情况不触发任何回调，只能靠这个计时兜住。 */
int player_seconds_since_audio(void)
{
    double last = g_last_audio_at;
    if (last <= 0) return -1;
    double d = (now_ms() - last) / 1000.0;
    return d < 0 ? 0 : (int)d;
}

int player_is_playing(void)
{
    return g_started && g_app.mode == MODE_PLAYING;
}

/* ---------------- 运行心跳（2026-11-03 加） ----------------
 *
 * 存在的理由：用户报「没声音」时，日志里往往只有一堆 FFmpeg 警告，
 * 分不清到底是「解码侧没出数据」还是「数据出了但系统没放」。这两类的
 * 处置完全不同，却都不能从现有日志判断 —— 只能靠人跑到机器前看音量合成器。
 *
 * 这里每 60 秒记一行，把判据摆出来：
 *   写入块数在涨 + 用户听不见  ⇒ 音频已进系统 → 查输出设备/端点/功放/线材
 *   写入块数不涨 + 距上次出音在涨 ⇒ 解码或读流卡住 → 看门狗会介入
 *
 * 60 秒一行，7×24 一天也就 1440 行，不刷屏。 */
void player_heartbeat(void)
{
    static double last_at = 0;
    static LONG   prev_frames = 0;

    double now = now_ms();
    if (last_at > 0 && now - last_at < 60000.0) return;
    last_at = now;

    LONG cur   = g_frames_written;
    LONG delta = cur - prev_frames;
    prev_frames = cur;
    if (delta < 0) delta = 0;     /* 会话重建时计数归零，别报个负数出来 */

    const char *mode = "播放中";
    if (g_app.mode == MODE_RETRYING)              mode = "重连中";
    else if (g_app.mode == MODE_WAITING_NETWORK)  mode = "等待网络";

    int silent = player_seconds_since_audio();
    char silent_txt[32];
    if (silent < 0) _snprintf(silent_txt, sizeof(silent_txt), "尚未出音");
    else            _snprintf(silent_txt, sizeof(silent_txt), "%d 秒", silent);

    char ep_txt[48];
    if (g_ep_vol_pct >= 0)
        _snprintf(ep_txt, sizeof(ep_txt), "｜端点音量 %d%%", g_ep_vol_pct);
    else
        _snprintf(ep_txt, sizeof(ep_txt), "｜端点音量 取不到");

    /* WASAPI 不可用时退回 waveOut 那个名字（虽然只是「Microsoft 声音映射器」，
     * 但总比什么都没有强） */
    const char *epname = strcmp(g_default_ep_name, "(未取到)") != 0
                         ? g_default_ep_name : g_audio_dev_name;

    log_event("心跳", "%s｜近 60 秒写入 %ld 块｜音量 %d%%%s%s｜距上次出音 %s｜端点 %s",
              mode, delta, g_app.volume, g_app.muted ? "(静音)" : "",
              ep_txt, silent_txt, epname);

    /* 写不进音频最需要留痕（看门狗 15 秒判死之前，日志里往往什么都不留），
     * 单独提一行，别埋在心跳里被忽略。 */
    if (silent > 20)
        log_event("心跳", "已 %d 秒没写入音频且写入块数不增长 —— 卡在解码/读流侧",
                  silent);
}

/* ---------------- 统计跨天归零 ---------------- */

static void stats_roll_day(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char today[16];
    _snprintf(today, sizeof(today), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    if (strcmp(today, g_app.stat_date) != 0) {
        _snprintf(g_app.stat_date, sizeof(g_app.stat_date), "%s", today);
        g_app.stat_disconnects = 0;
        g_app.stat_stalls = 0;
    }
}

/* ---------------- 播放线程 ---------------- */

/* 组一次 avformat_open_input 所需的选项。
 * 直播流不该按默认的 5 秒 / 5MB 去探测：
 *   HLS 的 master → 媒体列表 → 首片本来就要走三跳，实测冷启动每个新主机的
 *   TLS 握手就要 2.0~2.5 秒（之后同主机降到 0.1~0.4 秒），
 *   再叠上默认 5 秒的分析时间，调台要等十几秒，还会顶穿超时被判失败。
 *   纯音频流给 1.5MB / 3 秒足够定出 codec 参数（ts 里一个分片就有几秒音频）。
 * ⚠️ avformat_open_input 会消耗 dict 里它认识的项，所以每次打开都要现组一份，
 *    不能复用（复用会悄悄丢选项）。for_playback=0 用于自检（不需要 reconnect 系列）。 */
static AVDictionary *make_open_opts(const char *ua, const char *headers, int for_playback)
{
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "user_agent", ua ? ua : "ShouBo-nzg/1.0", 0);
    if (headers && headers[0]) av_dict_set(&opts, "headers", headers, 0);
    av_dict_set(&opts, "timeout", "10000000", 0);         /* 微秒 = 10 秒 */
    av_dict_set(&opts, "rw_timeout", "10000000", 0);
    if (for_playback) {
        /* ⚠️ 这里**故意不设** reconnect / reconnect_streamed。
         *
         * 2026-10-23 定案（「播半天后一直不动」根因）：
         * 旧配置 reconnect=1 + reconnect_streamed=1 会让 http 协议层在连接/分片
         * 失败后**无限重试且永不返回错误**，于是 av_read_frame 永久阻塞在 hls
         * demuxer 内部循环里 —— 线程既不报错也不退出，waveOut 队列播空后彻底静音；
         * 看门狗判死后 stop_play_thread 又杀不掉卡在 FFmpeg 里的线程（Windows 无法
         * 强制结束线程），每轮重建泄漏一个线程 + 若干 socket，累积半天后彻底不动。
         * 正确姿势是让失败快速冒泡：报错 → 线程干净退出 → 看门狗重建 →
         * **重新解析拿新签名**。 */
        av_dict_set(&opts, "fflags", "nobuffer+discardcorrupt", 0);
        /* litenews 的 100ycdn 会话签名失效后分片会 403/404；
         * hls demuxer 默认会一直重试同一分片（表现就是「一直不动」），
         * 限 2 次后返回 AVERROR，交给自愈去换票。 */
        av_dict_set(&opts, "seg_max_retry", "2", 0);
        /* 不复用长连接：CDN 会话过期时 keep-alive 连接会被静默挂死
         * （TCP 不 FIN、也不发数据），比直接 403 更难察觉。 */
        av_dict_set(&opts, "http_persistent", "0", 0);
        /* HLS 起播点。FFmpeg 默认 live_start_index=-3（从倒数第 3 片开始），
         * 落点 = start_seq_no + max(n_segments-3, 0)，窗口不足 3 片时会被压到
         * **最老那一片**。
         *
         * ⚠️ 2026-11-03 实测（tools/probe_hls_window.py）：litenews 那 36 台
         * 的 playlist **只有 2~3 个分片、每片 2 秒、总窗口 4~6 秒**，服务端每
         * 2 秒滚一片。起播就站在过期边界上 → 任何一点抖动都会让最老分片滚出
         * 窗口 → `skipping 1 segments ahead, expired from playlists`（每次丢
         * 2 秒音频）。这条警告因此是这批台的常态噪音，不是故障。
         *
         * 改 -1：窗口 ≥2 片时直接指到最后一片，永不贴过期边；
         * 长窗口台（鄂尔多斯 nxw，6 片 48 秒）仍保留 1 片 ≈8 秒缓冲。
         * 比 0 稳（0 完全没有余量），比 -3 准（-3 在短窗口上等于 0）；
         * skip 后 cur_seq_no 会被 demuxer 重置到 start_seq_no，照常继续读，
         * 所以这只是「少丢分片」，不影响出声。 */
        av_dict_set(&opts, "live_start_index", "-1", 0);
    }
    av_dict_set(&opts, "probesize", "1500000", 0);
    av_dict_set(&opts, "analyzeduration", "3000000", 0);
    return opts;
}

static DWORD WINAPI play_thread(LPVOID param)
{
    const Channel *ch = (const Channel *)param;
    if (!ch) return 0;

    /* 快照自己的会话代际：之后任何时刻 g_session_gen != my_gen，
     * 都说明本线程已被新会话顶替 —— 立即退出、释放资源（防僵尸占设备）。 */
    LONG my_gen = g_session_gen;

    char url[MAX_URL];
    int resolved = resolve_url(ch, url, sizeof(url));
    g_app.resolving = 0;
    /* 记录解析时刻：动态签名源靠这个时间做「定时换票」，
     * 不等它断（断了必然有 15 秒静音空档）。 */
    g_app.resolve_at = resolved ? now_ms() : 0;
    if (!resolved) {
        /* resolve.c 已记日志，这里把控制权交回主线程的自愈状态机 */
        g_thread_alive = 0;
        ui_post_tick();
        return 0;
    }

    /* 该台主源本会话坏过 → 直接用镜像（sticky），不再先在坏源上白等超时 */
    int used_backup = 0;
    const char *eff = url;
    if (g_backup_sticky_id == ch->id && ch->backup_url[0]) {
        eff = ch->backup_url;
        used_backup = 1;
        log_event("播放", "本台主源此前打不开，直接使用镜像");
    }

    /* 日志级别已在 fflog_install() 里统一设为 WARNING（含回调）。
     * 这里原来写 QUIET，会把 schannel/HTTP 的失败原因一起吞掉，
     * Win7 排障时最关键的一层证据就没了 —— 别改回去。 */

    AVFormatContext *fmt = NULL;
    char headers[1024];
    build_headers(ch, headers, sizeof(headers));

    const char *ua = NULL;
    for (int i = 0; i < ch->headers.header_count; i++)
        if (_stricmp(ch->headers.header_name[i], "User-Agent") == 0)
            ua = ch->headers.header_value[i];

    /* avformat_open_input 会**消耗**掉 dict 里它认识的项，
     * 所以主源/镜像各开一次就要各建一份，复用会悄悄丢选项。 */
    AVDictionary *opts = make_open_opts(ua, headers, 1);
    int aerr = avformat_open_input(&fmt, eff, NULL, &opts);
    av_dict_free(&opts);
    if (aerr < 0) {
        /* 把 FFmpeg 的错误码和错误文字都带上 —— 不然「打开流失败」分不清是
         * 连不上（瞬态抖动，自愈会好）还是超时/协议问题（要另查）。
         * 常见：-110 ETIMEDOUT 超时、-111 ECONNREFUSED 拒绝、
         *       -104 ECONNRESET 被重置、-1389436260 = "Server returned 4XX/5XX"。
         * 注意 %s 只取前 120 字符：litenews 的 URL 带一长串签名参数。 */
        char ebuf[96] = "";
        av_strerror(aerr, ebuf, sizeof(ebuf));
        log_event("断线", "打开流失败（averr=%d %s）：%.120s", aerr, ebuf, eff);
    } else if (used_backup) {
        log_event("播放", "使用源：镜像 backup_url");
    }

    /* 第一步失败 → 试同台的另一条源（主源 <-> backup_url）。
     *
     * ⚠️ 对「移植铁律 3：不许自动换源」的说明：那条针对的是**跨台**自动换源
     * （用户听着 A 台，程序悄悄跳去 B 台）。这里台没变，只是同一个台挂了
     * 双源里的另一条路，且成功后本会话内固定走这条（sticky），不会来回横跳。
     * 用户在源清单备注里也明确要求了镜像兜底（qtfm 的 lhttp / lhttp-hw 双 CDN）。
     * 镜像也失败 → 走原有的自愈重试流程，行为与从前一致。 */
    if (aerr < 0) {
        const char *other = used_backup ? url : ch->backup_url;
        if (other && other[0] && _stricmp(other, eff) != 0) {
            log_event("断线", "改试同台另一条源（%s）",
                      used_backup ? "主源" : "镜像 backup_url");
            opts = make_open_opts(ua, headers, 1);
            aerr = avformat_open_input(&fmt, other, NULL, &opts);
            av_dict_free(&opts);
            if (aerr >= 0) {
                eff = other;
                used_backup = (eff == ch->backup_url);
                if (used_backup) {
                    g_backup_sticky_id = ch->id;
                    log_event("播放", "镜像可用：本会话内该台固定走镜像");
                } else if (g_backup_sticky_id == ch->id) {
                    g_backup_sticky_id = 0;
                    log_event("播放", "主源已恢复：解除镜像 sticky");
                }
            } else {
                char ebuf2[96] = "";
                av_strerror(aerr, ebuf2, sizeof(ebuf2));
                log_event("断线", "另一条源也打不开（averr=%d %s）", aerr, ebuf2);
            }
        }
    }

    int ok = (aerr >= 0);
    if (ok && avformat_find_stream_info(fmt, NULL) < 0) {
        log_event("断线", "读取流信息失败");
        ok = 0;
    }

    int stream_idx = -1;
    if (ok) {
        stream_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (stream_idx < 0) {
            /* 部分县台把广播塞在带低码率视频的流里，退回任意轨 */
            stream_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        }
        if (stream_idx < 0) {
            log_event("断线", "流中找不到可解码轨道");
            ok = 0;
        }
    }

    AVCodecContext *dec = NULL;
    SwrContext *swr = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frm = NULL;
    /* swr 的输出平面指针数组。
     * 必须由我们分配再传进去：如果传一个指向 NULL 的指针，
     * swresample 会把 out->ch[0] 设成 NULL 然后照样往里写，
     * 直接访问违例（踩过：崩在 _conv_AV_SAMPLE_FMT_FLT_to_AV_SAMPLE_FMT_S16）。 */
    uint8_t **out_data = NULL;
    int out_cap_samples = 0;      /* out_data 目前能放多少样本 */
    int played_any = 0;
    /* 本线程私有的 waveOut 会话：设备与 4 槽全在这份 ctx 里，
     * 退出时只清理自己 —— 不会误关/误写后续会话的设备（见 WoCtx 注释） */
    WoCtx wo_ctx;
    memset(&wo_ctx, 0, sizeof(wo_ctx));

    if (ok) {
        const AVCodec *codec =
            avcodec_find_decoder(fmt->streams[stream_idx]->codecpar->codec_id);
        if (!codec) {
            log_event("断线", "找不到解码器");
            ok = 0;
        } else {
            dec = avcodec_alloc_context3(codec);
            if (!dec ||
                avcodec_parameters_to_context(dec, fmt->streams[stream_idx]->codecpar) < 0) {
                log_event("断线", "解码器上下文初始化失败");
                ok = 0;
            } else {
                dec->thread_count = 1;    /* 纯音频，单线程解码最省 */
                if (avcodec_open2(dec, codec, NULL) < 0) {
                    log_event("断线", "解码器打开失败");
                    ok = 0;
                }
            }
        }
    }

    if (ok && !waveout_open(&wo_ctx)) {
        log_event("音频", "无可用音频输出设备");
        ok = 0;
    }
    if (ok) apply_ctx_volume(&wo_ctx);

    if (ok) {
        /* 重采样：任意输入 → 44100/立体声/S16 */
        int in_rate = dec->sample_rate > 0 ? dec->sample_rate : OUT_RATE;
        int in_ch = dec->ch_layout.nb_channels > 0 ? dec->ch_layout.nb_channels : 1;

        AVChannelLayout in_layout, out_layout;
        av_channel_layout_default(&in_layout, in_ch);
        av_channel_layout_default(&out_layout, OUT_CHANNELS);

        swr = swr_alloc();
        if (!swr) {
            ok = 0;
        } else {
            av_opt_set_chlayout(swr, "in_chlayout", &in_layout, 0);
            av_opt_set_int(swr, "in_sample_rate", in_rate, 0);
            av_opt_set_int(swr, "in_sample_fmt", dec->sample_fmt, 0);
            av_opt_set_chlayout(swr, "out_chlayout", &out_layout, 0);
            av_opt_set_int(swr, "out_sample_rate", OUT_RATE, 0);
            av_opt_set_int(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
            if (swr_init(swr) < 0) {
                log_event("音频", "重采样初始化失败");
                ok = 0;
            }
        }
        if (ok) {
            log_event("播放", "开始播放：%s（源 %dHz/%dch）", ch->name, in_rate, in_ch);
        }
    }

    if (ok) {
        pkt = av_packet_alloc();
        frm = av_frame_alloc();
        if (!pkt || !frm) ok = 0;
    }

    while (ok && !g_thread_stop && g_session_gen == my_gen) {
        int ret = av_read_frame(fmt, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) { Sleep(20); continue; }
            if (ret == AVERROR_EOF) log_event("断线", "直播流结束（推流侧断开）");
            else log_event("断线", "读帧错误（%d）", ret);
            break;
        }
        if (pkt->stream_index != stream_idx) { av_packet_unref(pkt); continue; }

        if (avcodec_send_packet(dec, pkt) == 0) {
            while (avcodec_receive_frame(dec, frm) == 0) {
                /* swr_get_out_samples 给的是安全上界，按它扩容 */
                int want = swr_get_out_samples(swr, frm->nb_samples);
                if (want < 0) { av_frame_unref(frm); continue; }

                if (want > out_cap_samples) {
                    av_freep(&out_data);
                    if (av_samples_alloc_array_and_samples(&out_data, NULL,
                                                           OUT_CHANNELS,
                                                           want + 512,
                                                           AV_SAMPLE_FMT_S16, 0) < 0) {
                        out_data = NULL;
                        out_cap_samples = 0;
                        av_frame_unref(frm);
                        break;                 /* 内存不足，跳过这帧 */
                    }
                    out_cap_samples = want + 512;
                }

                int got = swr_convert(swr, out_data, want,
                                      (const uint8_t **)frm->extended_data,
                                      frm->nb_samples);
                if (got > 0 && out_data && out_data[0]) {
                    int bytes = got * OUT_CHANNELS * 2;

                    waveout_recycle(&wo_ctx);
                    int slot = waveout_free_slot(&wo_ctx);
                    if (slot < 0) {
                        /* 槽满：等一小会（最多约 200ms） */
                        for (int t = 0; t < 20 && !g_thread_stop && g_session_gen == my_gen; t++) {
                            Sleep(10);
                            waveout_recycle(&wo_ctx);
                            slot = waveout_free_slot(&wo_ctx);
                            if (slot >= 0) break;
                        }
                    }
                    if (slot >= 0) {
                        /* 超过一个槽的量就截断：宁可丢一点音频，也不能阻塞读流 */
                        if (bytes > SLOT_BYTES) bytes = SLOT_BYTES;
                        /* 必须拷到独立槽位：out_data 是 swr 内部缓冲，
                         * 下一轮会被覆写，而 waveOut 是异步播放的 */
                        memcpy(wo_ctx.slot[slot], out_data[0], (size_t)bytes);
                        memset(&wo_ctx.hdr[slot], 0, sizeof(WAVEHDR));
                        wo_ctx.hdr[slot].lpData         = (LPSTR)wo_ctx.slot[slot];
                        wo_ctx.hdr[slot].dwBufferLength = (DWORD)bytes;
                        if (waveOutPrepareHeader(wo_ctx.wo, &wo_ctx.hdr[slot], sizeof(WAVEHDR))
                            == MMSYSERR_NOERROR) {
                            if (waveOutWrite(wo_ctx.wo, &wo_ctx.hdr[slot], sizeof(WAVEHDR))
                                == MMSYSERR_NOERROR) {
                                wo_ctx.used[slot] = 1;
                                InterlockedIncrement(&g_frames_written);
                                g_last_audio_at = now_ms();
                                played_any = 1;
                            } else {
                                waveOutUnprepareHeader(wo_ctx.wo, &wo_ctx.hdr[slot], sizeof(WAVEHDR));
                            }
                        }
                    }
                    /* slot < 0 时静默丢这一帧 */
                }
                av_frame_unref(frm);
            }
        }
        av_packet_unref(pkt);
    }

    if (ok) {
        /* 让已提交的音频播完再拆设备，避免尾巴被切 */
        waveout_recycle(&wo_ctx);
        Sleep(60);
    }

    av_freep(&out_data);
    if (pkt) av_packet_free(&pkt);
    if (frm) av_frame_free(&frm);
    if (swr) swr_free(&swr);
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    /* opts 已在每次打开后立刻释放（make_open_opts 每次现组），这里不再需要 */
    waveout_close(&wo_ctx);

    /* 先清存活标志再发通知：主线程收到 WM_TICK 调看门狗时必须已经
     * 能看到「线程已死」，立即 fail 接管 —— 不允许出现
     * 「线程没了但状态机还在 PLAYING 等一个永远不来的首帧」的死局。 */
    g_thread_alive = 0;

    if (g_thread_stop || g_session_gen != my_gen) {
        log_event("播放", "播放线程正常退出（重建或退出）");
    } else if (played_any) {
        log_event("断线", "播放中断（已出声音后断开）");
        ui_post_tick();
    } else {
        log_event("断线", "未能取得音频数据");
        ui_post_tick();
    }
    return 0;
}

/* ---------------- 频道自检（--selftest） ----------------
 *
 * 出包后核对「生成的频道清单里每一台真的能拉起来」。
 * 不建窗口、不初始化 waveOut，纯串行跑完所有频道后退出，
 * 结果同时写日志和 exe 同目录的 selftest.txt。
 *
 * 复用的是播放线程里那套一模一样的调用序列（解析 → 打开 → 选轨 → 开解码器 → 解出首帧），
 * 所以自检通过基本等于能出声。
 */

static int selftest_one(const Channel *ch, char *verdict, int vcap,
                        char *used_url, int ucap)
{
    AVFormatContext *fmt = NULL;
    AVDictionary *opts = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frm = NULL;
    char headers[1024];
    int good = 0;

    used_url[0] = '\0';
    if (!resolve_url(ch, used_url, ucap)) {
        _snprintf(verdict, vcap, "FAIL 动态源解析失败");
        return 0;
    }

    build_headers(ch, headers, sizeof(headers));
    /* 与 play_thread 的 make_open_opts 保持同一套参数（自检不需要 reconnect 系列） */
    opts = make_open_opts("ShouBo-nzg/1.0", headers, 0);
    int aerr = avformat_open_input(&fmt, used_url, NULL, &opts);
    av_dict_free(&opts);
    opts = NULL;
    if (aerr < 0) {
        /* 主源打不开 → 试同台镜像（与 play_thread 的回退一致），
         * 不然「主源坏了但镜像活着」的台会被自检冤枉成 FAIL。 */
        if (ch->backup_url[0] && _stricmp(ch->backup_url, used_url) != 0) {
            log_event("自检", "主源打不开，改试镜像 backup_url");
            opts = make_open_opts("ShouBo-nzg/1.0", headers, 0);
            aerr = avformat_open_input(&fmt, ch->backup_url, NULL, &opts);
            av_dict_free(&opts);
            opts = NULL;
            if (aerr >= 0) {
                _snprintf(used_url, ucap, "%s", ch->backup_url);
                _snprintf(verdict, vcap, "OK  （镜像 backup_url 通过，主源打不开）");
            }
        }
        if (aerr < 0) {
            _snprintf(verdict, vcap, "FAIL 打开流失败");
            goto done;
        }
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        _snprintf(verdict, vcap, "FAIL 读流信息失败");
        goto done;
    }

    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (si < 0) {
        _snprintf(verdict, vcap, "FAIL 流里没有音频轨");
        goto done;
    }

    const AVCodec *codec =
        avcodec_find_decoder(fmt->streams[si]->codecpar->codec_id);
    if (!codec) {
        _snprintf(verdict, vcap, "FAIL 未编入 %s 解码器",
                  avcodec_get_name(fmt->streams[si]->codecpar->codec_id));
        goto done;
    }

    dec = avcodec_alloc_context3(codec);
    if (!dec ||
        avcodec_parameters_to_context(dec, fmt->streams[si]->codecpar) < 0) {
        _snprintf(verdict, vcap, "FAIL 解码器上下文初始化失败");
        goto done;
    }
    dec->thread_count = 1;
    if (avcodec_open2(dec, codec, NULL) < 0) {
        _snprintf(verdict, vcap, "FAIL 解码器打开失败");
        goto done;
    }

    pkt = av_packet_alloc();
    frm = av_frame_alloc();
    if (!pkt || !frm) {
        _snprintf(verdict, vcap, "FAIL 内存分配失败");
        goto done;
    }

    /* 直播流前面可能有几十个包才凑够一帧，给足 400 个包 / 8 秒 */
    double t0 = now_ms();
    for (int n = 0; n < 400 && !good; n++) {
        if (now_ms() - t0 > 8000) break;
        if (av_read_frame(fmt, pkt) < 0) break;
        if (pkt->stream_index == si && avcodec_send_packet(dec, pkt) == 0) {
            while (avcodec_receive_frame(dec, frm) == 0) { good = 1; break; }
        }
        av_packet_unref(pkt);
    }

    if (good) {
        int rate = frm->sample_rate > 0 ? frm->sample_rate : dec->sample_rate;
        int nch  = frm->ch_layout.nb_channels > 0 ? frm->ch_layout.nb_channels : 1;
        _snprintf(verdict, vcap, "OK   %s %dHz %dch",
                  avcodec_get_name(fmt->streams[si]->codecpar->codec_id), rate, nch);
        if (used_url[0] && ch->backup_url[0] &&
            _stricmp(used_url, ch->backup_url) == 0) {
            size_t vl = strlen(verdict);
            _snprintf(verdict + vl, vcap - vl, "（经镜像）");
        }
    } else {
        _snprintf(verdict, vcap, "FAIL 连上但取不到音频帧（%s）",
                  avcodec_get_name(fmt->streams[si]->codecpar->codec_id));
    }

done:
    if (frm) av_frame_free(&frm);
    if (pkt) av_packet_free(&pkt);
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    av_dict_free(&opts);
    return good;
}

int player_selftest(void)
{
    /* 总时间预算：自检是给人用的诊断工具，断网时不能跑成「挂死」。
     * 单台的**网络等待没有硬上限** —— http_get 的解析/连接/发送/接收四段各 8 秒
     * （最坏 ~34 秒），litenews 还要依次试 radio 和 tv 两个端点，
     * 39 台全卡住能拖到半小时以上，用户会以为程序死了。
     * 超预算的剩余台直接标 SKIP，并在报告里写明原因，不假装通过。 */
    const double BUDGET_MS = 12 * 60 * 1000.0;

    char dir[MAX_PATH], path[MAX_PATH];
    if (!exe_dir(dir, sizeof(dir))) _snprintf(dir, sizeof(dir), ".");
    _snprintf(path, sizeof(path), "%s\\selftest.txt", dir);

    FILE *rep = fopen(path, "w");
    int total = g_app.cfg.channel_count, ok = 0, bad = 0, skipped = 0;
    int live_cnt = 0, pad_cnt = 0, unknown_cnt = 0;
    double t_all = now_ms();

    log_event("自检", "===== 开始逐个频道自检，共 %d 台%s =====",
              total, g_opt_livecheck ? "（含活性探测）" : "");
    if (rep) fprintf(rep, "守播广播 Windows 版 频道自检（共 %d 台）\n\n", total);

    for (int i = 0; i < total; i++) {
        const Channel *ch = &g_app.cfg.channels[i];
        char verdict[160], url[MAX_URL];
        url[0] = '\0';

        if (now_ms() - t_all > BUDGET_MS) {
            skipped++;
            _snprintf(verdict, sizeof(verdict), "SKIP 超出 %d 分钟自检预算（网络太慢或已断网）",
                      (int)(BUDGET_MS / 60000));
            log_event("自检", "[%2d/%2d] %-10s %s", i + 1, total, "跳过", verdict);
            if (rep) fprintf(rep, "[%2d/%2d] %s\n        %s  %s\n\n",
                             i + 1, total, verdict, ch->category, ch->name);
            continue;
        }

        double t0 = now_ms();
        int good = selftest_one(ch, verdict, sizeof(verdict), url, sizeof(url));
        int tries = 1;
        /* 失败就再给一次机会，并**重新解析**（和真实播放一致：动态源签名会过期，
         * 重试必须重新取地址）。真实播放走的是退避重试、永不放弃的状态机，
         * 单次采样判死会低估 litenews 这类三跳 HLS —— 那常常只是冷连接慢，不是坏。
         * 报告里会写明是重试才过的，不掩盖首次失败。 */
        if (!good) {
            char first[160];
            _snprintf(first, sizeof(first), "%s", verdict);
            Sleep(1200);      /* 给 CDN 一点喘息；真实播放的退避第一档是 0 秒，第二档 5 秒 */
            {
                char v2[160], u2[MAX_URL];
                u2[0] = '\0';
                if (selftest_one(ch, v2, sizeof(v2), u2, sizeof(u2))) {
                    good = 1;
                    tries = 2;
                    _snprintf(verdict, sizeof(verdict), "%s（重试通过，首次 %s）", v2, first);
                    if (u2[0]) _snprintf(url, sizeof(url), "%s", u2);
                } else {
                    _snprintf(verdict, sizeof(verdict), "%s（重试仍失败）", first);
                }
            }
        }
        int ms = (int)(now_ms() - t0);
        if (good) ok++; else bad++;

        /* 活性探测（只在 --selftest --live 时做，且只对能判的台做）。
         * 平台停推时常留循环垫片，那玩意儿「拉得开、能解帧」，自检照样判过；
         * 这里把「时间戳有没有跟着墙上时钟走」也验一遍，把垫片揪出来。 */
        char live_note[80] = "";
        if (good && g_opt_livecheck) {
            int lv = probe_hls_liveness(url, 4);
            if (lv == 1) {
                live_cnt++;
                _snprintf(live_note, sizeof(live_note), "  [在播]");
            } else if (lv == 0) {
                pad_cnt++;
                _snprintf(live_note, sizeof(live_note), "  [疑似垫片]");
            } else {
                unknown_cnt++;
                _snprintf(live_note, sizeof(live_note), "  [活性无法判定]");
            }
        }

        log_event("自检", "[%2d/%2d] %-10s %s%s（%d ms，%d 次）",
                  i + 1, total, good ? "通过" : "失败", verdict, live_note, ms, tries);
        log_event("自检", "         %s | %s | %s",
                  ch->category, ch->name, url);
        if (rep) {
            fprintf(rep, "[%2d/%2d] %s%s\n", i + 1, total, verdict, live_note);
            fprintf(rep, "        %s  %s  %s\n", ch->category, ch->name, ch->player);
            fprintf(rep, "        %s\n        %d ms\n\n", url, ms);
        }
    }

    log_event("自检", "===== 自检结束：通过 %d / 失败 %d / 跳过 %d，总耗时 %.1f 秒 =====",
              ok, bad, skipped, (now_ms() - t_all) / 1000.0);
    if (g_opt_livecheck)
        log_event("自检", "活性：在播 %d / 疑似垫片 %d / 无法判定 %d"
                  "（垫片=时间戳不跟墙上时钟走，源还在但内容是死的循环）",
                  live_cnt, pad_cnt, unknown_cnt);
    if (rep) {
        fprintf(rep, "汇总：通过 %d，失败 %d，跳过 %d，总耗时 %.1f 秒\n",
                ok, bad, skipped, (now_ms() - t_all) / 1000.0);
        if (g_opt_livecheck)
            fprintf(rep, "活性：在播 %d，疑似垫片 %d，无法判定 %d\n",
                    live_cnt, pad_cnt, unknown_cnt);
        fclose(rep);
    }
    /* 退出码：0=全通过，2=有真失败（脚本判定的关键信号），
     * 1=没有失败但有跳过（多半是断网，别当成通过）。 */
    if (bad) return 2;
    return skipped ? 1 : 0;
}

/* ---------------- 线程生命周期 ---------------- */

static void stop_play_thread(void)
{
    g_thread_stop = 1;
    if (g_thread) {
        WaitForSingleObject(g_thread, 6000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    /* ⚠️ 这里不再复位 g_thread_stop！
     * 复位过的后果就是僵尸线程复活：旧线程卡在 FFmpeg 超过 6 秒后，
     * 标志已归零，它出来后继续播、永远不退、占着 waveOut 设备不还。
     * 复位职责移交给 spawn_play_thread（新线程启动前才归零），
     * 僵尸靠 g_session_gen 代际不匹配自行退出。 */
}

/* 当前频道快照：线程要用，不能引用可能变化的全局结构 */
static Channel g_snapshot;

static void spawn_play_thread(void)
{
    /* 铁律3：已释放就不再重建 */
    if (!g_started) return;
    if (g_app.cur_index < 0 || g_app.cur_index >= g_app.cfg.channel_count) return;
    const Channel *ch = cur_channel();
    if (!ch || !ch->primary_url[0]) {
        log_event("异常", "当前频道无地址，无法播放");
        return;
    }

    stop_play_thread();
    /* 先换代再归零 stop 标志：新线程读到的是新代际 + stop=0；
     * 旧僵尸的 my_gen 已落后，从 FFmpeg 出来即刻退出、释放设备。 */
    g_session_gen++;
    g_thread_stop = 0;
    g_snapshot = *ch;
    g_last_audio_at = 0;          /* 重置，避免刚切台就被判卡死 */
    g_frames_written = 0;         /* 心跳的「已写块数」按会话重算 */
    g_app.resolving = 1;

    g_thread = CreateThread(NULL, 0, play_thread, &g_snapshot, 0, NULL);
    if (!g_thread) {
        log_event("异常", "播放线程创建失败");
        g_app.resolving = 0;
        g_thread_alive = 0;      /* 没起来：看门狗下一跳立即接管 */
    } else {
        g_thread_alive = 1;
        g_spawn_at = now_ms();
    }
}

int player_thread_alive(void)
{
    return g_thread_alive;
}

/* 当前台是否为「动态签名源」——litenews / 河南大象 hntv。
 *
 * 这类源每次播放前解析出的是一张**带时效的票**（litenews 是 100ycdn 的
 * wsSession+wsIPSercert+wsBindIP，hntv 是 wsSecret+wsTime）。票过期后
 * CDN 要么 403，要么把 keep-alive 连接静默挂死 —— 后者就是「播半天后
 * 一直不动」的直接来源。所以这类台必须周期性重新解析（换票），
 * 不能指望断了再救。 */
int player_dynamic_source(void)
{
    const Channel *ch = cur_channel();
    if (!ch) return 0;
    return _strnicmp(ch->primary_url, "litenews://", 11) == 0 ||
           _strnicmp(ch->primary_url, "hntv://", 7) == 0;
}

double player_spawn_elapsed(void)
{
    double t = g_spawn_at;
    return t > 0 ? (now_ms() - t) / 1000.0 : 0;
}

/* ---------------- 对外接口 ---------------- */

int player_start(void)
{
    if (g_started) return 1;
    g_started = 1;
    stats_roll_day();
    if (g_app.cur_index < 0) g_app.cur_index = 0;
    g_app.mode = MODE_PLAYING;
    spawn_play_thread();
    return 1;
}

void player_stop(void)
{
    g_started = 0;
    stop_play_thread();
    /* 正常路径：播放线程退出时已 close 自己的会话（g_wo 置 NULL）。
     * 这里只兜底静音 —— UI 线程不知道哪个 ctx 属于哪个线程，
     * 不能替它 close；就算线程拖尾未死，设备也会随进程终止被系统回收。 */
    if (g_wo) {
        waveOutReset(g_wo);
        g_wo = NULL;
    }
}

/* 手动切台（用户主动的唯一入口） */
int player_switch(int index)
{
    if (index < 0 || index >= g_app.cfg.channel_count) return 0;
    if (g_app.cfg.channel_lock) {
        log_event("提示", "配置已锁定当前台，忽略切台");
        return 0;
    }
    if (index == g_app.cur_index) return 1;

    const Channel *ch = &g_app.cfg.channels[index];
    log_event("切台", "手动切换：%s", ch->name);

    g_app.cur_index = index;
    g_app.cfg.current_channel_id = ch->id;
    /* 切台等价硬重建：退避计数清零，从干净状态开始 */
    g_app.mode = MODE_PLAYING;
    g_app.failures = 0;
    g_app.soft_count = 0;
    g_app.stall_count = 0;
    g_app.last_fail_at = 0;
    g_app.next_retry_at = 0;

    spawn_play_thread();
    config_save_state();
    ui_refresh();
    return 1;
}

/* 硬重建：释放并重启播放（重连 / 网络恢复 / 配置变更都走这里） */
void player_hard_rebuild(const char *reason)
{
    if (!g_started) return;   /* 铁律3 */
    log_event("重启", "播放器重建：%s", reason);
    stats_roll_day();
    g_app.mode = MODE_PLAYING;
    g_app.failures = 0;
    g_app.soft_count = 0;
    g_app.stall_count = 0;
    g_app.last_fail_at = 0;
    g_app.next_retry_at = 0;
    spawn_play_thread();
    ui_refresh();
}

/* ---------------- 系统端点主音量顶满（对齐安卓版 AudioBooster 的 force_max 层） ----------------
 *
 * ⚠️ 2026-11-03 重写：原实现根本没生效，这是「小程序音量 100%、PCM 一直在写、
 *    但机房机器声音很小」的真根因。
 *
 * 旧代码：
 *     waveOutOpen(&g_main_out, WAVE_MAPPER, NULL, 0, 0, CALLBACK_NULL)
 *   —— 把 **NULL 当 WAVEFORMATEX** 传进去。MSDN 要求 pwfx 必须是有效格式，
 *   Vista+ 上该调用必然返回 MMSYSERR_INVALPARAM，于是 g_main_out 恒为 NULL、
 *   函数在第一个分支就 return。**这一层从上线起一次都没跑过**
 *   （佐证：所有历史日志里都找不到「系统主输出音量被调低」这行）。
 *
 *   更根本的是：即便 open 成功，waveOutSetVolume 在 Vista+ 也只作用于
 *   **本进程的音频会话**（就是 apply_waveout_volume 已经在管的那一层），
 *   物理上碰不到端点设备音量（右下角喇叭滑块）。想「顶满系统音量」只能用 WASAPI。
 *
 * 现在：IMMDeviceEnumerator → 默认渲染端点 → IAudioEndpointVolume
 *   先读 GetMute，静音就取消；再读 GetMasterVolumeLevelScalar，
 *   低于 95% 就 SetMasterVolumeLevelScalar(1.0)。
 *   首次读到的值一定进日志 —— 这条日志就是「声音小」的现场证据。
 *
 * 仍然受 audio.force_max 控制（会连带影响机器上其他程序，用户可关）。
 */
static void epvol_release(void)
{
    if (g_epvol) { g_epvol->lpVtbl->Release(g_epvol); g_epvol = NULL; }
    if (g_mmdev) { g_mmdev->lpVtbl->Release(g_mmdev); g_mmdev = NULL; }
}

/* ⚠️ MinGW-w64 只在头里 extern 声明这三个 GUID，库里没有符号
 * （nm libuuid.a / libole32.a 都搜不到），链接必然 undefined reference。
 * 所以在这里**手工定义**，用 _sdkIid 后缀避开头文件那份 extern 声明。
 * 值取自 Windows SDK（mmdeviceapi.h / endpointvolume.h），是公开固定值。 */
static const CLSID g_clsidMMDE =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID g_iidMMDE =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID g_iidEPVol =
    {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

/* PKEY_Device_FriendlyName（functiondiscoverykeys_devpkey.h，pid=14）。
 * 同样是「头里只有 DEFINE_PROPERTYKEY 声明」的问题，手写一份最省事。 */
static const PROPERTYKEY g_pkeyFriendly =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

/* 读默认端点的友好名，例如「扬声器 (Realtek High Definition Audio)」。 */
static void read_ep_name(IMMDevice *dev)
{
    IPropertyStore *ps = NULL;
    if (FAILED(dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &ps)) || !ps) return;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &g_pkeyFriendly, &pv)) &&
        pv.vt == VT_LPWSTR && pv.pwszVal) {
        char tmp[192];
        wide_to_utf8_or_die(tmp, sizeof(tmp), pv.pwszVal);
        _snprintf(g_default_ep_name, sizeof(g_default_ep_name), "%s", tmp);
    }
    PropVariantClear(&pv);
    ps->lpVtbl->Release(ps);
}

static int epvol_acquire(void)
{
    if (g_epvol) return 1;

    if (!g_com_ready) {
        /* 已在别的套间初始化过会返回 RPC_E_CHANGED_MODE，那也算可用 */
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) g_com_ready = 1;
        else return 0;
    }
    if (!g_mmdev) {
        if (FAILED(CoCreateInstance(&g_clsidMMDE, NULL, CLSCTX_ALL,
                                    &g_iidMMDE, (void **)&g_mmdev))) {
            g_mmdev = NULL;
            return 0;
        }
    }
    /* 每次现取默认端点：设备热插拔/切换后旧指针会失效 */
    IMMDevice *dev = NULL;
    if (FAILED(g_mmdev->lpVtbl->GetDefaultAudioEndpoint(g_mmdev, eRender, eConsole, &dev)))
        return 0;

    read_ep_name(dev);   /* 顺手把真名记下来，心跳要用 */
    HRESULT hr = dev->lpVtbl->Activate(dev, &g_iidEPVol, CLSCTX_ALL, NULL,
                                       (void **)&g_epvol);
    dev->lpVtbl->Release(dev);
    if (FAILED(hr)) { g_epvol = NULL; return 0; }
    return 1;
}

void player_force_max_volume(void)
{
    if (!g_app.cfg.audio.force_max) return;

    /* 每 tick 都查一次没必要：用户手动拉滑块也得几秒才反应过来，
     * 我们晚 10 秒纠正不影响观感，还能省掉频繁的 COM 调用。
     * 取不到端点时（无声卡/服务未起）退避到 5 分钟，避免 7×24 空转。 */
    static double last_at   = 0;
    static int    fail_streak = 0;
    double now = now_ms();
    double gap = fail_streak > 0 ? 300000.0 : 10000.0;
    if (last_at > 0 && now - last_at < gap) return;
    last_at = now;

    if (!epvol_acquire()) {
        fail_streak++;
        if (g_force_first) {
            g_force_first = 0;
            log_event("音量", "取不到系统端点音量（WASAPI 不可用），这一层放弃 —— "
                              "若声音偏小请手动检查右下角喇叭滑块");
        }
        g_ep_vol_pct = -1;
        return;
    }

    BOOL muted = FALSE;
    if (g_epvol->lpVtbl->GetMute(g_epvol, &muted) == S_OK && muted) {
        g_epvol->lpVtbl->SetMute(g_epvol, FALSE, NULL);
        log_event("音量", "系统输出端点处于静音，已取消（否则再怎么调都不会响）");
    }

    float cur = 0.0f;
    if (g_epvol->lpVtbl->GetMasterVolumeLevelScalar(g_epvol, &cur) != S_OK) {
        /* 端点失效（热插拔等）→ 释放，下一轮重建 */
        epvol_release();
        fail_streak++;
        g_ep_vol_pct = -1;
        return;
    }
    fail_streak = 0;                       /* 打通了，恢复正常节拍 */
    g_ep_vol_pct = (int)(cur * 100.0f + 0.5f);

    if (g_force_first) {
        g_force_first = 0;
        log_event("音量", "默认播放端点：%s", g_default_ep_name);
        log_event("音量", "系统端点主音量 %.0f%%（这一行是「声音小」的判据）", cur * 100.0f);
    }
    if (cur < 0.95f) {
        g_epvol->lpVtbl->SetMasterVolumeLevelScalar(g_epvol, 1.0f, NULL);
        g_ep_vol_pct = 100;
        log_event("音量", "系统端点主音量被调低到 %.0f%%，已顶回 100%%", cur * 100.0f);
    }
}