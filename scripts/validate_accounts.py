#!/usr/bin/env python3
"""Check generated accounts with implementations the library does not share.

Builds scripts/sample_finance.cpp, draws 1,000 people with two accounts each (2,000
accounts) per country and territory at three dates, plus safe ones, and checks every
account with schwifty and python-stdnum. Rules neither has are checked by code written
here from the national rules, and the report counts how far each oracle reached.

It also draws a fifth as many people with four cards each, from each source, and checks
them against stdnum's Luhn and against the card tables as this script reads them.
When raw_data/iban_registry.txt is saved, SWIFT's example IBANs are read back as well.

Both libraries are for this script only; they are never shipped or vendored. stdnum is
LGPL:

    python -m venv .venv && .venv/Scripts/pip install schwifty python-stdnum
    .venv/Scripts/python scripts/validate_accounts.py [--tier lite] [--count 1000] [--cxx clang++]

Target: every realistic account valid, and every safe account rejected or accepted as
its scheme's method declares (doc/data.md):

    test_value       published by the sandbox; its checks hold
    unassigned_code  the checks hold, and the bank code is in no row of a complete list
    wrong_national   the outer check holds and the national check fails
    wrong_outer      the check digits fail
    none             well-formed; the method promises nothing

A disagreement is read rather than assumed to be our bug.
"""
import argparse
import csv
import re
import subprocess
import sys
import warnings
from collections import Counter, defaultdict
from datetime import date, timedelta
from pathlib import Path

from fingen_data import read_registry

ROOT = Path(__file__).resolve().parent.parent
csv.field_size_limit(10_000_000)


def read_tsv(path):
    with open(path, encoding="utf-8", newline="") as handle:
        lines = (line for line in handle if not line.startswith("#"))
        yield from csv.DictReader(lines, delimiter="\t", quoting=csv.QUOTE_NONE)


def numerify(text):
    return "".join(str(int(c, 36)) for c in text)


def mod97_ok(iban):
    return int(numerify(iban[4:] + iban[:4])) % 97 == 1


def rib_key(body):
    """France's key: letters by the old punch-card values, then 97 - (body00 mod 97)."""
    values = {**{c: str(i + 1) for i, c in enumerate("ABCDEFGHI")},
              **{c: str(i + 1) for i, c in enumerate("JKLMNOPQR")},
              **{c: str(i + 2) for i, c in enumerate("STUVWXYZ")}}
    digits = "".join(values.get(c, c) for c in body)
    return f"{97 - int(digits + '00') % 97:02d}"


def mod97_bban(body):
    return f"{98 - int(numerify(body) + '00') % 97:02d}"


def ru_key_ok(bik, account):
    weights = (7, 1, 3)
    return sum(int(d) * weights[i % 3] for i, d in enumerate(bik[-3:] + account)) % 10 == 0


def nz_ok(formatted):
    """Inland Revenue's Payroll Calculations and Business Rules, section 7. stdnum reduces E and G's
    products with % 9, where the text adds digits twice, so it differs on 18, 27, 36, 45, 54 and 63."""
    bank, branch, account, suffix = formatted.split("-")
    digits = bank + branch + account + suffix.rjust(3, "0")
    sets = {"08": ("0000007654321000", 11, False), "09": ("000000000" + "5432" + "001", 11, True),
            "25": ("0000001731731000", 10, False), "33": ("0000001731731000", 10, False)}
    letters = {"26": "G", "28": "G", "29": "G"}
    if letters.get(bank) == "G":
        weights, modulus, digit_sum = "0000001371371371", 10, True
    elif bank in sets:
        weights, modulus, digit_sum = sets[bank]
    elif account >= "0990000":
        weights, modulus, digit_sum = "000000" + "0" + "T584210" + "00", 11, False
    else:
        weights, modulus, digit_sum = "006379" + "0" + "T584210" + "00", 11, False
    total = 0
    for digit, weight in zip(digits, weights.ljust(16, "0")):
        product = int(digit) * (10 if weight == "T" else int(weight))
        if digit_sum:
            product = product // 10 + product % 10
            product = product // 10 + product % 10
        total += product
    return total % modulus == 0


