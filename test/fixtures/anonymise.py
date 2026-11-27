#!/usr/bin/env python3
"""anonymise.py — P0-18.1 (WP-42): strip identifying data from OBS scene
collections before they enter the corpus (test/fixtures/scenes/).

D4 (decided 2026-11-23) makes the corpus a mix of public material and
synthetic fixtures; nothing private may ship. The four identifying classes
stripped from every string value:

  1. URLs            any "scheme://…" (https, rtmp, srt, …) -> ANON_URL
                     (token-bearing query strings go with the URL)
  2. absolute paths  POSIX /… (token start, >= 2 segments), Windows
                     C:\\… and C:/…, UNC \\\\server\\… -> ANON_PATH
  3. stream keys     values of settings named key / password / token /
                     secret / … -> ANON_SECRET
  4. window titles   values of settings named window / window_title /
                     capture_window_line / title / … -> ANON_WINDOW

Values are transformed after JSON parsing, so the output is always valid
JSON and the transformation is idempotent (ANON_* tokens survive a second
pass untouched — except that a bare "/ANON_PATH" re-matches the POSIX
pattern and rewrites to the same token).

Usage:
  anonymise.py IN OUT        write the anonymised copy
  anonymise.py --check F…    exit 1 if any file still contains a strippable
                             value (the committed corpus must pass this)
  anonymise.py --self-test   seeded-secrets unit test (P0-18.1 Done-when)

Stdlib only.
"""

import json
import re
import sys

RE_URL = re.compile(r"[A-Za-z][A-Za-z0-9+.\-]*://[^\s\"\\,;]+")
RE_UNC = re.compile(r"\\\\[\w.\-]+(?:\\[^\s\"\\,;]*)+")
RE_WIN = re.compile(r"[A-Za-z]:[\\/][^\s\":;]*")
# absolute POSIX paths: anchored at a token start (string start or after
# whitespace / a colon) with at least two segments, so slashes inside
# values (base64, "Mic/Aux" names, fractions) are left alone
RE_POSIX = re.compile(r"(?:^|(?<=\s))(?:/[^\s\"\\:,]+){2,}")

URL_TOKEN = "ANON_URL"
PATH_TOKEN = "ANON_PATH"
SECRET_TOKEN = "ANON_SECRET"
WINDOW_TOKEN = "ANON_WINDOW"

# normalised (lower-case, alnum only) setting keys whose values are secrets
SECRET_KEYS = {
    "key", "password", "passwd", "pass", "secret", "token", "apikey",
    "apitoken", "streamkey", "clientsecret", "accesstoken",
    "refreshtoken", "authorization",
}
# normalised setting keys whose values carry a window title (or a
# "handle:class:Title" capture line, which contains one)
WINDOW_KEYS = {"window", "windowtitle", "windowname", "capturewindowline",
               "title", "target"}
# hotkey binding objects (obs_hotkey save format) use "key" for the key
# name ("F9", "A", …) — those must not be treated as secrets
HOTKEY_MODIFIERS = ("shift", "control", "alt", "command")


def normalize_key(key):
    return re.sub(r"[^a-z0-9]", "", key.lower())


def scrub_string(value, counter):
    """Redact one string value; counter is a dict of class -> count."""
    out = value
    out, n = RE_URL.subn(URL_TOKEN, out)
    counter["url"] += n
    out, n = RE_UNC.subn(PATH_TOKEN, out)
    counter["path"] += n
    out, n = RE_WIN.subn(PATH_TOKEN, out)
    counter["path"] += n
    out, n = RE_POSIX.subn(PATH_TOKEN, out)
    counter["path"] += n
    return out


def scrub(obj, counter):
    """Anonymise a parsed JSON structure in place and count redactions."""
    if isinstance(obj, dict):
        is_hotkey_binding = any(m in obj for m in HOTKEY_MODIFIERS)
        for k in list(obj.keys()):
            v = obj[k]
            if isinstance(v, str) and v:
                norm = normalize_key(k)
                if norm in SECRET_KEYS and not (is_hotkey_binding and
                                                norm == "key"):
                    if v != SECRET_TOKEN:
                        counter["secret"] += 1
                        obj[k] = SECRET_TOKEN
                    continue
                if norm in WINDOW_KEYS:
                    if v != WINDOW_TOKEN:
                        counter["window"] += 1
                        obj[k] = WINDOW_TOKEN
                    continue
                obj[k] = scrub_string(v, counter)
            elif isinstance(v, (dict, list)):
                scrub(v, counter)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            if isinstance(v, str) and v:
                obj[i] = scrub_string(v, counter)
            elif isinstance(v, (dict, list)):
                scrub(v, counter)
    return obj


def anonymise_text(text):
    """Parse, anonymise and re-serialize a scene collection document.

    Returns (json_text, counter). Key order is preserved (json module);
    non-ASCII stays raw, separators mirror the corpus style (indent 4).
    """
    counter = {"url": 0, "path": 0, "secret": 0, "window": 0}
    data = json.loads(text)
    scrub(data, counter)
    return json.dumps(data, indent=4, ensure_ascii=False) + "\n", counter


