#!/usr/bin/env python3
"""web/ をローカルで配信する。ブラウザで http://localhost:8000 を開く。
Web Serial は HTTPS か localhost でしか使えないため、ファイルを直接開かずにこれを使う。"""
import functools
import http.server
import pathlib

web = pathlib.Path(__file__).resolve().parent.parent / "web"
# ライセンス表示（.txt）の日本語が文字化けしないように
http.server.SimpleHTTPRequestHandler.extensions_map[".txt"] = "text/plain; charset=utf-8"
handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(web))
print("http://localhost:8000 を Chrome または Edge で開いてください（終了は Ctrl+C）")
http.server.ThreadingHTTPServer(("127.0.0.1", 8000), handler).serve_forever()