def clabe_digit(body):
    weights = (3, 7, 1)
    return str((10 - sum(int(d) * weights[i % 3] for i, d in enumerate(body)) % 10) % 10)


def nuban_digit(body):
    weights = (3, 7, 3)
    return str((10 - sum(int(d) * weights[i % 3] for i, d in enumerate(body)) % 10) % 10)


class Checks:
    """The oracles, and what each one reached."""

    def __init__(self, tier):
        from schwifty import IBAN, checksum
        from schwifty.exceptions import NationalChecksumNotVerified
        from stdnum import bic, iban, luhn, numdb
        from stdnum.ar import cbu
        from stdnum.exceptions import InvalidComponent, ValidationError
        from stdnum.is_ import kennitala
        from stdnum.nz import bankaccount
        from stdnum.us import rtn

        warnings.simplefilter("ignore", NationalChecksumNotVerified)
        self.IBAN = IBAN
        self.stdnum_iban = iban
        self.bic = bic
        self.luhn = luhn
        self.cbu = cbu
        self.kennitala = kennitala
        self.nz = bankaccount
        self.rtn = rtn
        self.schwifty_national = {key.split(":")[0] for key in checksum.algorithms}

        base = ROOT / "resources" / tier
        self.schemes = {row["key"]: row for row in read_tsv(base / "schemes.tsv")}
        self.formats = {row["country_code"]: row for row in read_tsv(base / "iban_formats.tsv")}
        self.tests = defaultdict(set)
        for row in read_tsv(base / "test_accounts.tsv"):
            self.tests[row["scheme"]].add(re.sub(r"[ -]", "", row["number"]))
        self.books = defaultdict(dict)
        for row in read_tsv(base / "banks.tsv"):
            self.books[row["scheme"]][row["bank_code"]] = row
        self.iban_table = numdb.get("iban")
        self.stdnum_component = InvalidComponent
        self.stdnum_invalid = ValidationError
        self.reach = defaultdict(Counter)

    def stdnum_knows(self, cc):
        """stdnum's table holds the registry's countries, not the territories or the partial forms."""
        return any(props for _, props in self.iban_table.info(cc)[:1])

    def stdnum_problems(self, iban):
        """stdnum's Belgian module also wants the bank in its own older table, which lacks codes on
        the National Bank's register such as 606. That is not a rejection of the number."""
        try:
            self.stdnum_iban.validate(iban)
        except self.stdnum_component:
            if iban[:2] == "BE":
                self.stdnum_iban.validate(iban, check_country=False)
                return []
            return ["stdnum rejects: unknown component"]
        except self.stdnum_invalid as error:
            return [f"stdnum rejects: {type(error).__name__}"]
        return []

    def book_of(self, scheme):
        row = self.schemes[scheme]
        return self.books[row["banks_from"] or scheme]

    # -- The national check of an IBAN -----------------------------------------------

    def national_ok(self, row, iban):
        """Whether the national check holds; None when no oracle has a rule for it."""
        cc = iban[:2]
        fmt = self.formats[cc]
        bban = iban[4:]
        check = fmt["check_builder"]
        if cc in self.schwifty_national and check:
            self.reach["schwifty national"][row["scheme"]] += 1
            try:
                self.IBAN(iban, allow_invalid=True).bban.validate_national_checksum()
            except ValueError:
                return False
            return True
        if not check:
            return None
        self.reach["own national"][row["scheme"]] += 1
        at = int(fmt["check_from"]) if fmt["check_from"] else 0
        to = int(fmt["check_to"]) if fmt["check_to"] else 0
        if check == "rib_key":
            return rib_key(bban[:at]) == bban[at:to]
        if check == "mod97_bban":
            return mod97_bban(bban[:at]) == bban[at:to]
        if check == "ru_key":
            return ru_key_ok(bban[:9], bban[9:])
        if check == "fi":  # Aaland, under Finland's rule
            return self.luhn.is_valid(bban)
        raise SystemExit(f"no rule here for {cc}'s {check}")

    # -- Realistic ---------------------------------------------------------------------

    def realistic_iban(self, row):
        """The problems of an account with a realistic IBAN."""
        iban, cc = row["iban"], row["iban"][:2]
        problems = []
        self.reach["schwifty"][row["scheme"]] += 1
        try:
            parsed = self.IBAN(iban)
        except ValueError as error:
            return [f"schwifty rejects: {type(error).__name__}: {error}"]
        if self.stdnum_knows(cc):
            self.reach["stdnum iban"][row["scheme"]] += 1
            problems += self.stdnum_problems(iban)
        national = self.national_ok(row, iban)
        if national is False:
            problems.append("the national check fails")
        builder = self.schemes[row["scheme"]]["builder"]
        if builder == "iban_br":
            problems += self.brazil(row)
        elif builder == "iban_ru":
            if parsed.bban[:9] != row["bank"] or iban[13:] != row["account"]:
                problems.append("the fields do not decode from the IBAN")
        else:
            if parsed.bank_code != row["bank"]:
                problems.append(f"bank {row['bank']} but schwifty reads {parsed.bank_code}")
            # schwifty has no position for some territories' branch, which we take from the parent.
            if parsed.branch_code and parsed.branch_code != row["branch"]:
                problems.append(f"branch {row['branch']} but schwifty reads {parsed.branch_code}")
            if not iban.endswith(row["account"]):
                problems.append("the account is not the IBAN's tail")
        if cc == "IS":
            problems += self.iceland(row)
        book = self.book_of(row["scheme"])
        if book and builder == "iban" and row["bank"] not in book:
            problems.append(f"bank {row['bank']} is in no row")
        return problems

    def brazil(self, row):
        bban = row["iban"][4:]
        book = self.book_of(row["scheme"])
        ispb = bban[:8]
        problems = []
        if ispb not in book or book[ispb]["everyday_code"] != row["bank"]:
            problems.append("the everyday code is not the ISPB's")
        if bban[8] != "0" or bban[9:13] != row["branch"]:
            problems.append("the branch does not decode")
        if bban[13:23] != row["account"].rjust(10, "0"):
            problems.append("the account does not decode")
        if bban[23:] != ("P1" if row["kind"] == "savings" else "C1"):
            problems.append("the account type does not decode")
        return problems

    def iceland(self, row):
        bban = row["iban"][4:]
        holder = bban[12:]
        self.reach["stdnum kennitala"][row["scheme"]] += 1
        if not self.kennitala.is_valid(holder):
            return [f"stdnum rejects the kennitala {holder}"]
        born = date.fromisoformat(row["birth"])
        century = "9" if born.year < 2000 else "0"
        if holder[:6] != born.strftime("%d%m%y") or holder[9] != century:
            return ["the kennitala is not the holder's birth date"]
        return []

    def realistic_national(self, row):
        """The problems of an account with no IBAN."""
        plain = re.sub(r"[ -]", "", row["formatted"])
        builder = self.schemes[row["scheme"]]["builder"]
        scheme = row["scheme"]
        problems = []
        if builder == "us_aba":
            self.reach["stdnum"][scheme] += 1
            if not self.rtn.is_valid(row["bank"]):
                problems.append(f"stdnum rejects the routing number {row['bank']}")
        elif builder == "ar_cbu":
            self.reach["stdnum"][scheme] += 1
            if not self.cbu.is_valid(plain):
                problems.append("stdnum rejects the CBU")
        elif builder == "nz_account":
            self.reach["own national"][scheme] += 1
            if not nz_ok(row["formatted"]):
                problems.append("the account's check fails")
            # Bank 05 and 88 are in Inland Revenue's table and not in stdnum's.
            if row["bank"] not in ("05", "09", "26", "28", "29", "88"):
                self.reach["stdnum"][scheme] += 1
                if not self.nz.is_valid(row["formatted"]):
                    problems.append("stdnum rejects the account")
        elif builder == "mx_clabe":
            self.reach["own national"][scheme] += 1
            if len(plain) != 18 or clabe_digit(plain[:17]) != plain[17]:
                problems.append("the CLABE's check fails")
        elif builder == "ng_nuban":
            self.reach["own national"][scheme] += 1
            if len(plain) != 13 or nuban_digit(plain[:12]) != plain[12]:
                problems.append("the NUBAN's check fails")
        else:
            self.reach["structure only"][scheme] += 1
        return problems

    # -- Safe -------------------------------------------------------------------------

    def safe(self, row, method):
        scheme = row["scheme"]
        iban = row["iban"]
        problems = []
        if row["bank_name"]:
            problems.append("a safe account names a bank")
        if row["bic"] and row["bic"][7] != "0":
            problems.append("a safe BIC does not have SWIFT's test 0 in position 8")
        if method == "test_value":
            if re.sub(r"[ -]", "", iban or row["formatted"]) not in self.tests[scheme]:
                problems.append("not a published test number")
            if row["own_number"] != "1":
                problems.append("is_valid refuses a published test number")
        elif method == "unassigned_code":
            if not mod97_ok(iban):
                problems.append("the outer check fails")
            if self.national_ok(row, iban) is False:
                problems.append("the national check fails")
            if row["bank"] in self.book_of(scheme):
                problems.append(f"bank {row['bank']} has a row")
            parsed = self.IBAN(iban, allow_invalid=True)
            if parsed.bank is not None:
                problems.append(f"schwifty knows bank {row['bank']}: {parsed.bank['name']}")
            if row["bic"]:
                problems.append("an unassigned code has a BIC")
            if row["own_number"] != "0":
                problems.append("is_valid accepts an unassigned code")
        elif method == "wrong_national":
            if not mod97_ok(iban):
                problems.append("the outer check fails")
            national = self.national_ok(row, iban)
            if national is True:
                problems.append("the national check holds")
            elif national is None:
                problems.append("no national check to fail")
            if self.stdnum_iban.is_valid(iban) and iban[:2] in ("BE", "ES", "ME", "NO"):
                problems.append("stdnum accepts it")
            if row["own_number"] != "0":
                problems.append("is_valid accepts it")
        elif method == "wrong_outer":
            if iban:
                if mod97_ok(iban):
                    problems.append("the check digits hold")
                if not 2 <= int(iban[2:4]) <= 98:
                    problems.append("check digits outside 02 to 98")
                if self.stdnum_iban.is_valid(iban):
                    problems.append("stdnum accepts it")
            else:
                problems += ["the national check holds"] if not self.realistic_national(row) else []
            if row["own_number"] != "0":
                problems.append("is_valid accepts it")
        elif method == "none":
            if row["own_number"] != "1":
                problems.append("is_valid refuses it")
        return problems


