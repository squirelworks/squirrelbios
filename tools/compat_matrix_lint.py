#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Suite compatibility matrix honesty check (Phase G M17 / compat-matrix).

Parses release-notes/compat-matrix.md and verifies:

  - the suite table has BIOS / OS/2 / Path / QEMU / Check columns
  - every data row names a boot path class (legacy BIOS / CSM / UEFI)
  - published BIOS tags (no *(mark)* / *(planned)* annotation) have
    matching release-notes/vX.Y.Z.md
  - the OS/2 tag pinned in tests/os-boot.pin is cited on a row whose
    Check column mentions os-boot-sync
  - every release-notes/v*.md at or after v2.2.0 carries a
    Compatibility section (table or pointer at compat-matrix.md)

Cheap and deterministic: no QEMU, no network. --self-test covers
negative + positive fixtures.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MATRIX_PATH = Path("release-notes/compat-matrix.md")
PIN_PATH = Path("tests/os-boot.pin")
NOTES_DIR = Path("release-notes")

HEADING_RE = re.compile(r"^##\s+Suite matrix\s*$", re.MULTILINE)
COMPAT_HEADING_RE = re.compile(
    r"^##\s+Compatibility(?:\s*\([^)]*\))?\s*$", re.MULTILINE | re.IGNORECASE
)
TABLE_ROW_RE = re.compile(r"^\|(.+)\|\s*$")
TAG_RE = re.compile(r"\bv(\d+\.\d+\.\d+)\b")
PIN_TAG_RE = re.compile(
    r"(?:\*\*)?OS/2\s*`v(\d+\.\d+\.\d+)`(?:\*\*)?|\bOS2_TAG\b"
)
PATH_CLASS_RE = re.compile(
    r"legacy\s+BIOS|\bCSM\b|\bUEFI\b|\bBIOS\b", re.IGNORECASE
)
MARK_OR_PLANNED_RE = re.compile(r"\*\((?:mark|planned)[^)]*\)\*", re.IGNORECASE)
VERSION_NOTE_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)\.md$")


def load_pin_os2_tag(root: Path) -> str:
    pin = root / PIN_PATH
    if not pin.is_file():
        raise FileNotFoundError(f"missing pin file {PIN_PATH.as_posix()}")
    tag = ""
    for raw in pin.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        if key.strip() == "OS2_TAG":
            tag = val.strip()
    if not tag:
        raise ValueError(f"{PIN_PATH.as_posix()}: missing OS2_TAG")
    return tag


def parse_table(md_text: str) -> tuple[list[str], list[list[str]]]:
    """Return (headers, rows) for the ## Suite matrix table."""
    m = HEADING_RE.search(md_text)
    if not m:
        raise ValueError("missing '## Suite matrix' heading")
    lines = md_text[m.end() :].splitlines()
    headers: list[str] | None = None
    rows: list[list[str]] = []
    for line in lines:
        if line.startswith("## "):
            break
        tm = TABLE_ROW_RE.match(line)
        if not tm:
            if headers is not None and rows:
                break
            continue
        cells = [c.strip() for c in tm.group(1).split("|")]
        if headers is None:
            headers = cells
            continue
        if all(re.fullmatch(r":?-{3,}:?", c.replace(" ", "")) for c in cells):
            continue
        rows.append(cells)
    if not headers:
        raise ValueError("suite matrix table missing header row")
    if not rows:
        raise ValueError("suite matrix table has no data rows")
    return headers, rows


def col_index(headers: list[str], *needles: str) -> int:
    lowered = [h.lower() for h in headers]
    for i, h in enumerate(lowered):
        if all(n.lower() in h for n in needles):
            return i
    raise ValueError(f"matrix missing column matching {needles!r}: {headers}")


def bios_tag_published(cell: str) -> tuple[str | None, bool]:
    """Return (vX.Y.Z or None, requires_release_note)."""
    m = TAG_RE.search(cell)
    if not m:
        return None, False
    tag = f"v{m.group(1)}"
    if MARK_OR_PLANNED_RE.search(cell):
        return tag, False
    return tag, True


def pinned_os2_in_cell(cell: str) -> str | None:
    """Return vX.Y.Z when the cell cites a concrete pinned OS/2 tag."""
    m = re.search(r"OS/2\s*`v(\d+\.\d+\.\d+)`", cell)
    if m:
        return f"v{m.group(1)}"
    m = re.search(r"`v(\d+\.\d+\.\d+)`\s*\(pinned\)", cell)
    if m:
        return f"v{m.group(1)}"
    return None


