#!/usr/bin/env python3
"""Check every table in both tiers; print what a person must read after a data change.

Fails on a pattern that does not parse, an IBAN length that disagrees, a bank code
that does not fit, a test number failing its checks, or a high-income credit share of
zero. Prints the top bank weights of the largest schemes, the population served by a
real scheme, and the earliest published test expiry, which validators cannot judge.
"""
import re
import sys

from fingen_data import RESOURCES, SIBLINGS, TIERS, fits, pattern_lengths, read_tsv

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
BUILDERS = {"iban", "iban_br", "iban_ru", "national", "bic_account", "generic", "us_aba", "ng_nuban", "mx_clabe",
            "ar_cbu", "nz_account"}
SAFE_METHODS = {"test_value", "unassigned_code", "wrong_national", "wrong_outer", "none"}
CHECK_BUILDERS = {"", "rib_key", "mod97_bban", "mod97_variant", "it_cin", "cz", "ru_key", "de_method", "es", "pl",
                  "be", "fi", "no", "ee", "is"}
GERMAN_METHODS = {"09", "63", "00", "88", "06", "13", "10", "76", "32", "28",
                  "34", "99", "20", "01", "61", "38", "91", "24", "33", "60"}
ABA_PREFIXES = set(range(0, 13)) | set(range(21, 33)) | set(range(61, 73)) | {80}
TOP_SCHEMES = 20
problems = []


def fail(message):
    problems.append(message)


def luhn(number):
    total = 0
    for i, ch in enumerate(reversed(number)):
        digit = int(ch)
        if i % 2:
            digit = digit * 2 - 9 if digit > 4 else digit * 2
        total += digit
    return total % 10 == 0


def iban_ok(iban):
    text = iban[4:] + iban[:4]
    return int("".join(str(int(ch, 36)) for ch in text)) % 97 == 1


def aba_ok(code):
    d = [int(ch) for ch in code]
    return (int(code[:2]) in ABA_PREFIXES
            and (3 * (d[0] + d[3] + d[6]) + 7 * (d[1] + d[4] + d[7]) + d[2] + d[5] + d[8]) % 10 == 0)


def expand(pattern):
    """A pattern with fixed counts as class letters, or None when a count varies."""
    out = ""
    for symbol, low, high in re.findall(r"([#@*A-Z0-9])(?:\{(\d+)(?:,(\d+))?\})?", pattern):
        if high:
            return None
        out += {"#": "n", "@": "a", "*": "c"}.get(symbol, "a" if symbol.isalpha() else "n") * int(low or 1)
    return out


def in_range(number, row):
    size = len(row["range_from"])
    return int(row["range_from"]) <= int(number[:size]) <= int(row["range_to"])


