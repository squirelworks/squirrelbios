#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Docs honesty linter for SquirrelBIOS (CONTINUANCE §3c).

Scan ROADMAP.md, TODO.md and HANDOFF.md for checkbox lines that name a
smoke check or make-test gate. Every checked box that names a check must
find that check in the harness; every open box that names a check must
not find it yet.

A box "names a check" when its text contains:
  - a backtick token that is a known smoke case name (from tests/smoke.sh),
  - a smoke-shaped id used with claim language (Smoke-test, "case",
    "`name` smoke", or "Test:"), even if the case is not in the harness yet,
  - a harness path under tests/ (script or helper), or
  - a make-test gate: make test, make linux-*, make f4-scale, make os-boot*,
    make fuzz-fat, make fuzz-pe.

Wrapped checkbox bodies (indented continuation lines) are joined so a
Test:/smoke reference on the next line still counts.

Harness inventory is built from tests/smoke.sh (run / KBD_NAME / VBE_NAME /
tables_check / PASS lines), tests/* helpers, and Makefile phony test
targets. Historical mismatches that cannot be fixed cheaply go in
tools/docs_lint_allowlist.txt.

Cheap and deterministic: no QEMU, no network.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC_NAMES = ("ROADMAP.md", "TODO.md", "HANDOFF.md")
ALLOWLIST_PATH = ROOT / "tools" / "docs_lint_allowlist.txt"

BOX_RE = re.compile(r"^(\s*)- \[([ xX])\]\s+(.*)$")
BACKTICK_RE = re.compile(r"`([^`]+)`")

# Harness paths and make-test gates named in docs.
TESTS_PATH_RE = re.compile(
    r"^(?:\.\/)?tests/(?:[\w./-]+\.(?:sh|py|asm|S|c)|[\w./-]+)$"
)
MAKE_GATE_RE = re.compile(
    r"^make\s+(?:test|linux[\w-]*|f4-scale|os-boot[\w-]*|fuzz-(?:fat|pe))$"
)
BARE_MAKE_GATE_RE = re.compile(
    r"(?<![`\w])make\s+(?:test|linux[\w-]*|f4-scale|os-boot[\w-]*|fuzz-(?:fat|pe))(?![`\w-])"
)
# Smoke case ids look like x86-boot / x86_64-uefi-sol / rpi4-uefi-sd.
# Artifact names (floppy.img, os.img) are not case ids.
SMOKE_ID_RE = re.compile(
    r"^[A-Za-z][A-Za-z0-9]*(?:[._-][A-Za-z0-9]+)+$"
)
ARTIFACT_SUFFIX_RE = re.compile(
    r"\.(?:img|iso|efi|bin|fd|rom|py|sh|asm|S|c|md|txt|cfg|json)$",
    re.IGNORECASE,
)
# Ways a missing smoke-shaped id is still claimed as a check.
SMOKE_CLAIM_RE = re.compile(
    r"`([^`]+)`\s+smoke\b"
    r"|\b(?:new\s+)?case\s+`([^`]+)`"
    r"|`([^`]+)`\s+case\b"
    r"|\bSmoke-test\b",
    re.IGNORECASE,
)
LEADING_SMOKE_TEST_RE = re.compile(
    r"^`([^`]+)`\s+[—-]\s+Smoke-test\b",
    re.IGNORECASE,
)

# How cases are declared inside tests/smoke.sh.
RUN_RE = re.compile(r"^\s*run\s+([A-Za-z0-9][\w.-]*)", re.MULTILINE)
NAME_ASSIGN_RE = re.compile(
    r"(?:KBD_NAME|VBE_NAME)=([A-Za-z0-9][\w.-]*)"
)
TABLES_CHECK_RE = re.compile(
    r"^\s*tables_check\s+([A-Za-z0-9][\w.-]*)", re.MULTILINE
)
PASS_LIT_RE = re.compile(r'echo\s+"PASS\s+([^"$]+)"')
MAKE_TARGET_RE = re.compile(r"^([A-Za-z_][\w-]*)\s*:[^=]", re.MULTILINE)

# Makefile targets that count as the make-test / optional gate surface.
GATE_TARGET_RE = re.compile(
    r"^(?:test|linux[\w-]*|f4-scale|os-boot[\w-]*|fuzz-(?:fat|pe))$"
)


def repo_rel(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def load_allowlist(path: Path) -> set[tuple[str, str, str]]:
    """Return set of (rule, doc, check) triples.

    Allowlist line format (whitespace-separated):
      <rule> <doc> <check>
    where rule is missing-check or unexpected-check.
    Lines starting with # and blank lines are ignored.
    """
    out: set[tuple[str, str, str]] = set()
    if not path.is_file():
        return out
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) != 3:
            raise SystemExit(
                f"docs_lint: bad allowlist line in {path}: {raw!r}"
            )
        rule, doc, check = parts
        if rule not in ("missing-check", "unexpected-check"):
            raise SystemExit(
                f"docs_lint: unknown allowlist rule {rule!r} in {path}"
            )
        out.add((rule, doc, check))
    return out


def collect_smoke_cases(smoke_sh: Path) -> set[str]:
    """Case names that tests/smoke.sh can PASS."""
    names: set[str] = set()
    if not smoke_sh.is_file():
        return names
    text = smoke_sh.read_text(encoding="utf-8", errors="replace")
    for m in RUN_RE.finditer(text):
        names.add(m.group(1))
    for m in NAME_ASSIGN_RE.finditer(text):
        names.add(m.group(1))
    for m in TABLES_CHECK_RE.finditer(text):
        names.add(m.group(1))
    for m in PASS_LIT_RE.finditer(text):
        # "PASS  name" or "PASS  name (detail)" — take the first token.
        first = m.group(1).strip().split()[0]
        if re.match(r"^[A-Za-z0-9][\w.-]*$", first):
            names.add(first)
    return names


def collect_harness(root: Path) -> dict[str, set[str]]:
    """Build smoke names, make gates, and tests/ paths."""
    cases = collect_smoke_cases(root / "tests" / "smoke.sh")
    targets: set[str] = set()
    paths: set[str] = set()

    makefile = root / "Makefile"
    if makefile.is_file():
        text = makefile.read_text(encoding="utf-8", errors="replace")
        for m in MAKE_TARGET_RE.finditer(text):
            name = m.group(1)
            if GATE_TARGET_RE.match(name):
                targets.add(name)
                targets.add(f"make {name}")
        # Root make test is always the suite gate when the target exists.
        if "test" in targets or re.search(r"^test\s*:", text, re.M):
            targets.add("test")
            targets.add("make test")

    tests_dir = root / "tests"
    if tests_dir.is_dir():
        paths.add("tests")
        paths.add("tests/")
        paths.add("tests/smoke.sh")
        for p in tests_dir.rglob("*"):
            if not p.is_file():
                continue
            if p.suffix not in (".sh", ".py", ".asm", ".S", ".c"):
                continue
            rel = repo_rel(p)
            paths.add(rel)
            paths.add(p.name)
            # Parent dirs under tests/ (e.g. tests/sb).
            for parent in p.parents:
                if parent == root:
                    break
                paths.add(repo_rel(parent))

    return {"cases": cases, "targets": targets, "paths": paths}


def is_harness_token(token: str, harness: dict[str, set[str]]) -> bool:
    """True when a backtick token is inventory/path/make shaped."""
    t = token.strip().rstrip(".,;)")
    if not t:
        return False
    if MAKE_GATE_RE.match(t):
        return True
    if t in harness["targets"]:
        return True
    if TESTS_PATH_RE.match(t) or t in harness["paths"]:
        return True
    # Path-shaped under tests/ even if not yet present (missing-check).
    if t.startswith("tests/") or t.startswith("./tests/"):
        return True
    if t in harness["cases"]:
        return True
    return False


def claimed_missing_smoke_ids(body: str) -> list[str]:
    """Smoke-shaped ids claimed by wording even when not yet in smoke.sh."""
    found: list[str] = []
    m = LEADING_SMOKE_TEST_RE.match(body.strip())
    if m:
        found.append(m.group(1))
    for m in SMOKE_CLAIM_RE.finditer(body):
        for g in m.groups():
            if g:
                found.append(g)
                break
    # After "Test:" / "Tests:", smoke-shaped backticks are case names.
    for m in re.finditer(r"\bTests?:\s*(.*)$", body, re.IGNORECASE):
        for tok in BACKTICK_RE.findall(m.group(1)):
            found.append(tok)
    out: list[str] = []
    seen: set[str] = set()
    for raw in found:
        name = raw.strip().rstrip(".,;)")
        if not name or name in seen:
            continue
        if not SMOKE_ID_RE.match(name) or ARTIFACT_SUFFIX_RE.search(name):
            continue
        seen.add(name)
        out.append(name)
    return out


def extract_named_checks(body: str, harness: dict[str, set[str]]) -> list[str]:
    """Unique check names named by a checkbox body, in order."""
    found: list[str] = []
    seen: set[str] = set()

    def add(name: str) -> None:
        name = name.strip().rstrip(".,;)")
        if not name or name in seen:
            return
        seen.add(name)
        found.append(name)

    for tok in BACKTICK_RE.findall(body):
        if is_harness_token(tok, harness):
            add(tok)

    for tok in claimed_missing_smoke_ids(body):
        add(tok)

    for m in BARE_MAKE_GATE_RE.finditer(body):
        add(m.group(0))

    return found


def parse_boxes(doc: Path, harness: dict[str, set[str]]) -> list[dict]:
    """Parse checkbox lines, joining indented wrap continuations into body."""
    boxes: list[dict] = []
    lines = doc.read_text(encoding="utf-8").splitlines()
    i = 0
    while i < len(lines):
        m = BOX_RE.match(lines[i])
        if not m:
            i += 1
            continue
        indent = m.group(1)
        checked = m.group(2).lower() == "x"
        body_parts = [m.group(3)]
        j = i + 1
        while j < len(lines):
            nxt = lines[j]
            if BOX_RE.match(nxt):
                break
            if not nxt.strip():
                break
            # Same or deeper indent, not a markdown heading.
            if nxt.startswith(indent + " ") or nxt.startswith(indent + "\t"):
                if nxt.lstrip().startswith("#"):
                    break
                body_parts.append(nxt.strip())
                j += 1
                continue
            break
        body = " ".join(body_parts)
        checks = extract_named_checks(body, harness)
        if checks:
            boxes.append(
                {
                    "doc": repo_rel(doc),
                    "lineno": i + 1,
                    "checked": checked,
                    "body": body,
                    "checks": checks,
                }
            )
        i = j
    return boxes


def check_exists(name: str, harness: dict[str, set[str]], root: Path) -> bool:
    n = name.strip().rstrip(".,;)")
    if n.startswith("./"):
        n = n[2:]
    if n in harness["cases"] or n in harness["targets"]:
        return True
    if n in harness["paths"]:
        return True
    if MAKE_GATE_RE.match(n):
        return n[5:] in harness["targets"] or n in harness["targets"]
    candidate = root / n
    if candidate.exists():
        return True
    # Basename match for scripts named without a tests/ prefix.
    base = Path(n).name
    if any(Path(p).name == base for p in harness["paths"]):
        return True
    return False


def lint(
    root: Path,
    allowlist: set[tuple[str, str, str]],
) -> tuple[list[str], list[str]]:
    """Return (errors, notes). Non-empty errors means failure."""
    harness = collect_harness(root)
    errors: list[str] = []
    notes: list[str] = []
    boxes: list[dict] = []
    for name in DOC_NAMES:
        doc = root / name
        if doc.is_file():
            boxes.extend(parse_boxes(doc, harness))

    used_allow: set[tuple[str, str, str]] = set()

    for box in boxes:
        for check in box["checks"]:
            exists = check_exists(check, harness, root)
            if box["checked"]:
                if exists:
                    continue
                key = ("missing-check", box["doc"], check)
                if key in allowlist:
                    used_allow.add(key)
                    notes.append(
                        f"allowlist missing-check {box['doc']}:{box['lineno']}"
                        f" `{check}`"
                    )
                    continue
                errors.append(
                    f"{box['doc']}:{box['lineno']}: checked box names `{check}` "
                    f"but that check is not in the smoke/make-test harness"
                )
            else:
                if not exists:
                    continue
                key = ("unexpected-check", box["doc"], check)
                if key in allowlist:
                    used_allow.add(key)
                    notes.append(
                        f"allowlist unexpected-check {box['doc']}:{box['lineno']}"
                        f" `{check}`"
                    )
                    continue
                errors.append(
                    f"{box['doc']}:{box['lineno']}: open box names `{check}` "
                    f"but that check already exists in the smoke/make-test harness"
                )

    for rule, doc, check in sorted(allowlist - used_allow):
        notes.append(f"unused allowlist entry: {rule} {doc} {check}")

    return errors, notes


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--root",
        type=Path,
        default=ROOT,
        help="repository root (default: parent of tools/)",
    )
    ap.add_argument(
        "--allowlist",
        type=Path,
        default=None,
        help="allowlist file (default: tools/docs_lint_allowlist.txt)",
    )
    ap.add_argument(
        "--list-checks",
        action="store_true",
        help="print named checks from docs and exit 0",
    )
    ap.add_argument(
        "--list-harness",
        action="store_true",
        help="print smoke case inventory size and exit 0",
    )
    args = ap.parse_args(argv)
    root = args.root.resolve()
    allow_path = args.allowlist or (root / "tools" / "docs_lint_allowlist.txt")
    allowlist = load_allowlist(allow_path)
    harness = collect_harness(root)

    if args.list_harness:
        print(
            f"docs_lint: {len(harness['cases'])} smoke case(s), "
            f"{len(harness['targets'])} gate target(s), "
            f"{len(harness['paths'])} tests path(s)"
        )
        return 0

    if args.list_checks:
        for name in DOC_NAMES:
            doc = root / name
            if not doc.is_file():
                continue
            for box in parse_boxes(doc, harness):
                state = "x" if box["checked"] else " "
                checks = ", ".join(f"`{c}`" for c in box["checks"])
                print(f"{box['doc']}:{box['lineno']}: [{state}] {checks}")
        return 0

    errors, notes = lint(root, allowlist)
    for n in notes:
        print(f"note: {n}", file=sys.stderr)
    if errors:
        for e in errors:
            print(e, file=sys.stderr)
        print(
            f"docs_lint: {len(errors)} honesty issue(s)",
            file=sys.stderr,
        )
        return 1

    nboxes = 0
    for name in DOC_NAMES:
        doc = root / name
        if doc.is_file():
            nboxes += len(parse_boxes(doc, harness))
    print(f"docs_lint: ok ({nboxes} box(es) naming checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
