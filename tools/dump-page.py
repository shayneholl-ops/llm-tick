# Dump the current provisioning page: notice markup + all field values.
import os
import re
import urllib.request

html = urllib.request.urlopen('http://192.168.4.1/', timeout=10).read().decode()
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'form-after-wrong.html')
open(out, 'w', encoding='utf-8').write(html)
print('bytes:', len(html))
print('--- lines with notice/verif/fail/error ---')
for i, line in enumerate(html.splitlines()):
    if re.search('otice|erify|ail|rror', line):
        print(i, line.strip()[:130])
print('--- input values ---')
for m in re.finditer(r'<input[^>]*>', html):
    tag = m.group(0)
    n = re.search(r'name="([^"]+)"', tag).group(1)
    v = re.search(r'value="([^"]*)"', tag)
    print(n, '=', repr(v.group(1)) if v else '(none)')
