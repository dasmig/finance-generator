#!/usr/bin/env python3
"""Download the raw sources into raw_data/ (git-ignored). Existing files are kept.

schwifty is read at a fixed tag, so its parsed registry and bank lists do not move
between builds. SWIFT refuses scripts, so its registry is saved by hand when wanted
(raw_data/iban_registry.txt); prepare_iban.py cross-checks it when it is there.
Exits non-zero if anything could not be fetched.
"""
import csv
import io
import json
import sys
import time
import urllib.parse
import urllib.request
import zipfile

from fingen_data import RAW

HEADERS = {"User-Agent": "dasmig-data-build/1.0"}
SCHWIFTY_TAG = "2026.11.0"
SCHWIFTY = f"https://raw.githubusercontent.com/mdomke/schwifty/{SCHWIFTY_TAG}/schwifty"
SCHWIFTY_TREE = f"https://api.github.com/repos/mdomke/schwifty/git/trees/{SCHWIFTY_TAG}?recursive=1"
FILES = {
    "ifsc_banknames.json": "https://raw.githubusercontent.com/razorpay/ifsc/master/src/banknames.json",
    "ifsc_branches.json": "https://raw.githubusercontent.com/razorpay/ifsc/master/src/IFSC.json",
    "zengin_banks.json": "https://raw.githubusercontent.com/zengin-code/source-data/master/data/banks.json",
    "br_str.csv": "https://www.bcb.gov.br/content/estabilidadefinanceira/str1/ParticipantesSTR.csv",
}
RU_BIK = "https://www.cbr.ru/s/newbik"
ZENGIN_BRANCHES = "https://raw.githubusercontent.com/zengin-code/source-data/master/data/branches/{}.json"
ZENGIN_RETAIL_BELOW = 3000
ZENGIN_POST_BANK = "9900"
GLEIF_LATEST = "https://mapping.gleif.org/api/v2/bic-lei/latest"
FDIC = "https://api.fdic.gov/banks/institutions?"
WORLD_BANK = "https://api.worldbank.org/v2/country/all/indicator/{}?"
INDICATORS = ["fiaccount.t.d", "fiaccount.t.d.3", "fiaccount.t.d.4", "fin2.t.d", "fin2.t.d.3", "fin2.t.d.4",
              "fin10", "fin10.3", "fin10.4"]
GLEIF_RECORDS = "https://api.gleif.org/api/v1/lei-records?"
GLEIF_BATCH = 100
# GLEIF's names are looked up only for countries whose bank list comes from BICs.
GLEIF_SKIP = {"US"}

failures = []


def get(url, retries=4, wait=10):
    request = urllib.request.Request(url, headers=HEADERS)
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(request, timeout=280) as response:
                return response.read()
        except OSError as e:
            print(f"  retry {attempt}: {url[:90]}: {e}")
            time.sleep(wait * (attempt + 1))
    return None


def save(name, data):
    if data is None:
        failures.append(name)
        print(f"FAILED {name}")
        return
    path = RAW / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    print(f"{name}: {len(data)} bytes")


def fetch(name, url):
    if not (RAW / name).exists():
        save(name, get(url))


def fetch_schwifty():
    tree = get(SCHWIFTY_TREE)
    if tree is None:
        failures.append("schwifty tree")
        return
    for entry in json.loads(tree)["tree"]:
        path = entry["path"]
        if path.startswith(("schwifty/bank_registry/", "schwifty/iban_registry/")) and path.endswith(".json"):
            relative = path.removeprefix("schwifty/")
            fetch(f"schwifty/{relative}", f"{SCHWIFTY}/{relative}")