def check_tier(tier):
    folder = RESOURCES / tier
    countries = read_tsv(folder / "countries.tsv")
    schemes = {r["key"]: r for r in read_tsv(folder / "schemes.tsv")}
    formats = {r["country_code"]: r for r in read_tsv(folder / "iban_formats.tsv")}
    banks = read_tsv(folder / "banks.tsv")

    for row in countries:
        if row["primary_scheme"] not in schemes:
            fail(f"{tier} countries: {row['country_code']} names an unknown scheme")
        if not re.fullmatch(r"[A-Z]{3}", row["currency"]):
            fail(f"{tier} countries: {row['country_code']} currency {row['currency']}")

    for key, row in schemes.items():
        if row["builder"] not in BUILDERS:
            fail(f"{tier} schemes: {key} builder {row['builder']}")
        if row["safe_method"] not in SAFE_METHODS:
            fail(f"{tier} schemes: {key} safe method {row['safe_method']}")
        if row["banks_from"] and row["banks_from"] not in schemes:
            fail(f"{tier} schemes: {key} takes banks from an unknown scheme")
        try:
            for field in ("bank_pattern", "branch_pattern", "pattern"):
                pattern_lengths(row[field])
        except ValueError as e:
            fail(f"{tier} schemes: {key}: {e}")
        if row["builder"].startswith("iban") and row["country"] not in formats:
            fail(f"{tier} schemes: {key} has no IBAN format")
        if row["safe_method"] == "unassigned_code" and not row["list_complete"]:
            fail(f"{tier} schemes: {key} draws an unassigned code from a list that is not complete")

    for cc, row in formats.items():
        low, high = pattern_lengths(row["bban_pattern"])
        if low != high or low + 4 != int(row["length"]):
            fail(f"{tier} iban_formats: {cc} pattern gives {low}-{high}, length says {row['length']}")
        if row["check_builder"] not in CHECK_BUILDERS:
            fail(f"{tier} iban_formats: {cc} check {row['check_builder']}")
        for start, end in (("bank_from", "bank_to"), ("branch_from", "branch_to"), ("account_from", "account_to"),
                           ("check_from", "check_to")):
            if row[start] and not 0 <= int(row[start]) < int(row[end]) <= low:
                fail(f"{tier} iban_formats: {cc} {start} {row[start]}..{row[end]} is outside {low}")
        if row["check_builder"] in ("rib_key", "mod97_variant") and not row["check_from"]:
            fail(f"{tier} iban_formats: {cc} has a check with no position")

    seen = set()
    for row in banks:
        scheme = schemes.get(row["scheme"])
        code, identity = row["bank_code"], (row["scheme"], row["bank_code"])
        if scheme is None:
            fail(f"{tier} banks: unknown scheme {row['scheme']}")
            continue
        if identity in seen:
            fail(f"{tier} banks: {identity} twice")
        seen.add(identity)
        if not row["name"]:
            fail(f"{tier} banks: {identity} has no name")
        # Weight 0 marks a code that is assigned but never drawn, which only a complete list needs.
        if int(row["weight"]) < 0 or (int(row["weight"]) == 0 and not scheme["list_complete"]):
            fail(f"{tier} banks: {identity} weight {row['weight']}")
        if row["bic"] and len(row["bic"]) not in (8, 11):
            fail(f"{tier} banks: {identity} BIC {row['bic']}")
        builder = scheme["builder"]
        if builder.startswith("iban"):
            form = formats[scheme["country"]]
            classes = expand(form["bban_pattern"])[int(form["bank_from"]):int(form["bank_to"])]
            if not fits(code, classes):
                fail(f"{tier} banks: {identity} does not fit {classes}")
        elif scheme["bank_pattern"]:
            classes = expand(scheme["bank_pattern"])
            if classes and not fits(code, classes):
                fail(f"{tier} banks: {identity} does not fit {classes}")
        if row["method"] and scheme["country"] != "DE":
            fail(f"{tier} banks: {identity} has a method outside Germany")
        if scheme["country"] == "DE" and int(row["weight"]) and row["method"] not in GERMAN_METHODS:
            fail(f"{tier} banks: {identity} has no method")
        if builder == "us_aba" and not aba_ok(code):
            fail(f"{tier} banks: {identity} fails 3-7-1 or its prefix")

    networks = read_tsv(folder / "networks.tsv")
    by_network = {}
    for row in networks:
        by_network.setdefault(row["network"], []).append(row)
        if len(row["range_from"]) != len(row["range_to"]) or int(row["range_from"]) > int(row["range_to"]):
            fail(f"{tier} networks: {row['network']} {row['range_from']}-{row['range_to']}")
        for length in row["lengths"].split(","):
            if not 12 <= int(length) <= 19:
                fail(f"{tier} networks: {row['network']} length {length}")
        if int(row["length"]) not in map(int, row["lengths"].split(",")):
            fail(f"{tier} networks: {row['network']} default length is not among its lengths")
        if sum(map(int, row["format"].split("-"))) != int(row["length"]):
            fail(f"{tier} networks: {row['network']} format {row['format']} is not {row['length']} digits")
    expiries = []
    pairs = set()
    for row in read_tsv(folder / "test_cards.tsv"):
        number, network = row["number"], row["network"]
        if (row["source"], number) in pairs:
            fail(f"{tier} test_cards: {row['source']} {number} twice")
        pairs.add((row["source"], number))
        if not luhn(number):
            fail(f"{tier} test_cards: {number} fails Luhn")
        if not any(in_range(number, r) for r in by_network[network]):
            fail(f"{tier} test_cards: {number} is outside {network}")
        if row["expires"]:
            expiries.append((row["expires"], row["source"], number))
    for row in read_tsv(folder / "test_accounts.tsv"):
        number = row["number"]
        if row["scheme"] not in schemes:
            fail(f"{tier} test_accounts: unknown scheme {row['scheme']}")
        elif row["scheme"].startswith("iban-"):
            form = formats[schemes[row["scheme"]]["country"]]
            if len(number) != int(form["length"]) or not iban_ok(number):
                fail(f"{tier} test_accounts: {number} fails its IBAN checks")
        elif row["scheme"] == "us-aba" and not aba_ok(row["bank_code"]):
            fail(f"{tier} test_accounts: {number} fails 3-7-1")

    income = {r["cca2"]: r["income_level"] for r in
              read_tsv(SIBLINGS / "country-generator/resources/full/countries.tsv")}
    for row in read_tsv(folder / "ownership.tsv"):
        for field in ("account", "debit", "credit"):
            if not 0 <= float(row[field]) <= 1:
                fail(f"{tier} ownership: {row['country_code']} {field} {row[field]}")
        if "high" in income.get(row["country_code"], "").lower() and float(row["credit"]) <= 0:
            fail(f"{tier} ownership: high-income {row['country_code']} has a credit share of zero")
    return countries, schemes, banks, expiries


