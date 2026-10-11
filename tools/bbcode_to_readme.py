"""Generates release/AutoLockpick_README.txt (plain text, the readme inside the zip) from
release/nexus_description.txt, so the two say the same thing. tools/package.ps1 runs it. From TrueFOV.

The changelog's first entry becomes {VERSION}, which package.ps1 fills in.
"""
import os
import re
import textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIDTH = 96

src = open(os.path.join(ROOT, "release", "nexus_description.txt"), encoding="utf-8").read().replace("\r\n", "\n")


def link(m):
    url, text = m.group(1), m.group(2)
    if not text.strip():
        return ""
    bare = re.sub(r"^https?://(www\.)?", "", url).rstrip("/")
    return text if text == bare or text == url else "%s (%s)" % (text, url)


def inline(s):
    s = re.sub(r"\[img[^\]]*\].*?\[/img\]", "", s)
    s = re.sub(r"\[url=([^\]]+)\](.*?)\[/url\]", link, s)
    s = re.sub(r"\[font=[^\]]*\]|\[/font\]", "", s)
    s = re.sub(r"\[color=[^\]]*\]|\[/color\]", "", s)
    s = re.sub(r"\[size=\d+\]|\[/size\]", "", s)
    s = re.sub(r"\[/?(b|i|u|center)\]", "", s)
    return s.strip()


def wrap(text, first, rest):
    return textwrap.fill(text, WIDTH, initial_indent=first, subsequent_indent=rest,
                         break_long_words=False, break_on_hyphens=False)


lines = src.split("\n")
out = []
# Title block: the first two lines.
title = inline(lines[0]) + " {VERSION}"
subtitle = inline(lines[1])
out += [title, subtitle, "=" * max(len(title), len(subtitle)), ""]

section = None
numbered = 0
changelog_first = True
i = 2
while i < len(lines):
    line = lines[i].strip()
    i += 1
    if line in ("", "[line]"):
        if out and out[-1] != "":
            out.append("")
        continue
    m = re.fullmatch(r"\[size=5\]\[b\](.*)\[/b\]\[/size\]", line)
    if m:  # section heading
        section = m.group(1)
        while out and out[-1] == "":
            out.pop()
        name = section.upper()
        out += ["", "", name, "-" * len(name)]
        continue
    if line in ("[list]", "[list=1]"):
        numbered = 1 if line == "[list=1]" else 0
        continue
    if line == "[/list]":
        numbered = 0
        if section != "Changelog":
            out.append("")  # a paragraph straight after a list reads better apart from it
        continue
    m = re.fullmatch(r"\[\*\](.*?)(\[/\*\])?", line)
    if m:  # list item
        item = m.group(1)
        if section == "Controls":
            km = re.fullmatch(r"\[b\](.*?):?\[/b\]:? (.*)", item)
            out.append("  %-22s%s" % (inline(km.group(1)), inline(km.group(2))))
        elif section == "Changelog":
            vm = re.fullmatch(r"\[b\](.*?):\[/b\] (.*)", item)
            out.append("{VERSION}" if changelog_first else inline(vm.group(1)))
            changelog_first = False
            out.append(wrap(inline(vm.group(2)), "  - ", "    "))
        elif numbered:
            out.append(wrap(inline(item), "%d. " % numbered, "   "))
            numbered += 1
        else:
            out.append(wrap(inline(item), "- ", "  "))
        continue
    m = re.fullmatch(r"\[b\](.*)\[/b\]", line)
    if m:  # a bold line on its own: a sub-heading
        text = inline(m.group(1))
        out.append(text + ("" if text.endswith(":") else ":"))
        continue
    text = inline(line)
    if text:
        out.append(wrap(text, "", ""))

while out and out[-1] == "":
    out.pop()
text = "\n".join(out) + "\n"
open(os.path.join(ROOT, "release", "AutoLockpick_README.txt"), "w", encoding="utf-8", newline="").write(text)
print("Wrote release/AutoLockpick_README.txt from release/nexus_description.txt")
