/* 动态源解析
 *
 * 29 个台里有 20 个的地址不是固定直链，必须在「播放前」实时换取：
 *   litenews://  17 台 —— app.litenews.cn 按 orgid 查，签名几分钟就失效
 *   hntv://       3 台 —— 河南大象系 xy-gw 网关，需 timestamp + sha256(salt+ts) + clientId 三个头
 *   page://             —— 抓 HTML 取其中第一个 m3u8
 *   juyun://            —— 接口返回 AES-ECB 密文地址，解密才拿到真链
 *
 * 这里用 WinHTTP（Win7 起系统自带）而不是 socket，直线省掉 TLS/重定向的坑。
 * 与安卓版一样：解析失败回落到 primary_url 原文，由自愈状态机接管重试。
 */
#include "nzgwin.h"
#include "json.h"
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* juyun 平台通用密钥（与安卓版一致） */
static const unsigned char JUYUN_KEY[] = {
    0x8e, 0x87, 0x77, 0x4c, 0x04, 0x6f, 0xf2, 0x26,
    0xeb, 0x14, 0x07, 0xdc, 0x9a, 0xdd, 0x87, 0x3d
};
/* 河南大象系 xy-gw：sign = sha256(salt + timestamp) */
static const char *HNTV_SALT = "6ca114a836ac7d73";

/* 河南各县App 专属 clientId（键为拼音，避免中文 host 解析问题） */
static const struct { const char *key; const char *client; } HNTV_CLIENTS[] = {
    { "shenqiu", "09a9ab11660d4aa5a4a0fb6a7809f1e6" },
    { "taikang", "71a0b3560f4e422ab7e5260ea14f1a8f" },
    { "xihua",   "069b05fa076f4a9b9a97fea2eac9c97d" },
    { "luyi",    "ecddfc8273dd42508b2d5d427437d067" },
    { "fugou",   "558feebc64524f988fa12d9ecaddf81e" },
};

/* 默认 UA：与安卓版保持一致，部分站点按 UA 灰度 */
static const char *DEFAULT_UA =
    "Mozilla/5.0 (Linux; Android 9; TV) AppleWebKit/537.36 ShouBo/0.1";

/* ---------------- SHA-256（hntv 签名用） ---------------- */

typedef struct {
    unsigned int  h[8];
    unsigned char buf[64];
    unsigned long long len;
} Sha256;

static const unsigned int SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR32(x,n) (((x) >> (n)) | ((x) << (32-(n))))