def lint_matrix(root: Path) -> list[str]:
    errors: list[str] = []
    matrix = root / MATRIX_PATH
    if not matrix.is_file():
        return [f"missing {MATRIX_PATH.as_posix()}"]

    try:
        pin_tag = load_pin_os2_tag(root)
    except (OSError, ValueError) as exc:
        return [str(exc)]

    text = matrix.read_text(encoding="utf-8")
    try:
        headers, rows = parse_table(text)
        i_bios = col_index(headers, "bios")
        i_os2 = col_index(headers, "os/2")
        i_path = col_index(headers, "path")
        i_qemu = col_index(headers, "qemu")
        i_check = col_index(headers, "check")
    except ValueError as exc:
        return [f"{MATRIX_PATH.as_posix()}: {exc}"]

    pin_cited = False
    for n, row in enumerate(rows, start=1):
        width = max(i_bios, i_os2, i_path, i_qemu, i_check) + 1
        if len(row) < width:
            errors.append(
                f"{MATRIX_PATH.as_posix()}: row {n}: expected ≥{width} cells, "
                f"got {len(row)}"
            )
            continue
        bios, os2, path, qemu, check = (
            row[i_bios],
            row[i_os2],
            row[i_path],
            row[i_qemu],
            row[i_check],
        )
        for label, cell in (
            ("BIOS tag", bios),
            ("OS/2 tag", os2),
            ("Path", path),
            ("QEMU machine", qemu),
            ("Check / script", check),
        ):
            if not cell:
                errors.append(
                    f"{MATRIX_PATH.as_posix()}: row {n}: empty {label} cell"
                )
        if path and not PATH_CLASS_RE.search(path):
            errors.append(
                f"{MATRIX_PATH.as_posix()}: row {n}: Path must name "
                f"legacy BIOS / CSM / UEFI (got {path!r})"
            )
        tag, need_note = bios_tag_published(bios)
        if need_note and tag:
            note = root / NOTES_DIR / f"{tag}.md"
            if not note.is_file():
                errors.append(
                    f"{MATRIX_PATH.as_posix()}: row {n}: published BIOS "
                    f"{tag} lacks {NOTES_DIR.as_posix()}/{tag}.md"
                )
        pinned = pinned_os2_in_cell(os2)
        if pinned and "os-boot-sync" in check:
            pin_cited = True
            if pinned != pin_tag:
                errors.append(
                    f"{MATRIX_PATH.as_posix()}: row {n}: pinned OS/2 "
                    f"{pinned} disagrees with {PIN_PATH.as_posix()} "
                    f"OS2_TAG={pin_tag}"
                )

    if not pin_cited:
        errors.append(
            f"{MATRIX_PATH.as_posix()}: no row cites pinned OS/2 "
            f"{pin_tag} on an os-boot-sync check "
            f"(see {PIN_PATH.as_posix()})"
        )
    return errors


def lint_release_notes(root: Path) -> list[str]:
    """Every v2.2.0+ release note needs a Compatibility section."""
    errors: list[str] = []
    notes = root / NOTES_DIR
    if not notes.is_dir():
        return [f"missing {NOTES_DIR.as_posix()}/"]
    for path in sorted(notes.glob("v*.md")):
        m = VERSION_NOTE_RE.match(path.name)
        if not m:
            continue
        major, minor, patch = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        if (major, minor, patch) < (2, 2, 0):
            continue
        text = path.read_text(encoding="utf-8")
        if COMPAT_HEADING_RE.search(text):
            continue
        if "compat-matrix.md" in text and re.search(
            r"(?i)compatib", text
        ):
            continue
        errors.append(
            f"{path.relative_to(root).as_posix()}: missing "
            f"'## Compatibility' section (or pointer to {MATRIX_PATH.as_posix()})"
        )
    return errors


def lint(root: Path) -> list[str]:
    return lint_matrix(root) + lint_release_notes(root)


