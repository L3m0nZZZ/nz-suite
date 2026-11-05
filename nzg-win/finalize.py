"""收尾：把 exe 改成正确的中文名 + 清掉乱码残留 + 打印验收信息。

为什么需要这一步：
    Git Bash 把 UTF-8 的中文文件名交给原生程序时会走 ANSI 代码页转换，
    结果磁盘上落地的名字是「ÊØ²¥¹ã²¥.exe」这种乱码（在资源管理器里也是乱的）。
    Python 用的是宽字符 API，改名后才是真正的「守播广播.exe」。
    所以 gcc 输出统一用 ASCII 名 shoubo-radio.exe，最后在这里改名。
"""
import os
import shutil
import struct
import sys

ROOT = r"D:\ningz\nzg-win"
DIST = os.path.join(ROOT, "dist")
TMP_NAME = "shoubo-radio.exe"
FINAL_NAME = "\u5b88\u64ad\u5e7f\u64ad.exe"   # 守播广播.exe

# 历史遗留的乱码名，出包时顺手清掉
MOJIBAKE = ["\u00ca\u00d8\u00b2\u00a5\u00b9\u00e3\u00b2\u00a5.exe"]


def main():
    os.makedirs(DIST, exist_ok=True)

    tmp = os.path.join(DIST, TMP_NAME)
    final = os.path.join(DIST, FINAL_NAME)

    if not os.path.exists(tmp):
        print("找不到 %s，先跑 build.sh" % tmp, file=sys.stderr)
        return 1

    if os.path.exists(final):
        os.remove(final)
    os.replace(tmp, final)

    for bad in MOJIBAKE:
        p = os.path.join(DIST, bad)
        if os.path.exists(p):
            os.remove(p)
            print("已清理乱码残留：%s" % bad)

    size = os.path.getsize(final)
    with open(final, "rb") as f:
        data = f.read(0x400)

    # PE 头：e_lfanew 在 0x3C
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    machine = struct.unpack_from("<H", data, pe_off + 4)[0]
    arch = {0x014c: "i386 (32 位)", 0x8664: "x64", 0xAA64: "ARM64"}.get(machine, hex(machine))

    print("==> 产物：%s" % final)
    print("    体积：%d 字节（%.1f MB）" % (size, size / 1048576.0))
    print("    PE 机架：%s" % arch)
    return 0


if __name__ == "__main__":
    sys.exit(main())