static void sha256_block(Sha256 *c, const unsigned char *p)
{
    unsigned int w[64], a,b,cc,d,e,f,g,h,t1,t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((unsigned)p[i*4]<<24)|((unsigned)p[i*4+1]<<16)|
               ((unsigned)p[i*4+2]<<8)|((unsigned)p[i*4+3]);
    for (i = 16; i < 64; i++) {
        unsigned int s0 = ROTR32(w[i-15],7) ^ ROTR32(w[i-15],18) ^ (w[i-15] >> 3);
        unsigned int s1 = ROTR32(w[i-2],17) ^ ROTR32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3];
    e=c->h[4]; f=c->h[5]; g=c->h[6];  h=c->h[7];
    for (i = 0; i < 64; i++) {
        unsigned int S1 = ROTR32(e,6) ^ ROTR32(e,11) ^ ROTR32(e,25);
        unsigned int ch = (e & f) ^ ((~e) & g);
        t1 = h + S1 + ch + SHA256_K[i] + w[i];
        unsigned int S0 = ROTR32(a,2) ^ ROTR32(a,13) ^ ROTR32(a,22);
        unsigned int maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g;  c->h[7]+=h;
}

static void sha256_hex(const char *in, char *out65)
{
    Sha256 c;
    c.h[0]=0x6a09e667; c.h[1]=0xbb67ae85; c.h[2]=0x3c6ef372; c.h[3]=0xa54ff53a;
    c.h[4]=0x510e527f; c.h[5]=0x9b05688c; c.h[6]=0x1f83d9ab; c.h[7]=0x5be0cd19;
    c.len = 0;
    size_t n = strlen(in);
    const unsigned char *p = (const unsigned char *)in;
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha256_block(&c, p + i);
    unsigned char tail[128];
    size_t rem = n - i;
    memcpy(tail, p + i, rem);
    tail[rem++] = 0x80;
    size_t tl = (rem <= 56) ? 64 : 128;
    memset(tail + rem, 0, tl - rem - 8);
    unsigned long long bits = (unsigned long long)n * 8ULL;
    for (int k = 0; k < 8; k++) tail[tl - 1 - k] = (unsigned char)(bits >> (8*k));
    sha256_block(&c, tail);
    if (tl == 128) sha256_block(&c, tail + 64);
    static const char *hex = "0123456789abcdef";
    for (int k = 0; k < 8; k++) {
        for (int b = 0; b < 4; b++) {
            unsigned char v = (unsigned char)(c.h[k] >> (24 - 8*b));
            out65[k*8 + b*2]     = hex[v >> 4];
            out65[k*8 + b*2 + 1] = hex[v & 15];
        }
    }
    out65[64] = '\0';
}

/* ---------------- Base64 / AES-128-ECB（juyun 解密用） ---------------- */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_decode(const char *in, unsigned char *out, int out_cap)
{
    int len = 0, val = 0, bits = -8;
    for (const char *p = in; *p && *p != '='; p++) {
        const char *q = strchr(B64, *p);
        if (!q) { if (*p=='\n'||*p=='\r'||*p==' ') continue; return -1; }
        val = (val << 6) + (int)(q - B64);
        bits += 6;
        if (bits >= 0) {
            if (len >= out_cap) return -1;
            out[len++] = (unsigned char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return len;
}

static unsigned char g_aes_sbox[256], g_aes_inv[256];

static void aes_build_tables(void)
{
    static int inited = 0;
    if (inited) return;
    inited = 1;
    /* 由 GF(2^8) 模 0x11B 生成 S 盒（避免引入外部表，防篡改） */
    unsigned char p = 1, q = 1;
    do {
        p = (unsigned char)(p ^ (unsigned char)(p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q ^= (unsigned char)(q << 1);
        q ^= (unsigned char)(q << 2);
        q ^= (unsigned char)(q << 4);
        if (q & 0x80) q ^= 0x09;
        unsigned char x = (unsigned char)(q ^ (unsigned char)((q << 1) | (q >> 7)) ^
                          (unsigned char)((q << 2) | (q >> 6)) ^
                          (unsigned char)((q << 3) | (q >> 5)) ^
                          (unsigned char)((q << 4) | (q >> 4)));
        g_aes_sbox[p] = (unsigned char)(x ^ 0x63);
    } while (p != 1);
    g_aes_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) g_aes_inv[g_aes_sbox[i]] = (unsigned char)i;
}

static unsigned char g_aes_rcon[11];
static void aes_init_rcon(void)
{
    unsigned char v = 1;
    for (int i = 0; i < 10; i++) { g_aes_rcon[i + 1] = v; v = (unsigned char)(v << 1); }
}

static void aes_add_round_key(unsigned char *s, const unsigned char *rk)
{
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

static void aes_encrypt_block(const unsigned char *in, unsigned char *out,
                              const unsigned char *rk)
{
    unsigned char s[16];
    memcpy(s, in, 16);
    aes_add_round_key(s, rk);

    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = g_aes_sbox[s[i]];

        /* ShiftRows：状态按列主序，行 r 左移 r 字节 */
        unsigned char t[16];
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++)
                t[c*4 + r] = s[((c + r) % 4)*4 + r];
        }
        memcpy(s, t, 16);

        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                unsigned char *p = s + c*4;
                unsigned char a0=p[0],a1=p[1],a2=p[2],a3=p[3];
                p[0] = (unsigned char)(a0 ^ a1 ^ a2 ^ a3);
                p[1] = (unsigned char)(a0 ^ ((unsigned char)(a1<<1) | (a1>>7)) ^ a2 ^ ((unsigned char)(a3<<1) | (a3>>7)));
                p[2] = (unsigned char)(a0 ^ a1 ^ ((unsigned char)(a2<<1) | (a2>>7)) ^ ((unsigned char)(a3<<1) | (a3>>7)));
                p[3] = (unsigned char)(a0 ^ ((unsigned char)(a1<<1)|(a1>>7)) ^ ((unsigned char)(a2<<1)|(a2>>7)) ^ a3);
            }
        }
        aes_add_round_key(s, rk + round * 16);
    }
    memcpy(out, s, 16);
}

static void aes_decrypt_block(const unsigned char *in, unsigned char *out,
                              const unsigned char *rk)
{
    unsigned char s[16];
    memcpy(s, in, 16);
    aes_add_round_key(s, rk + 160);

    for (int round = 9; round >= 0; round--) {
        /* InvShiftRows */
        unsigned char t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[((c + r) % 4)*4 + r] = s[c*4 + r];
        memcpy(s, t, 16);

        for (int i = 0; i < 16; i++) s[i] = g_aes_inv[s[i]];

        aes_add_round_key(s, rk + round * 16);

        if (round != 0) {
            for (int c = 0; c < 4; c++) {
                unsigned char *p = s + c*4;
                unsigned char a0=p[0],a1=p[1],a2=p[2],a3=p[3];
                p[0] = (unsigned char)(a0 ^ a1 ^ a2 ^ a3);
                p[1] = (unsigned char)(a0 ^ a1 ^ a2 ^ ((unsigned char)((a3<<1)|(a3>>7))));
                p[2] = (unsigned char)(a0 ^ a1 ^ ((unsigned char)((a2<<1)|(a2>>7))) ^ a3);
                p[3] = (unsigned char)(a0 ^ ((unsigned char)((a1<<1)|(a1>>7))) ^ a2 ^ a3);
            }
        }
    }
    memcpy(out, s, 16);
}

static void aes_expand_key(const unsigned char *key, unsigned char *rk)
{
    aes_build_tables();
    aes_init_rcon();
    memcpy(rk, key, 16);
    unsigned char t[4];
    for (int i = 4; i < 44; i++) {
        t[0] = rk[(i-1)*4 + 0];
        t[1] = rk[(i-1)*4 + 1];
        t[2] = rk[(i-1)*4 + 2];
        t[3] = rk[(i-1)*4 + 3];
        if (i % 4 == 0) {
            unsigned char tmp = t[0];
            t[0] = g_aes_sbox[t[1]] ^ g_aes_rcon[i/4];
            t[1] = g_aes_sbox[t[2]];
            t[2] = g_aes_sbox[t[3]];
            t[3] = g_aes_sbox[tmp];
        }
        for (int k = 0; k < 4; k++)
            rk[i*4 + k] = (unsigned char)(rk[(i-4)*4 + k] ^ t[k]);
    }
}

/* AES-128-ECB + PKCS7 解密，输入输出都是 Base64 字符串 */
static int juyun_decrypt(const char *enc_b64, char *out, int out_cap)
{
    unsigned char buf[1024];
    int n = b64_decode(enc_b64, buf, (int)sizeof(buf));
    if (n <= 0 || (n % 16) != 0) return 0;
    unsigned char rk[176];
    aes_expand_key(JUYUN_KEY, rk);
    int o = 0;
    for (int i = 0; i < n; i += 16) {
        unsigned char plain[16];
        aes_decrypt_block(buf + i, plain, rk);
        int copy = 16;
        /* 最后一块按 PKCS7 去填充 */
        if (i + 16 == n) {
            int pad = plain[15];
            if (pad < 1 || pad > 16) return 0;
            for (int k = 0; k < pad; k++) if (plain[15-k] != pad) return 0;
            copy = 16 - pad;
        }
        if (o + copy >= out_cap) copy = out_cap - o - 1;
        if (copy <= 0) break;
        memcpy(out + o, plain, (size_t)copy);
        o += copy;
    }
    out[o] = '\0';
    return o > 0;
}

/* ---------------- WinHTTP GET ---------------- */

/* 显式开启 TLS 1.2 / 1.1 / 1.0。
 *
 * ⚠️ 这一段是 **Win7 能不能用的关键**，踩过（2026-10-10 真机 Win7 报 12175）：
 *
 *   Windows 7 上 **WinHTTP 的默认协议列表只有 TLS 1.0**。即使系统本身已经支持 TLS 1.2，
 *   应用也必须显式调 `WinHttpSetOption(WINHTTP_OPTION_SECURE_PROTOCOLS)` 才会去协商。
 *   而现在的站点（app.litenews.cn、100ycdn…）只收 TLS 1.2+，
 *   于是握手阶段就被拒 -> `WinHttpSendRequest` 直接失败，错误码 **12175
 *   = ERROR_WINHTTP_SECURE_FAILURE**，日志表现为「动态源解析失败，暂用原地址重试」，
 *   但源是好的、网络也是通的。
 *
 *   **为什么开发机（Win11）100% 测不出来**：Win11 默认就协商 TLS 1.3，不需要这段设置。
 *
 *   **为什么同一台 Win7 上 https 直链却能正常播**：FFmpeg 用的是 **Schannel**
 *   （`libavformat.a` 里能搜到 schannel 符号），Schannel 直接跟系统协商、
 *   不受 WinHTTP 那份默认协议列表约束；而 Schannel 能协商上 TLS 1.2，
 *   说明**这台机器的系统 TLS 1.2 本来就是可用的**（Win7 自带 TLS 1.2，
 *   KB3140245 只是把它默认打开）。这组对照正好证明故障点只在 WinHTTP 那一段，
 *   改这一段就够，不必动 FFmpeg。
 *
 *   若这里 SetOption 仍失败、或设了之后依旧 12175 -> 系统 TLS 1.2 没启用，
 *   去装 KB3140245（Win7 的 TLS 1.2 补丁）。
 */
void http_enable_modern_tls(void *session)
{
    static int logged = 0;          /* 只打一次，别刷屏 */
    HINTERNET s = (HINTERNET)session;

    DWORD want = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 |
                 WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
                 WINHTTP_FLAG_SECURE_PROTOCOL_TLS1;
    if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &want, sizeof(want))) {
        if (!logged) {
            logged = 1;
            log_event("网络", "TLS：⚠️ 启用 TLS1.0/1.1/1.2 失败（err=%lu）。"
                      "Win7 需安装 KB3140245 才支持 TLS 1.2",
                      (unsigned long)GetLastError());
        }
        return;
    }
    if (!logged) {
        logged = 1;
        log_event("网络", "TLS：已请求启用 TLS1.0/1.1/1.2"
                  "（WinHTTP 不支持读回该选项，只能靠请求是否成功来验证；"
                  "若仍报 12175 说明系统 TLS 1.2 没启用，去装 KB3140245）");
    }
}