def _write_fixture(base: Path, *, bad: bool) -> None:
    (base / "release-notes").mkdir(parents=True)
    (base / "tests").mkdir(parents=True)
    pin_tag = "v2.3.0"
    (base / "tests" / "os-boot.pin").write_text(
        f"OS2_TAG={pin_tag}\nOS2_TIP=deadbeef\n"
        "OS_IMG_SHA256=" + ("a" * 64) + "\n"
        "OS_IMG_URL=https://example.invalid/os.img\n",
        encoding="utf-8",
    )
    (base / "release-notes" / "v2.2.0.md").write_text(
        "# v2.2.0\n\n## Compatibility (suite matrix row)\n\n"
        "| BIOS | OS/2 | Path | QEMU | Checks |\n"
        "|------|------|------|------|--------|\n"
        "| v2.2.0 | tip | UEFI | `q35` | `x86_64-capsule` |\n",
        encoding="utf-8",
    )
    (base / "release-notes" / "v2.1.1.md").write_text(
        "# v2.1.1\n\npre-matrix note\n", encoding="utf-8"
    )
    if bad:
        # Missing Compatibility on v2.2.0 is already fixed above; break the
        # matrix instead: empty path + wrong pin citation + missing note.
        (base / "release-notes" / "compat-matrix.md").write_text(
            "# matrix\n\n## Suite matrix\n\n"
            "| BIOS tag | OS/2 tag (min) | Path | QEMU machine | Check / script |\n"
            "|----------|----------------|------|--------------|----------------|\n"
            "| v9.9.9 | tip | (none) | `q35` | `x86_64-capsule` |\n"
            "| v2.3.1 *(mark)* | **OS/2 `v1.0.0`** (pinned) | "
            "legacy BIOS | `pc` | `os-boot-sync` |\n",
            encoding="utf-8",
        )
        (base / "release-notes" / "v2.2.0.md").write_text(
            "# v2.2.0\n\nno compatibility section\n", encoding="utf-8"
        )
    else:
        (base / "release-notes" / "compat-matrix.md").write_text(
            "# matrix\n\n## Suite matrix\n\n"
            "| BIOS tag | OS/2 tag (min) | Path | QEMU machine | Check / script |\n"
            "|----------|----------------|------|--------------|----------------|\n"
            "| v2.1.1 | v2.1.0 / tip | legacy BIOS, CSM, UEFI | "
            "`pc` / `q35` | `tests/os-boot.sh` |\n"
            "| v2.2.0 | tip at cut | legacy BIOS, CSM, UEFI | `q35` | "
            "`x86_64-capsule` |\n"
            "| v2.3.1 *(mark; SQ_VERSION still 2.3.0)* | "
            "**OS/2 `v2.3.0`** (pinned) | legacy BIOS / CSM `os-boot` | "
            "`pc` / `q35` | `os-boot-sync`, `tests/os-boot.sh` |\n"
            "| v2.9.0 *(planned)* | OS/2 2.9.x | UEFI SB + TPM | "
            "`q35` + swtpm | `x86_64-m3-rehearsal` |\n",
            encoding="utf-8",
        )


def run_self_test() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="compat_matrix_selftest_"))
    try:
        bad = tmp / "bad"
        good = tmp / "good"
        _write_fixture(bad, bad=True)
        _write_fixture(good, bad=False)

        bad_errors = lint(bad)
        if not bad_errors:
            print(
                "compat_matrix self-test: expected failures on bad fixture",
                file=sys.stderr,
            )
            return 1
        joined = "\n".join(bad_errors)
        need = (
            "Path must name" in joined,
            "lacks release-notes/v9.9.9.md" in joined
            or "lacks release-notes/v9.9.9.md" in joined.replace("\\", "/"),
            "disagrees with" in joined or "OS2_TAG=" in joined,
            "missing '## Compatibility'" in joined,
        )
        # Path / missing note / pin mismatch / release-note section
        if sum(1 for x in need if x) < 3:
            print(
                "compat_matrix self-test: bad fixture missed classes:\n"
                + joined,
                file=sys.stderr,
            )
            return 1

        good_errors = lint(good)
        if good_errors:
            print(
                "compat_matrix self-test: good fixture failed:\n"
                + "\n".join(good_errors),
                file=sys.stderr,
            )
            return 1

        print(
            "compat_matrix self-test: ok "
            f"(bad={len(bad_errors)} issue(s), good clean)"
        )
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--root",
        type=Path,
        default=None,
        help="repository root (default: parent of tools/)",
    )
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="run negative + positive fixtures and exit",
    )
    args = ap.parse_args(argv)
    if args.self_test:
        return run_self_test()

    root = (args.root or ROOT).resolve()
    errors = lint(root)
    if errors:
        print(
            f"compat_matrix: {len(errors)} honesty issue(s)",
            file=sys.stderr,
        )
        for err in errors:
            print(err, file=sys.stderr)
        return 1
    print(f"compat_matrix: ok ({MATRIX_PATH.as_posix()}; pin + notes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
