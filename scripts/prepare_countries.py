#!/usr/bin/env python3
"""Write countries.tsv: the 199 countries id-generator covers, plus territories by request.

Population, currency and minor unit come from country-generator. Territories carry
territory=1 so they can be asked for by name but are never drawn.
"""
from fingen_data import SIBLINGS, read_tsv, survey, write_all_tiers

COLUMNS = ["country_code", "population", "currency", "currency_digits", "primary_scheme", "territory"]


def main():
    resources = SIBLINGS / "country-generator/resources/full"
    source = {r["cca2"]: r for r in read_tsv(resources / "countries.tsv")}
    digits = {r["code"]: r["digits"] for r in read_tsv(resources / "currencies.tsv")}
    drawn = {r["country_code"] for r in read_tsv(SIBLINGS / "id-generator/resources/full/countries.tsv")}
    rows, problems = [], []
    for row in sorted(survey(), key=lambda r: r["country"]):
        cc = row["country"]
        territory = row["klass"] == "territory"
        if territory == (cc in drawn):
            problems.append(f"{cc}: survey class {row['klass']} disagrees with id-generator's list")
        entry = source.get(cc)
        if entry is None:
            problems.append(f"{cc}: not in country-generator")
            continue
        currency = entry["currency_code"].split(",")[0].strip()
        if currency not in digits:
            problems.append(f"{cc}: currency {currency!r} has no minor unit")
            continue
        rows.append({"country_code": cc, "population": entry["population"], "currency": currency,
                     "currency_digits": digits[currency], "primary_scheme": row["scheme"],
                     "territory": "1" if territory else ""})
    if problems:
        raise SystemExit("\n".join(problems))
    write_all_tiers("countries.tsv", "country-generator (population, currency, minor unit); the account survey (scheme)",
                    COLUMNS, rows)
    print(f"countries.tsv: {len(rows)} rows, {sum(1 for r in rows if r['territory'])} territories")


if __name__ == "__main__":
    main()