/* 把 WinHTTP 错误码翻成人话（只翻我们真会碰到的几个） */
static const char *winhttp_err_text(DWORD e)
{
    switch (e) {
    case 12175: return " TLS 握手被拒 = Win7 没启用 TLS 1.2，装 KB3140245 可解决";
    case 12007: return " 连接超时";
    case 12029: return " 连接被拒";
    case 12002: return " 操作超时";
    default:    return "";
    }
}

typedef struct {
    char *name;
    char *value;
} ExtraHeader;

int http_get(const char *url, char **body_out, const char *ua,
             const char *referer, const char *extra_headers)
{
    *body_out = NULL;
    if (!url || !*url) return 0;

    /* 拆 URL：scheme://host/path */
    const char *p = strstr(url, "://");
    if (!p) return 0;
    char host[256] = {0}, path[2048] = {0};
    int https = (p - url == 5 && _strnicmp(url, "https", 5) == 0);
    const char *hoststart = p + 3;
    const char *slash = strchr(hoststart, '/');
    if (slash) {
        size_t hl = (size_t)(slash - hoststart);
        if (hl >= sizeof(host)) hl = sizeof(host) - 1;
        memcpy(host, hoststart, hl);
        _snprintf(path, sizeof(path), "%s", slash);
    } else {
        _snprintf(host, sizeof(host), "%s", hoststart);
        _snprintf(path, sizeof(path), "/");
    }
    /* 去端口 */
    char *colon = strchr(host, ':');
    if (colon) *colon = '\0';
    if (!host[0]) return 0;

    /* ⚠️ 必须是 DEFAULT_PROXY 而不是 AUTOMATIC_PROXY。
     * AUTOMATIC_PROXY（值 4）是 Win8.1+ 才支持的枚举，在 Win7 上
     * WinHttpOpen 会直接失败返回 NULL —— 结果就是「所有走 HTTP 的动态源
     * 全挂」，而直链（绕过本函数直接交给 FFmpeg）却全好，极难排查。
     * 本程序要兼容 Win7，只能用从 XP 时代就存在的 DEFAULT_PROXY。 */
    HINTERNET hSession = WinHttpOpen(L"shoubo-nzg/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return 0;
    http_enable_modern_tls(hSession);
    WinHttpSetTimeouts(hSession, 8000, 8000, 8000, 10000);

    /* WinHTTP 是纯 Unicode API：UTF-8 host/path/头都要转宽串 */
    WCHAR *whost = (WCHAR *)utf8_to_wide_or_die(host);
    WCHAR *wpath = (WCHAR *)utf8_to_wide_or_die(path);
    if (!whost || !wpath) {
        free(whost); free(wpath);
        WinHttpCloseHandle(hSession);
        return 0;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, whost,
                                        https ? INTERNET_DEFAULT_HTTPS_PORT
                                              : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) {
        free(whost); free(wpath);
        WinHttpCloseHandle(hSession);
        return 0;
    }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath,
                                            NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        free(whost); free(wpath);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 0;
    }

    /* 忽略证书校验问题（未知 CA / 域名不匹配 / 过期 / 用途不符）。
     *
     * ⚠️ 这是有意为之，不是偷懒，三层理由：
     *  1) FFmpeg 拉 https 流时**默认就不验证证书**（tls 协议的 verify 选项默认 0）。
     *     解析器这边校验得再严、那边不校验，防不住任何东西，
     *     只会白白增加 Win7 上的失败率。
     *  2) 取的是公开的频道元数据，不带任何凭据。
     *  3) 踩过（2026-10-10 真机 Win7，err=12175）：
     *     litenews 的证书链是 *.litenews.cn <- GeoTrust TLS RSA CA G1
     *                                <- **DigiCert Global Root G2**（自签根），
     *     而 **Win7 的根证书库里没有 DigiCert Global Root G2**
     *     （2013 年后的新根，靠 Windows Update 按需下发，Win7 已停止支持拿不到）。
     *     WinHTTP 一验证书就 12175，所有动态源全挂。
     *     实测服务器是接受 Win7 schannel 支持的 TLS1.2 CBC 套件的
     *     （ECDHE-RSA-AES128-SHA256 / AES128-SHA256 等），协议层没问题。
     *
     * 更讲究的替代方案（不想关校验的话）：把 DigiCert Global Root G2 导入
     * 「受信任的根证书颁发机构」（归档里有 DigiCertGlobalRootG2.crt，双击安装），
     * 不用打补丁、不用改代码也能通。
     */
    DWORD secignore = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                     &secignore, sizeof(secignore));

    /* 头：UA + Referer + 频道自定义头 + 附加头（hntv 需要）
     * 注意：FFmpeg/WinHTTP 都要求头之间用 \r\n 换行 */
    char headers[4096];
    headers[0] = '\0';
    _snprintf(headers, sizeof(headers), "User-Agent: %s\r\n", ua ? ua : DEFAULT_UA);
    if (referer && *referer)
        _snprintf(headers + strlen(headers), sizeof(headers) - strlen(headers),
                  "Referer: %s\r\n", referer);
    if (extra_headers && *extra_headers) {
        _snprintf(headers + strlen(headers), sizeof(headers) - strlen(headers),
                  "%s\r\n", extra_headers);
    }
    WCHAR *wheaders = (WCHAR *)utf8_to_wide_or_die(headers);

    BOOL ok = wheaders
            ? WinHttpSendRequest(hRequest, wheaders, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            : FALSE;
    int result = 0;
    if (!ok) {
        DWORD e = (DWORD)GetLastError();
        log_event("网络", "发送请求失败（err=%lu）%s%s", (unsigned long)e, url,
                  winhttp_err_text(e));
    } else if (!WinHttpReceiveResponse(hRequest, NULL)) {
        DWORD e = (DWORD)GetLastError();
        log_event("网络", "接收响应失败（err=%lu）%s%s", (unsigned long)e, url,
                  winhttp_err_text(e));
    } else {
        /* ⚠️ size 必须初始化成 sizeof(status)，不能是 0。
         * WinHttpQueryHeaders 把 lpdwBufferLength 当「输入缓冲区字节数」，
         * 传 0 会立刻以 ERROR_INSUFFICIENT_BUFFER 失败、status 保持 0，
         * 表现就是日志里一片「HTTP 0 请求 …」而看不出任何原因
         * —— 踩过：18 台 litenews 动态源全挂就是这个。 */
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            /* 分块读干净：WinHttpQueryDataAvailable 只报「此刻可读」的字节数，
             * 大响应（litenews 的 JSON 带长 desc 字段，可达几十 KB）一次
             * WinHttpReadData 读不全，必须循环读到 0 为止。 */
            char *buf = NULL;
            size_t cap = 0, len = 0;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
                if (len + avail + 1 > 8 * 1024 * 1024) break;   /* 防呆：不信服务器报的长度 */
                if (len + avail + 1 > cap) {
                    size_t ncap = cap ? cap : 16384;
                    while (ncap < len + avail + 1) ncap *= 2;
                    char *nb = (char *)realloc(buf, ncap);
                    if (!nb) break;
                    buf = nb; cap = ncap;
                }
                DWORD read = 0;
                if (!WinHttpReadData(hRequest, buf + len, avail, &read) || read == 0) break;
                len += read;
            }
            if (buf && len > 0) { buf[len] = '\0'; *body_out = buf; result = 1; }
            else free(buf);
        } else {
            log_event("网络", "HTTP %lu 请求 %s", (unsigned long)status, url);
        }
    }

    free(wheaders);
    free(whost);
    free(wpath);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

