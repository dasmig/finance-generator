#!/usr/bin/env python3
"""Write banks.tsv: per scheme, the banks a code can name, with weights.

IBAN countries come from schwifty's pinned lists; schwifty generates some from the
authority's register and keeps others by hand, so `source` says which. India comes
from razorpay/ifsc, Japan from zengin-code, BIC-based and generic schemes from
GLEIF's BIC-to-LEI file, and the rest from curated/banks_curated.tsv. A scheme with
no list of codes has no rows: a BIC does not give a national code, so its code is
drawn from the pattern and names no bank rather than pairing a bank with a code
that is not its own.

Weights are branch rows where the list has them, deposits for the US, otherwise flat.
"""
import csv
import html
import json
import re

from fingen_data import CURATED, RAW, TIERS, fits, read_tsv, spec_classes, survey, write_table
from prepare_iban import load_formats

COLUMNS = ["scheme", "bank_code", "name", "weight", "bic", "branch_codes", "method", "everyday_code", "source"]
LITE_PER_SCHEME = 60
BRANCH_SAMPLE = {"lite": 8, "full": 32}

# Decision 6: the 20 German check methods most bank codes use (95 are in use).
GERMAN_METHODS = {"09", "63", "00", "88", "06", "13", "10", "76", "32", "28",
                  "34", "99", "20", "01", "61", "38", "91", "24", "33", "60"}

CENTRAL = re.compile(
    r"bundesbank|nationalbank|banque de france|banque nationale de belgique|nationale bank van belg|banca d'italia"
    r"|banco de espa|banco de portugal|nederlandsche bank|bank of england|narodowy bank polski|riksbank|norges bank"
    r"|central bank|zentralbank|magyar nemzeti|narodni banka|národní banka|narodna banka|národná banka|banka slovenije"
    r"|eesti pank|latvijas banka|lietuvos bankas|suomen pankki|banque centrale|banco central"
    r"|national bank of (ukraine|belgium|poland|serbia|moldova|slovakia|romania|kazakhstan|georgia|north macedonia)"
    r"|national bank of (ethiopia|rwanda|cambodia|tajikistan|the kyrgyz|angola)|banco nacional de angola"
    r"|banca nationala|nemzeti bank|sedlabanki|reserve bank|monetary authority|bank indonesia|bank negara malaysia"
    r"|bank of (japan|korea|thailand|israel|albania|mongolia|uganda|tanzania|ghana|botswana|namibia|zambia|mauritius"
    r"|algeria|sudan|papua new guinea|sierra leone|lao|guyana|jamaica|canada|italy|spain|france|russia)"
    r"|(?<!national )bank of greece"
    r"|state bank of vietnam|bangko sentral|state bank of pakistan|treasury|ministry", re.I)
NOT_RETAIL = re.compile(
    r"\b(funds?|fondos?|asset management|securities|insurance|clearing|custody|brokers?"
    r"|brokerage|leasing|factoring|holdings?|e-?money|electronic money|payments?|pension"
    r"|wealth management|private equity|development bank|export[- ]import|european investment bank|rating agency"
    r"|trustly|zimpler|juni technology|steven ab)\b", re.I)
# These words also appear in real banks' names (First Investment Bank, Forex Bank, Grupo Mutual, CBC Banque et Assurances).
NOT_RETAIL_UNLESS_BANK = re.compile(r"\b(investments?|exchange|forex|mutual|assurances?)\b", re.I)
NAMED_BANK = re.compile(r"bank|banque|banco|banca|banka|grupo mutual", re.I)
BANK_WORD = re.compile(
    r"bank|banque|banco|banca|banc\b|bancorp|bancaria|sparkasse|caisse|caja|credit union|building society"
    r"|cooperativ|co-operativ|savings|kredit|cr[eé]dit|pank|ngan hang|santander|hsbc|barclays|lloyds|coutts|bunq|\bubl\b"
    r"|银行|銀行|은행|ธนาคาร|банк|بنك|बैंक", re.I)
