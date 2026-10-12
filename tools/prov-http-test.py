# tools/prov-http-test.py — drive the provisioning page over HTTP from the PC.
#
# Stages:
#   get           fetch the page, dump every form field (visible + hidden), save the HTML
#   post-wrong    POST the form with the pass field replaced by a wrong value;
#                 poll until the failure notice appears
#   post-correct  POST the form with the real pass (read locally from gitignored
#                 src/secrets.h, never printed); then watch the AP go away, which
#                 is the success signal: the Board reboots onto the verified network
import re
import sys
import time
import pathlib
import urllib.parse
import urllib.request

URL = 'http://192.168.4.1/'
HERE = pathlib.Path(__file__).resolve().parent


def get():
    return urllib.request.urlopen(URL, timeout=10).read().decode()


def fields(html):
    out = []
    for m in re.finditer(r'<input[^>]*>', html):
        tag = m.group(0)
        name = re.search(r'name="([^"]+)"', tag).group(1)
        val = re.search(r'value="([^"]*)"', tag)
        out.append((name, val.group(1) if val else ''))
    return out


def notice(html):
    m = re.search(r'<p class="notice[^"]*"[^>]*>([^<]*)</p>', html)
    return m.group(1) if m else '(no notice)'


def stage_get():
    html = get()
    (HERE / 'form-get.html').write_text(html, encoding='utf-8')
    print('bytes:', len(html))
    for name, val in fields(html):
        print(f'{name:10s} value={val!r}')
    print('NOTICE:', notice(html))


def stage_post(pass_value, label, stop_on=None, max_wait=45):
    vals = dict(fields(get()))
    vals['pass'] = pass_value
    body = urllib.parse.urlencode(vals).encode()
    resp = urllib.request.urlopen(urllib.request.Request(URL, data=body), timeout=15)
    html = resp.read().decode()
    print(f'[{label}] POST -> {resp.status} {len(html)} B; notice: {notice(html)!r}')
    t0 = time.time()
    while time.time() - t0 < max_wait:
        time.sleep(3)
        try:
            n = notice(get())
        except Exception as e:
            print(f'[{label}] poll +{int(time.time() - t0)}s: {e!r}')
            continue
        print(f'[{label}] poll +{int(time.time() - t0)}s notice: {n!r}')
        if stop_on and stop_on in n:
            print(f'[{label}] STOP: saw {stop_on!r}')
            return True
    print(f'[{label}] timeout: {stop_on!r} never appeared')
    return False


def main():
    stage = sys.argv[1]
    if stage == 'get':
        stage_get()
    elif stage == 'post-wrong':
        stage_post('definitely-wrong-pass', 'wrong', stop_on='Connection failed')
    elif stage == 'post-correct':
        sec = (HERE.parent / 'src' / 'secrets.h').read_text(encoding='utf-8')
        pw = re.search(r'#define WIFI_PASS\s+"([^"]+)"', sec).group(1)
        stage_post(pw, 'correct', stop_on=None, max_wait=1)
        t0 = time.time()
        while time.time() - t0 < 45:
            time.sleep(2)
            try:
                print(f'+{int(time.time() - t0)}s page still up: {notice(get())!r}')
            except Exception:
                print(f'AP down after {int(time.time() - t0)}s — the Board rebooted onto the verified network')
                return
        print('45 s and the page is still up — the Board did NOT leave Provisioning')


if __name__ == '__main__':
    main()