/* 取 scheme://host 之后到 ? 或 & 之前的部分（orgid / clientId 就是这一段） */
static int scheme_key(const char *url, const char *scheme, char *out, int cap)
{
    size_t sl = strlen(scheme);
    if (_strnicmp(url, scheme, sl) != 0) return 0;
    const char *p = url + sl;
    while (*p && *p != '?' && *p != '&' && *p != '/' && *p != '#') {
        if ((size_t)(p - (url + sl)) >= (size_t)cap - 1) break;
        *out++ = *p++;
    }
    *out = '\0';
    return 1;
}

/* 取 query 参数（已 URL 解码） */
static int query_param(const char *url, const char *key, char *out, int cap)
{
    if (!out || cap <= 0) return 0;
    out[0] = '\0';
    const char *q = strchr(url, '?');
    if (!q) return 0;
    q++;
    size_t klen = strlen(key);
    while (*q) {
        const char *amp = strchr(q, '&');
        if (!amp) amp = q + strlen(q);
        if ((size_t)(amp - q) > klen && strncmp(q, key, klen) == 0 && q[klen] == '=') {
            /* ⚠️ 必须用独立的目标下标 o 写 out，不能用源下标 i。
             * 用 i 等于把「URL 编码串里的位置」当成「解码后的位置」：
             * %E7%A6%B9 这类三字节序列会在 out[0]、out[3]、out[6] 落下字节，
             * out[1]、out[2] 全是未初始化的栈垃圾 —— 解出来的频道名是乱码，
             * 后面的名字包含匹配就变成随机命中（踩过：litenews 时好时坏，
             * 同一个 orgid 下「交通广播」能过、「新闻综合广播」却不过）。 */
            const char *v = q + klen + 1;
            int o = 0;
            while (v < amp && o < cap - 1) {
                char c = *v;
                if (c == '%' && (amp - v) >= 3) {
                    char hx[3] = { v[1], v[2], 0 };
                    char *end = NULL;
                    long val = strtol(hx, &end, 16);
                    if (end && *end == 0) {
                        out[o++] = (char)val;
                        v += 3;
                        continue;
                    }
                }
                out[o++] = (c == '+') ? ' ' : c;
                v++;
            }
            out[o] = '\0';
            return 1;
        }
        if (!*amp) break;
        q = amp + 1;
    }
    return 0;
}

