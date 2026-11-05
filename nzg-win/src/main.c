/* 入口
 *
 * 绿色单exe 的启动序列：
 *   1. 初始化日志（后续任何异常都要能留痕）
 *   2. 加载配置：外部 config.json > 内置清单
 *   3. 读回运行态（上次停在哪个台；音量默认回 startup_volume，不记忆）
 *   4. 设启动音量 + 系统音量顶满纠偏
 *   5. 拉起播放线程
 *   6. 进消息循环，看门狗由定时器驱动
 */
#include "nzgwin.h"
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

/* 单实例：已有实例在跑就不再启动第二个，避免两个进程抢同一个 waveOut 设备 */
static int acquire_single_instance(void)
{
    HANDLE h = CreateMutexA(NULL, TRUE, "Global\\ShouboRadioNZGWin_Single");
    if (!h) return 1;                    /* 建不了就当没有，继续跑 */
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* 已有实例：请它把自己亮出来（v1.12 托盘模式下窗口是隐藏的，
         * 用户双击 exe 就是想看界面 —— 走消息让第一实例自己处理） */
        HWND other = FindWindowA("ShouboRadioWin", NULL);
        if (other) PostMessage(other, WM_SHOWSELF, 0, 0);
        CloseHandle(h);
        return 0;
    }
    /* 故意不 CloseHandle：句柄存活期间锁一直持有 */
    return 1;
}

