/* 配置层
 *
 * 与安卓版一致的优先级：exe 同目录 config.json > 内置资源 config_default.json。
 * 绿色单 exe 的要求是「双击即用」，所以 29 台频道清单必须编进 exe；
 * 想改台时才放一个 config.json 过去覆盖，不需要重新编译。
 *
 * 运行态（当前台/音量）单独写 state.json，不污染用户自己的 config.json。
 */
#include "nzgwin.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 由 build 时生成的 resource.c 提供 */
extern const unsigned char g_config_default_json[];
extern const unsigned int  g_config_default_json_len;

/* 默认自愈参数（与安卓版一致，配置文件缺失时兜底） */
static void resilience_defaults(Resilience *r)
{
    r->buffer_stall_sec     = 10;
    r->audio_stall_sec      = 15;
    r->soft_retry_max       = 3;
    r->hard_retry_max       = 3;
    r->retry_intervals[0]   = 0;
    r->retry_intervals[1]   = 5;
    r->retry_intervals[2]   = 15;
    r->retry_intervals[3]   = 30;
    r->retry_interval_count = 4;
    r->network_probe_sec    = 30;
    /* 动态签名源（litenews/hntv）主动换票周期。
     * 2026-10-23 六小时长测定案：单县 155 / 巨野 139 用**同一个 100ycdn 签名 URL**
     * 连续请求 1428 次（各 714 次，间隔 30s），**全部 HTTP 200、fails 恒 0、playlist 一路推进**
     * → 签名有效期实测 ≥ 6 小时（此前"几分钟就过期"的说法只对个别平台成立，别外推）。
     * 取 3 小时 = 对实测下界的 2 倍余量；换票要掉 **实测 6 秒**音频
     * （v1.13.2 实测：15:02:26 重建 → 15:02:32 重新出声，大头是 100ycdn 新域名的冷 TLS），
     * 20 分钟一次等于每天 72 次断音（按 6 秒算约 7 分钟静音），对 7×24 的收音机是纯损。
     * 万一某个平台票更短，失效后仍是「报错 → 线程干净退出 → 看门狗重建」兜底，不会卡死。 */
    r->dynamic_refresh_sec  = 10800;
}

static void copy_str(char *dst, int cap, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    _snprintf(dst, cap, "%s", src);
    dst[cap - 1] = '\0';
}

static void parse_channel(const JValue *o, Channel *ch, int index)
{
    memset(ch, 0, sizeof(*ch));
    ch->id = (long long)json_num(o, "id", index + 1);
    copy_str(ch->name, MAX_NAME, json_str(o, "name", "未命名"));
    copy_str(ch->primary_url, MAX_URL, json_str(o, "primary_url", ""));
    copy_str(ch->backup_url, MAX_URL, json_str(o, "backup_url", ""));
    copy_str(ch->category, MAX_CATEGORY, json_str(o, "category", ""));
    copy_str(ch->region, MAX_REGION, json_str(o, "region", ""));
    copy_str(ch->player, 16, json_str(o, "player", "auto"));

    const JValue *h = json_get(o, "headers");
    if (h && h->type == JSON_OBJECT) {
        for (int i = 0; i < h->n && ch->headers.header_count < MAX_HEADERS; i++) {
            if (h->members[i].val->type != JSON_STRING) continue;
            ch->headers.header_count++;
            int k = ch->headers.header_count - 1;
            copy_str(ch->headers.header_name[k], 64, h->members[i].key);
            copy_str(ch->headers.header_value[k], 256, h->members[i].val->str);
        }
    }
}

