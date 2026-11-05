/* Win32 UI 层
 *
 * 形态：小窗口（默认 480×360），现代深色 —— 用户明确要求「无白底白字输入框」，
 *所以配色是纯深色底 + 高对比文字，不是系统默认灰白控件。
 *
 * 全部 GDI 自绘（无 comctl32 主题依赖），这样：
 *   - Win7 上不会出现 Win10 风格的控件外观差异
 *   - 体积为零额外依赖
 *
 * 交互：
 *   左键点击     呼出/收起频道侧边栏
 *   ↑ ↓ / PgUp PgDn / 数字键  选台
 *   ← → / -+  音量
 *   M  静音    F  全屏    L  日志    Esc  关侧边栏    Q / Alt+F4  退出
 */
#include "nzgwin.h"
#include <commctrl.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* 看门狗周期与安卓版一致：5 秒一跳 */
#define WATCHDOG_PERIOD_MS 5000

/* 自定义消息：播放线程用它通知主线程刷新界面 */
#define WM_TICK (WM_APP + 1)

#define WIN_MIN_W 360
#define WIN_MIN_H 260
#define WIN_DEF_W 480
#define WIN_DEF_H 360
#define PANEL_W   300

/* 深色配色（RGB） */
#define CLR_BG        RGB(18, 20, 26)
#define CLR_CARD      RGB(28, 31, 40)
#define CLR_LINE      RGB(52, 57, 70)
#define CLR_TEXT      RGB(238, 240, 245)
#define CLR_SUB       RGB(150, 157, 172)
#define CLR_ACCENT    RGB(88, 166, 255)
#define CLR_OK        RGB(88, 214, 141)
#define CLR_WARN      RGB(240, 180, 41)
#define CLR_ERR       RGB(240, 92, 92)

static HWND      g_hwnd = NULL;
static HFONT     g_font_ttl = NULL, g_font_sub = NULL, g_font_item = NULL,
                 g_font_badge = NULL, g_font_small = NULL;
static HDC       g_dc = NULL;
static PAINTSTRUCT g_ps;
static int       g_fullscreen = 0;
static WINDOWPLACEMENT g_prev_placement;
static UINT      g_timer_id = 1;

/* ---------------- 字体 ---------------- */

static HFONT make_font(const char *name, int px, int weight, BOOL italic)
{
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;                       /* 负值 = 字符高度更直观 */
    lf.lfWeight = weight;
    lf.lfItalic = italic;
    /* 优先微软雅黑（Win7 起全中文系统都有），退化到黑体，最后系统默认 */
    lf.lfCharSet = GB2312_CHARSET;
    /* lfFaceName 是 UTF-16，字体名本身是 ASCII，逐字节展宽即可 */
    for (int i = 0; i < LF_FACESIZE - 1 && name[i]; i++)
        lf.lfFaceName[i] = (WCHAR)(unsigned char)name[i];
    HFONT f = CreateFontIndirectW(&lf);
    if (!f) return NULL;
    /* 校验字体是否真的存在；不存在则回退 */
    HFONT old = (HFONT)SelectObject(GetDC(g_hwnd), f);
    TEXTMETRIC tm;
    GetTextMetrics(GetDC(g_hwnd), &tm);
    SelectObject(GetDC(g_hwnd), old);
    if (tm.tmHeight <= 0) { DeleteObject(f); f = NULL; }
    return f;
}

static void ensure_fonts(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    /* px 是逻辑像素，按 96dpi 折算成点 */
    g_font_ttl   = make_font("Microsoft YaHei UI", 24, FW_BOLD, FALSE);
    g_font_sub   = make_font("Microsoft YaHei UI", 14, FW_NORMAL, FALSE);
    g_font_item  = make_font("Microsoft YaHei UI", 14, FW_NORMAL, FALSE);
    g_font_badge = make_font("Microsoft YaHei UI", 12, FW_BOLD, FALSE);
    g_font_small = make_font("Microsoft YaHei UI", 11, FW_NORMAL, FALSE);
    HDC dc = GetDC(g_hwnd);
    if (!g_font_ttl)   g_font_ttl   = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!g_font_sub)   g_font_sub   = g_font_ttl;
    if (!g_font_item)  g_font_item  = g_font_sub;
    if (!g_font_badge) g_font_badge = g_font_sub;
    if (!g_font_small) g_font_small = g_font_sub;
    ReleaseDC(g_hwnd, dc);
}