/* 崩溃捕获：把异常码与故障地址写进日志。
 *
 * 现场调试用（用 nm/addr2line 能把地址对回具体函数），
 * 交付后也有用 —— 用户报「闪退」时 logs 里至少有东西可查，
 * 而不是只剩一句「打不开」。
 */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord) {
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        /* 判断故障地址落在哪个模块里，区分「我们的代码」与「系统 dll」 */
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "?";
        char self[MAX_PATH] = "";
        GetModuleFileNameA(NULL, self, sizeof(self));
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &mod) && mod) {
            GetModuleFileNameA(mod, modname, sizeof(modname));
        }
        log_event("崩溃", "异常 0x%08lX 于地址 %p（模块 %s）",
                  (unsigned long)ep->ExceptionRecord->ExceptionCode, addr, modname);
        log_event("崩溃", "运行态：mode=%d failures=%d cur_index=%d 播放中=%d",
                  (int)g_app.mode, g_app.failures, g_app.cur_index,
                  player_is_playing());

        /* 转储栈顶 96 个双字：地址被破坏时，靠这个能把调用链还原出来
         * （用带符号的构建 + nm/addr2line 反查） */
        if (ep->ContextRecord) {
            DWORD *sp = (DWORD *)ep->ContextRecord->Esp;
            if (sp) {
                char line[256];
                for (int i = 0; i < 96; i += 6) {
                    int n = 0;
                    line[0] = '\0';
                    for (int k = 0; k < 6 && i + k < 96; k++) {
                        n += _snprintf(line + n, sizeof(line) - n, "%08lX ", (unsigned long)sp[i + k]);
                        if (n >= (int)sizeof(line) - 12) break;
                    }
                    log_event("崩溃栈", "[esp+%03X] %s", i * 4, line);
                }
                log_event("崩溃", "eip=%08lX esp=%08lX ebp=%08lX",
                          (unsigned long)ep->ContextRecord->Eip,
                          (unsigned long)ep->ContextRecord->Esp,
                          (unsigned long)ep->ContextRecord->Ebp);
            }
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;   /* 写完日志就结束进程，不进调试器 */
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nCmdShow)
{
    (void)hPrev; (void)cmdLine; (void)nCmdShow;

    SetUnhandledExceptionFilter(crash_filter);

    /* 控制台程序会闪黑窗，Win7 上尤其明显 —— 强制 GUI 子系统 */
    log_event("启动", "守播广播 Windows 版 v%s 启动", NZG_WIN_VERSION);

    /* 崩溃捕获：把异常码与故障地址写进日志。
     * 现场调试用（可用 nm / addr2line 把地址对回函数），
     * 交付后也有用 —— 用户报「闪退」时 logs 里至少有东西可查。 */

    /* Win7 高 DPI 感知：需要以 manifest 声明，这里用动态方式兜底 */
    /* （Shim 不支持就直接忽略，界面只是略糊，不影响使用） */

    if (!config_load()) {
        MessageBoxA(NULL,
                    "内置频道配置解析失败，且未找到外部 config.json。\r\n"
                    "请确认 exe 文件完整未被截断。\r\n\r\n"
                    "日志见logs 目录。",
                    "守播广播 - 配置错误", MB_ICONERROR);
        log_event("启动", "无任何可用配置，退出");
        return 1;
    }

    config_load_state();

    /* 环境信息：机器是什么系统、跑的是哪个构建。
     * 都是排障刚需 —— 远程排查时经常分不清日志来自哪台机器/哪个版本的 exe
     * （踩过：v1.4 归档的是旧构建，日志格式对不上，排查被带偏）。
     * 放在自检分派**之前**：用户在目标机器上跑的就是 --selftest，
     * 报告里必须能看出「这是哪台机器、哪个构建」。 */
    {
        /* OSVERSIONINFOEXA 才带 wServicePackMajor（OSVERSIONINFOA 没有）；
         * GetVersionExA 两种 size 都收。 */
        OSVERSIONINFOEXA vi;
        ZeroMemory(&vi, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(vi);
        GetVersionExA((OSVERSIONINFOA *)&vi);
        const char *name = "Windows";
        if (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 1)      name = "Windows 7";
        else if (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 2) name = "Windows 8/10(伪装)";
        else if (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 3) name = "Windows 8.1";
        else if (vi.dwMajorVersion == 10)                          name = "Windows 10/11";
        log_event("启动", "本机系统：%s（%ld.%ld SP%u）",
                  name, vi.dwMajorVersion, vi.dwMinorVersion, vi.wServicePackMajor);

        char exepath[MAX_PATH] = "";
        if (GetModuleFileNameA(NULL, exepath, sizeof(exepath))) {
            /* A 版拿到的是 ANSI(GBK) 字节，日志是 UTF-8，必须转一层再打 */
            char exe_u8[MAX_PATH * 3];
            ansi_to_utf8(exe_u8, sizeof(exe_u8), exepath);
            log_event("启动", "程序：%s（构建于 %s %s）", exe_u8, __DATE__, __TIME__);
        }
    }

    /* 启动音量与静音状态必须显式记一行（2026-11-03 加）。
     * 静音是**跨重启记忆**的（config.c 里从 state.json 读 muted，且不受
     * remember_volume 控制），而且恢复时原来完全不记日志 —— 一旦它置位，
     * 表现就是「程序一切正常、日志只有 FFmpeg 警告、就是没声音」，
     * 事后无从判断。这行是那一类报障最快的判据。 */
    log_event("启动", "音量 %d%%（%s），静音：%s",
              g_app.volume,
              g_app.cfg.audio.remember_volume ? "记忆上次" : "取配置 startup_volume",
              g_app.muted ? "是 —— 若无声音先按 M 解除" : "否");

    /* FFmpeg 的 WARNING/ERROR 接进日志 —— 自检和真实播放都受益。
     * 必须在第一次 avformat_open_input 之前装（见 player.c 的 ff_log_cb 注释）。 */
    fflog_install();

    /* 自检模式：不建窗口、不初始化 waveOut，逐个频道试拉流，
     * 结果写日志 + exe 同目录 selftest.txt。用法：守播广播.exe --selftest
     * 追加 --live 会顺带做 HLS 活性探测（揪出循环垫片），每台多花约 4 秒。
     * 退出码由 player_selftest 给：0=全通过 1=有跳过(多半断网) 2=有失败。 */
    if (cmdLine && strstr(cmdLine, "--selftest")) {
        g_opt_livecheck = (cmdLine && strstr(cmdLine, "--live")) ? 1 : 0;
        int code = player_selftest();
        log_event("自检", "自检模式退出（退出码 %d）", code);
        return code;
    }

    /* 单实例检查放在自检分派**之后**。
     * 踩过（2026-10-23）：部署机上程序常驻后台时跑 `--selftest`，
     * 会被已在运行的实例挡掉 —— 进程 2 秒退出、退出码 0、selftest.txt 是空的，
     * 看上去像「全部通过」，实际一台都没测。自检是离线诊断，
     * 天然应该允许与前台实例并存（它不建窗口、不占 waveOut）。 */
    if (!acquire_single_instance()) {
        log_event("启动", "已有实例在运行，本次退出");
        return 0;
    }

    /* --show：本次启动显示主界面（调试用）。开机自启/普通双击都走纯后台。 */
    if (cmdLine && strstr(cmdLine, "--show")) {
        g_opt_show = 1;
        log_event("启动", "命令行 --show：本次显示主界面");
    }

    /* v1.12 开机自启：HKCU Run 幂等写 exe 全路径（无需管理员；
     * exe 挪位置后下次启动自动跟随）。用户明确要求「失败提示」。 */
    if (!autorun_setup()) {
        log_event("启动", "开机自启未能自动配置（见上一条失败原因）");
        MessageBoxA(NULL,
                    "开机自启自动配置失败（注册表写入被拒绝）。\r\n"
                    "程序本次仍会正常播放，但重启电脑后不会自动运行。\r\n\r\n"
                    "可手动处理：把本程序快捷方式放入「启动」文件夹，\r\n"
                    "或以管理员运行一次本程序重试。",
                    "守播广播 - 开机自启配置失败", MB_ICONWARNING);
    }

    /* 启动音量：默认每次都回到 audio.startup_volume（v1.12 起 100%）。
     * 音量键（←/→）改的是同一个 g_app.volume，改完即生效。 */
    log_event("启动", "启动音量 %d%%%s，系统音量顶满=%s",
              g_app.volume,
              g_app.cfg.audio.remember_volume ? "（记住上次）" : "（每次启动固定）",
              g_app.cfg.audio.force_max ? "开" : "关");

    /* boost_mb 仍保留在配置里（与安卓版字段对齐），但**已不参与设备音量计算** ——
     * waveOutSetVolume 上限就是 100%，往设备音量上乘增益只会让 26% 以上的档位全部饱和，
     * 音量键会失效。要真正的数字增益得做在 PCM 域（乘采样 + 限幅），本次不做。
     * 这里如实记一笔，免得以后看到配置里有 boost 却听不出它没生效。 */
    log_event("启动", "数字增益 %d毫巴（上限 %d，已停用：设备音量上限 100%%，乘上去只会顶死）",
              g_app.cfg.audio.boost_mb, g_app.cfg.audio.max_boost_mb);

    ui_init(hInst);
    if (!g_app.cfg.channel_count) {
        log_event("启动", "无频道，退出");
        return 1;
    }

    player_start();

    MSG msg;
    BOOL ret;
    while ((ret = GetMessage(&msg, NULL, 0, 0)) != 0 && ret != -1) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    player_stop();
    config_save_state();
    log_event("退出", "正常退出");
    return 0;
}