def add_years(day, years):
    try:
        return day.replace(year=day.year + years)
    except ValueError:
        return day.replace(year=day.year + years, day=28)


def check_common(row):
    problems = []
    born, opened, reference = (date.fromisoformat(row[k]) for k in ("birth", "opened", "reference"))
    if not add_years(born, 16) <= opened <= reference:
        problems.append(f"opened {opened} outside {born}+16..{reference}")
    return problems


class CardChecks:
    """The cards' rules, read from the tables by this script and not by the header."""

    def __init__(self, tier):
        folder = ROOT / "resources" / tier
        self.ranges = defaultdict(list)
        for row in read_tsv(folder / "networks.tsv"):
            self.ranges[row["network"]].append(row)
        self.doubtful = {row["network"] for rows in self.ranges.values() for row in rows if row["luhn"] == "unsure"}
        self.tests = {(row["source"], row["number"]): row for row in read_tsv(folder / "test_cards.tsv")}
        self.bands = defaultdict(set)
        for row in read_tsv(folder / "card_networks.tsv"):
            self.bands[row["country_code"]].add(row["network"])

    def holders(self, number):
        def fits(row):
            size = len(row["range_from"])
            return row["range_from"] <= number[:size] <= row["range_to"] and str(len(number)) in row["lengths"].split(",")
        return {name for name, rows in self.ranges.items() if any(fits(r) for r in rows)}

    def country_networks(self, country):
        return self.bands.get(country) or self.bands["*"]

    def check(self, row, luhn):
        problems = []
        number, source, network = row["number"], row["source"], row["network"]
        holders = self.holders(number)
        if not number.isdigit():
            problems.append("not digits")
        if network not in holders:
            problems.append(f"outside {network}'s ranges")
        if row["formatted"].replace(" ", "") != number:
            problems.append("formatted is another number")
        if luhn.is_valid(number) != (source != "wrong_check"):
            problems.append(f"stdnum's Luhn disagrees with {source}")
        if (row["own_valid"] == "1") != (source != "wrong_check"):
            problems.append(f"card_valid disagrees with {source}")
        if source == "wrong_check" and holders & self.doubtful:
            problems.append(f"wrong_check in {sorted(holders & self.doubtful)}, whose cards may fail Luhn")
        if (row["reserved"] == "1") != (source != "network_range"):
            problems.append("reserved")
        size = 4 if network == "amex" else 3
        if source == "network_range":
            if row["security_code"]:
                problems.append("a security code on a network_range number")
        elif len(row["security_code"]) != size:
            problems.append("security code length")

        born, issued, reference = (date.fromisoformat(row[k]) for k in ("birth", "issued", "reference"))
        expires = date.fromisoformat(row["expires"] + "-01")
        last = (expires.replace(day=28) + timedelta(days=4)).replace(day=1) - timedelta(days=1)
        if not issued <= reference <= last:
            problems.append(f"dates {issued} {row['expires']} do not span {reference}")
        if issued < add_years(born, 18 if row["kind"] == "credit" else 16):
            problems.append(f"{row['kind']} issued too young")

        allowed = {int(y) for r in self.ranges[network] for y in r["validity_years"].split(",")}
        published = self.tests.get((row["test_source"], number))
        if source == "test_number":
            if published is None:
                return problems + ["not a published number"]
            if published["network"] != network:
                problems.append("not as published")
            if published["kind"] not in ("any", row["kind"]):
                problems.append("another kind than published")
            if published["country_code"] not in ("", row["country"]):
                problems.append("another country's number")
            if published["security_code"] and published["security_code"] != row["security_code"]:
                problems.append("another code than published")
        if source != "test_number" or not published["expires"]:
            if expires.year - issued.year not in allowed or expires.month != issued.month:
                problems.append(f"validity {issued} to {row['expires']}")
        elif published["expires"] != row["expires"]:
            problems.append("another expiry than published")

        if source == "network_range" and len(holders & self.country_networks(row["country"])) > 1:
            problems.append(f"also in {sorted(holders - {network})}, of the same country")
        return problems


