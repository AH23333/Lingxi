#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""批次 6 端到端演练：本地假 GitHub Releases 服务（仅 127.0.0.1）。

设计：docs/update-design-2026-11-03.md §10 批次 6。配合 update_test.exe --e2e
与环境变量 LINGXI_UPDATE_TEST_BASE（只认回环 host）使用，绝不指向真实网络。

发布场景（GET /repos/<owner>/<repo>/releases/latest）：
  new        v9.9.9 完整三资产（x64/x86 zip + SHA256SUMS.txt）→ 发现新版
  same       v1.0.0（= 本机版本）→ 已是最新
  empty      404 → 尚未发布任何版本（正常态）
  missing86  缺 LingxiIme-win-x86.zip → 发布包不完整（不可用）
  badsum     x64 资产字节被篡改（SHA256SUMS 仍是真值）→ 校验失败整包丢弃
资产下载：GET /downloads/<name>（badsum 的 x64 指向 /downloads-bad/）。

包内容契约（staging 布局，见 §2 发版契约）：zip 根 = 该位架构子树——
  ImeCore.dll / LingxiUI.exe（--exe 传入的 update_test.exe 原件，充当探针桩，
  --version 输出 9.9.9）/ data/pinyin.txt。
"""
import argparse
import hashlib
import io
import json
import os
import sys
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TAG_NEW = "v9.9.9"
TAG_SAME = "v1.0.0"


def build_payload(exe_path):
    exe_bytes = open(exe_path, "rb").read() if exe_path else b"PLACEHOLDER-EXE"
    def make_zip(dll_content, py_content):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("ImeCore.dll", dll_content)
            z.writestr("LingxiUI.exe", exe_bytes)
            z.writestr("data/pinyin.txt", py_content)
        return buf.getvalue()

    z64 = make_zip(b"E2E-NEW64", b"PY64")
    z32 = make_zip(b"E2E-NEW32", b"PY32")
    sums = ("%s  LingxiIme-win-x64.zip\n%s  LingxiIme-win-x86.zip\n" %
            (hashlib.sha256(z64).hexdigest(), hashlib.sha256(z32).hexdigest())).encode()
    bad64 = bytearray(z64)
    bad64[len(bad64) // 2] ^= 0xFF  # 单字节翻转 → SHA256 必不匹配
    return {"LingxiIme-win-x64.zip": z64, "LingxiIme-win-x86.zip": z32,
            "SHA256SUMS.txt": sums, "bad/LingxiIme-win-x64.zip": bytes(bad64)}


def asset_json(base, name, fname, data):
    return {"name": name, "size": len(data),
            "browser_download_url": "%s/%s" % (base, fname)}


class Handler(BaseHTTPRequestHandler):
    payload = None
    def log_message(self, *a):  # 静默，防刷屏演练日志
        pass

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        base = "http://%s:%d/downloads" % (self.server.server_address[0], self.server.server_port)
        parts = self.path.strip("/").split("/")
        if len(parts) == 5 and parts[0] == "repos" and parts[3] == "releases" and parts[4] == "latest":
            repo = parts[2]
            P = self.payload
            def rel(tag, assets, notes):
                body = json.dumps({"tag_name": tag, "body": notes, "assets": assets}).encode()
                self._send(200, body, "application/json")
            if repo == "new":
                rel(TAG_NEW, [asset_json(base, "LingxiIme-win-x64.zip", "LingxiIme-win-x64.zip", P["LingxiIme-win-x64.zip"]),
                              asset_json(base, "LingxiIme-win-x86.zip", "LingxiIme-win-x86.zip", P["LingxiIme-win-x86.zip"]),
                              asset_json(base, "SHA256SUMS.txt", "SHA256SUMS.txt", P["SHA256SUMS.txt"])],
                      "e2e drill release notes")
            elif repo == "same":
                rel(TAG_SAME, [], "same as local")
            elif repo == "missing86":
                rel(TAG_NEW, [asset_json(base, "LingxiIme-win-x64.zip", "LingxiIme-win-x64.zip", P["LingxiIme-win-x64.zip"]),
                              asset_json(base, "SHA256SUMS.txt", "SHA256SUMS.txt", P["SHA256SUMS.txt"])],
                      "missing x86 asset")
            elif repo == "badsum":
                bad = P["bad/LingxiIme-win-x64.zip"]
                bad_url = base.replace("/downloads", "/downloads-bad") + "/LingxiIme-win-x64.zip"
                rel(TAG_NEW, [{"name": "LingxiIme-win-x64.zip", "size": len(bad),
                               "browser_download_url": bad_url},
                              asset_json(base, "LingxiIme-win-x86.zip", "LingxiIme-win-x86.zip", P["LingxiIme-win-x86.zip"]),
                              asset_json(base, "SHA256SUMS.txt", "SHA256SUMS.txt", P["SHA256SUMS.txt"])],
                      "tampered x64 asset")
            elif repo == "empty":
                self._send(404, json.dumps({"message": "Not Found"}).encode(), "application/json")
            else:
                self._send(404, b'{"message":"Not Found"}', "application/json")
            return
        if parts[:1] == ["downloads"] or parts[:1] == ["downloads-bad"]:
            if len(parts) == 2:
                name = ("bad/" if parts[0] == "downloads-bad" else "") + parts[1]
                data = self.payload.get(name)
                if data:
                    self._send(200, data, "application/octet-stream")
                    return
        self._send(404, b'{"message":"Not Found"}', "application/json")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--exe", default=None, help="update_test.exe 路径（嵌入假包充当探针桩）")
    a = ap.parse_args()
    if a.exe and not os.path.isfile(a.exe):
        print("EXE_NOT_FOUND " + a.exe)
        sys.exit(2)
    Handler.payload = build_payload(a.exe)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print("READY %d" % a.port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
