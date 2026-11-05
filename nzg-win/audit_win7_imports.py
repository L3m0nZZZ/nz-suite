# -*- coding: utf-8 -*-
"""审计 PE 导入表，找出 Win7 上不存在的 API。

为什么要做：exe 是**静态导入**（不是 GetProcAddress），
只要导入表里有一个 Win7 没有的函数，**程序在 Win7 上根本起不来**
（不是「某个功能不能用」，是加载器直接报错）。
开发机是 Win11，这个问题在开发机上 100% 测不出来，只能靠审导入表。

用法： python audit_win7_imports.py _build/imports.txt
"""
import re
import sys
from collections import OrderedDict

# 明确是 Win8 / Win8.1 / Win10 才引入的 API（出现在导入表里 = Win7 起不来）
POST_WIN7 = {
    # kernel32
    "WaitOnAddress", "WakeByAddressAll", "WakeByAddressSingle",
    "GetSystemTimePreciseAsFileTime", "SetThreadDescription", "GetThreadDescription",
    "GetTempPath2W", "GetTempPath2A", "CopyFile2", "GetOverlappedResultEx",
    "SetProcessMitigationPolicy", "GetProcessInformation", "GetPackageFullName",
    "InitializeSynchronizationBarrier", "EnterSynchronizationBarrier",
    "DeleteSynchronizationBarrier", "GetPackageFamilyName", "GetCurrentPackageFullName",
    "SetThreadInformation", "GetThreadInformation", "ExpandEnvironmentStringsW2",
    "CreateThreadpoolTimer2", "GetSystemCpuSetInformation", "SetThreadSelectedCpuSets",
    "GetMachineTypeAttributes", "IsWow64Process2", "SetProcessInformation",
    "GetApplicationUserModelId", "GetPackagePathByFullName",
    # user32
    "GetPointerType", "GetPointerInfo", "GetPointerTouchInfo",
    "GetDpiForWindow", "GetDpiForSystem", "SetProcessDpiAwarenessContext",
    "EnableNonClientDpiScaling", "AdjustWindowRectExForDpi",
    "SystemParametersInfoForDpi", "GetSystemMetricsForDpi",
    "SetThreadDpiAwarenessContext", "GetThreadDpiAwarenessContext",
    "RegisterSuspendResumeNotification", "UnregisterSuspendResumeNotification",
    "LogicalToPhysicalPointForPerMonitorDPI", "PhysicalToLogicalPointForPerMonitorDPI",
    "SetProcessDpiAwareness", "GetAwarenessFromDpiAwarenessContext",
    # ole32 / others
    "RoInitialize", "RoUninitialize", "CoGetApartmentType",
    "GetFileVersionInfoExW", "GetFileVersionInfoSizeExW",
    # 虚拟 DLL / API Set（出现就说明链了 ucrt 或 apiset，Win7 不认识）
    "__stdio_common_vfprintf", "__stdio_common_vsprintf",
    "__acrt_iob_func", "ucrtbase",
    # msvcrt 里没有的（Win7 的 msvcrt.dll 只导出 _snprintf 不导出 snprintf）
    "snprintf", "vsnprintf", "strtok_s", "strcpy_s",
}

# Lib 名字里出现这些就是链错了
BAD_DLL = re.compile(r"^(api-ms-win-|ext-ms-|ucrtbase|libgcc|libwinpthread)", re.I)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "_build/imports.txt"
    text = open(path, encoding="utf-8", errors="replace").read().splitlines()

    imports = OrderedDict()
    cur = None
    mem = re.compile(r"^[0-9a-f]{8}\s+<none>\s+[0-9a-f]{4}\s+(\S+)$")
    for line in text:
        line = line.rstrip()
        m = re.match(r"^\tDLL Name:\s*(\S+)", line)
        if m:
            cur = m.group(1)
            imports.setdefault(cur, [])
            continue
        if cur is None:
            continue
        m = mem.match(line.strip())
        if m:
            imports[cur].append(m.group(1))

    total = sum(len(v) for v in imports.values())
    print("导入 DLL %d 个，函数 %d 个\n" % (len(imports), total))

    problems = []
    for dll, funcs in imports.items():
        if BAD_DLL.match(dll):
            problems.append("DLL 不对：%s（Win7 没有这个）" % dll)
        hits = sorted({f for f in funcs if f in POST_WIN7})
        print("%-16s %3d 个" % (dll, len(funcs)))
        for f in funcs:
            if f in POST_WIN7:
                print("      ⚠️  %s  ← Win8+/Win10 才有" % f)
        if hits:
            problems.append("%s: %s" % (dll, ", ".join(hits)))

    print()
    if problems:
        print("❌ 发现 %d 处 Win7 不兼容：" % len(problems))
        for p in problems:
            print("   - " + p)
    else:
        print("✅ 导入表里没有已知的 post-Win7 API")

    # 顺带把 msvcrt 的导入全列出来，便于人工复核 CRT 层
    print("\n--- msvcrt.dll 导入明细（Win7 的 msvcrt 是老版本，重点看这里）---")
    for f in sorted(set(imports.get("msvcrt.dll", []))):
        print("   " + f)


if __name__ == "__main__":
    main()
