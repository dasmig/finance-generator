#!/usr/bin/env python3
"""Write ownership.tsv from Global Findex, by the rule of design 5.5.

Each economy takes its latest positive value per indicator, because the 2024 credit
card value of 0 for 43 economies is unanswered, not measured. `year` is the credit
value's year. A country with no value takes its subregion's population-weighted mean,
and then `filled` is 1. Groups: all (15+), young (15-24), older (25+).
"""
import json

from fingen_data import RAW, SIBLINGS, read_tsv, survey, write_all_tiers

COLUMNS = ["country_code", "group", "account", "debit", "credit", "year", "filled"]
GROUPS = {"all": "", "young": ".3", "older": ".4"}
INDICATORS = {"account": "fiaccount.t.d", "debit": "fin2.t.d", "credit": "fin10"}
FIELDS = ("account", "debit", "credit")


def latest_positive(indicator, cca3_to_cc):
    """{cc: (share, year)} at each economy's latest positive value."""
    body = json.loads((RAW / f"findex_{indicator}.json").read_text(encoding="utf-8"))
    best = {}
    for row in body[1] or []:
        cc = cca3_to_cc.get(row["country"]["id"])   # countryiso3code is empty for this source
        value = row["value"]
        if cc is None or not value or value <= 0:
            continue
        year = int(row["date"])
        if cc not in best or year > best[cc][1]:
            best[cc] = (round(value / 100, 4), year)
    return best


def main():
    countries = {r["cca2"]: r for r in read_tsv(SIBLINGS / "country-generator/resources/full/countries.tsv")}
    cca3_to_cc = {r["cca3"]: cc for cc, r in countries.items()}
    measured = {}
    for group, suffix in GROUPS.items():
        for field, indicator in INDICATORS.items():
            measured[group, field] = latest_positive(indicator + suffix, cca3_to_cc)
    served = [r["country"] for r in survey() if r["klass"] != "territory"]

    def means(group, field):
        total, weight = {}, {}
        for cc in served:
            value = measured[group, field].get(cc)
            if value:
                sub = countries[cc]["subregion"]
                pop = int(countries[cc]["population"] or 0)
                total[sub] = total.get(sub, 0) + value[0] * pop
                weight[sub] = weight.get(sub, 0) + pop
        return {sub: round(total[sub] / weight[sub], 4) for sub in total if weight[sub]}

    averages = {key: means(*key) for key in measured}
    world = {}
    for field in FIELDS:
        values = [v[0] for v in measured["all", field].values()]
        world[field] = round(sum(values) / len(values), 4)
    rows = []
    filled_all = 0
    for cc in sorted(served):
        sub = countries[cc]["subregion"]
        for group in GROUPS:
            row = {"country_code": cc, "group": group, "filled": ""}
            for field in FIELDS:
                value = measured[group, field].get(cc) or measured["all", field].get(cc)
                if value:
                    row[field] = value[0]
                    if field == "credit":
                        row["year"] = value[1]
                else:
                    row[field] = averages[group, field].get(sub) or averages["all", field].get(sub) or world[field]
                    row["filled"] = "1"
            rows.append(row)
            filled_all += group == "all" and row["filled"] == "1"
    write_all_tiers("ownership.tsv", "The World Bank: Global Findex Database 2025: fiaccount.t.d, fin2.t.d, fin10 and their age breakdowns; latest positive value, filled from the subregion's mean",
                    COLUMNS, rows)
    print(f"ownership.tsv: {len(rows)} rows, {filled_all} of {len(served)} countries filled in part")

    # For the read by hand the design asks for: economies whose latest credit value is not from 2024.
    older = sorted((cc, v) for cc, v in measured["all", "credit"].items() if v[1] < 2024 and cc in served)
    print("credit value older than 2024:", " ".join(f"{cc}={v[0]:.2f}@{v[1]}" for cc, v in older))
    missing = [cc for cc in served if cc not in measured["all", "credit"]]
    print("no positive credit value at all:", " ".join(sorted(missing)))


if __name__ == "__main__":
    main()