static void del_fonts(void)
{
    if (g_font_ttl)   DeleteObject(g_font_ttl);
    if (g_font_sub)   DeleteObject(g_font_sub);
    if (g_font_item)  DeleteObject(g_font_item);
    if (g_font_badge) DeleteObject(g_font_badge);
    if (g_font_small) DeleteObject(g_font_small);
}

/* ---------------- 绘制原语 ---------------- */

static void fill_rect(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void set_font(HDC dc, HFONT f)
{
    SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
}

static void set_text_color(HDC dc, COLORREF c)
{
    SetTextColor(dc, c);
}

/* 状态灯 */
static void draw_dot(HDC dc, int cx, int cy, int r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

/* 进度条（音量） */
static void draw_bar(HDC dc, int x, int y, int w, int h, double pct, COLORREF fg)
{
    fill_rect(dc, x, y, w, h, CLR_LINE);
    int fw = (int)(w * pct);
    if (fw > 0) fill_rect(dc, x, y, fw, h, fg);
}

/* ---------------- 状态文案 ---------------- */

static COLORREF mode_color(void)
{
    switch (g_app.mode) {
        case MODE_PLAYING:        return CLR_OK;
        case MODE_RETRYING:       return CLR_WARN;
        case MODE_WAITING_NETWORK: return CLR_ERR;
    }
    return CLR_SUB;
}

static const char *mode_text(void)
{
    switch (g_app.mode) {
        case MODE_PLAYING:
            return g_app.muted ? "播放中 · 已静音" : "播放中";
        case MODE_RETRYING: {
            if (g_app.next_retry_at > now_ms()) {
                static char buf[64];
                int s = (int)((g_app.next_retry_at - now_ms()) / 1000.0 + 0.5);
                _snprintf(buf, sizeof(buf), "重连中 · %d 秒后第 %d 次尝试", s, g_app.failures);
                return buf;
            }
            return "重连中";
        }
        case MODE_WAITING_NETWORK:
            return "等待网络 · 恢复后自动重连";
    }
    return "";
}

static const Channel *cur_ch(void)
{
    if (g_app.cur_index < 0 || g_app.cur_index >= g_app.cfg.channel_count) return NULL;
    return &g_app.cfg.channels[g_app.cur_index];
}

/* UTF-8 → 宽串，栈上缓冲，避免到处 malloc */
static const WCHAR *utf8_w(const char *s)
{
    static WCHAR buf[512];
    buf[0] = 0;
    if (!s) return buf;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 512);
    return buf;
}

/* ---------------- 主面板 ---------------- */

static void draw_main(HDC dc, int W, int H)
{
    /* 背景 */
    fill_rect(dc, 0, 0, W, H, CLR_BG);

    /* 顶部：状态灯 + 台名 */
    const Channel *ch = cur_ch();
    int pad = 22;

    draw_dot(dc, pad + 6, 34, 6, mode_color());

    set_font(dc, g_font_badge);
    set_text_color(dc, mode_color());
    RECT r = { pad + 22, 20, W - pad, 50 };
    DrawTextW(dc, (LPCWSTR)utf8_w(mode_text()), -1, &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* 右上角：地区标签 + 版本 */
    if (ch) {
        set_font(dc, g_font_small);
        set_text_color(dc, CLR_SUB);
        char loc[MAX_CATEGORY + MAX_REGION + 8];
        if (ch->region[0] && ch->category[0])
            _snprintf(loc, sizeof(loc), "%s · %s", ch->category, ch->region);
        else if (ch->category[0]) _snprintf(loc, sizeof(loc), "%s", ch->category);
        else _snprintf(loc, sizeof(loc), "%s", "");
        WCHAR wlocc[256];
        MultiByteToWideChar(CP_UTF8, 0, loc, -1, wlocc, 256);
        RECT rl = { W - pad - 200, 20, W - pad, 46 };
        DrawTextW(dc, wlocc, -1, &rl, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    /* 台名大字 */
    set_font(dc, g_font_ttl);
    set_text_color(dc, CLR_TEXT);
    RECT rn = { pad, 58, W - pad, 58 + 56 };
    if (ch) {
        WCHAR wname[MAX_NAME * 2];
        MultiByteToWideChar(CP_UTF8, 0, ch->name, -1, wname, MAX_NAME * 2);
        DrawTextW(dc, wname, -1, &rn,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    /* 分隔线 */
    fill_rect(dc, pad, 122, W - pad * 2, 1, CLR_LINE);

    /* 音量条 */
    set_font(dc, g_font_small);
    set_text_color(dc, CLR_SUB);
    char voltxt[64];
    _snprintf(voltxt, sizeof(voltxt), "音量 %d%%", g_app.muted ? 0 : g_app.volume);
    WCHAR wvol[64];
    MultiByteToWideChar(CP_UTF8, 0, voltxt, -1, wvol, 64);
    RECT rv = { pad, 134, W - pad, 152 };
    DrawTextW(dc, wvol, -1, &rv, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    int barw = W - pad * 2;
    draw_bar(dc, pad, 158, barw, 6, (g_app.muted ? 0 : g_app.volume) / 100.0, CLR_ACCENT);

    /* 统计条：今日断线 / 卡死 */
    set_font(dc, g_font_small);
    set_text_color(dc, CLR_SUB);
    char stat[160];
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf(stat, sizeof(stat), "今日断线 %d 次 · 卡死 %d 次 · 第 %d / %d 台",
              g_app.stat_disconnects, g_app.stat_stalls,
              g_app.cur_index + 1, g_app.cfg.channel_count);
    WCHAR wstat[320];
    MultiByteToWideChar(CP_UTF8, 0, stat, -1, wstat, 320);
    RECT rs = { pad, 172, W - pad, 192 };
    DrawTextW(dc, wstat, -1, &rs, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* 底部提示 */
    fill_rect(dc, 0, H - 26, W, 1, CLR_LINE);
    set_font(dc, g_font_small);
    set_text_color(dc, CLR_SUB);
    const char *hint = "点击或按 L 打开频道列表　↑↓ 选台　←→ 音量　M 静音　F 全屏";
    WCHAR whint[256];
    MultiByteToWideChar(CP_UTF8, 0, hint, -1, whint, 256);
    RECT rh = { pad, H - 22, W - pad, H - 4 };
    DrawTextW(dc, whint, -1, &rh, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* ---------------- 侧边栏（频道列表） ---------------- */

/* 按省份分组，二级再按地区平铺 —— 与安卓版 UI 规范一致 */
static int build_group_list(char groups[][MAX_CATEGORY], int *group_cnt_max)
{
    /* 只列省份顺序，不重复 */
    static char seen[MAX_CATEGORY][MAX_CATEGORY];
    int n = 0;
    for (int i = 0; i < g_app.cfg.channel_count; i++) {
        const char *c = g_app.cfg.channels[i].category;
        if (!c[0]) continue;
        int found = 0;
        for (int k = 0; k < n; k++) if (strcmp(seen[k], c) == 0) { found = 1; break; }
        if (!found && n < 64) {
            _snprintf(seen[n], MAX_CATEGORY, "%s", c);
            _snprintf(groups[n], MAX_CATEGORY, "%s", c);
            n++;
        }
    }
    if (n > *group_cnt_max) n = *group_cnt_max;
    *group_cnt_max = n;
    return n;
}

static void draw_panel(HDC dc, int W, int H)
{
    if (!g_app.panel_open) return;
    static char groups[64][MAX_CATEGORY];
    int gmax = 64;
    int ng = build_group_list(groups, &gmax);

    /* 半透明遮罩：用多层 alpha 模拟（不用 AlphaBlend，省依赖） */
    for (int i = 0; i < 3; i++)
        fill_rect(dc, 0, 0, W, H, RGB(14 + i, 16, 21));

    int px = 0, pw = PANEL_W < W - 40 ? PANEL_W : W - 40;
    int ph = H;
    fill_rect(dc, px, 0, pw, ph, CLR_CARD);

    /* 右侧竖分隔线 */
    fill_rect(dc, pw, 0, 1, ph, CLR_LINE);

    /* 面板标题 */
    set_font(dc, g_font_badge);
    set_text_color(dc, CLR_SUB);
    WCHAR wtit[128];
    MultiByteToWideChar(CP_UTF8, 0, "频道列表", -1, wtit, 128);
    RECT rt = { 18, 12, pw - 18, 36 };
    DrawTextW(dc, wtit, -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* 列表区（可滚动） */
    int list_y = 44;
    int list_h = ph - list_y - 8;
    int row_h = 34;
    int visible = list_h / row_h;
    if (visible < 1) visible = 1;

    int total_rows = g_app.cfg.channel_count + ng;   /* 分组标题也占行 */
    int max_scroll = total_rows > visible ? total_rows - visible : 0;
    if (g_app.panel_scroll > max_scroll) g_app.panel_scroll = max_scroll;
    if (g_app.panel_scroll < 0) g_app.panel_scroll = 0;

    int first = g_app.panel_scroll;
    int y = list_y + 8 - (int)(g_app.panel_scroll * (double)row_h);
    /* y 为绝对坐标，随滚动平移；用 cliprect 裁掉越界部分 */
    int saved = SaveDC(dc);
    IntersectClipRect(dc, 0, list_y, pw, list_y + visible * row_h);

    for (int i = 0; i < g_app.cfg.channel_count; i++) {
        const Channel *ch = &g_app.cfg.channels[i];

        /* 省份分组标题 */
        if (ch->category[0]) {
            int firstOfGroup = 1;
            for (int k = 0; k < i; k++)
                if (strcmp(g_app.cfg.channels[k].category, ch->category) == 0) { firstOfGroup = 0; break; }
            if (firstOfGroup) {
                set_font(dc, g_font_small);
                set_text_color(dc, CLR_ACCENT);
                WCHAR wc[MAX_CATEGORY * 2];
                MultiByteToWideChar(CP_UTF8, 0, ch->category, -1, wc, MAX_CATEGORY * 2);
                RECT rg = { 18, y, pw - 18, y + 20 };
                DrawTextW(dc, wc, -1, &rg, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                y += 20;
            }
        }

        int selected = (i == g_app.cur_index);
        int hovered  = (g_app.panel_hover == i);

        if (selected) fill_rect(dc, 0, y, pw, row_h, RGB(38, 52, 72));
        else if (hovered) fill_rect(dc, 0, y, pw, row_h, RGB(34, 38, 48));

        if (selected) fill_rect(dc, 0, y, 3, row_h, CLR_ACCENT);

        set_font(dc, selected ? g_font_badge : g_font_item);
        set_text_color(dc, selected ? CLR_TEXT : CLR_SUB);
        WCHAR wname[MAX_NAME * 2];
        MultiByteToWideChar(CP_UTF8, 0, ch->name, -1, wname, MAX_NAME * 2);
        RECT rn = { 16, y, pw - 46, y + row_h };
        DrawTextW(dc, wname, -1, &rn,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* 右侧地区小字 */
        if (ch->region[0]) {
            set_font(dc, g_font_small);
            set_text_color(dc, RGB(110, 116, 130));
            WCHAR wreg[MAX_REGION * 2];
            MultiByteToWideChar(CP_UTF8, 0, ch->region, -1, wreg, MAX_REGION * 2);
            RECT rr = { pw - 130, y, pw - 12, y + row_h };
            DrawTextW(dc, wreg, -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        y += row_h;
    }

    RestoreDC(dc, saved);

    /* 滚动条（仅在需要时显示） */
    if (max_scroll > 0) {
        int sbx = pw - 6;
        fill_rect(dc, sbx, list_y, 3, list_h, RGB(44, 48, 60));
        double ratio = (double)(first) / (double)max_scroll;
        int th = list_h * visible / total_rows;
        if (th < 20) th = 20;
        int ty = list_y + (int)((list_h - th) * ratio);
        fill_rect(dc, sbx, ty, 3, th, CLR_ACCENT);
    }
}

/* ---------------- 绘制入口 ---------------- */

static void on_paint(void)
{
    g_dc = BeginPaint(g_hwnd, &g_ps);
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    draw_main(g_dc, W, H);
    draw_panel(g_dc, W, H);

    EndPaint(g_hwnd, &g_ps);
}

void ui_refresh(void)
{
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
    tray_sync();   /* tooltip 跟随 台名/状态/静音（v1.12） */
}

/* 显示/隐藏主界面（托盘单击切换；后台↔前台） */
static void ui_show_window(void)
{
    if (!g_hwnd) return;
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
    UpdateWindow(g_hwnd);
}

static void ui_toggle_window(void)
{
    if (!g_hwnd) return;
    if (IsWindowVisible(g_hwnd))
        ShowWindow(g_hwnd, SW_HIDE);      /* 回到纯后台 */
    else
        ui_show_window();                 /* 换台需要界面 */
}

/* 托盘右键菜单：显示主界面 / 后台播放 / 退出 */
static void show_tray_menu(HWND h)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, 1, L"显示主界面");
    AppendMenuW(m, MF_STRING, 2, L"后台播放");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"退出");
    /* 经典坑：托盘菜单弹出前必须 SetForegroundWindow，否则点外面不消失 */
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, h, NULL);
    DestroyMenu(m);
    PostMessage(h, WM_NULL, 0, 0);
    if (cmd == 1)      ui_show_window();
    else if (cmd == 2) ShowWindow(h, SW_HIDE);
    else if (cmd == 3) PostMessage(h, WM_CLOSE, 0, 0);
}

/* 从播放线程投递到主线程刷新（Win32 消息队列跨线程安全） */
void ui_post_tick(void)
{
    if (g_hwnd) PostMessage(g_hwnd, WM_TICK, 0, 0);
}

/* ---------------- 全屏 ---------------- */

static void toggle_fullscreen(void)
{
    if (!g_fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        GetWindowPlacement(g_hwnd, &g_prev_placement);
        GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLong(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 1;
    } else {
        SetWindowLong(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(g_hwnd, &g_prev_placement);
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
    ui_refresh();
}

/* ---------------- 交互 ---------------- */

static void goto_relative(int delta)
{
    int n = g_app.cfg.channel_count;
    if (n <= 0) return;
    int idx = g_app.cur_index + delta;
    while (idx < 0) idx += n;
    while (idx >= n) idx -= n;
    player_switch(idx);
}

static void goto_number(int n)
{
    /* 数字键 1-9：跳到第 n 个频道（安卓版没有，这里作为桌面便利） */
    if (n < 1 || n > 9) return;
    if (n <= g_app.cfg.channel_count) player_switch(n - 1);
}

static LRESULT on_mouse(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_LBUTTONDOWN: {
            RECT rc; GetClientRect(h, &rc);
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int pw = PANEL_W < rc.right - 40 ? PANEL_W : rc.right - 40;

            if (!g_app.panel_open) { g_app.panel_open = 1; ui_refresh(); return 0; }

            /* 点侧边栏列表 → 选台 */
            int list_y = 44;
            int row_h = 34;
            int y_abs = list_y + 8 - (int)(g_app.panel_scroll * (double)row_h);
            int idx = 0;
            for (int i = 0; i < g_app.cfg.channel_count; i++) {
                const Channel *ch = &g_app.cfg.channels[i];
                if (ch->category[0]) {
                    int firstOfGroup = 1;
                    for (int k = 0; k < i; k++)
                        if (strcmp(g_app.cfg.channels[k].category, ch->category) == 0) { firstOfGroup = 0; break; }
                    if (firstOfGroup) y_abs += 20;
                }
                if (y >= y_abs && y < y_abs + row_h) {
                    if (x < pw) { idx = i; break; }
                }
                y_abs += row_h;
            }
            if (idx > 0 && x < pw) {
                player_switch(idx);
                g_app.panel_open = 0;
                ui_refresh();
            } else if (x >= pw) {
                /* 点面板外 → 关闭（与安卓版一致） */
                g_app.panel_open = 0;
                ui_refresh();
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_app.panel_open) {
                RECT rc; GetClientRect(h, &rc);
                int pw = PANEL_W < rc.right - 40 ? PANEL_W : rc.right - 40;
                int list_y = 44, row_h = 34;
                int y = GET_Y_LPARAM(lp);
                int y_abs = list_y + 8 - (int)(g_app.panel_scroll * (double)row_h);
                int hover = -1;
                for (int i = 0; i < g_app.cfg.channel_count; i++) {
                    const Channel *ch = &g_app.cfg.channels[i];
                    if (ch->category[0]) {
                        int firstOfGroup = 1;
                        for (int k = 0; k < i; k++)
                            if (strcmp(g_app.cfg.channels[k].category, ch->category) == 0) { firstOfGroup = 0; break; }
                        if (firstOfGroup) y_abs += 20;
                    }
                    if (y >= y_abs && y < y_abs + row_h) hover = i;
                    y_abs += row_h;
                }
                if (hover != g_app.panel_hover) { g_app.panel_hover = hover; ui_refresh(); }
            }
            (void)wp;
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (g_app.panel_open) {
                g_app.panel_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120 * 2;
                ui_refresh();
            }
            return 0;
        }
    }
    return DefWindowProc(h, msg, wp, lp);
}

static LRESULT on_key(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_KEYDOWN: {
            switch (wp) {
                case VK_UP:       goto_relative(-1); return 0;
                case VK_DOWN:     goto_relative(1);  return 0;
                case VK_PRIOR:    goto_relative(-5); return 0;
                case VK_NEXT:     goto_relative(5);  return 0;
                case VK_LEFT:     player_set_volume(g_app.volume - 5); return 0;
                case VK_RIGHT:    player_set_volume(g_app.volume + 5); return 0;
                case VK_SUBTRACT: player_set_volume(g_app.volume - 5); return 0;
                case VK_ADD:      player_set_volume(g_app.volume + 5); return 0;
                case 'M':         player_toggle_mute(); return 0;
                case 'F':         toggle_fullscreen(); return 0;
                case 'L':         g_app.panel_open = !g_app.panel_open; ui_refresh(); return 0;
                case 'R':         player_hard_rebuild("用户按R 手动重启"); return 0;
                case VK_ESCAPE:   g_app.panel_open = 0; ui_refresh(); return 0;
                case 'Q':         DestroyWindow(h); return 0;
                case '1': case '2': case '3': case '4': case '5':
                case '6': case '7': case '8': case '9':
                    goto_number((int)(wp - '0')); return 0;
            }
            return 0;
        }
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ---------------- 定时器：看门狗 + 音量纠偏 ---------------- */

static void on_timer(void)
{
    /* 看门狗 + 自愈状态机（严格遵守移植铁律） */
    player_tick_watchdog();
    player_force_max_volume();
    /* 60 秒一行运行态心跳（内部自带节流）——「没声音」这类报障的第一手证据 */
    player_heartbeat();
    ui_refresh();
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_PAINT:  on_paint(); return 0;
        case WM_ERASEBKGND: return 1;          /* 防止闪烁 */
        case WM_TIMER:  if (wp == g_timer_id) { on_timer(); return 0; } break;
        case WM_KEYDOWN: return on_key(h, msg, wp, lp);
        case WM_MOUSEWHEEL:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
            return on_mouse(h, msg, wp, lp);
        case WM_TICK:
            /* 播放线程死亡通知：必须真正交给自愈状态机。
             * ⚠️ 此前只 InvalidateRect（v1.6 及以前）—— 线程死了状态机
             * 不知道，配合「首帧未到不判死」形成零日志永久无声死局
             * （实机 2026-10-12 ~ 10-13 踩过，见 heal.c 同日注释）。 */
            player_tick_watchdog();
            InvalidateRect(h, NULL, FALSE);
            return 0;
        case WM_TRAYICON: {
            /* v1.12 托盘：左键单击 显示/隐藏主界面（换台入口），右键 菜单 */
            if (lp == WM_LBUTTONUP)      { ui_toggle_window(); return 0; }
            if (lp == WM_LBUTTONDBLCLK)  { ui_show_window();    return 0; }
            if (lp == WM_RBUTTONUP)      { show_tray_menu(h);   return 0; }
            break;
        }
        case WM_SHOWSELF:
            /* 第二实例（用户又双击了 exe）：把自己亮出来 */
            ui_show_window();
            return 0;
        case WM_COMMAND:
            /* 预留：托盘菜单若改走 WM_COMMAND 路径 */
            break;
        case WM_SIZE:
            /* 最小化 = 藏进托盘（纯后台定位：任务栏不留窗口，图标常驻） */
            if (wp == SIZE_MINIMIZED && !g_fullscreen) {
                ShowWindow(h, SW_HIDE);
                return 0;
            }
            InvalidateRect(h, NULL, FALSE);
            return 0;
        case WM_CLOSE:
            player_stop();
            config_save_state();
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            KillTimer(h, g_timer_id);
            tray_remove();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

void ui_init(HINSTANCE hInst)
{
    ensure_fonts();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"ShouboRadioWin";
    if (!RegisterClassExW(&wc)) {
        log_event("异常", "RegisterClassEx 失败（%lu）", GetLastError());
        return;
    }

    int ww = WIN_DEF_W, wh = WIN_DEF_H;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw > 0 && ww > sw - 40) ww = sw - 40;
    if (sh > 0 && wh > sh - 80) wh = sh - 80;

    g_hwnd = CreateWindowExW(0, L"ShouboRadioWin", L"守播广播",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, ww, wh,
                             NULL, NULL, hInst, NULL);
    if (!g_hwnd) {
        log_event("异常", "CreateWindowEx 失败（%lu）", GetLastError());
        return;
    }
    /* v1.12：启动即纯后台 —— 窗口默认隐藏，托盘图标驻留；
     * --show 参数（或单击托盘图标）才显示主界面。
     * 托盘**无条件安装**：--show 实例也要能回后台，
     * 否则窗口一藏就再也找不回来了。 */
    tray_init(g_hwnd);
    if (g_opt_show) {
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        UpdateWindow(g_hwnd);
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
    }
    SetTimer(g_hwnd, g_timer_id, WATCHDOG_PERIOD_MS, NULL);
}