def main(argv):
    if argv and argv[0] == "--self-test":
        return self_test()
    if argv and argv[0] == "--check":
        bad = 0
        for path in argv[1:]:
            with open(path, encoding="utf-8") as f:
                text = f.read()
            _, counter = anonymise_text(text)
            total = sum(counter.values())
            if total:
                bad += 1
                print(f"{path}: NOT ANONYMISED ({total} strippable values: "
                      + ", ".join(f"{c} {k}" for k, c in counter.items()
                                  if c) + ")", file=sys.stderr)
        if bad:
            print(f"--check: {bad} file(s) fail", file=sys.stderr)
            return 1
        print(f"--check: {len(argv[1:])} file(s) clean")
        return 0
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[0], encoding="utf-8") as f:
        text = f.read()
    out, counter = anonymise_text(text)
    with open(argv[1], "w", encoding="utf-8") as f:
        f.write(out)
    print(f"{argv[0]} -> {argv[1]}: "
          + ", ".join(f"{c} {k}" for k, c in counter.items() if c)
          + ("" if any(counter.values()) else " (already clean)"))
    return 0


# ---------------------------------------------------------------------------
# self-test (P0-18.1 Done-when: seeded secrets, none found in the output)
# ---------------------------------------------------------------------------

def self_test():
    # seeded identifying values — none of these substrings may survive
    seeds = {
        "posix_path": "/home/secretuser/Videos/Clip.mp4",
        "win_path": "C:\\Users\\secretuser\\Desktop\\take.mp4",
        "unc_path": "\\\\fileserver\\shares\\obs\\take.mp4",
        "https_url": "https://stream.example.com/live?token=sekrettoken123",
        "rtmp_url": "rtmp://ingest.example.com/app/stream?secret=sekrettoken123",
        "stream_key": "live_9f8e7d6c5b4a3210SECRETKEY",
        "api_token": "ghp_SEKRETAPISECRET42",
        "window_title": "1189:0x000403e6:My Secret Meeting Window",
        "bare_title": "My Secret Meeting Window",
        "process_target": "1189:0x000403e6:Another Secret Window",
    }
    doc = {
        "name": "Seed Test",
        "version": 2,
        "current_scene": "Scene 1",
        "current_program_scene": "Scene 1",
        "scene_order": [{"name": "Scene 1"}],
        "sources": [
            {
                "id": "browser_source",
                "versioned_id": "browser_source",
                "name": "Browser",
                "uuid": "00000000-0000-4000-8000-000000000001",
                "settings": {
                    "url": seeds["https_url"],
                    "local_file": seeds["posix_path"],
                },
            },
            {
                "id": "ffmpeg_source",
                "versioned_id": "ffmpeg_source",
                "name": "Media",
                "uuid": "00000000-0000-4000-8000-000000000002",
                "settings": {
                    "input": seeds["rtmp_url"],
                    "local_file": seeds["win_path"],
                    "alt_file": seeds["unc_path"],
                },
            },
            {
                "id": "game_capture",
                "versioned_id": "game_capture",
                "name": "Game",
                "uuid": "00000000-0000-4000-8000-000000000003",
                "settings": {
                    "window": seeds["window_title"],
                    "window_title": seeds["bare_title"],
                },
            },
            {
                "id": "wasapi_process_output_capture",
                "versioned_id": "wasapi_process_output_capture",
                "name": "App",
                "uuid": "00000000-0000-4000-8000-000000000005",
                "settings": {"device_id": "default",
                             "target": seeds["process_target"]},
            },
        ],
        "modules": {
            "rtmp-test": {
                "key": seeds["stream_key"],
                "api_token": seeds["api_token"],
            },
        },
        "quick_transitions": [
            {"name": "Fade to Black", "duration": 300,
             "hotkeys": [{"control": True, "key": "F9"}],
             "id": 3, "fade_to_black": True},
        ],
    }
    text = json.dumps(doc, indent=4)
    out, counter = anonymise_text(text)

    checks = 0

    def check(cond, label):
        nonlocal checks
        assert cond, f"self-test FAILED: {label}"
        checks += 1

    # 1. output is valid JSON
    parsed = json.loads(out)
    check(isinstance(parsed, dict), "output is valid JSON")
    # 2. no seed substring survives (checked in the raw serialized text so
    #    escaped forms cannot hide either)
    for label, seed in seeds.items():
        check(seed not in out, f"seed {label} absent")
    for fragment in ("secretuser", "sekrettoken123", "My Secret Meeting",
                     "fileserver", "SECRETKEY", "SEKRETAPISECRET42",
                     "0x000403e6"):
        check(fragment not in out, f"fragment {fragment!r} absent")
    # 3. all four classes were actually stripped
    check(counter["url"] >= 2, "URLs redacted")
    check(counter["path"] >= 3, "paths redacted")
    check(counter["secret"] >= 2, "secrets redacted")
    check(counter["window"] >= 2, "window titles redacted")
    # 4. structure preserved (deep key equality, values may change)
    def keys_of(o):
        if isinstance(o, dict):
            return {k: keys_of(v) for k, v in o.items()}
        if isinstance(o, list):
            return [keys_of(v) for v in o]
        return None
    check(keys_of(parsed) == keys_of(doc), "structure preserved")
    # 4b. a hotkey binding "key" is a key name, not a secret
    hk = parsed["quick_transitions"][0]["hotkeys"][0]
    check(hk["key"] == "F9", "hotkey binding key survives")
    # 4c. a process-capture target line (contains a window title) is redacted
    check(parsed["sources"][3]["settings"]["target"] == WINDOW_TOKEN,
          "process target redacted")
    # 5. idempotence
    out2, _ = anonymise_text(out)
    check(out2 == out, "idempotent")
    print(f"self-test: OK ({checks} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
