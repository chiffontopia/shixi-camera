#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ai_relay.py — 宿主机上的 AI 转发服务（配合板端 core/ai.c 的 relay_url 使用）

为什么需要它：开发板只跑一个静态 curl，直接连公网 HTTPS 又慢又难调试，
所以让板子 POST 到本机这个小服务，由本机带着真正的密钥去调云端模型。

板子侧配置（$SHIXI_ROOT/）：
    relay_url   ->  http://<本机IP>:3688/v1/chat/completions
    relay_key   ->  本服务要求的口令（随便设一个，别用云端密钥）

本机侧配置（tools/ai_secret.conf，**已在 .gitignore 里，不会被提交**）：
    endpoint=https://api.example.com/v1/chat/completions
    api_key=<云端密钥>
    relay_key=<与板子 relay_key 相同的口令>
也可用环境变量：SHIXI_API_ENDPOINT / SHIXI_API_KEY / SHIXI_RELAY_KEY

用法：
    python3 tools/ai_relay.py                 # 监听 0.0.0.0:3688
    PORT=9000 python3 tools/ai_relay.py       # 换端口
"""
import json
import os
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
CONF = os.environ.get("SHIXI_RELAY_CONF", os.path.join(HERE, "ai_secret.conf"))


def load_conf():
    cfg = {}
    if os.path.exists(CONF):
        with open(CONF, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    cfg.setdefault("endpoint", os.environ.get("SHIXI_API_ENDPOINT", ""))
    cfg.setdefault("api_key", os.environ.get("SHIXI_API_KEY", ""))
    cfg.setdefault("relay_key", os.environ.get("SHIXI_RELAY_KEY", ""))
    cfg["port"] = int(os.environ.get("PORT", cfg.get("port", 3688)))
    return cfg


CFG = load_conf()
PORT = CFG["port"]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):        # 精简日志，只留有用的
        sys.stderr.write("  %s\n" % (fmt % args))

    def _send(self, code, body):
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self._send(200, json.dumps({"ok": True, "service": "shixi ai relay"}))

    def do_POST(self):
        if not CFG["endpoint"]:
            self._send(500, json.dumps({"error": {"message": "relay 未配置 endpoint"}}))
            return
        n = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(n) if n else b""

        # 口令校验：板子只持有这个本地口令，云端密钥不出本机
        if CFG["relay_key"]:
            auth = self.headers.get("Authorization", "")
            if auth != "Bearer " + CFG["relay_key"]:
                print("  !! 口令不符，拒绝", flush=True)
                self._send(401, json.dumps({"error": {"message": "relay: unauthorized"}}))
                return

        # 模型名可以由板子带，也可以在这里固定覆盖
        if CFG.get("model"):
            try:
                obj = json.loads(body.decode("utf-8"))
                obj["model"] = CFG["model"]
                body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            except Exception:
                pass

        # 注意：默认的 "Python-urllib/x.y" User-Agent 会被某些 CDN/WAF 直接拒
        # （实测某云端接口的 Cloudflare 返回 403 error code 1010），
        # 这里伪装成 curl —— 与 curl 转发路径保持一致的行为。
        req = urllib.request.Request(
            CFG["endpoint"], data=body,
            headers={"Content-Type": "application/json",
                     "User-Agent": CFG.get("user_agent", "curl/8.5.0"),
                     "Authorization": "Bearer " + CFG["api_key"]},
            method="POST")
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                data = r.read()
                print("  -> %d，%d 字节" % (r.status, len(data)), flush=True)
                self._send(r.status, data)
        except urllib.error.HTTPError as e:
            data = e.read()
            print("  -> HTTP %d：%s" % (e.code, data[:200].decode("utf-8", "ignore")), flush=True)
            self._send(e.code, data)
        except Exception as e:
            print("  -> 转发失败：%s" % e, flush=True)
            self._send(502, json.dumps({"error": {"message": "relay 转发失败：%s" % e}}))


def main():
    if not CFG["api_key"]:
        print("警告：没有配置云端 api_key（%s 里写 api_key=...，或用环境变量）" % CONF)
    if not CFG["relay_key"]:
        print("提示：没有配置 relay_key，任何人可访问本服务（内网自用可接受）")
    print("AI relay 监听 0.0.0.0:%d" % PORT)
    print("  转发目标: %s" % (CFG["endpoint"] or "(未配置)"))
    print("  板子侧:   relay_url -> http://<本机IP>:%d/v1/chat/completions" % PORT)
    print("  板子侧:   relay_key -> 与这里 relay_key 相同的口令")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