/* UTF-8 子串匹配（台名匹配用）。
 * 按 UTF-8 字符长度步进，而不是盲跳 2 字节 —— 台名里混着
 * 中文（3 字节）和 ASCII（FM88.9、89.6 等 1 字节），
 * 按固定步长会漏掉起始字节落在字符中间的对齐点。 */
static int utf8_contains(const char *hay, const char *needle)
{
    if (!needle || !*needle) return 1;
    if (!hay) return 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; ) {
        if (strncmp(p, needle, nl) == 0) return 1;
        unsigned char c = (unsigned char)*p;
        p += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    }
    return 0;
}

/* ---------------- 各协议解析 ---------------- */

/* 频道名匹配，两级：
 *   1) 正向包含：接口名里含配置的台名。绝大多数走这条。
 *   2) 前后缀兜底：接口名是我们台名的「后缀」或「前缀」。
 *
 * 为什么必须要有第 2 级：litenews 各 orgid 的命名风格并不统一 ——
 *   orgid 223/235/115/249 给的是完整名（「夏津新闻综合广播」「禹城市融媒体中心综合广播」），
 *   orgid 635 却只给频道后缀（「新闻广播 / 交通广播 / 经济广播 / 音乐广播」），
 *   orgid 157 只给（「新闻综合广播 / 交通广播 / 音乐广播」），
 *   orgid 141 给的是前缀（「鄄城县融媒体中心」，我们配的是「鄄城县融媒体中心广播」）。
 * 配置里写的是完整台名，只做正向包含就会整批漏掉。
 *
 * 两条兜底规则都限定「整个接口名必须完整对齐我们台名的一头」，
 * 且要求接口名至少 6 字节（两个汉字），避免「交通」这类短名乱撞。
 * 命中兜底时返回 2 / 3，调用方会记一条日志 —— 兜底是容错，
 * 不该静默发生，否则哪天选错台都查不出来。 */
