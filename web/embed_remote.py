#!/usr/bin/env python3
# web/remote.html -> source/remote_html.h (the phone remote the 3DS serves on Wi-Fi). Run after editing remote.html.
import os
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, 'remote.html'), encoding='utf-8').read()
out = ['// generated from web/remote.html (web/embed_remote.py); served by the Wi-Fi MIDI remote', 'static const char REMOTE_HTML[] =']
for line in src.splitlines(True):
    esc = line.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
    out.append('\t"' + esc + '"')
out[-1] += ';'
open(os.path.join(here, '..', 'source', 'remote_html.h'), 'w', encoding='utf-8').write('\n'.join(out) + '\n')
