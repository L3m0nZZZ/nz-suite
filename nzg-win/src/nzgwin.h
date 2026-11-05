/* nzg-win 公共定义
 *
 * 守播广播播放器（nzg）Windows 桌面版。
 * 目标：Windows 7/8/10/11 32位，单 exe 绿色运行，零外部依赖。
 *
 * 移植自 Android 版 com.shoubo.radio，行为以安卓版为准（尤其是自愈状态机）。
 */
#ifndef NZGWIN_H
#define NZGWIN_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#define NZG_WIN_VERSION "1.16.2"
/* 只作 sanity 上限。频道数组是运行时按实际条数 malloc 的，
 * 不是静态数组 —— 静态 MAX_CHANNELS*sizeof(Channel) 曾让 g_app 一个
 * 全局变量就吃掉 2.45MB（.data 全是零），占了整个 exe 的 30%。 */
#define MAX_CHANNELS 4096
#define MAX_NAME 128
#define MAX_URL 1024
#define MAX_HEADERS 8
#define MAX_CATEGORY 64
#define MAX_REGION 64

/* ---------------- 频道 ---------------- */

typedef struct {
    char name[MAX_NAME];
    char header_name[MAX_HEADERS][64];
    char header_value[MAX_HEADERS][256];
    int  header_count;
} HeaderSet;

typedef struct {
    long long id;
    char  name[MAX_NAME];
    char  primary_url[MAX_URL];
    char  backup_url[MAX_URL];
    char  category[MAX_CATEGORY];   /* 一级：省份 */
    char  region[MAX_REGION];      /* 二级：地市/区县 */
    char  player[16];
    HeaderSet headers;
} Channel;

/* ---------------- 自愈参数（对齐安卓 resilience 段） ---------------- */

typedef struct {
    int buffer_stall_sec;          /* 传输层缓冲停滞阈值 */
    int audio_stall_sec;           /* 音频断流阈值（安卓是 frame_stall，音频台改为音频） */
    int soft_retry_max;            /* 软重连次数上限 */
    int hard_retry_max;            /* 硬重连次数上限 */
    int retry_intervals[8];        /* 0/5/15/30 */
    int retry_interval_count;
    int network_probe_sec;         /* 休眠后网络探测周期 */
    int dynamic_refresh_sec;       /* 动态签名源（litenews/hntv）主动换票周期，0=关闭 */
} Resilience;

typedef struct {
    int force_max;                 /* 系统音量顶满并纠偏 */
    int boost_mb;                  /* 软件数字增益，毫巴（已不参与设备音量，见 player.c 注释） */
    int max_boost_mb;
    int startup_volume;            /* 每次启动的初始音量 0..100 */
    int remember_volume;           /* 1=记住上次音量；0=每次启动都用 startup_volume */
} AudioCfg;

typedef struct {
    Channel  *channels;            /* 运行时 malloc，容量 = channel_cap */
    int       channel_count;
    int       channel_cap;
    long long current_channel_id;
    long long favorites[128];
    int       favorite_count;
    Resilience res;
    AudioCfg  audio;
    int       channel_lock;        /* 1 = 锁定当前台，禁止自动/手动换台 */
} Config;

/* ---------------- 播放状态 ---------------- */

typedef enum {
    MODE_PLAYING = 0,
    MODE_RETRYING,
    MODE_WAITING_NETWORK
} PlayMode;

/* ---------------- 全局运行态 ---------------- */

typedef struct {
    Config       cfg;
    int          cur_index;        /* 当前频道在cfg.channels 中的下标，-1 无 */

    PlayMode     mode;
    int          failures;         /* 连续失败次数 */
    int          soft_count;       /* 已用软重连次数 */
    int          stall_count;      /* 已用硬重连次数 */
    double       last_fail_at;     /* GetTickCount64 基准 */
    double       next_retry_at;    /* 下次重试的绝对时刻，0=无 */
    int          reconnecting;     /* 正在重连中（区别于等待退避） */
    int          resolving;        /* 正在解析动态源 */
    double       resolve_at;       /* 本次会话动态源解析成功的时刻（换票计时基准） */

    /* 音频电平：waveOut 已提交但尚未播放的字节数，无数据即视为断流 */
    int          muted;
    int          volume;           /* 0..100 软件音量 */
    int          boosting;         /* 数字增益是否已应用 */

    /* 运行统计 */
    int          stat_disconnects;
    int          stat_stalls;
    char         stat_date[16];    /* 统计归属日期，跨天归零 */

    /* 侧边栏 */
    int          panel_open;
    int          panel_scroll;
    int          panel_hover;
} AppState;