static int lite_name_match(const char *api_name, const char *want)
{
    if (!api_name || !*api_name || !want || !*want) return 0;
    if (utf8_contains(api_name, want)) return 1;

    size_t al = strlen(api_name), wl = strlen(want);
    if (al < 6 || al >= wl) return 0;

    /* 后缀：api_name 正好落在 want 的结尾 */
    const char *tail = want + (wl - al);
    unsigned char c = (unsigned char)*tail;
    if (c < 0x80 || c >= 0xC0) {          /* 必须是合法的 UTF-8 起点 */
        if (strcmp(tail, api_name) == 0) return 2;
    }

    /* 前缀：api_name 正好是 want 的开头 */
    {
        char head[256];
        if (al < sizeof(head)) {
            memcpy(head, want, al);
            head[al] = '\0';
            if (strcmp(head, api_name) == 0) return 3;
        }
    }
    return 0;
}

/* litenews：radio 端点优先，其次 tv
 * 与安卓版一致：nzp 只查 tv 所以取不到广播，这里先 radio 后 tv。 */
static int fetch_lite_stream(const char *orgid, const char *kind,
                             const char *want, char *out, int cap)
{
    char url[512];
    _snprintf(url, sizeof(url), "https://app.litenews.cn/v1/app/play/%s/live?orgid=%s",
              kind, orgid);
    char *body = NULL;
    if (!http_get(url, &body,
                  "Mozilla/5.0 (Linux; Android 9; TV) AppleWebKit/537.36", NULL, NULL))
        return 0;

    int found = 0, weak = 0;
    char hitname[MAX_NAME] = "";
    char listed[512] = "";      /* 里层候选名，仅用于日志 */
    JValue *root = json_parse(body);
    free(body);
    if (root && root->type == JSON_OBJECT) {
        if ((int)json_num(root, "code", 0) == 1) {
            const JValue *data = json_get(root, "data");
            if (data && data->type == JSON_ARRAY) {
                for (int i = 0; i < data->n && !found; i++) {
                    const JValue *c = json_at(data, i);
                    if (!c || c->type != JSON_OBJECT) continue;
                    const char *nm = json_str(c, "name", "");
                    const char *st = json_str(c, "stream", "");
                    if (st[0] != 'h') continue;
                    if (listed[0]) {
                        size_t l = strlen(listed);
                        if (l + strlen(nm) + 4 < sizeof(listed))
                            _snprintf(listed + l, sizeof(listed) - l, " | %s", nm);
                    } else {
                        _snprintf(listed, sizeof(listed), "%s", nm);
                    }
                    int m = lite_name_match(nm, want);
                    if (m) {
                        _snprintf(out, cap, "%s", st);
                        _snprintf(hitname, sizeof(hitname), "%s", nm);
                        weak = (m != 1);
                        found = 1;
                    }
                }
            }
        }
    }
    if (found && weak)
        log_event("解析", "litenews orgid=%s %s 端点：台名「%s」未精确命中，兜底匹配到「%s」",
                  orgid, kind, want, hitname);
    if (!found)
        log_event("解析", "litenews orgid=%s %s 端点无「%s」，候选：%s",
                  orgid, kind, want, listed[0] ? listed : "（空）");
    json_free(root);
    return found;
}