# GLEIF names an entity in its own script. A Chinese bank's branch abroad is then a name its customers would not read.
CJK = re.compile(r"[一-鿿぀-ヿ가-힯]")
CJK_COUNTRIES = {"CN", "TW", "HK", "MO", "JP", "KR", "SG"}
# Zengin's own numbering: below 3000 are banks, shinkin and credit co-operatives; above are farm and fishery co-operatives.
JP_RETAIL_BELOW = 3000
JP_POST_BANK = "9900"
JP_CENTRAL = "0000"
RBI = "RBIS"
BRAZIL_BANK = re.compile(r"\bbanco\b|\bbank\b|\bbanrisul\b|\bbanestes\b|\bbrb\b", re.I)
RU_ENTRY = re.compile(r'<BICDirectoryEntry BIC="(\d+)"><ParticipantInfo NameP="([^"]*)"[^>]*?PtType="(\d+)"')
RU_BANK_TYPES = {"20", "30"}
# A bank code of four capitals is the BIC's first four, so a BIC gives the code (GB, NL, IE, PK and others).
LETTER_CODE = "aaaa"
JOINED_CODES = {"SI"}


def clean(name):
    return re.sub(r"\s+", " ", name or "").strip()


def retail(name):
    if CENTRAL.search(name) or NOT_RETAIL.search(name):
        return False
    return not NOT_RETAIL_UNLESS_BANK.search(name) or bool(NAMED_BANK.search(name))


def schwifty_rows(cc):
    """Live rows of a country, generated files first, with `bank_codes` lists expanded."""
    folder = RAW / "schwifty/bank_registry"
    files = sorted(folder.glob(f"generated_{cc.lower()}*.json")) + sorted(folder.glob(f"manual_{cc.lower()}*.json"))
    for path in files:
        data = json.loads(path.read_text(encoding="utf-8"))
        for entry in data["entries"] if isinstance(data, dict) else data:
            if entry.get("status"):
                continue
            codes = entry.get("bank_codes") or [entry.get("bank_code", "")]
            for code in codes:
                yield {**entry, "bank_code": code, "file": "generated" if "generated" in path.name else "manual"}


def from_schwifty(scheme, cc, entry, report):
    """The banks of an IBAN country whose code fits the registry's bank position."""
    positions = entry.get("positions", {}).get("bank_code")
    if not positions:
        return []
    classes = spec_classes(entry["bban_spec"])[positions[0]:positions[1]]
    letters = classes == LETTER_CODE
    grouped = {}
    for row in schwifty_rows(cc):
        code = row["bank_code"]
        # Slovenia's list holds bank and branch together, while its IBAN separates them.
        if cc in JOINED_CODES:
            code = code[:len(classes)]
        if not fits(code, classes):
            report["misfit"] += 1
            continue
        grouped.setdefault(code, []).append(row)
    banks = []
    for code, rows in sorted(grouped.items()):
        main = next((r for r in rows if r.get("primary")), rows[0])
        if cc in JOINED_CODES:
            # The rows of one code are one bank's branches; its lowest code names the bank.
            main = rows[sorted(range(len(rows)), key=lambda i: (rows[i]["bank_code"], i))[0]]
        name = clean(main.get("short_name") or main.get("name"))
        # A letter-coded list is a BIC list, which holds any company with a BIC.
        if not name or not retail(name) or (letters and not BANK_WORD.search(name)):
            report["not_retail"] += 1
            continue
        method = main.get("checksum_algo", "") if cc == "DE" else ""
        if cc == "DE" and method not in GERMAN_METHODS:
            report["german_method"] += 1
            continue
        banks.append({"scheme": scheme, "bank_code": code, "name": name, "weight": len(rows),
                      "bic": main.get("bic") or "", "method": method, "source": f"schwifty-{main['file']}",
                      "branches": []})
    return banks


def from_letter_bics(scheme, cc, entry, by_country, known):
    """GLEIF's banks where the bank code is the BIC's first four letters and the list lacks them."""
    positions = entry.get("positions", {}).get("bank_code")
    if not positions or spec_classes(entry["bban_spec"])[positions[0]:positions[1]] != LETTER_CODE:
        return []
    banks, codes = [], set(known)
    for row in by_country.get(cc, []):
        code = row["bic"][:4]
        if code not in codes:
            codes.add(code)
            banks.append({"scheme": scheme, "bank_code": code, "name": row["name"], "weight": 1, "bic": row["bic"],
                          "method": "", "source": "gleif", "branches": []})
    return banks