static int config_parse_text(const char *text, const char *source)
{
    JValue *root = json_parse(text);
    if (!root || root->type != JSON_OBJECT) {
        json_free(root);
        log_event("配置", "解析失败（%s），内容不是合法 JSON 对象", source);
        return 0;
    }

    Config *cfg = &g_app.cfg;
    /* 支持重复加载（外部配置解析失败时会回退内置配置再解析一次），
     * 先释放上一轮分配的频道数组，避免泄漏 */
    free(cfg->channels);
    cfg->channels = NULL;
    memset(cfg, 0, sizeof(*cfg));
    resilience_defaults(&cfg->res);
    cfg->audio.force_max = 1;
    cfg->audio.boost_mb = 1200;
    cfg->audio.max_boost_mb = 1500;
    cfg->audio.startup_volume = 70;    /* 用户要求：启动后自动 70% */
    cfg->audio.remember_volume = 0;    /* 默认不记忆，每次启动都回到 70% */

    const JValue *arr = json_get(root, "channels");
    if (arr && arr->type == JSON_ARRAY) {
        int n = arr->n;
        if (n > MAX_CHANNELS) n = MAX_CHANNELS;   /* 只是防呆上限 */
        if (n > 0) {
            cfg->channels = (Channel *)calloc((size_t)n, sizeof(Channel));
            if (!cfg->channels) {
                json_free(root);
                log_event("配置", "内存不足，无法分配 %d 个频道", n);
                return 0;
            }
            cfg->channel_cap = n;
            for (int i = 0; i < n; i++) {
                const JValue *o = json_at(arr, i);
                if (!o || o->type != JSON_OBJECT) continue;
                parse_channel(o, &cfg->channels[cfg->channel_count], i);
                cfg->channel_count++;
            }
        }
    }

    cfg->current_channel_id = (long long)json_num(root, "current_channel_id", 1);

    const JValue *fav = json_get(root, "favorites");
    if (fav && fav->type == JSON_ARRAY) {
        for (int i = 0; i < fav->n && cfg->favorite_count < 128; i++) {
            const JValue *v = json_at(fav, i);
            if (!v) continue;
            if (v->type == JSON_NUMBER) cfg->favorites[cfg->favorite_count++] = (long long)v->num;
            else if (v->type == JSON_STRING) cfg->favorites[cfg->favorite_count++] = strtoll(v->str, NULL, 10);
        }
    }

    const JValue *r = json_get(root, "resilience");
    if (r && r->type == JSON_OBJECT) {
        Resilience *res = &cfg->res;
        res->buffer_stall_sec  = (int)json_num(r, "buffer_stall_sec", res->buffer_stall_sec);
        /* 安卓版叫 frame_stall_sec（视频画面停滞），这里是纯音频，
         * 沿用同一个配置键以保持两份配置同构 */
        res->audio_stall_sec   = (int)json_num(r, "frame_stall_sec", res->audio_stall_sec);
        res->soft_retry_max    = (int)json_num(r, "soft_retry_max", res->soft_retry_max);
        res->hard_retry_max    = (int)json_num(r, "hard_retry_max", res->hard_retry_max);
        res->network_probe_sec = (int)json_num(r, "network_probe_sec", res->network_probe_sec);
        res->dynamic_refresh_sec = (int)json_num(r, "dynamic_refresh_sec", res->dynamic_refresh_sec);
        const JValue *ia = json_get(r, "retry_intervals_sec");
        if (ia && ia->type == JSON_ARRAY && ia->n > 0) {
            res->retry_interval_count = 0;
            for (int i = 0; i < ia->n && res->retry_interval_count < 8; i++) {
                const JValue *v = json_at(ia, i);
                if (v && v->type == JSON_NUMBER)
                    res->retry_intervals[res->retry_interval_count++] = (int)v->num;
            }
            if (res->retry_interval_count == 0) resilience_defaults(res);
        }
    }

    const JValue *a = json_get(root, "audio");
    if (a && a->type == JSON_OBJECT) {
        cfg->audio.force_max    = json_bool(a, "force_max", 1);
        cfg->audio.boost_mb     = (int)json_num(a, "boost_mb", 1200);
        cfg->audio.max_boost_mb = (int)json_num(a, "max_boost_mb", 1500);
        cfg->audio.startup_volume = (int)json_num(a, "startup_volume", 70);
        cfg->audio.remember_volume = json_bool(a, "remember_volume", 0);
        if (cfg->audio.startup_volume < 0)   cfg->audio.startup_volume = 0;
        if (cfg->audio.startup_volume > 100) cfg->audio.startup_volume = 100;
        /* 上限保护：与安卓版一致，再高几乎必然削波 */
        if (cfg->audio.boost_mb < 0) cfg->audio.boost_mb = 0;
        if (cfg->audio.boost_mb > 2000) cfg->audio.boost_mb = 2000;
        if (cfg->audio.max_boost_mb > 2000) cfg->audio.max_boost_mb = 2000;
    }

    const JValue *lk = json_get(root, "lock");
    if (lk) cfg->channel_lock = json_bool(lk, "channel_lock", 0);

    json_free(root);

    if (cfg->channel_count == 0) {
        log_event("配置", "没有任何频道（%s）", source);
        return 0;
    }

    /* 当前台：优先 id 命中，否则退到第一个 */
    g_app.cur_index = config_index_of(cfg->current_channel_id);
    if (g_app.cur_index < 0) g_app.cur_index = 0;
    cfg->current_channel_id = cfg->channels[g_app.cur_index].id;

    log_event("配置", "已加载 %d 个频道（来源：%s），当前台=%s",
              cfg->channel_count, source, cfg->channels[g_app.cur_index].name);
    return 1;
}

