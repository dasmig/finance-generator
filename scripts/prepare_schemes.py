#!/usr/bin/env python3
"""Write schemes.tsv: one row per survey scheme, with how it is built and how `safe` treats it.

IBAN rows take everything from the registry and the survey. Other schemes take their
parts from curated/schemes_national.tsv, which must match the survey key for key.
"""
import re

from fingen_data import CURATED, RAW, pattern_lengths, read_tsv, survey, write_all_tiers
from prepare_iban import BANKS_FROM, national_check

COLUMNS = ["key", "country", "builder", "display", "safe_method", "evidence", "bank_pattern", "branch_pattern",
           "pattern", "check", "list_complete", "banks_from", "params", "source"]

# Two countries have both forms, so a builder of their own (design 4.1).
IBAN_BUILDERS = {"BR": "iban_br", "RU": "iban_ru"}
# A check with its own builder outside the IBAN; the structure-only schemes have none.
NATIONAL_CHECKS = {"us_aba", "ng_nuban", "mx_clabe", "ar_cbu", "nz_account"}
COMMENT = "the account survey, designs/finance-generator-accounts.tsv; each row names its source"
# The everyday form of a country with both (design 4.1). Brazil: Stripe's table. Russia: the BIK and a
# 20-digit account, 40817 (a person's account) and 810 (roubles) from secondary sources; the 9th digit is the key.
EVERYDAY = {"BR": {"display": "{bank} {branch} {account}", "bank_pattern": "#{3}", "branch_pattern": "#{4}",
                   "pattern": "#{7,10}"},
            "RU": {"display": "{bank} {account}", "bank_pattern": "#{9}", "pattern": "40817810#{12}"}}


def complete_lists():
    """A country whose list schwifty generates from its authority's register, not by hand."""
    names = (p.name for p in (RAW / "schwifty/bank_registry").glob("generated_*.json"))
    return {re.match(r"generated_(\w\w)", n).group(1).upper() for n in names}


def safe_method(builder, check, has_test, complete):
    if has_test:
        return "test_value"
    if complete:
        return "unassigned_code"
    if check:
        return "wrong_national" if builder.startswith("iban") else "wrong_outer"
    if builder.startswith("iban"):
        return "wrong_outer"
    return "none"


def main():
    national = {r["scheme"]: r for r in read_tsv(CURATED / "schemes_national.tsv")}
    tests = {r["scheme"] for r in read_tsv(CURATED / "test_accounts.tsv")}
    complete = complete_lists()
    by_scheme = {r["scheme"]: r for r in survey()}
    rows, problems, used = [], [], set()
    for row in sorted(survey(), key=lambda r: r["scheme"]):
        key, cc, klass = row["scheme"], row["country"], row["klass"]
        common = {"key": key, "country": cc, "evidence": row["evidence"], "source": row["source"]}
        if klass in ("iban", "iban_unregistered", "territory"):
            builder = IBAN_BUILDERS.get(cc, "iban")
            check = national_check(row, by_scheme)
            listed = cc in complete
            rows.append({**common, "builder": builder, "display": "{iban}", **EVERYDAY.get(cc, {}), "check": check,
                         "list_complete": "1" if listed else "", "banks_from": BANKS_FROM.get(cc, ""),
                         "safe_method": safe_method(builder, check, key in tests, listed)})
        elif klass == "generic":
            rows.append({**common, "builder": "generic", "display": "{account}", "pattern": "#{10}",
                         "safe_method": "none"})
        else:
            parts = national.get(key)
            if parts is None:
                problems.append(f"{key}: not in curated/schemes_national.tsv")
                continue
            used.add(key)
            builder = parts["builder"]
            for field in ("bank_pattern", "branch_pattern", "account_pattern"):
                pattern_lengths(parts[field])
            check = builder if builder in NATIONAL_CHECKS else ""
            rows.append({**common, "builder": builder, "display": parts["display"], "check": check,
                         "bank_pattern": parts["bank_pattern"], "branch_pattern": parts["branch_pattern"],
                         "pattern": parts["account_pattern"], "params": parts.get("params", ""),
                         "safe_method": safe_method(builder, check, key in tests, False)})
    for extra in sorted(set(national) - used):
        problems.append(f"{extra}: in curated/schemes_national.tsv but not in the survey")
    if problems:
        raise SystemExit("\n".join(problems))
    write_all_tiers("schemes.tsv", COMMENT, COLUMNS, rows)
    print(f"schemes.tsv: {len(rows)} schemes")


if __name__ == "__main__":
    main()