def gleif_by_country():
    with open(RAW / "gleif_names.jsonl", encoding="utf-8") as f:
        entities = {e["lei"]: e for e in map(json.loads, f)}
    by_country = {}
    with open(RAW / "gleif_bic_lei.csv", encoding="utf-8") as f:
        next(f)
        pairs = sorted(line.strip().split(",") for line in f if line.strip())
    seen = set()
    for lei, bic in pairs:
        if not (len(bic) == 8 or bic.endswith("XXX")) or (lei, bic[4:6]) in seen:
            continue
        entity = entities.get(lei)
        if entity is None or entity["status"] != "ACTIVE" or entity["category"] not in ("GENERAL", "BRANCH"):
            continue
        name = clean(entity["latin"] or entity["name"])
        if CJK.search(name) and bic[4:6] not in CJK_COUNTRIES:
            continue
        if retail(name) and BANK_WORD.search(name):
            seen.add((lei, bic[4:6]))
            by_country.setdefault(bic[4:6], []).append({"name": name, "bic": bic})
    return by_country


def from_gleif(scheme, cc, by_country):
    return [{"scheme": scheme, "bank_code": e["bic"][:8], "name": e["name"], "weight": 1, "bic": e["bic"],
             "method": "", "source": "gleif", "branches": []} for e in by_country.get(cc, [])]


def from_ifsc():
    names = json.loads((RAW / "ifsc_banknames.json").read_text(encoding="utf-8"))
    branches = json.loads((RAW / "ifsc_branches.json").read_text(encoding="utf-8"))
    banks = []
    for code, values in sorted(branches.items()):
        if code == RBI or code not in names or not values:
            continue
        padded = sorted(f"{v:06d}" if isinstance(v, int) else str(v) for v in values)
        banks.append({"scheme": "in-ifsc", "bank_code": code, "name": clean(names[code]), "weight": len(values),
                      "bic": "", "method": "", "source": "ifsc", "branches": padded})
    return banks


def from_zengin():
    data = json.loads((RAW / "zengin_banks.json").read_text(encoding="utf-8"))
    banks = []
    for code, row in sorted(data.items()):
        if code == JP_CENTRAL or not (int(code) < JP_RETAIL_BELOW or code == JP_POST_BANK):
            continue
        path = RAW / f"zengin_branches/{code}.json"
        branches = sorted(json.loads(path.read_text(encoding="utf-8"))) if path.exists() else []
        banks.append({"scheme": "jp-zengin", "bank_code": code, "name": clean(row["name"]),
                      "weight": max(1, len(branches)), "bic": "", "method": "", "source": "zengin",
                      "branches": branches})
    return banks


def from_brazil():
    """The central bank's STR participants: the ISPB goes in the IBAN, the COMPE code is the everyday one."""
    banks = []
    with open(RAW / "br_str.csv", encoding="utf-8-sig", newline="") as f:
        for row in csv.reader(f):
            if len(row) < 6 or not row[2].isdigit() or not row[0].isdigit():
                continue
            name = clean(row[5])
            if retail(name) and BRAZIL_BANK.search(name):
                banks.append({"scheme": "iban-br", "bank_code": row[0], "name": name, "weight": 1, "bic": "",
                              "method": "", "source": "bcb", "branches": [], "everyday_code": row[2].zfill(3)})
    return banks


def from_russia():
    """The Bank of Russia's directory: banks (20) and their branches (30). Each BIK is one row,
    so a bank with many branches is drawn more often. Others are its own offices, treasuries,
    banks in liquidation (90) and non-bank credit organisations."""
    text = (RAW / "ru_bik.xml").read_text(encoding="cp1251")
    banks = []
    for bic, name, kind in sorted(set(RU_ENTRY.findall(text))):
        name = clean(html.unescape(name))
        if kind in RU_BANK_TYPES and "НКО" not in name:
            banks.append({"scheme": "iban-ru", "bank_code": bic, "name": name, "weight": 1, "bic": "", "method": "",
                          "source": "cbr", "branches": []})
    return banks


