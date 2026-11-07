#!/usr/bin/env python3
"""Write networks.tsv, card_networks.tsv, test_cards.tsv and test_accounts.tsv from curated/."""
from fingen_data import CURATED, read_tsv, write_all_tiers

NETWORKS = ["network", "range_from", "range_to", "length", "lengths", "weight", "validity_years", "min_age", "luhn", "format",
            "source"]
CARD_NETWORKS = ["country_code", "kind", "network", "band", "evidence"]
TEST_CARDS = ["source", "number", "network", "kind", "country_code", "expires", "security_code"]
TEST_ACCOUNTS = ["source", "scheme", "number", "bank_code", "branch_code", "account_number"]
BANDS = {"main", "common", "rare"}
KINDS = {"credit", "debit", "prepaid", "any"}


def main():
    networks = read_tsv(CURATED / "networks.tsv")
    names = {r["network"] for r in networks}
    bands = read_tsv(CURATED / "card_networks.tsv")
    cards = read_tsv(CURATED / "test_cards.tsv")
    problems = [f"card_networks: unknown network {r['network']}" for r in bands if r["network"] not in names]
    problems += [f"card_networks: unknown band {r['band']}" for r in bands if r["band"] not in BANDS]
    problems += [f"test_cards: {r['number']} has an unknown network" for r in cards if r["network"] not in names]
    problems += [f"test_cards: {r['number']} has an unknown kind" for r in cards if r["kind"] not in KINDS]
    if problems:
        raise SystemExit("\n".join(problems))
    write_all_tiers("networks.tsv", "Wikipedia's Payment card number table, ActiveMerchant (MIT), BINDB and the sandboxes' numbers, per row; the networks' own documents were not found. Validity and minimum age are judgement",
                    NETWORKS, networks)
    write_all_tiers("card_networks.tsv", "curated bands, not measurements: main 8, common 2, rare 0.5; no open source for network share by country was found",
                    CARD_NETWORKS, bands)
    write_all_tiers("test_cards.tsv", "numbers Stripe, Adyen and Braintree publish for their sandboxes, read 2026-11-06; kind any means the page states none",
                    TEST_CARDS, cards)
    accounts = read_tsv(CURATED / "test_accounts.tsv")
    write_all_tiers("test_accounts.tsv", "Stripe's published test values, read 2026-11-06; several are well-known examples at real banks' codes",
                    TEST_ACCOUNTS, accounts)
    print(f"networks {len(networks)}, card_networks {len(bands)}, test_cards {len(cards)}, test_accounts {len(accounts)}")


if __name__ == "__main__":
    main()
