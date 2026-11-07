"""Shared helpers for finance-generator's data build (stdlib only)."""
import csv
import re
from pathlib import Path

csv.field_size_limit(10**7)

SPEC_SYMBOL = {"n": "#", "a": "@", "c": "*"}

ROOT = Path(__file__).resolve().parent.parent
CURATED = ROOT / "scripts" / "curated"
RAW = ROOT / "raw_data"
RESOURCES = ROOT / "resources"
SIBLINGS = ROOT.parent
TIERS = ("lite", "full")


def read_tsv(path):
    """Rows of a TSV as dicts; `#` lines are comments, short rows are padded."""
    with open(path, encoding="utf-8", newline="") as f:
        lines = [line for line in f.read().split("\n") if line and not line.startswith("#")]
    header = lines[0].rstrip("\r").split("\t")
    rows = []
    for line in lines[1:]:
        cells = line.rstrip("\r").split("\t")
        cells += [""] * (len(header) - len(cells))
        rows.append(dict(zip(header, cells)))
    return rows


def write_table(tier, name, comment, columns, rows):
    """Write resources/<tier>/<name> with the source in its first line."""
    path = RESOURCES / tier / name
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# {comment}\n")
        f.write("\t".join(columns) + "\n")
        for row in rows:
            cells = [str(row.get(c, "")) for c in columns]
            for cell in cells:
                if "\t" in cell or "\n" in cell:
                    raise ValueError(f"{name}: a control character in {cell!r}")
            f.write("\t".join(cells) + "\n")
    return path


def write_all_tiers(name, comment, columns, rows):
    for tier in TIERS:
        write_table(tier, name, comment, columns, rows)


def survey():
    """The account survey, one row per country and territory."""
    return read_tsv(CURATED / "survey_accounts.tsv")


def spec_parts(spec):
    """A registry specification as (count, class): "8!n2!a" gives (8, "n"), (2, "a")."""
    parts = [(int(n), c) for n, c in re.findall(r"(\d+)!([nace])", spec)]
    if "".join(f"{n}!{c}" for n, c in parts) != spec:
        raise ValueError(f"unreadable specification {spec!r}")
    return parts


def spec_pattern(spec):
    """The core pattern for a registry specification: "8!n10!n" gives "#{8}#{10}"."""
    return "".join(f"{SPEC_SYMBOL[c]}{{{n}}}" for n, c in spec_parts(spec))


def spec_classes(spec):
    """One class letter per position, to test a bank code against its place."""
    return "".join(c * n for n, c in spec_parts(spec))


def fits(code, classes):
    """Whether a code has exactly the length and the character classes given."""
    if len(code) != len(classes):
        return False
    test = {"n": str.isdigit, "a": lambda ch: "A" <= ch <= "Z", "c": lambda ch: ch.isdigit() or "A" <= ch <= "Z",
            "e": str.isspace}
    return all(ch.isascii() and test[cls](ch) for ch, cls in zip(code, classes))


def pattern_lengths(pattern):
    """The shortest and longest text a core pattern of # @ * {n} {n,m} and literal capitals or digits
    can give; ValueError otherwise."""
    if not pattern:
        return 0, 0
    tokens = re.findall(r"[#@*A-Z0-9](?:\{\d+(?:,\d+)?\})?", pattern)
    if "".join(tokens) != pattern:
        raise ValueError(f"unreadable pattern {pattern!r}")
    low = high = 0
    for token in tokens:
        counts = re.findall(r"\d+", token) or ["1"]
        low += int(counts[0])
        high += int(counts[-1])
    return low, high
