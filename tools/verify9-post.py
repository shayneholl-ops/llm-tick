#!/usr/bin/env python3
"""#9 hardware-verify driver: POST the provisioning form (correct passphrase,
location kept/overridden) and watch the page through the resolve sub-phase.

Usage:  python verify9-post.py <loc> [loc2 ...]
  loc = value for the `weather` form field on the POST.
  '-' = keep whatever the page pre-fills.
Stages: GET (show prefill + any notice), POST, then poll the page every 2 s
for up to 90 s printing each notice line until the AP goes away (reboot).
The passphrase is read from src/secrets.h (gitignored) — never printed.
"""
import os
import re
import sys
import time
import urllib.parse
import urllib.request

BASE = 'http://192.168.4.1/'
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, '..', 'src'))


def load_pass():
    with open(os.path.join(SRC, 'secrets.h'), 'r', encoding='utf-8') as f:
        m = re.search(r'WIFI_PASS\s+"([^"]+)"', f.read())
    if not m:
        raise SystemExit('WIFI_PASS not found in src/secrets.h')
    return m.group(1)


def get():
    r = urllib.request.urlopen(BASE, timeout=5)
    return r.status, r.read().decode('utf-8', 'replace')


def fields(html):
    out = {}
    for m in re.finditer(r'<input\b[^>]*>', html):
        t = m.group(0)
        name = re.search(r'name="([^"]+)"', t)
        if not name:
            continue
        val = re.search(r'value="([^"]*)"', t)
        out[name.group(1)] = val.group(1) if val else ''
    return out


def notice(html):
    m = re.search(r'<p class="notice[^"]*"[^>]*>(.*?)</p>', html, re.S)
    return ' '.join(m.group(1).split()) if m else '(no notice)'


def main():
    loc = sys.argv[1] if len(sys.argv) > 1 else '-'
    st, html = get()
    print('GET', st)
    f = fields(html)
    print('  weather prefill:', repr(f.get('weather')))
    print('  notice:', notice(html))
    f['pass'] = load_pass()
    if loc != '-':
        f['weather'] = loc
        print('  weather set to:', repr(loc))
    body = urllib.parse.urlencode(f).encode('utf-8')
    req = urllib.request.Request(BASE, data=body, method='POST')
    try:
        r = urllib.request.urlopen(req, timeout=15)
        st = r.status
    except Exception as e:  # board may drop the socket while it reboots
        print('POST transport:', type(e).__name__, str(e)[:80])
        return
    html = r.read().decode('utf-8', 'replace')
    print('POST', st, '  notice:', notice(html))
    # Watch the page through verify -> resolve -> grace -> reboot.
    for i in range(45):
        time.sleep(2)
        try:
            st2, html2 = get()
        except Exception as e:
            print(f'[{i * 2:>3}s] AP gone ({type(e).__name__}) — board left provisioning')
            return
        n = notice(html2)
        if i == 0 or n != last:
            print(f'[{i * 2:>3}s] GET {st2} notice: {n}')
            last = n
    print('watch window elapsed — AP still up (failure expected to persist)')


if __name__ == '__main__':
    main()
