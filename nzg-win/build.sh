#!/usr/bin/env bash
# 守播广播 Windows 版构建脚本
#
# 产出：单个 守播广播.exe（32 位、静态链接、零外部 dll）
# 目标平台：Windows 7 SP1 及以上（Win7/8/10/11 均可用，32 位与 64 位系统都能跑）
#
# 关键点：
#  - 用 msvcrt 而非 ucrt：ucrt 需要 Win10，msvcrt 才是 Win7 通吃的
#  - -static：把libgcc/libstdc++/libwinpthread 全静态链进去，绿色无依赖
#  - 只链 Win7 就有的系统 dll：kernel32/user32/gdi32/msvcrt/winmm/winhttp/advapi32/shell32
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/_build"
SRC="$ROOT/src"
TOOLCHAIN="$BUILD/mw/mingw32/bin"
FFPREFIX="$BUILD/ffmpeg_install"
OUT="$ROOT/dist"

PY=/c/Users/zlst/.workbuddy/binaries/python/versions/3.13.12/python.exe

mkdir -p "$BUILD" "$OUT"

# ---------- 1) 生成内置配置资源 ----------
# 注意：源必须是本目录的 config_default.json（由 gen_config_from_csv.py 从
# 「本次 41 台」表格生成），不能用安卓工程的 assets/config_default.json ——
# 那个只有 29 台，会把这版覆盖回旧清单。
echo "==> 生成内置频道配置"
# python 是原生 Windows 程序，看不懂 /d/... 这种 MSYS 路径（会理解成 D:\d\...），
# 所以切到工作目录用相对路径调用。
( cd "$ROOT" && "$PY" gen_resource.py config_default.json _build/resource.c )

# ---------- 2) 编译 FFmpeg 静态库（只在缺库时编） ----------
NEED_FF=0
if [ ! -f "$FFPREFIX/lib/libavformat.a" ]; then
    NEED_FF=1
fi

if [ "$NEED_FF" = "1" ]; then
    echo "==> 编译 FFmpeg 静态库（首次较慢，约 10-20 分钟）"
    mkdir -p "$BUILD/tmp"
    cd "$BUILD/ffmpeg-6.1.2"
    export PATH="$TOOLCHAIN:$PATH"
    # MSYS 的 sh 会把 C:\... 当相对路径，configure 的 sanity test 会失败，
    # 必须把 TMPDIR 指到 MSYS 风格的 D:/... 路径。
    export TMPDIR="D:/ningz/nzg-win/_build/tmp"
    export TMP="$TMPDIR"
    export TEMP="$TMPDIR"
    # MSYS(Git Bash) 环境无法创建符号链接，FFmpeg 的 sanity test 会卡在
    # `ln -s -f` 上。ln_s 是 configure 支持覆盖的默认项，改用 cp 即可，
    # 对 Windows 静态构建完全等价。注意必须带 -r：sanity test 会拷目录。
    export ln_s="cp -rf"
    # 原生 i686 构建：不传 --target/--host（FFmpeg 6.x 在原生 MinGW 下会拒绝这两个选项）
    ./configure \
        --prefix="D:/ningz/nzg-win/_build/ffmpeg_install" \
        --enable-static --disable-shared \
        --disable-programs \
        --enable-pic \
        --disable-doc \
        --disable-avdevice --disable-postproc --disable-avfilter \
        --disable-x86asm \
        --disable-iconv \
        --disable-debug \
        --disable-stripping \
        --enable-decoder=aac,mp3,ac3,mp2,alac \
        --enable-demuxer=hls,mpegts,flv,mp3,http,tcp \
        --enable-protocol=http,https,tcp,file,httpproxy \
        --enable-muxer=null \
        --enable-parser=aac,mpegaudio \
        --enable-swresample \
        --disable-encoders \
        --disable-filters \
        2>&1 | tail -25
    mingw32-make -j"$(nproc 2>/dev/null || echo 4)" 2>&1 | tail -5
    mingw32-make install 2>&1 | tail -5
fi

# ---------- 3) 编译播放器本体 ----------
echo "==> 编译播放器"
cd "$ROOT"
mkdir -p dist

# ⚠️ 工具链里的 gcc.exe / python.exe 都是「原生 Windows 程序」，看不懂 /d/...
# 这种 MSYS 路径（会当成 D:\d\... 去找文件）。所以这里一律用相对路径，
# 不写绝对路径。
#
# GUI 子系统（不闪黑窗）+ 静态 + 剥符号
./_build/mw/mingw32/bin/gcc.exe -m32 -Os -static -s \
    -mwindows \
    -DUNICODE -D_UNICODE \
    -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 \
    -Isrc -I_build/ffmpeg_install/include \
    -ffunction-sections -fdata-sections \
    -Wl,--gc-sections \
    -o "dist/shoubo-radio.exe" \
    -Wl,-Map=_build/link.map \
    src/main.c \
    src/ui.c \
    src/tray.c \
    src/player.c \
    src/heal.c \
    src/resolve.c \
    src/liveness.c \
    src/config.c \
    src/json.c \
    src/util.c \
    _build/resource.c \
    _build/ffmpeg_install/lib/libavformat.a \
    _build/ffmpeg_install/lib/libavcodec.a \
    _build/ffmpeg_install/lib/libswresample.a \
    _build/ffmpeg_install/lib/libavutil.a \
    _build/ffmpeg_install/lib/libswscale.a \
    -lwinmm -lwinhttp -ladvapi32 -lshell32 -lgdi32 -luser32 -lkernel32 -lm \
    -lole32 -loleaut32 -lbcrypt -lsecur32 -luserenv -lws2_32 -lcrypt32 \
    -Wl,--enable-auto-import \
    -static-libgcc -static-libstdc++

echo "==> 构建完成"

# ---------- 4) 验收：位数 + dll 依赖 ----------
echo "==> 验收：依赖检查（正常只应有 Win7 自带系统 dll，不能有 libgcc/libwinpthread/ucrt）"
./_build/mw/mingw32/bin/objdump.exe -p "dist/shoubo-radio.exe" 2>/dev/null \
    | grep -iE "DLL Name" || echo "(objdump 没列出 dll)"

# 中文名必须用 python 改（Git Bash 直传会变乱码，见 finalize.py 注释）
( cd "$ROOT" && "$PY" finalize.py )