int config_index_of(long long id)
{
    if (!g_app.cfg.channels) return -1;
    for (int i = 0; i < g_app.cfg.channel_count; i++) {
        if (g_app.cfg.channels[i].id == id) return i;
    }
    return -1;
}

const Channel *config_find(long long id)
{
    int i = config_index_of(id);
    return i < 0 ? NULL : &g_app.cfg.channels[i];
}

int config_load(void)
{
    /* 1) 优先用 exe 同目录的 config.json（用户改台走这条路） */
    char dir[MAX_PATH], path[MAX_PATH];
    if (exe_dir(dir, sizeof(dir))) {
        _snprintf(path, sizeof(path), "%s\\config.json", dir);
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(h, NULL);
            CloseHandle(h);
            if (size > 0 && size < 4 * 1024 * 1024) {
                FILE *f = fopen(path, "rb");
                if (f) {
                    char *buf = (char *)malloc(size + 1);
                    if (buf) {
                        size_t got = fread(buf, 1, size, f);
                        buf[got] = '\0';
                        fclose(f);
                        if (config_parse_text(buf, "外部 config.json")) { free(buf); return 1; }
                        free(buf);
                    } else fclose(f);
                }
                log_event("配置", "外部 config.json 解析失败，回退内置配置");
            }
        }
    }

    /* 2) 内置资源（编译进 exe 的 29 台清单） */
    char *text = (char *)malloc(g_config_default_json_len + 1);
    if (!text) return 0;
    memcpy(text, g_config_default_json, g_config_default_json_len);
    text[g_config_default_json_len] = '\0';
    int ok = config_parse_text(text, "内置配置");
    free(text);
    return ok;
}

/* 运行态落盘：只存当前台与音量，不碰用户的 config.json。
 * 这样「绿色版」拷到别的电脑上不会带着别人机器的运行态。 */
void config_save_state(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    if (!exe_dir(dir, sizeof(dir))) return;
    _snprintf(path, sizeof(path), "%s\\state.json", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "{\n"
        "  \"current_channel_id\": %lld,\n"
        "  \"volume\": %d,\n"
        "  \"muted\": %d\n"
        "}\n",
        g_app.cfg.current_channel_id, g_app.volume, g_app.muted);
    fclose(f);
}

/* 启动时读回运行态（必须在 config_load 之后、player_start 之前调用） */
void config_load_state(void)
{
    char dir[MAX_PATH], path[MAX_PATH];

    /* 音量：默认**每次启动都回到 audio.startup_volume**（用户要求「启动后自动 70%」）。
     * 只有显式把 audio.remember_volume 置 1 才恢复上次退出时的音量。
     * 这段必须放在读 state.json **之前**：否则下面那条恢复分支里的默认值
     * 会把配置里改过的 startup_volume 覆盖回写死的数。 */
    g_app.volume = g_app.cfg.audio.startup_volume;
    if (g_app.volume < 0)   g_app.volume = 0;
    if (g_app.volume > 100) g_app.volume = 100;

    if (!exe_dir(dir, sizeof(dir))) return;
    _snprintf(path, sizeof(path), "%s\\state.json", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz < 64 * 1024) {
        char *buf = (char *)malloc(sz + 1);
        if (buf) {
            size_t got = fread(buf, 1, sz, f);
            buf[got] = '\0';
            JValue *v = json_parse(buf);
            if (v && v->type == JSON_OBJECT) {
                long long cid = (long long)json_num(v, "current_channel_id", -1);
                int idx = config_index_of(cid);
                if (idx >= 0) {
                    g_app.cur_index = idx;
                    g_app.cfg.current_channel_id = g_app.cfg.channels[idx].id;
                    /* 启动日志那行「当前台=」打的是**配置默认值**，早于 state 恢复，
                     * 不补这行就会出现「日志说鹤壁、其实在播韶关」的矛盾（2026-11-02 踩到）。 */
                    log_event("配置", "恢复上次状态：台=%s", g_app.cfg.channels[idx].name);
                }
                /* 只在「记住音量」打开时才恢复，否则保持 startup_volume */
                if (g_app.cfg.audio.remember_volume) {
                    g_app.volume = (int)json_num(v, "volume", g_app.volume);
                    if (g_app.volume < 0) g_app.volume = 0;
                    if (g_app.volume > 100) g_app.volume = 100;
                }
                g_app.muted  = json_bool(v, "muted", 0);
            }
            json_free(v);
            free(buf);
        }
    }
    fclose(f);
}