def from_curated():
    fdic = json.loads((RAW / "fdic_institutions.json").read_text(encoding="utf-8"))
    deposits = {str(d["data"]["CERT"]): d["data"].get("DEP", 0) for d in fdic["data"]}
    banks = []
    for row in read_tsv(CURATED / "banks_curated.tsv"):
        # FDIC reports deposits in thousands of dollars; millions keep the weights readable.
        weight = int(row["weight"]) if row["weight"] != "fdic" else max(1, deposits[row["key"]] // 1000)
        banks.append({"scheme": row["scheme"], "bank_code": row["bank_code"], "name": row["name"], "weight": weight,
                      "bic": "", "method": "", "source": "curated", "branches": []})
    return banks


def sample(values, count):
    """Evenly spaced, so a sample does not lean on the lowest numbers."""
    if len(values) <= count:
        return values
    return [values[i * len(values) // count] for i in range(count)]


def main():
    formats, _ = load_formats()
    gleif = gleif_by_country()
    report = {"misfit": 0, "not_retail": 0, "german_method": 0}
    banks = from_ifsc() + from_zengin() + from_curated() + from_brazil() + from_russia()
    listed = {b["scheme"] for b in banks}
    without = []
    for row in survey():
        scheme, cc, klass = row["scheme"], row["country"], row["klass"]
        if scheme in listed:
            continue
        found = []
        if klass in ("iban", "iban_unregistered", "territory"):
            found = from_schwifty(scheme, cc, formats[cc], report)
            found += from_letter_bics(scheme, cc, formats[cc], gleif, {b["bank_code"] for b in found})
        elif klass in ("generic", "bic_account"):
            found = from_gleif(scheme, cc, gleif)
        banks += found
        if not found:
            without.append(row)
    banks.sort(key=lambda b: (b["scheme"], b["bank_code"]))
    month = (RAW / "gleif_version.txt").read_text(encoding="utf-8")
    stamp = re.search(r"(\d{4})(\d\d)\d\d", month)
    version = {"10": "October"}.get(stamp.group(2), stamp.group(2)) + " " + stamp.group(1)
    comment = ("schwifty 2026.11.0 (MIT), razorpay/ifsc (MIT), zengin-code, Banco Central do Brasil, Bank of Russia, curated pages, GLEIF. "
               f"BICs marked gleif: SWIFT © and database rights {version}. All rights reserved. This Mapping Table has been "
               "developed by SWIFT. Any use of the Mapping Table, in whole or in part, is subject to the BIC/LEI Mapping Table "
               "License Agreement as published with the Mapping Table available on GLEIF's website. The Mapping Table is updated "
               "monthly. For the latest BIC information and updates, always refer to www.swift.com/bic.")
    for tier in TIERS:
        rows = []
        for scheme in sorted({b["scheme"] for b in banks}):
            members = [b for b in banks if b["scheme"] == scheme]
            if tier == "lite":
                members = sorted(members, key=lambda b: (-b["weight"], b["bank_code"]))[:LITE_PER_SCHEME]
                members.sort(key=lambda b: b["bank_code"])
            for b in members:
                rows.append({**b, "branch_codes": ",".join(sample(b["branches"], BRANCH_SAMPLE[tier]))})
        write_table(tier, "banks.tsv", comment, COLUMNS, rows)
        print(f"banks.tsv {tier}: {len(rows)} rows over {len({r['scheme'] for r in rows})} schemes")
    print("dropped:", report)
    total = sum(int(r["population"] or 0) for r in survey() if r["klass"] != "territory")
    gap = [r for r in without if r["klass"] != "territory"]
    share = sum(int(r["population"] or 0) for r in gap) / total
    print(f"no named banks: {len(gap)} countries, {share:.1%} of population:")
    print(" ".join(r["scheme"] for r in sorted(gap, key=lambda r: -int(r["population"] or 0))))


if __name__ == "__main__":
    main()