extern AppState g_app;

/* ---------------- 模块接口 ---------------- */

/* util.c */
void   log_event(const char *category, const char *fmt, ...);
char  *utf8_to_wide_or_die(const char *s);       /* 返回需 free 的宽串 */
void   wide_to_utf8_or_die(char *dst, int cap, const WCHAR *src);
void   ansi_to_utf8(char *dst, int cap, const char *ansi);  /* 打印 ANSI 路径到 UTF-8 日志前必备 */
double now_ms(void);
long long unix_now(void);                  /* Unix 秒（墙上时钟）；别用 now_ms 代替，起点不同 */
int    ensure_app_dir(char *out, int cap);
int    exe_dir(char *out, int cap);

/* config.c */
int    config_load(void);                        /* 0 成功 */
void   config_load_state(void);                  /* 读回上次的当前台与音量 */
const Channel *config_find(long long id);
int    config_index_of(long long id);
void   config_save_state(void);

/* resolve.c */
int    resolve_url(const Channel *ch, char *out, int cap);
int    http_get(const char *url, char **body_out, const char *ua,
                const char *referer, const char *extra_headers);
/* nzgwin.h 不 include <winhttp.h>（保持纯头文件），故用 void* 收 HINTERNET */
void   http_enable_modern_tls(void *session);  /* Win7 必须显式开 TLS 1.2，否则 12175 */

/* liveness.c —— HLS 活性探测：区分「真推流」与「循环垫片」 */
int    probe_hls_liveness(const char *url, int wait_sec);
/*   返回 1=在播  0=疑似垫片  -1=无法判定（非 m3u8 / 分片名里没时间戳） */
extern int g_opt_livecheck;                /* 命令行 --live：自检时顺带做活性探测 */

/* player.c */
void   fflog_install(void);               /* 把 FFmpeg 的 WARNING/ERROR 接进我们的日志（排障） */
int    player_start(void);
void   player_stop(void);
int    player_switch(int index);
void   player_hard_rebuild(const char *reason);
void   player_tick_watchdog(void);
int    player_is_playing(void);
void   player_set_volume(int v);
void   player_toggle_mute(void);
int    player_seconds_since_audio(void);
int    player_thread_alive(void);                /* 播放线程是否还活着（0=已退出，看门狗须立即接管） */
double player_spawn_elapsed(void);               /* 距本会话线程启动过了多少秒（首帧超时判死用） */
int    player_dynamic_source(void);              /* 当前台是否为动态签名源（litenews/hntv） */
void   player_force_max_volume(void);
void   player_heartbeat(void);                   /* 每 60 秒一行运行态心跳（内部节流） */
int    player_selftest(void);                    /* --selftest：逐个频道自检，返回失败台数 */

/* net.c */
int    net_probe(void);                          /* 1=在线 0=离线 */

/* tray.c —— 托盘图标 + 开机自启（v1.12） */
#define WM_TRAYICON (WM_APP + 2)                 /* 托盘回调：lParam=鼠标事件 */
#define WM_SHOWSELF (WM_APP + 4)                 /* 第二实例请求显示主界面 */
int    tray_init(HWND hwnd);                     /* 装托盘图标（启动即后台） */
void   tray_remove(void);                        /* 退出时卸图标 */
void   tray_sync(void);                          /* tooltip 跟随 台名/状态/静音 */
int    autorun_setup(void);                      /* 幂等写 HKCU Run，0=失败（调用方须提示） */
extern int g_opt_show;                           /* --show：本次启动显示主界面（默认纯后台） */

/* ui.c */
void   ui_init(HINSTANCE);
void   ui_post_tick(void);
void   ui_refresh(void);

#endif /* NZGWIN_H */