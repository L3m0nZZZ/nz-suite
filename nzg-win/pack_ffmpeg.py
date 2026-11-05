"""把 FFmpeg 的 .o 重新打包成「扁平化命名」的静态库。

为什么必须扁平化：
    ar 只按 basename 建成员表。libavcodec/x86/mpegvideo.o 会把
    libavcodec/mpegvideo.o 覆盖掉（ar r 见到同名就替换），
    结果 mpegvideo 的符号全丢 —— 表现为链接时一片
    undefined reference to ff_init_block_index / ff_mpv_common_init ...。
    扁平化成 avcodec_x86_mpegvideo.o / avcodec_mpegvideo.o 就互不干扰。

为什么不用 make 自带规则：
    1) ARFLAGS=rcD 里的 D 需要 binutils 的确定性归档辅助程序，2.47 版不带
    2) 同上，make 的原生规则本身就有覆盖 bug

为什么用 Python 而不是 bash 循环：
    2000 个 cp 在 Git Bash 下要 spawn 2000 个进程（Windows 上很慢），
    Python 单进程 copyfile 几秒就完事。

用法：
    python pack_ffmpeg.py            # 打包全部 5 个库
    python pack_ffmpeg.py libavcodec # 只打包指定的库
"""
import glob
import os
import shutil
import subprocess
import sys

BUILD = r"D:\ningz\nzg-win\_build"
FF = os.path.join(BUILD, "ffmpeg-6.1.2")
LIB = os.path.join(BUILD, "ffmpeg_install", "lib")
STAGE = os.path.join(BUILD, "_stage")
AR = os.path.join(BUILD, "mw", "mingw32", "bin", "ar.exe")
RANLIB = os.path.join(BUILD, "mw", "mingw32", "bin", "ranlib.exe")

LIBS = ["libavutil", "libavcodec", "libavformat", "libswresample", "libswscale"]

# 每条 ar 命令最多带多少个成员：Windows 命令行上限 32KB，
# 扁平化名字平均 30 字符，400 个 ≈ 12KB，留足余量。
BATCH = 400


def pack(lib):
    src = os.path.join(FF, lib)
    stage = os.path.join(STAGE, lib)
    os.makedirs(stage, exist_ok=True)

    objs = glob.glob(os.path.join(src, "**", "*.o"), recursive=True)
    if not objs:
        print("  %-14s 没有 .o，跳过" % lib)
        return False

    flat_names = []
    for o in objs:
        rel = os.path.relpath(o, src).replace("\\", "/")
        # libavcodec/x86/mpegvideo.o -> avcodec_x86_mpegvideo.o
        flat = lib.replace("lib", "", 1) + "_" + rel.replace("/", "_")
        dst = os.path.join(stage, flat)
        shutil.copyfile(o, dst)
        flat_names.append(flat)

    flat_names.sort()
    dup = len(flat_names) - len(set(flat_names))
    if dup:
        print("  %s 有 %d 个重名（不应该发生）" % (lib, dup))
        return False

    out_tmp = os.path.join(LIB, lib + ".a.tmp")
    if os.path.exists(out_tmp):
        os.remove(out_tmp)

    # 先写 .tmp 再改名：打包失败不会毁掉已有的 .a
    for i in range(0, len(flat_names), BATCH):
        chunk = flat_names[i:i + BATCH]
        subprocess.check_call([AR, "q", out_tmp] + chunk, cwd=stage)
    subprocess.check_call([RANLIB, out_tmp])

    final = os.path.join(LIB, lib + ".a")
    os.replace(out_tmp, final)

    size_kb = os.path.getsize(final) // 1024
    print("  %-14s %4d 个目标 -> %6d KB" % (lib, len(flat_names), size_kb))
    return True


def main():
    want = sys.argv[1:] or LIBS
    print("==> 重新打包 FFmpeg 静态库（扁平化命名，避免 ar 同名覆盖）")
    for lib in want:
        pack(lib)
    print("PACK_OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
