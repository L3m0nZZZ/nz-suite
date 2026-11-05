#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""极简 TCP 限速代理（只做 CONNECT 隧道，不碰 TLS）。

用途：在开发机上模拟「机房主机那条慢链路」，复现 HLS 分片下载耗时接近
分片时长时不停 `skipping N segments ahead, expired from playlists` 的现象，
用来验证 live_start_index 等参数在不同链路条件下的表现。

用法：
    python tools/slowproxy.py --port 18080 --rate 25        # 每方向 25 KB/s
    # 另开窗口：
    set https_proxy=http://127.0.0.1:18080
    ffmpeg -loglevel warning -live_start_index -1 -i <hls url> -t 180 -f null -

注意：限速是**每连接每方向**的 token bucket，握手/小包不受影响，
所以它模拟的是带宽不足，不是 RTT。要模拟高延迟另说。
"""
import argparse
import socket
import threading
import time


class Bucket:
    __slots__ = ("rate", "cap", "tokens", "last", "lock")

    def __init__(self, rate_bps):
        self.rate = float(rate_bps)
        self.cap = max(self.rate, 4096.0)      # 允许 1 秒突发
        self.tokens = self.cap
        self.last = time.time()
        self.lock = threading.Lock()

    def take(self, n):
        with self.lock:
            now = time.time()
            self.tokens = min(self.cap, self.tokens + (now - self.last) * self.rate)
            self.last = now
            if self.tokens >= n:
                self.tokens -= n
                return 0.0
            need = n - self.tokens
            self.tokens = 0.0
            return need / self.rate


def pump(src, dst, bucket, chunk=8192):
    try:
        while True:
            data = src.recv(chunk)
            if not data:
                break
            if bucket:
                delay = bucket.take(len(data))
                if delay > 0:
                    time.sleep(delay)
            dst.sendall(data)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                s.close()
            except OSError:
                pass


def handle(client, args):
    try:
        client.settimeout(30)
        head = b""
        while b"\r\n\r\n" not in head:
            part = client.recv(4096)
            if not part:
                client.close()
                return
            head += part
            if len(head) > 65536:
                client.close()
                return

        line = head.split(b"\r\n", 1)[0].decode("latin-1", "replace")
        if not line.upper().startswith("CONNECT"):
            client.sendall(b"HTTP/1.1 405 Only CONNECT\r\n\r\n")
            client.close()
            return

        hostport = line.split()[1]
        host, _, port = hostport.rpartition(":")
        upstream = socket.create_connection((host, int(port)), timeout=15)
        client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")

        rate = args.rate * 1024
        t1 = threading.Thread(target=pump, args=(client, upstream, Bucket(rate)), daemon=True)
        t2 = threading.Thread(target=pump, args=(upstream, client, Bucket(rate)), daemon=True)
        t1.start()
        t2.start()
        t1.join()
    except OSError:
        try:
            client.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18080)
    ap.add_argument("--rate", type=float, default=25.0, help="每个方向的 KB/s")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(64)
    print("限速代理已启动 127.0.0.1:%d，每方向 %.0f KB/s" % (args.port, args.rate), flush=True)
    try:
        while True:
            c, _ = srv.accept()
            threading.Thread(target=handle, args=(c, args), daemon=True).start()
    except KeyboardInterrupt:
        print("退出")


if __name__ == "__main__":
    main()