def run_cards(tier, sample):
    from stdnum import luhn
    checks = CardChecks(tier)
    tally = Counter()
    networks = Counter()
    failures = defaultdict(list)
    for row in read_tsv(sample):
        tally[row["source"]] += 1
        networks[row["network"]] += 1
        for problem in checks.check(row, luhn):
            failures[re.sub(r"\d{3,}", "#", problem)].append((row["country"], row["network"], problem))
    print(f"  cards: {dict(sorted(tally.items()))}")
    print(f"  by network: {dict(networks.most_common())}")
    if not failures:
        print("  every card as declared")
        return True
    print(f"  {sum(len(v) for v in failures.values()):,} card problems of {len(failures)} kinds:")
    for _, found in sorted(failures.items(), key=lambda kv: -len(kv[1]))[:40]:
        print(f"    {len(found):,} x {found[0]}")
    return False


# SWIFT's examples whose bank is not on our complete list: a placeholder or a code not on the
# register. Each passes every structure and check-digit rule once its bank is added as a layer.
REGISTRY_REFUSED = {"lite": {"LV", "SI", "SK"}, "full": {"BE", "LV", "RO", "SI", "SK"}}


def write_registry_examples(path):
    """SWIFT's example IBANs, for the sampler to read back; False when the text is not saved."""
    registry = read_registry()
    if registry is None:
        return False
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        for code, entry in sorted(registry.items()):
            out.write(f"iban-{code.lower()}\t{entry['example']}\n")
    return True


