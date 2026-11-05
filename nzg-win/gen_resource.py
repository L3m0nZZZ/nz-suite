"""把 nzg 的 config_default.json 转成 C 数组，编译进 exe。

绿色单 exe 的要求是「双击即用」，所以 29 台频道清单必须编进二进制。
生成的是 unsigned char数组（不是字符串常量），避免编译器的字符串驻留
优化把大对象塞到 .rdata 造成对齐/截断问题。
"""
import json
import sys
import os


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else r"D:\ningz\nzg\app\src\main\assets\config_default.json"
    dst = sys.argv[2] if len(sys.argv) > 2 else "resource.c"

    with open(src, "r", encoding="utf-8") as f:
        data = f.read()

    # 校验必须是合法 JSON，避免把坏配置编进 exe
    cfg = json.loads(data)
    channels = cfg.get("channels", [])
    if not channels:
        print("配置里没有任何频道，中止", file=sys.stderr)
        return 1

    raw = data.encode("utf-8")

    lines = []
    lines.append("/* 自动生成，请勿手改。源文件：%s */" % src.replace("\\", "/"))
    lines.append("const unsigned int g_config_default_json_len = %dU;" % len(raw))
    lines.append("const unsigned char g_config_default_json[] = {")
    for i in range(0, len(raw), 20):
        chunk = raw[i:i + 20]
        lines.append("    " + " ".join("0x%02x," % b for b in chunk))
    lines.append("};")
    lines.append("")

    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print("已生成 %s：%d 字节，%d 个频道" % (dst, len(raw), len(channels)))
    return 0


if __name__ == "__main__":
    sys.exit(main())