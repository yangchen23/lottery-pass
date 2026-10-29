import os
import re

root = "main"
chars = set()

literal = re.compile(r'"((?:[^"\\]|\\.)*)"')

for name in ("app_ui.c", "lottery.c", "main.c"):
    src = open(os.path.join(root, name), encoding="utf-8").read()
    for lit in literal.findall(src):
        for ch in lit:
            if ord(ch) >= 0x80:
                chars.add(ch)

for c in "·—…、。，：；（）％±":
    chars.add(c)

s = "".join(sorted(chars))
open("tools/symbols.txt", "w", encoding="utf-8").write(s)
print("non-ascii chars:", len(chars))
print(s)