static int resolve_litenews(const char *raw, char *out, int cap)
{
    char orgid[64], want[MAX_NAME], kind[16];
    if (!scheme_key(raw, "litenews://", orgid, sizeof(orgid))) return 0;
    query_param(raw, "name", want, sizeof(want));
    query_param(raw, "kind", kind, sizeof(kind));

    const char *endpoints[2];
    int ne = 0;
    if (_stricmp(kind, "radio") == 0)      endpoints[ne++] = "radio";
    else if (_stricmp(kind, "tv") == 0)    endpoints[ne++] = "tv";
    else { endpoints[ne++] = "radio"; endpoints[ne++] = "tv"; }

    for (int i = 0; i < ne; i++) {
        if (fetch_lite_stream(orgid, endpoints[i], want, out, cap)) return 1;
    }
    return 0;
}

/* 河南大象系：xy-gw 网关，sign = sha256(salt + unix秒) */
static int resolve_hntv(const char *raw, char *out, int cap)
{
    char key[128];
    if (!scheme_key(raw, "hntv://", key, sizeof(key))) return 0;

    const char *client = key;
    for (size_t i = 0; i < sizeof(HNTV_CLIENTS)/sizeof(HNTV_CLIENTS[0]); i++) {
        if (_stricmp(HNTV_CLIENTS[i].key, key) == 0) { client = HNTV_CLIENTS[i].client; break; }
    }

    char cls[16] = "1";
    query_param(raw, "cls", cls, sizeof(cls));

    char ts[32];
    _snprintf(ts, sizeof(ts), "%I64d", (long long)(GetTickCount64() / 1000 + 0) );
    /* GetTickCount64 是开机时长，不是 Unix 秒，这里用系统时间 */
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    unsigned long long unix_s = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ULL - 11644473600ULL;
    _snprintf(ts, sizeof(ts), "%llu", unix_s);

    char sign_input[128];
    _snprintf(sign_input, sizeof(sign_input), "%s%s", HNTV_SALT, ts);
    char sign[65];
    sha256_hex(sign_input, sign);

    char extra[512];
    _snprintf(extra, sizeof(extra),
              "timestamp: %s\r\nsign: %s\r\nclientId: %s", ts, sign, client);

    char url[256];
    _snprintf(url, sizeof(url), "https://xy-gw.hntv.tv/program/getAuth/live/class/%s", cls);

    char *body = NULL;
    if (!http_get(url, &body, "okhttp/4.9.0", NULL, extra)) return 0;

    int found = 0;
    /* 从 "streams":["<url>"] 里取第一个 http(s) 地址 */
    const char *s = strstr(body, "\"streams\":[");
    if (s) {
        s = strchr(s, '[');
        if (s) {
            s = strchr(s + 1, '"');
            if (s) {
                s++;
                const char *e = strchr(s, '"');
                if (e && (size_t)(e - s) < cap) {
                    int n = (int)(e - s);
                    memcpy(out, s, (size_t)n);
                    out[n] = '\0';
                    /* 安卓版把 \/ 还原成 / */
                    for (int i = 0; i < n - 1; i++) {
                        if (out[i] == '\\' && out[i+1] == '/') { out[i] = '/'; memmove(out+i+1, out+i+2, (size_t)(n-i)); n--; }
                    }
                    found = 1;
                }
            }
        }
    }
    free(body);
    return found;
}

