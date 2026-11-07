#!/usr/bin/env python3
"""Write iban_formats.tsv from schwifty's parsed copy of SWIFT's registry, at a fixed tag.

overwrite.json adds forms outside the registry (registered is empty for those) and
corrects positions. SWIFT refuses scripts, so its own text is saved by hand and is
not read yet; schwifty's copy is the only source until a person saves it (design 14).
"""
import json

from fingen_data import RAW, spec_classes, spec_pattern, survey, write_all_tiers

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


def main():
    formats, generated = load_formats()
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
        positions = entry.get("positions", {})

        def span(name):
            # schwifty writes an absent field as an empty span such as [0, 0].
            found = positions.get(name)
            return found if found and found[0] != found[1] else ["", ""]

        bank, branch, account, check = (span(n) for n in ("bank_code", "branch_code", "account_code",
                                                          "national_checksum_digits"))
        rows.append({"country_code": cc, "bban_pattern": spec_pattern(spec), "length": entry["iban_length"],
                     "bank_from": bank[0], "bank_to": bank[1], "branch_from": branch[0], "branch_to": branch[1],
                     "account_from": account[0], "account_to": account[1], "check_from": check[0], "check_to": check[1],
                     "check_builder": CHECK_BUILDERS[row["check"]], "sepa": "1" if entry.get("in_sepa_zone") else "",
                     "registered": "" if entry.get("aspirational") or cc not in generated else "1"})
    if problems:
        raise SystemExit("\n".join(problems))
    write_all_tiers("iban_formats.tsv", "SWIFT IBAN Registry release 99 through schwifty 2026.11.0's parsed copy (MIT); registered empty means a form in use outside the registry",
                    COLUMNS, rows)
    registered = sum(1 for r in rows if r["registered"])
    print(f"iban_formats.tsv: {len(rows)} formats, {registered} registered")
    if not (RAW / "iban_registry.txt").exists():
        print("note: raw_data/iban_registry.txt is not saved, so schwifty's copy is uncross-checked")


if __name__ == "__main__":
    main()
