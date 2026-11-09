#!/usr/bin/env python3
"""同梱している mdns と MicroLink の参照元（本家）が更新されているかを調べる。

    python3 .claude/skills/check-upstream/check_upstream.py

元にした版は components/<名前>/VERSION から読む。
  mdns      : esp-protocols のタグ mdns-vX.Y.Z と比べる（新しいタグの ESP-IDF の要件も出す）
  microlink : 本家の既定のブランチの先頭と比べ、増えたコミットを出す。
              取り込んだ未マージの PR（VERSION の「Upstream PRs applied」）が、
              本家でマージ・クローズされたかと、VERSION の「as of 日付」より後に
              作られた・更新された PR も出す

終了コード: 0 = 更新なし、10 = 更新あり、2 = 調べられなかった
GitHub の API は認証なしだと 1 時間に 60 回まで。環境変数 GITHUB_TOKEN があれば使う
"""
import json
import os
import pathlib
import re
import subprocess
import sys
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[3]
MDNS_REPO = "espressif/esp-protocols"
ML_REPO = "CamM2325/microlink"
IDF_VERSION = "5.3.1"  # このプロジェクトのビルドに使う ESP-IDF（README の「ビルド」）


def api(path):
    req = urllib.request.Request(f"https://api.github.com/{path}",
                                 headers={"Accept": "application/vnd.github+json",
                                          "User-Agent": "headboard-check-upstream"})
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)


def raw(repo, ref, path):
    url = f"https://raw.githubusercontent.com/{repo}/{ref}/{path}"
    with urllib.request.urlopen(url, timeout=20) as r:
        return r.read().decode()


def ls_remote(repo, *patterns):
    out = subprocess.run(["git", "ls-remote", f"https://github.com/{repo}", *patterns],
                         capture_output=True, text=True, timeout=60, check=True).stdout
    return [line.split("\t") for line in out.splitlines() if "\t" in line]


def vtuple(v):
    return tuple(int(x) for x in v.split("."))


def check_mdns():
    text = (ROOT / "components/mdns/VERSION").read_text()
    m = re.search(r"mdns-v(\d+\.\d+\.\d+)", text)
    if not m:
        raise RuntimeError("components/mdns/VERSION に mdns-vX.Y.Z が見つかりません")
    cur = m.group(1)
    tags = {}
    for sha, ref in ls_remote(MDNS_REPO, "refs/tags/mdns-v*"):
        t = re.fullmatch(r"refs/tags/mdns-v(\d+\.\d+\.\d+)(\^\{\})?", ref)
        if t:
            tags[t.group(1)] = sha
    newer = sorted((v for v in tags if vtuple(v) > vtuple(cur)), key=vtuple)
    print(f"## mdns（{MDNS_REPO}）")
    print(f"- 同梱: mdns-v{cur}")
    if not newer:
        print("- 更新なし")
        return False
    print(f"- 新しい版: {', '.join('mdns-v' + v for v in newer)}")
    latest = newer[-1]
    try:
        yml = raw(MDNS_REPO, f"mdns-v{latest}", "components/mdns/idf_component.yml")
        req = re.search(r"idf:\s*\n\s*version:\s*[\"']?([^\"'\n]+)", yml)
        print(f"- mdns-v{latest} の ESP-IDF の要件: {req.group(1) if req else '（不明）'}"
              f"（このプロジェクトは v{IDF_VERSION}）")
    except urllib.error.URLError as e:
        print(f"- ESP-IDF の要件を読めませんでした: {e}")
    print(f"- 変更履歴: https://github.com/{MDNS_REPO}/blob/mdns-v{latest}/components/mdns/CHANGELOG.md")
    return True


def check_microlink():
    text = (ROOT / "components/microlink/VERSION").read_text()
    m = re.search(r"commit ([0-9a-f]{40})", text)
    if not m:
        raise RuntimeError("components/microlink/VERSION にコミットが見つかりません")
    base = m.group(1)
    applied = [int(n) for n in re.findall(r"^\s+- #(\d+)\b", text, re.M)]
    head = ls_remote(ML_REPO, "HEAD")[0][0]
    print(f"## MicroLink（{ML_REPO}）")
    print(f"- 同梱の元: {base[:7]}（ほかに未マージの PR {', '.join('#' + str(n) for n in applied)} を取り込み済み）")
    updated = False
    if head == base:
        print("- 本家の既定のブランチ: 更新なし")
    else:
        updated = True
        cmp = api(f"repos/{ML_REPO}/compare/{base}...{head}")
        print(f"- 本家の既定のブランチ: {head[:7]}（{cmp['ahead_by']} コミット増えている。"
              f"変わったファイル {len(cmp.get('files', []))} 個）")
        for c in cmp["commits"][-20:]:
            msg = c["commit"]["message"].splitlines()[0]
            print(f"  - {c['sha'][:7]} {c['commit']['author']['date'][:10]} {msg}")
        print(f"  - 差分: https://github.com/{ML_REPO}/compare/{base[:7]}...{head[:7]}")

    # 取り込んだ PR の本家での扱い
    for n in applied:
        pr = api(f"repos/{ML_REPO}/pulls/{n}")
        if pr["merged_at"]:
            updated = True
            print(f"- PR #{n} は本家でマージされた（{pr['merged_at'][:10]}）。取り込み直すときは二重に当てないこと")
        elif pr["state"] == "closed":
            updated = True
            print(f"- PR #{n} はマージされずにクローズされた（{pr['closed_at'][:10]}）: {pr['title']}")
    # 取り込んでいない、開いている PR。VERSION の「as of 日付」（前回 PR を見直した日）より後に
    # 作られた・更新されたものだけを新しいものとして出す
    as_of = re.search(r"as of (\d{4}-\d{2}-\d{2})", text)
    as_of = as_of.group(1) if as_of else "0000-00-00"
    others = [p for p in api(f"repos/{ML_REPO}/pulls?state=open&per_page=100") if p["number"] not in applied]
    fresh = [p for p in others if p["updated_at"][:10] > as_of]
    if fresh:
        updated = True
        print(f"- {as_of} より後に作られた・更新された、取り込んでいない PR:")
        for p in fresh:
            print(f"  - #{p['number']} {p['updated_at'][:10]} 更新 {p['title']}  {p['html_url']}")
    print(f"- {as_of} までに見直して取り込まなかった開いている PR: {len(others) - len(fresh)} 件（表示しない）")
    return updated


def main():
    updated = False
    ok = True
    for check in (check_mdns, check_microlink):
        try:
            updated |= check()
        except (subprocess.SubprocessError, urllib.error.URLError, RuntimeError, KeyError, OSError) as e:
            ok = False
            print(f"- 調べられませんでした: {e}")
        print()
    if not ok:
        sys.exit(2)
    print("結果: 更新あり" if updated else "結果: 更新なし")
    sys.exit(10 if updated else 0)


main()
