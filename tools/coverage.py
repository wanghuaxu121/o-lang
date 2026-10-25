#!/usr/bin/env python3
"""规范 ↔ 测试 覆盖矩阵。

从 SPEC.md 提取「可测语义条目」（§3–§12 的 ### N.M；以及没有子节的 ## N），
从 examples/ tests/ 的用例里提取 `SPEC §N.M` 标签，输出覆盖率与缺口。
"""
import re, sys, pathlib, glob

root = pathlib.Path(__file__).resolve().parent.parent
spec = root / "SPEC.md"
text = spec.read_text(encoding="utf-8")

h3 = re.findall(r"^### (\d+\.\d+)\s", text, re.M)
h2 = re.findall(r"^## (\d+)\.\s", text, re.M)
parents = {x.split(".")[0] for x in h3}
# 可测条目 = 所有 N.M；外加没有子节的 N（§10 语义表、§11 EBNF 属于文档，排除）
in_scope = lambda s: 3 <= int(s.split(".")[0]) <= 12
testable = sorted((set(h3) | {n for n in h2 if n not in parents and n not in {"10", "11"}}),
                  key=lambda s: [int(x) for x in s.split(".")])
# 纯文档性条目：定义/目标/差异说明，无法用用例验证
PROSE = {"4.2", "9.1"}
testable = [s for s in testable if in_scope(s) and s not in PROSE]

tags = {}
for f in sorted(glob.glob(str(root / "examples" / "*.o")) +
                glob.glob(str(root / "tests" / "*" / "*.o"))):
    for m in re.findall(r"SPEC §(\d+(?:\.\d+)?)", pathlib.Path(f).read_text(encoding="utf-8", errors="replace")):
        tags.setdefault(m, []).append(str(pathlib.Path(f).relative_to(root)))

covered = [s for s in testable if s in tags]
missing = [s for s in testable if s not in tags]
untagged = [f for f in glob.glob(str(root / "examples" / "*.o")) +
                      glob.glob(str(root / "tests" / "*" / "*.o"))
            if not re.search(r"SPEC §\d", pathlib.Path(f).read_text(encoding="utf-8", errors="replace"))]

print("== 规范 ↔ 测试 覆盖 ==")
print(f"  可测条目: {len(testable)}   已覆盖: {len(covered)}   覆盖率: {len(covered)/len(testable)*100:.0f}%")
print()
print("  未覆盖:" + (" 无" if not missing else ""))
for s in missing:
    print(f"    §{s}")
print()
print(f"  未标注用例: {len(untagged)} 个" + ("" if not untagged else ""))
for f in untagged:
    print("    " + str(pathlib.Path(f).relative_to(root)))