/* 页面源：抓 HTML 取其中第一个 .m3u8 */
static int resolve_page(const char *raw, char *out, int cap)
{
    const char *page = raw + strlen("page://");
    char *body = NULL;
    if (!http_get(page, &body, DEFAULT_UA, NULL, NULL)) return 0;

    int found = 0;
    const char *p = body;
    while ((p = strstr(p, "http")) != NULL) {
        const char *end = p;
        while (*end && *end != '"' && *end != '\'' && *end != '\\' &&
               *end != '<' && *end != '>' && *end != ' ' && *end != '\t' &&
               *end != '\n' && *end != '\r') end++;
        size_t len = (size_t)(end - p);
        const char *m = strstr(p, ".m3u8");
        if (m && m < end && len < (size_t)cap) {
            memcpy(out, p, len);
            out[len] = '\0';
            found = 1;
            break;
        }
        p = end;
    }
    free(body);
    return found;
}

/* juyun：接口返回 AES 密文，解密后才是真地址 */
static int resolve_juyun(const char *raw, char *out, int cap)
{
    char id[64];
    if (!scheme_key(raw, "juyun://", id, sizeof(id))) return 0;
    char url[256];
    _snprintf(url, sizeof(url),
              "https://api.juyun.tv/juxian/api/is_pay_tv.jsp?id=%s&callback=cb", id);

    char *body = NULL;
    if (!http_get(url, &body, DEFAULT_UA, "https://kanxianyang.com/", NULL)) return 0;

    int found = 0;
    const char *s = strstr(body, "\"address\":\"");
    if (s) {
        s += strlen("\"address\":\"");
        const char *e = strchr(s, '"');
        if (e && (size_t)(e - s) < 1024) {
            char enc[1024];
            size_t n = (size_t)(e - s);
            memcpy(enc, s, n);
            enc[n] = '\0';
            char dec[1024];
            if (juyun_decrypt(enc, dec, sizeof(dec)) && dec[0] == 'h') {
                if (dec[0] == '/' && dec[1] == '/') {
                    _snprintf(out, cap, "https:%s", dec);
                } else {
                    _snprintf(out, cap, "%s", dec);
                }
                found = 1;
            }
        }
    }
    free(body);
    return found;
}

/* 统一入口：把配置里的 primary_url 解析成可播放的真实地址 */
int resolve_url(const Channel *ch, char *out, int cap)
{
    const char *raw = ch->primary_url;
    int ok = 0;

    if (_strnicmp(raw, "litenews://", 11) == 0)      ok = resolve_litenews(raw, out, cap);
    else if (_strnicmp(raw, "hntv://", 7) == 0)      ok = resolve_hntv(raw, out, cap);
    else if (_strnicmp(raw, "page://", 7) == 0)      ok = resolve_page(raw, out, cap);
    else if (_strnicmp(raw, "juyun://", 8) == 0)     ok = resolve_juyun(raw, out, cap);
    else if (_strnicmp(raw, "http://", 7) == 0 ||
             _strnicmp(raw, "https://", 8) == 0) {
        /* 本来就是直链，不需要解析 —— 别走进下面的「解析失败」分支打误导日志 */
        _snprintf(out, cap, "%s", raw);
        return 1;
    }

    if (ok && out[0] == 'h') {
        log_event("解析", "%s -> 实时地址已获取", ch->name);
        return 1;
    }

    /* 解析失败：回落到原文（非 http 的 scheme 回落时其实不可播，
     * 但交给自愈状态机继续等，总比直接退出强 —— 与安卓版同策略） */
    _snprintf(out, cap, "%s", raw);
    log_event("解析", "%s 动态源解析失败，暂用原地址重试", ch->name);
    return 0;
}