#!/usr/bin/env python3
"""Write iban_formats.tsv from schwifty's parsed copy of SWIFT's registry, at a fixed tag.

overwrite.json adds forms outside the registry (registered is empty for those) and
corrects positions. SWIFT refuses scripts, so its own text is saved by hand and is
not read yet; schwifty's copy is the only source until a person saves it (design 14).
"""
import json
import re

from fingen_data import RAW, read_registry, spec_classes, spec_pattern, survey, write_all_tiers

# Where the table differs from SWIFT's text on purpose: schwifty separates a national check
# digit or a branch that the text leaves inside a field, and the check rules need them apart.
REGISTRY_DIFFERS = {("AL", "branch"): "the text's 5-digit branch holds a national check digit",
                    ("SI", "bank"): "the text's 5-digit bank is a 2-digit bank and a 3-digit branch"}

COLUMNS = ["country_code", "bban_pattern", "length", "bank_from", "bank_to", "branch_from", "branch_to",
           "account_from", "account_to", "check_from", "check_to", "check_builder", "sepa", "registered"]

# The survey's words for a national check, as the builder that computes it.
CHECK_BUILDERS = {
    "MOD 97 only": "",
    "97-(body00 mod 97), French letter values": "rib_key",
    "FR key": "rib_key",
    "MOD 97-10 on the BBAN": "mod97_bban",
    "MOD 97-10 variant": "mod97_variant",
    "IT CIN": "it_cin",
    "CZ rule": "cz",
    "RU key: 7-1-3 with the BIK": "ru_key",
    "per bank: method column": "de_method",
    "ES rule": "es",
    "PL rule": "pl",
    "BE rule": "be",
    "FI rule": "fi",
    "NO rule": "no",
    "EE rule": "ee",
    "IS rule": "is",
}


# A territory uses its parent's banks and national check, since its IBAN carries the same bank codes.
BANKS_FROM = {**dict.fromkeys("RE GP MQ YT GF PF NC MF WF BL PM TF".split(), "iban-fr"),
              **dict.fromkeys("JE IM GG".split(), "iban-gb"), "AX": "iban-fi", "GL": "iban-dk", "FO": "iban-dk"}
# The check digit inside a field the registry does not separate: the Russian key is the 9th digit of the
# 20-digit account after the 9-digit BIK; the Icelandic one is the 9th digit of the holder's kennitala.
CHECK_POSITIONS = {"RU": [17, 18], "IS": [20, 21]}
# UEMOA bank codes open with the country's code (design 4.3). It is literal, not two drawn characters.
COUNTRY_PREFIX = set("BF BJ CI ML NE SN TG".split())


def national_check(row, by_scheme):
    """The builder of a survey row's national check, a territory taking its parent's."""
    parent = BANKS_FROM.get(row["country"])
    return CHECK_BUILDERS[by_scheme[parent]["check"] if parent else row["check"]]


def load_formats():
    folder = RAW / "schwifty/iban_registry"
    generated = json.loads((folder / "generated.json").read_text(encoding="utf-8"))
    overwrite = json.loads((folder / "overwrite.json").read_text(encoding="utf-8"))
    merged = {cc: dict(entry) for cc, entry in generated.items()}
    for cc, entry in overwrite.items():
        base = merged.get(cc, {})
        # Positions merge field by field: an overwrite naming one position keeps the others.
        positions = {**base.get("positions", {}), **entry.get("positions", {})}
        merged[cc] = {**base, **entry, "positions": positions}
    return merged, generated


def expand(pattern):
    """A bban_pattern as one symbol per character, so #{4}#{6} equals #{10}."""
    return "".join(symbol * int(size) for symbol, size in re.findall(r"(.)\{(\d+)\}", pattern))


def position(text):
    """The text's 1-based inclusive `4-8` as the table's 0-based half-open pair; None when empty."""
    match = re.fullmatch(r"(\d+)\s*-\s*(\d+)", text)
    return (int(match[1]) - 1, int(match[2])) if match else None