def check_registry_examples(tier, output, examples):
    refused = {line.split("\t")[1][5:].upper() for line in output.splitlines() if line.startswith("refused\t")}
    total = sum(1 for _ in open(examples, encoding="utf-8"))
    print(f"  SWIFT's examples: {total}, refused {sorted(refused)}")
    if refused != REGISTRY_REFUSED[tier]:
        print(f"  expected {sorted(REGISTRY_REFUSED[tier])}, so a check rule may be wrong")
        return False
    return True


def build(cxx, out):
    command = [cxx, "-std=c++23", "-O2", "-I", str(ROOT), "-o", str(out), str(ROOT / "scripts/sample_finance.cpp")]
    subprocess.run(command, check=True)


def run_tier(tier, count, cxx, build_dir):
    binary = build_dir / ("sample_finance.exe" if sys.platform == "win32" else "sample_finance")
    build(cxx, binary)
    sample = build_dir / f"accounts_{tier}.tsv"
    card_sample = build_dir / f"cards_{tier}.tsv"
    examples = build_dir / "registry_examples.tsv"
    command = [str(binary), str(ROOT / "resources" / tier), str(count), str(sample), str(card_sample)]
    has_examples = write_registry_examples(examples)
    if has_examples:
        command.append(str(examples))
    result = subprocess.run(command, check=True, capture_output=True, text=True)

    checks = Checks(tier)
    tally = Counter()
    failures = defaultdict(list)
    for row in read_tsv(sample):
        scheme = row["scheme"]
        if row["safe"] == "1":
            method = checks.schemes[scheme]["safe_method"]
            tally[f"safe {method}"] += 1
            problems = checks.safe(row, method)
        else:
            tally["realistic"] += 1
            problems = checks.realistic_iban(row) if row["iban"] else checks.realistic_national(row)
            if row["own_number"] != "1" or row["own_formatted"] != "1":
                problems.append("is_valid refuses it")
            if row["bic"]:
                checks.reach["stdnum bic"][scheme] += 1
                if not checks.bic.is_valid(row["bic"]):
                    problems.append(f"stdnum rejects the BIC {row['bic']}")
        problems += check_common(row)
        for problem in problems:
            key = (scheme, re.sub(r"\d{3,}", "#", problem))
            failures[key].append((row["iban"] or row["formatted"], problem))

    print(f"== {tier}")
    for name, value in sorted(tally.items()):
        print(f"  {name}: {value:,}")
    print("  reach (schemes, accounts):")
    for name, schemes in sorted(checks.reach.items()):
        print(f"    {name}: {len(schemes)}, {sum(schemes.values()):,}")
    cards_ok = run_cards(tier, card_sample)
    if has_examples:
        cards_ok = check_registry_examples(tier, result.stdout, examples) and cards_ok
    if not failures:
        print("  every account as declared")
        return cards_ok
    total = sum(len(v) for v in failures.values())
    print(f"  {total:,} problems of {len(failures)} kinds:")
    for (scheme, _kind), found in sorted(failures.items(), key=lambda kv: -len(kv[1]))[:60]:
        number, problem = found[0]
        print(f"    {scheme}: {len(found):,} x {problem}  [{number}]")
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tier", choices=("lite", "full"), action="append")
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    args.build_dir.mkdir(exist_ok=True)
    results = [run_tier(tier, args.count, args.cxx, args.build_dir) for tier in (args.tier or ["lite", "full"])]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