def report(countries, schemes, banks, expiries):
    population = {r["primary_scheme"]: int(r["population"] or 0) for r in countries if not r["territory"]}
    total = sum(population.values())
    named = {r["scheme"] for r in banks}

    def has_banks(key):
        return key in named or schemes[key]["banks_from"] in named

    real = sum(p for k, p in population.items() if schemes[k]["builder"] != "generic")
    with_banks = sum(p for k, p in population.items() if has_banks(k))
    print(f"population on a real scheme: {real / total:.1%}; with named banks: {with_banks / total:.1%}")
    print("largest schemes with no named bank:",
          " ".join(k for k, p in sorted(population.items(), key=lambda kv: -kv[1]) if not has_banks(k))[:400])
    print(f"earliest published test expiry: {min(expiries)}" if expiries else "no published expiry")
    print(f"\ntop weights, {TOP_SCHEMES} largest schemes by population:")
    for key, people in sorted(population.items(), key=lambda kv: -kv[1])[:TOP_SCHEMES]:
        members = sorted((r for r in banks if r["scheme"] == key), key=lambda r: -int(r["weight"]))
        whole = sum(int(r["weight"]) for r in members)
        top = ", ".join(f"{r['name'][:26]} {int(r['weight']) / whole:.1%}" for r in members[:5])
        print(f"  {key} ({people / total:.1%} of people, {len(members)} banks): {top}")


def check_lists(lite, full):
    """A list that says it is complete holds every code the full tier has."""
    def codes(banks, scheme):
        return {r["bank_code"] for r in banks if r["scheme"] == scheme}
    for key, row in lite[1].items():
        if row["list_complete"] and codes(lite[2], key) != codes(full[2], key):
            fail(f"lite schemes: {key} says its list is complete, but full has other codes")


def main():
    results = {}
    for tier in TIERS:
        results[tier] = check_tier(tier)
    check_lists(results["lite"], results["full"])
    for tier in TIERS:
        print(f"{tier}: {len(results[tier][2])} banks, {len(results[tier][1])} schemes")
    report(*results["full"])
    if problems:
        print(f"\n{len(problems)} problems:")
        print("\n".join(problems[:60]))
        sys.exit(1)
    print("\nchecks passed")


if __name__ == "__main__":
    main()
