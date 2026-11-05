/* 基础工具：日志、编码转换、计时、路径 */
#include "nzgwin.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <shlobj.h>
#include <time.h>

AppState g_app;

/* ---------------- 日志 ----------------
 * 与安卓版一致：事件日志落盘 exe 同目录 logs/，按天分文件，保留 7 天。
 * 现场排障靠它，这里保证即使解析失败也有痕迹。
 */

static CRITICAL_SECTION g_log_lock;
static int g_log_init = 0;
static char g_log_dir[MAX_PATH];

static void log_ensure_init(void)
{
    if (g_log_init) return;
    InitializeCriticalSection(&g_log_lock);
    g_log_init = 1;
}

int exe_dir(char *out, int cap)
{
    DWORD n = GetModuleFileNameA(NULL, out, cap);
    if (n == 0 || n >= (DWORD)cap) return 0;
    char *slash = strrchr(out, '\\');
    if (slash) *slash = '\0'; else out[0] = '\0';
    return 1;
}

int ensure_app_dir(char *out, int cap)
{
    char dir[MAX_PATH];
    if (!exe_dir(dir, sizeof(dir))) return 0;
    _snprintf(out, cap, "%s\\logs", dir);
    CreateDirectoryA(out, NULL);
    return 1;
}

/* 把本机时区时间格式化成 yyyy-mm-dd hh:mm:ss */
static void now_text(char *buf, int cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static void today_text(char *buf, int cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf(buf, cap, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
}

/* 清理 7 天前的日志文件 */
static void log_cleanup_old(void)
{
    char pat[MAX_PATH];
    _snprintf(pat, sizeof(pat), "%s\\log-*.txt", g_log_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME nowft;
    SystemTimeToFileTime(&st, &nowft);
    do {
        if (strncmp(fd.cFileName, "log-", 4) == 0 && strlen(fd.cFileName) == 14) {
            /* log-yyyy-mm-dd.txt */
            char d[16];
            memcpy(d, fd.cFileName + 4, 10); d[10] = '\0';
            int y, m, dd;
            if (sscanf(d, "%d-%d-%d", &y, &m, &dd) == 3 && y > 1900 && y < 2200) {
                SYSTEMTIME ts; memset(&ts, 0, sizeof(ts));
                ts.wYear = y; ts.wMonth = m; ts.wDay = dd;
                FILETIME ft;
                if (SystemTimeToFileTime(&ts, &ft)) {
                    /* ft 精度为 100ns，(nowft - ft) / 864000000000 = 相差天数 */
                    ULARGE_INTEGER a, b;
                    a.LowPart = nowft.dwLowDateTime; a.HighPart = nowft.dwHighDateTime;
                    b.LowPart = ft.dwLowDateTime;    b.HighPart = ft.dwHighDateTime;
                    if (a.QuadPart > b.QuadPart &&
                        (a.QuadPart - b.QuadPart) / 864000000000ULL > 7) {
                        char fp[MAX_PATH];
                        _snprintf(fp, sizeof(fp), "%s\\%s", g_log_dir, fd.cFileName);
                        DeleteFileA(fp);
                    }
                }
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void log_event(const char *category, const char *fmt, ...)
{
    log_ensure_init();
    if (!g_log_dir[0]) {
        if (!ensure_app_dir(g_log_dir, sizeof(g_log_dir)))
            _snprintf(g_log_dir, sizeof(g_log_dir), ".");
    }
    char ts[32];
    now_text(ts, sizeof(ts));
    char day[16];
    today_text(day, sizeof(day));

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_log_lock);
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s\\log-%s.txt", g_log_dir, day);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "[%s] [%s] %s\n", ts, category, msg);
        fclose(f);
    }
    static int cleaned = 0;
    if (!cleaned) { cleaned = 1; log_cleanup_old(); }
    LeaveCriticalSection(&g_log_lock);
}

/* ---------------- 编码转换 ----------------
 * 配置与台名全是 UTF-8，Win32 窗口 API 要 UTF-16。
 */

char *utf8_to_wide_or_die(const char *s)
{
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) n = 1;
    WCHAR *w = (WCHAR *)malloc(n * sizeof(WCHAR));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); return NULL; }
    return (char *)w;   /* 调用方按宽串用完 free */
}

void wide_to_utf8_or_die(char *dst, int cap, const WCHAR *src)
{
    if (!dst || cap <= 0) return;
    dst[0] = '\0';
    if (!src) return;
    /* 注意：缓冲区不够时 WideCharToMultiByte 返回 0 且不保证写 NUL。
     * 现有调用方的设备名都在几十字节、cap=192，实际不会触发；
     * 下次真要收口就在这里补一句 dst[cap-1]='\0'（要连带出包验一次）。 */
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, cap, NULL, NULL);
}

/* 系统 ANSI（简体中文 = GBK）→ UTF-8。
 *
 * 为什么需要：文件 API 走的是 A 版（fopen / CreateDirectoryA / GetModuleFileNameA），
 * 拿到的是 ANSI 字节，而日志文件是 **UTF-8**。把 ANSI 字节直接塞进 log_event，
 * 中文路径就会变成 `v1.13_69̨_20261023\ز㲥.exe` 这种乱码 —— 排障时第一眼就会
 * 误判成「路径坏了」（2026-10-23 踩过）。打印路径前必须过这一层。 */
void ansi_to_utf8(char *dst, int cap, const char *ansi)
{
    if (!dst || cap <= 0) return;
    dst[0] = '\0';
    if (!ansi || !ansi[0]) return;

    WCHAR wbuf[MAX_PATH * 2];
    int n = MultiByteToWideChar(CP_ACP, 0, ansi, -1, wbuf,
                                (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (n <= 0) {
        lstrcpynA(dst, ansi, cap);   /* 转换失败退回原样：宁可乱码也别丢信息 */
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, dst, cap, NULL, NULL);
}

/* ---------------- 计时 ----------------
 * 统一用 GetTickCount64（毫秒，64 位，WinXP 起可用，Win7 无虞）。
 * 全局单调，不受系统时间调整影响 —— 自愈逻辑必须这样，
 * 否则用户改系统时间会让看门狗误判。
 */
double now_ms(void)
{
    return (double)GetTickCount64();
}

/* Unix 秒（墙上时钟）。
 *
 * ⚠️ 别拿 now_ms() 去和 HLS 分片名里的 unix 时间戳比 ——
 * now_ms() 是 GetTickCount64，单调、不受系统时间调整影响（自愈逻辑要的就是这个），
 * 原点是开机时刻，跟 unix 时间戳毫无关系。
 * 这里单独给一个真实墙上时钟；1601→1970 的偏移是 11644473600 秒。
 */
long long unix_now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t =
        ((unsigned long long)ft.dwHighDateTime << 32) | (unsigned long long)ft.dwLowDateTime;
    return (long long)(t / 10000000ULL) - 11644473600LL;
}
