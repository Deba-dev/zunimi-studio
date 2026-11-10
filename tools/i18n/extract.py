"""Lists every English string the app looks up: L("...") calls, plus the tables read through L(variable).

usage: python tools/i18n/extract.py <repo> <out.json>   (writes a JSON list, in source order, without duplicates)
"""
import json
import os
import re
import sys

root, out = sys.argv[1], sys.argv[2]
LIT = r'"(?:[^"\\\n]|\\.)*"'
CALL = re.compile(r'\bL\((' + LIT + r'(?:\s*' + LIT + r')*)\s*\)')
ONE = re.compile(LIT)

# tables whose entries reach the screen through L(field)
TABLES = [
    ('src/ui/MainWindow.cpp', r'const Kind KINDS\[\] = \{(.*?)\n\};', 1),
    ('src/ui/MainWindow.cpp', r'blends\[\] = \{(.*?)\};', 0),
    ('src/ui/MainWindow.cpp', r'scales\[\] = \{(.*?)\};', 0),
    ('src/ui/MainWindow.cpp', r'effects\[\] = \{(.*?)\};', 0),
    ('src/ui/WindowModes.cpp', r'TIPS\[\] = \{(.*?)\};', 0),
    ('src/ui/Dialogs.cpp', r'sides\[\] = \{(.*?)\};', 0),
    ('src/ui/SettingsDialog.cpp', r'const Platform PLATFORMS\[\] = \{(.*?)\n\};', 0),
    ('src/ui/SetupWizard.cpp', r'const Platform PLATFORMS\[\] = \{(.*?)\n\};', 0),
]
# made-up names and chat lines for test alerts, and things that only look like words
NOT_UI = {'Aarav', 'PixelNinja', 'Kavya_plays', 'NightOwl', 'Rohan_07', 'Sana', 'Bhai kya clutch tha! 🔥', 'Cheer500 GG',
          '7 months already!', 'Twitch', 'YouTube', 'Kick', 'StreamElements', 'Streamlabs', 'NVENC', 'x264 (CPU)'}


def decode(lit_run):
    text = ''.join(m.group(0)[1:-1] for m in ONE.finditer(lit_run))
    out_chars, i = [], 0
    while i < len(text):
        ch = text[i]
        if ch == '\\' and i + 1 < len(text):
            n = text[i + 1]
            if n == 'n':
                out_chars.append('\n'); i += 2
            elif n == 't':
                out_chars.append('\t'); i += 2
            elif n in '"\\\'?':
                out_chars.append(n); i += 2
            elif n == 'u':
                out_chars.append(chr(int(text[i + 2:i + 6], 16))); i += 6
            elif n == 'x':
                m = re.match(r'[0-9a-fA-F]+', text[i + 2:])
                out_chars.append(chr(int(m.group(0), 16))); i += 2 + len(m.group(0))
            else:
                out_chars.append(n); i += 2
        else:
            out_chars.append(ch); i += 1
    return ''.join(out_chars)


def keep(text):
    if text in NOT_UI or not re.search(r'[A-Za-z]{2,}', text):
        return False
    if re.fullmatch(r'[a-z0-9_./:\-]+', text) or '://' in text[:10]:
        return False
    return True


seen, strings = set(), []


def add(text):
    if keep(text) and text not in seen:
        seen.add(text)
        strings.append(text)


for dirpath, _, files in os.walk(os.path.join(root, 'src')):
    for f in sorted(files):
        if f.endswith('.cpp'):
            src = open(os.path.join(dirpath, f), encoding='utf-8').read()
            for m in CALL.finditer(src):
                add(decode(m.group(1)))

for rel, pattern, _ in TABLES:
    src = open(os.path.join(root, rel), encoding='utf-8').read()
    m = re.search(pattern, src, re.S)
    assert m, (rel, pattern)
    for lit in re.finditer(LIT + r'(?:\s*' + LIT + r')*', m.group(1)):
        text = decode(lit.group(0))
        if ' ' in text or text[:1].isupper():
            add(text)

json.dump(strings, open(out, 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
print(len(strings), 'strings,', sum(len(s) for s in strings), 'characters')