def cross_check(rows):
    """Compare the table with SWIFT's own text, when it is saved. Any difference not listed fails."""
    registry = read_registry()
    if registry is None:
        print("note: raw_data/iban_registry.txt is not saved, so schwifty's copy is uncross-checked")
        return
    ours = {r["country_code"]: r for r in rows}
    # A territory is registered under its parent's entry: France's lists GF, GB's lists JE.
    listed = set(registry) | {code for entry in registry.values() for code in re.findall(r"\b[A-Z]{2}\b", entry["includes"])}
    problems = [f"{cc}: in SWIFT's text and not in the table" for cc in listed if cc not in ours]
    problems += [f"{cc}: marked registered and not in SWIFT's text" for cc, r in ours.items() if r["registered"] and cc not in listed]
    problems += [f"{cc}: in SWIFT's text and not marked registered" for cc in listed if cc in ours and not ours[cc]["registered"]]
    for cc, entry in registry.items():
        row = ours.get(cc)
        if row is None:
            continue
        spec = spec_pattern(entry["bban"])
        if expand(spec) != expand(row["bban_pattern"]):
            problems.append(f"{cc}: structure {entry['bban']} in the text, {row['bban_pattern']} here")
        if int(entry["length"]) != int(row["length"]):
            problems.append(f"{cc}: length {entry['length']} in the text, {row['length']} here")
        if entry["sepa"].lower().startswith("yes") != bool(row["sepa"]):
            problems.append(f"{cc}: SEPA {entry['sepa']} in the text, {row['sepa'] or 'no'} here")
        for field in ("bank", "branch"):
            theirs = position(entry[field])
            mine = (int(row[f"{field}_from"]), int(row[f"{field}_to"])) if row[f"{field}_from"] != "" else None
            if theirs and theirs != mine and (cc, field) not in REGISTRY_DIFFERS:
                problems.append(f"{cc}: {field} at {theirs} in the text, {mine} here")
    if problems:
        raise SystemExit("SWIFT's text disagrees:\n" + "\n".join(problems))
    print(f"cross-checked against SWIFT's text: {len(registry)} countries and {len(listed) - len(registry)} territories agree, "
          f"apart from {sorted(REGISTRY_DIFFERS)}")


def main():
    formats, generated = load_formats()
    by_scheme = {r["scheme"]: r for r in survey()}
    wanted = [r for r in survey() if r["klass"] in ("iban", "iban_unregistered", "territory")]
    rows, problems = [], []
    for row in sorted(wanted, key=lambda r: r["country"]):
        cc = row["country"]
        entry = formats.get(cc)
        if entry is None:
            problems.append(f"{cc}: no format in schwifty's registry")
            continue
        spec = entry["bban_spec"]
        if len(spec_classes(spec)) != entry["bban_length"] or entry["iban_length"] != entry["bban_length"] + 4:
            problems.append(f"{cc}: {spec} does not give {entry['bban_length']} and {entry['iban_length']}")
        positions = dict(entry.get("positions", {}))
        parent = BANKS_FROM.get(cc)
        if parent and formats[parent[-2:].upper()]["bban_spec"] == spec:
            # An overwrite may name only some of a territory's positions, so the parent's whole set wins.
            positions = {**positions, **formats[parent[-2:].upper()].get("positions", {})}
        if cc in CHECK_POSITIONS:
            positions["national_checksum_digits"] = CHECK_POSITIONS[cc]

        def span(name):
            # schwifty writes an absent field as an empty span such as [0, 0].
            found = positions.get(name)
            return found if found and found[0] != found[1] else ["", ""]

        bank, branch, account, check = (span(n) for n in ("bank_code", "branch_code", "account_code",
                                                          "national_checksum_digits"))
        pattern = spec_pattern(spec)
        if cc in COUNTRY_PREFIX:
            pattern = cc + pattern.split("{2}", 1)[1]
        rows.append({"country_code": cc, "bban_pattern": pattern, "length": entry["iban_length"],
                     "bank_from": bank[0], "bank_to": bank[1], "branch_from": branch[0], "branch_to": branch[1],
                     "account_from": account[0], "account_to": account[1], "check_from": check[0], "check_to": check[1],
                     "check_builder": national_check(row, by_scheme), "sepa": "1" if entry.get("in_sepa_zone") else "",
                     "registered": "" if entry.get("aspirational") or cc not in generated else "1"})
    if problems:
        raise SystemExit("\n".join(problems))
    write_all_tiers("iban_formats.tsv", "SWIFT IBAN Registry release 99 through schwifty 2026.11.0's parsed copy (MIT); registered empty means a form in use outside the registry",
                    COLUMNS, rows)
    registered = sum(1 for r in rows if r["registered"])
    print(f"iban_formats.tsv: {len(rows)} formats, {registered} registered")
    cross_check(rows)


if __name__ == "__main__":
    main()