def fetch_gleif():
    if (RAW / "gleif_bic_lei.csv").exists():
        return
    latest = get(GLEIF_LATEST)
    if latest is None:
        failures.append("gleif_bic_lei.csv")
        return
    attributes = json.loads(latest)["data"]["attributes"]
    archive = get(attributes["downloadLink"])
    if archive is None:
        failures.append("gleif_bic_lei.csv")
        return
    with zipfile.ZipFile(io.BytesIO(archive)) as z:
        save("gleif_bic_lei.csv", z.read(z.namelist()[0]))
    save("gleif_version.txt", str(attributes.get("fileName", attributes["downloadLink"])).encode())


def fetch_gleif_names():
    """Name and status of each head-office BIC's entity, one line of JSON per entity (CC0)."""
    path = RAW / "gleif_names.jsonl"
    done = set()
    if path.exists():
        with open(path, encoding="utf-8") as f:
            done = {json.loads(line)["lei"] for line in f}
    with open(RAW / "gleif_bic_lei.csv", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    wanted = sorted({r["LEI"] for r in rows if r["BIC"][4:6] not in GLEIF_SKIP
                     and (len(r["BIC"]) == 8 or r["BIC"].endswith("XXX"))} - done)
    print(f"gleif names: {len(done)} kept, {len(wanted)} to fetch")
    with open(path, "a", encoding="utf-8", newline="\n") as out:
        for start in range(0, len(wanted), GLEIF_BATCH):
            batch = wanted[start:start + GLEIF_BATCH]
            query = {"filter[lei]": ",".join(batch), "page[size]": str(GLEIF_BATCH)}
            body = get(GLEIF_RECORDS + urllib.parse.urlencode(query))
            if body is None:
                failures.append("gleif_names.jsonl")
                return
            for record in json.loads(body)["data"]:
                attributes = record["attributes"]
                entity = attributes["entity"]
                names = entity.get("transliteratedOtherNames") or []
                out.write(json.dumps({"lei": attributes["lei"], "name": entity["legalName"]["name"],
                                      "latin": names[0]["name"] if names else "",
                                      "status": entity.get("status"), "category": entity.get("category")},
                                     ensure_ascii=False) + "\n")
            out.flush()
            time.sleep(1.1)


def fetch_russia():
    """The Bank of Russia's BIK directory, the day's file, kept as its XML (WINDOWS-1251)."""
    if (RAW / "ru_bik.xml").exists():
        return
    archive = get(RU_BIK)
    if archive is None:
        failures.append("ru_bik.xml")
        return
    with zipfile.ZipFile(io.BytesIO(archive)) as z:
        save("ru_bik.xml", z.read(z.namelist()[0]))


def fetch_zengin_branches():
    """Branch lists of the banks the table keeps; their counts are the weights."""
    codes = json.loads((RAW / "zengin_banks.json").read_text(encoding="utf-8"))
    for code in sorted(codes):
        if int(code) < ZENGIN_RETAIL_BELOW or code == ZENGIN_POST_BANK:
            fetch(f"zengin_branches/{code}.json", ZENGIN_BRANCHES.format(code))


def fetch_fdic():
    if (RAW / "fdic_institutions.json").exists():
        return
    query = {"filters": "ACTIVE:1", "fields": "NAME,DEP,CERT,CITY,STALP", "sort_by": "DEP", "sort_order": "DESC",
             "limit": "10000", "format": "json"}
    save("fdic_institutions.json", get(FDIC + urllib.parse.urlencode(query)))


def fetch_findex():
    for indicator in INDICATORS:
        name = f"findex_{indicator}.json"
        if not (RAW / name).exists():
            query = {"source": "28", "format": "json", "per_page": "20000"}
            save(name, get(WORLD_BANK.format(indicator) + urllib.parse.urlencode(query)))


def main():
    RAW.mkdir(exist_ok=True)
    fetch_schwifty()
    for name, url in FILES.items():
        fetch(name, url)
    fetch_russia()
    fetch_zengin_branches()
    fetch_gleif()
    fetch_gleif_names()
    fetch_fdic()
    fetch_findex()
    if failures:
        sys.exit("could not fetch: " + ", ".join(failures))


if __name__ == "__main__":
    main()
