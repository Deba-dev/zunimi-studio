"""Lists the English lines the app looks up that resources/i18n/<lang>.json doesn't translate yet, and checks the
translations it has (same %1 placeholders, HTML tags, newlines and edge spaces as the English).

usage: python tools/i18n/missing.py [lang]      (default hi; run from the repo root)
Translate by adding "English": "translation" pairs to resources/i18n/<lang>.json. An empty translation is allowed:
sentence pieces like "The " can have no word of their own. Keep names, units and keywords out of the file
(they are listed in tools/i18n/keep_english.json).
"""
import json
import os
import re
import subprocess
import sys
import tempfile

sys.stdout.reconfigure(encoding='utf-8')
lang = sys.argv[1] if len(sys.argv) > 1 else 'hi'
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
with tempfile.TemporaryDirectory() as tmp:
    out = os.path.join(tmp, 'en.json')
    subprocess.run([sys.executable, os.path.join(root, 'tools', 'i18n', 'extract.py'), root, out], check=True,
                   stdout=subprocess.DEVNULL)
    english = json.load(open(out, encoding='utf-8'))
table = json.load(open(os.path.join(root, 'resources', 'i18n', lang + '.json'), encoding='utf-8'))

tag = re.compile(r'</?\w+')
bad = 0
for en, tr in table.items():
    problems = []
    if sorted(re.findall(r'%\d', en)) != sorted(re.findall(r'%\d', tr)):
        problems.append('placeholders')
    if sorted(tag.findall(en)) != sorted(tag.findall(tr)):
        problems.append('html tags')
    if en.count('\n') != tr.count('\n'):
        problems.append('newlines')
    if tr and (en[:1].isspace() != tr[:1].isspace() or en[-1:].isspace() != tr[-1:].isspace()):
        problems.append('edge spaces')
    if problems:
        bad += 1
        print('CHECK', ', '.join(problems), json.dumps(en, ensure_ascii=False)[:90])
# names, units, formats and keywords that stay as they are in every language
keep = set(json.load(open(os.path.join(root, 'tools', 'i18n', 'keep_english.json'), encoding='utf-8')))
missing = [en for en in english if en not in table and en not in keep]
for en in missing:
    print('MISSING', json.dumps(en, ensure_ascii=False))
print(f'{len(english) - len(missing)} of {len(english)} translated, {len(missing)} missing, {bad} to check')
