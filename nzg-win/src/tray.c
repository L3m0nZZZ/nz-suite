/* 托盘图标 + 开机自启（v1.12，2026-10-13）
 *
 * 需求（用户原话）：加一个自动配置开机自启，如果失败提示；可以纯后台播放
 * 没有窗口，在图标里显示就行；开启后自动纯后台，要换台点一下图标才显示
 * 主界面。
 *
 * 形态：
 *   - 启动即 SW_HIDE（除非 --show），窗口不出现，托盘图标驻留
 *   - tooltip 跟随 台名 / 播放状态 / 静音（tray_sync，由 ui_refresh 驱动）
 *   - 左键单击托盘 = 显示主界面（ui.c 的 WM_TRAYICON 处理），右键 = 菜单
 *
 * 自启：HKCU\Software\Microsoft\Windows\CurrentVersion\Run 幂等写入
 *       exe 全路径（每次启动覆盖一次 —— exe 挪位置后自启路径自动跟随）。
 *       HKCU 不需要管理员权限；写失败时 main.c 负责弹窗提示。
 *
 * 图标：运行时 GDI 自绘（深蓝圆底 + 白色播放三角），与主界面主题同色，
 *       不引入 .ico 资源文件链（保持 build.sh 的极简编译）。
 */
#include "nzgwin.h"
#include <shellapi.h>
#include <stdio.h>

int g_opt_show = 0;   /* --show：本次启动显示主界面 */

static NOTIFYICONDATAW g_nid;
static int    g_tray_ready = 0;
static HICON  g_tray_icon = NULL;

/* 自绘应用图标：32x32 深蓝圆底 + 白色播放三角（与 UI 的 CLR_BG/CLR_ACCENT 同色系） */
static HICON make_app_icon(void)
{
    HDC sdc = GetDC(NULL);
    if (!sdc) return NULL;
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bm = CreateCompatibleBitmap(sdc, 32, 32);
    HBITMAP obm = (HBITMAP)SelectObject(mdc, bm);
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, NULL);   /* 1bpp 掩码：全 0 = 不透明 */

    RECT r = { 0, 0, 32, 32 };
    HBRUSH bg = CreateSolidBrush(RGB(18, 20, 26));
    FillRect(mdc, &r, bg);
    DeleteObject(bg);

    HBRUSH blue = CreateSolidBrush(RGB(88, 166, 255));
    HBRUSH ob = (HBRUSH)SelectObject(mdc, blue);
    Ellipse(mdc, 1, 1, 31, 31);
    SelectObject(mdc, ob);
    DeleteObject(blue);

    POINT tri[3] = { {12, 8}, {12, 24}, {25, 16} };
    HBRUSH wh = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH ow = (HBRUSH)SelectObject(mdc, wh);
    Polygon(mdc, tri, 3);
    SelectObject(mdc, ow);
    DeleteObject(wh);

    SelectObject(mdc, obm);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);

    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    ii.fIcon    = TRUE;
    ii.hbmMask  = mask;
    ii.hbmColor = bm;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(bm);
    DeleteObject(mask);
    return ic;
}

int tray_init(HWND hwnd)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;

    if (!g_tray_icon) g_tray_icon = make_app_icon();
    g_nid.hIcon = g_tray_icon ? g_tray_icon
                              : LoadIcon(NULL, IDI_APPLICATION);  /* 自绘失败退系统图标 */

    MultiByteToWideChar(CP_UTF8, 0, "守播广播", -1, g_nid.szTip, 32);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        /* Win7 偶发 explorer 未就绪：等 1.5 秒重试一次 */
        Sleep(1500);
        if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
            log_event("异常", "托盘图标注册失败（explorer 未就绪或被策略禁用）");
            return 0;
        }
    }
    g_tray_ready = 1;
    log_event("启动", "托盘图标已就绪（纯后台模式，单击图标显示主界面）");
    return 1;
}

void tray_remove(void)
{
    if (!g_tray_ready) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_tray_ready = 0;
    if (g_tray_icon) { DestroyIcon(g_tray_icon); g_tray_icon = NULL; }
}

/* tooltip 跟随状态：台名 / 播放中·重连中·等待网络 / 静音。
 * 由 ui_refresh() 驱动（所有状态变化都会路过那里）。 */
void tray_sync(void)
{
    if (!g_tray_ready) return;

    const char *name = "";
    if (g_app.cur_index >= 0 && g_app.cur_index < g_app.cfg.channel_count)
        name = g_app.cfg.channels[g_app.cur_index].name;

    const char *st = "播放中";
    if (g_app.mode == MODE_RETRYING)         st = "重连中";
    else if (g_app.mode == MODE_WAITING_NETWORK) st = "等待网络";

    wchar_t tip[128];
    wchar_t wname[MAX_NAME];
    wchar_t wst[32];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, MAX_NAME);
    MultiByteToWideChar(CP_UTF8, 0, st, -1, wst, 32);
    if (g_app.muted)
        _snwprintf(tip, 128, L"守播广播 %hs｜%s｜%s｜已静音", NZG_WIN_VERSION, wname, wst);
    else
        _snwprintf(tip, 128, L"守播广播 %hs｜%s｜%s", NZG_WIN_VERSION, wname, wst);
    tip[127] = 0;

    NOTIFYICONDATAW nd = g_nid;
    nd.uFlags = NIF_TIP;
    lstrcpynW(nd.szTip, tip, 128);
    Shell_NotifyIconW(NIM_MODIFY, &nd);
}

/* 开机自启：HKCU Run 幂等写 exe 全路径（无需管理员）。
 * 返回 0 = 失败（调用方 main.c 必须弹窗提示，用户明确要求）。
 *
 * ⚠️ 必须走宽字符链路（2026-10-23 修）：
 *   原来用 GetModuleFileNameA + RegSetValueExA，两个后果——
 *   ① 取回的是系统 ANSI（简体中文 = GBK）字节，直接写进 UTF-8 日志就成乱码
 *      （实测日志里 `v1.13_69̨_20261023\ز㲥.exe`，排障时极易误判成路径坏了）；
 *   ② REG_SZ 里存的是 GBK 字节，换到非中文 ANSI 页的机器上路径会失效，
 *      开机自启静默不生效 —— 用户第二天开机没声，还查不出原因。
 *   宽字符 + RegSetValueExW（字节数按 WCHAR 算）才是跨语言安全的写法。 */
int autorun_setup(void)
{
    HKEY k;
    LONG r = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &k);
    if (r != ERROR_SUCCESS) {
        log_event("启动", "开机自启配置失败：打开注册表 Run 键被拒（%ld）", r);
        return 0;
    }

    WCHAR wpath[MAX_PATH] = L"";
    if (!GetModuleFileNameW(NULL, wpath, MAX_PATH)) {
        RegCloseKey(k);
        log_event("启动", "开机自启配置失败：取 exe 路径失败");
        return 0;
    }

    WCHAR wval[MAX_PATH + 4];
    _snwprintf(wval, MAX_PATH + 4, L"\"%s\"", wpath);   /* 引号包路径，防空格 */
    r = RegSetValueExW(k, L"ShouboRadioWin", 0, REG_SZ,
                       (const BYTE *)wval,
                       (DWORD)((wcslen(wval) + 1) * sizeof(WCHAR)));
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) {
        log_event("启动", "开机自启配置失败：写注册表值被拒（%ld）", r);
        return 0;
    }

    char u8[MAX_PATH * 4];
    wide_to_utf8_or_die(u8, sizeof(u8), wpath);
    log_event("启动", "开机自启已配置（HKCU Run → %s，路径跟随当前 exe）", u8);
    return 1;
}
