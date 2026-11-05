"""裁剪 FFmpeg 生成的组件清单，实现「轻量」而不重新编译。

原理：FFmpeg 的静态注册表（codec_list.c / demuxer_list.c / ...）里每个条目
都是一个指向具体实现的指针。只要把不需要的条目删掉，链接器在
--gc-sections 下就不会把那些实现拉进 exe。

比重新 configure + make 快得多（那要 20+ 分钟，这只用几秒），
效果对「未引用即丢弃」的静态链接来说是一样的。

守播广播是纯音频播放器，所以：
  - 解码器只留音频类（视频解码器是大头，h264/hevc/vp9/av1 合计数 MB）
  - 复用器全部去掉（我们只拉流解码，从不封装输出）
  - 解复用器/协议/解析器留音频流与网络相关的一套

注意：这是保守白名单，宁可多留一点，也不要漏掉某个台用到的格式。
"""
import io
import os
import re
import sys


FF = r"D:\ningz\nzg-win\_build\ffmpeg-6.1.2"

# ---------- 解码器白名单（音频） ----------
DECODER_ALLOW = set("""
aac aac_fixed aac_latm
mp1 mp1float mp2 mp2float mp3 mp3float mp3adu mp3adufloat mp3on4 mp3on4float
ac3 ac3_fixed eac3
alac flac opus vorbis
wmav1 wmav2 wmapro wmalossless wmavoice
dca truehd mlp
ra_144 ra_288 cook sipr nellymoser
atrac1 atrac3 atrac3al atrac3p atrac3pal atrac9
on2avc speex wavpack shn dst
gsm gsm_ms g723_1 qdm2 taq twvq
sbc aptx aptx_hd hca als imc
mace3 mace6 iac
g728 g729 evrc smv
""".split())

DECODER_PREFIX_ALLOW = ("pcm_", "adpcm_")

# ---------- 解复用器白名单 ----------
DEMUXER_ALLOW = set("""
hls mpegts mpegtsraw mpegps
flv mp3 aac ac3 eac3 ogg matroska mov asf wav w64
aiff au voc caf wv tta
concat mjpeg mpjpeg
rtp rtsp sdp
data image2
""".split())

# ---------- 协议白名单 ----------
PROTOCOL_ALLOW = set("""
http https tcp tls crypto httpproxy
file concat cache pipe
rtmp rtmps rtmpt
udp rtp srt
""".split())

# ---------- 解析器白名单 ----------
# 只留音频解析器。视频解析器（h264/hevc/mpeg4/vc1）会连带把
# h264qpel / h264_mb / h264pred / h264dsp / vc1dsp 等 DSP 实现拉进来，
# 实测约 700KB —— 而这是纯音频播放器，视频轨我们既不选也不解，
# 少一个视频解析器只会让探测少认一种视频流，不影响选音频轨。
PARSER_ALLOW = set("""
aac aac_latm mpegaudio ac3 eac3 dca
vorbis opus flac
mlp truehd
""".split())

# ---------- 比特流过滤器白名单 ----------
BSF_ALLOW = set("""
aac_adtstoasc
extract_extradata
null
""".split())


def filter_list(path, allow, prefix_allow=(), keep_none=False):
    """把 `&ff_xxx_decoder,` 这类条目过滤成白名单内的。"""
    if not os.path.exists(path):
        return None
    text = io.open(path, encoding="utf-8", errors="replace").read()
    lines = text.split("\n")
    out = []
    kept = 0
    dropped = 0
    for ln in lines:
        m = re.match(r"\s*&ff_([A-Za-z0-9_]+)_(decoder|demuxer|protocol|parser|bsf|muxer)\s*,?\s*$", ln)
        if not m:
            out.append(ln)
            continue
        name = m.group(1)
        if name in allow:
            out.append(ln)
            kept += 1
        elif any(name.startswith(p) for p in prefix_allow):
            out.append(ln)
            kept += 1
        else:
            dropped += 1
    io.open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out))
    return kept, dropped


def main():
    total_kept = total_drop = 0
    jobs = [
        (os.path.join(FF, "libavcodec", "codec_list.c"), DECODER_ALLOW, DECODER_PREFIX_ALLOW, "解码器"),
        (os.path.join(FF, "libavcodec", "parser_list.c"), PARSER_ALLOW, (), "解析器"),
        (os.path.join(FF, "libavcodec", "bsf_list.c"), BSF_ALLOW, (), "比特流过滤器"),
        (os.path.join(FF, "libavformat", "demuxer_list.c"), DEMUXER_ALLOW, (), "解复用器"),
        (os.path.join(FF, "libavformat", "protocol_list.c"), PROTOCOL_ALLOW, (), "协议"),
        (os.path.join(FF, "libavformat", "muxer_list.c"), {"null"}, (), "复用器"),
    ]
    for path, allow, pref, label in jobs:
        r = filter_list(path, allow, pref)
        if r is None:
            print("  跳过（不存在）：%s" % path)
            continue
        kept, dropped = r
        total_kept += kept
        total_drop += dropped
        print("  %-8s 保留 %3d，裁掉 %3d" % (label, kept, dropped))
    print("合计保留 %d，裁掉 %d" % (total_kept, total_drop))
    return 0


if __name__ == "__main__":
    sys.exit(main())
