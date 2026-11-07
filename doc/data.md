# Data and Method

The data in `resources/` and how finance-generator uses it (rule 6 of the dasmig standard).

Accounts drawn without `safe` are structurally valid and may match a real person's account. The library cannot tell. They are test data: never use them for payments, direct debits, to open accounts or to pass a check. The library looks nothing up.

## Tiers

| Table | `lite` rows | `full` rows |
| --- | --- | --- |
| `countries.tsv` | 220 | 220 |
| `schemes.tsv` | 220 | 220 |
| `iban_formats.tsv` | 126 | 126 |
| `banks.tsv` | 2,959 | 28,305 |
| `test_accounts.tsv` | 21 | 21 |
| `networks.tsv` | 68 | 68 |
| `card_networks.tsv` | 32 | 32 |
| `test_cards.tsv` | 129 | 129 |
| `ownership.tsv` | 597 | 597 |

The tables total 0.28 MB in `lite` and 2.13 MB in `full`. Only `banks.tsv` differs in its rows: `lite` keeps at most the 60 heaviest banks of a scheme, and `full` has every code of the long lists, such as Germany's and Britain's. Cutting a list stops it being complete, so `list_complete` and `safe_method` in `schemes.tsv` differ by tier (see [Safe](#safe)).

## Columns

Every table is TSV, with a first-line `#` comment naming its source. Layers use the same columns.

| Table | Key | Columns |
| --- | --- | --- |
| `countries.tsv` | `country_code` | `population`, `currency`, `currency_digits`, `primary_scheme`; optional `territory` |
| `schemes.tsv` | `key` | `country`, `builder`, `display`, `safe_method`, `evidence`; optional `bank_pattern`, `branch_pattern`, `pattern`, `check`, `list_complete`, `banks_from`, `params`, `source`, `min_age` (default 16) |
| `iban_formats.tsv` | `country_code` | `bban_pattern`, `length`; optional `bank_from`, `bank_to`, `branch_from`, `branch_to`, `account_from`, `account_to`, `check_from`, `check_to`, `check_builder`, `sepa`, `registered` |
| `banks.tsv` | `scheme`, `bank_code` | `name`, `weight`; optional `bic`, `branch_codes`, `method`, `everyday_code`, `source` |
| `test_accounts.tsv` | `source`, `scheme`, `number` | optional `bank_code`, `branch_code`, `account_number` |
| `networks.tsv` | `network`, `range_from` | `range_to`, `length`, `lengths`, `weight`; optional `validity_years`, `min_age`, `format`, `luhn`, `source` |
| `card_networks.tsv` | `country_code`, `kind`, `network` | `band`, `evidence` |
| `test_cards.tsv` | `source`, `number` | `network`, `kind`; optional `country_code`, `expires`, `security_code` |
| `ownership.tsv` | `country_code`, `group` | `account`, `debit`, `credit`; optional `year`, `filled` |

## Sources

Each row of `schemes.tsv` and `banks.tsv` names its own. [`LICENSE_DATA.txt`](../LICENSE_DATA.txt) gives each source's terms.

| Data | Source | How reliable |
| --- | --- | --- |
| Population, currency, minor unit | country-generator | One current value per country, whatever the reference date. |
| IBAN structure | SWIFT IBAN Registry release 99, through schwifty 2026.11.0's parsed copy (MIT) | The structures are the registry's. The national-check positions of Russia and Iceland, and the territories' checks, were added here. |
| Bank lists | schwifty's lists; razorpay/ifsc (India); zengin-code (Japan); Banco Central do Brasil; Bank of Russia; Payments NZ's branch register; curated pages; GLEIF's BIC/LEI table where nothing better exists | Lists from an authority are complete. Others have what the source has. |
| Weights | Branch counts where a file has branches; FDIC deposits for the US; otherwise flat | Weights are flat in many lists, so a small bank is drawn as often as a large one. |
| German check methods | Deutsche Bundesbank, *Prüfzifferberechnungsmethoden und Prüfzifferverfahren* (June 2018) | The text of the authority. Twenty methods are implemented, covering 95.5% of codes. |
| New Zealand's check | Inland Revenue, *Payroll Calculations and Business Rules*, section 7 | The text of the authority. |
| Brazil's IBAN | Banco Central do Brasil's layout | The account type letter `P` for savings comes only from a third-party guide; the bank's registry example shows `C1`. |
| Russia's account | Bank of Russia's BIK directory; the key rule; the prefix `40817810` (a person's account, in roubles) from secondary sources | Structure and key are firm; the prefix is secondary. |
| Test numbers | Stripe's published values, read 2026-11-06 | Several are well-known examples at real banks' codes. |
| Card ranges | Wikipedia's *Payment card number* table; ActiveMerchant (Elo, Hipercard); BINDB (Verve, Troy) | All secondary: the networks' own documents were not found. The sandboxes' numbers do fall in these ranges. |
| Test cards | The sandbox pages of Stripe, Adyen and Braintree, read 2026-11-06 | The publishers' own, and they will age. The earliest published expiry is 2029-12. |
| Networks per country | Curated bands | **Judgement, not data.** No open source for network share by country was found, and the Nilson Report is paid. |
| Who holds what | World Bank, Global Findex Database 2025 | Each economy's latest **positive** value, with its year in the table (see below). |

## Generation Model

The call seed splits into `"country"` and `"birth_date"`, and each account by position into `"bank"`, `"number"` and `"opened"`. A given country or birth date moves nothing else, and asking for another account leaves the first ones unchanged.

1. **Country.** By population among countries that are not territories; flat with `weighted = false`. A territory only when named.
2. **Birth date.** Flat over ages 18 to 80 at the reference date, unless given.
3. **Account.** Each kind asked for gives one account in the country's primary scheme. A person under the scheme's minimum age has no account, which is not an error.
4. **Bank.** By `weight`; flat over banks of positive weight with `weighted = false`. A bank of weight 0 is never drawn and stays known to `is_valid`. Filtered banks (central banks, funds, wholesale banks and others) and banks with a German method that is not implemented have weight 0.
5. **Number.** See below, redrawing when a body admits no check digit, up to 64 times.
6. **Opened.** Flat from the minimum-age day to the reference date. No source gives a scheme's start date, so a date can predate the scheme: a 1983 IBAN is a number in today's form with an old opening date.

`kind` is a label, except in Brazil, whose IBAN carries it.

## Account Schemes

| `builder` | Countries | What is made | Checks |
| --- | --- | --- | --- |
| `iban` | 124 | The registry's BBAN with the bank and branch from the row, national check, MOD 97-10 | See below |
| `iban_br` | BR | IBAN from the ISPB, `0`, branch, account, type, position; everyday form `bank branch account` with the COMPE code | MOD 97-10; the everyday form has none |
| `iban_ru` | RU | IBAN from the BIK and a 20-digit account; everyday form `BIK account` | MOD 97-10; the Bank of Russia's key |
| `us_aba` | US | Routing number and account | 3-7-1 on the routing number, prefix in 00–12, 21–32, 61–72, 80 |
| `ng_nuban` | NG | Bank, 9 digits, check | 3-7-3 |
| `mx_clabe` | MX | Bank, branch, 11 digits, check | 3-7-1 |
| `ar_cbu` | AR | Two groups, each with its check | 3-1-7-9 from the right |
| `nz_account` | NZ | Bank, branch, account, suffix | Inland Revenue's weight set by bank |
| `national` | 23 | Bank, branch and account by pattern | Structure only |
| `bic_account` | 26 | A BIC and an account by pattern | Structure only |
| `generic` | 40 | Ten digits, `evidence` is `generic` | None |

An IBAN's letter positions are drawn as digits, since real accounts are mostly digits; `is_valid` still reads letters. So French IBANs begin `FR76` when the bank code is digits, as they nearly always do.

**National checks inside the BBAN** (`check_builder`):

| Check | Countries |
| --- | --- |
| `de_method`, by the bank's method | DE |
| `rib_key` | FR, MC, France's territories, MA, seven UEMOA and six CEMAC countries |
| `it_cin` | IT, SM |
| `mod97_bban` | BA, ME, MK, PT, RS, SI, TL, XK |
| `mod97_variant` | MR, TN |
| `cz` | CZ, SK |
| `be`, `ee`, `es`, `fi`, `is`, `no`, `pl`, `ru_key` | BE, EE, ES, FI and AX, IS, NO, PL, RU |

Three details:

- **Iceland.** The account ends with the holder's kennitala, built from the birth date, with its own check digit and century digit.
- **Poland.** The check digit is the last digit of the bank code.
- **Norway.** Sources differ on accounts with `00` in positions 5 and 6: schwifty counts only the next four digits, stdnum all of them. None is drawn, and `is_valid` accepts either reading.

**`is_valid`** takes the electronic or the display form. It checks the structure, every check digit, and a known bank when the scheme's list is complete. A scheme's published test numbers count as valid. It looks nothing up, and true does not mean an account exists. It is false for `unassigned_code`, `wrong_national` and `wrong_outer`, and true for `test_value` and `none`.

## Safe

`safe` gives accounts by each scheme's method. **What it promises varies, and for 88 countries with 62.8% of the world's people it promises nothing.** Those schemes have no check digit and no complete list, so a number there can match a real account.

| `safe_method` | What is made | `is_valid` | Countries (`lite`) | Countries (`full`) | People |
| --- | --- | --- | --- | --- | --- |
| `test_value` | A number a sandbox publishes | true | 21 | 21 | 9.1% |
| `unassigned_code` | The checks hold, and the bank code is in no row of a complete list | false | 4 | 10 | 0.2%, 2.3% |
| `wrong_national` | The outer check holds, and the national check fails | false | 42 | 39 | 6.7%, 5.3% |
| `wrong_outer` | The check digits fail, within 02 to 98; on a national form, its check digit | false | 65 | 62 | 21.3%, 20.5% |
| `none` | A normal draw. **No promise** | true | 88 | 88 | 62.8% |

By country, on `full`:

- `test_value`: AT AU BE CH DE EE ES FI FR GB GI HR IE LI LT LU NL NO PT SE US.
- `unassigned_code`: CZ GE HU IT LV PL RO SI SK UA.
- `wrong_national`: AX BA BF BJ BL CF CG CI CM GA GF GP GQ IS MA MC ME MF MK ML MQ MR NC NE PF PM RE RS RU SM SN TD TF TG TL TN WF XK YT.
- `wrong_outer`: AD AE AL AO AR AZ BG BH BI BR BY CR CV CY DJ DK DO EG FK FO GG GL GR GT GW HN IL IM IQ IR JE JO KM KW KZ LB LC LY MD MG MN MT MU MX MZ NG NI NZ OM PK PS QA SA SC SD SO ST SV TR VA VG YE.
- `none`: all the others, including CN, IN, ID, JP, KR, CA, ZA and every generic country.

On `lite`, a shortened list cannot show a code is unassigned, so CZ, IT and PL use `wrong_national` there, and HU, RO and UA use `wrong_outer`. `schemes.tsv` of each tier is the reference.

Limits of each method:

- A `test_value` number can be at a real bank's code, as `DE89 3704 0044 0532 0130 00` is at Commerzbank's. It promises "a number a sandbox publishes", and the list is finite, so a unique batch above its size is refused.
- `wrong_national` and `wrong_outer` make the whole number invalid. Its bank, branch and account fields alone can still be real.
- Brazil's everyday form has no check digit, so a safe Brazilian account shows its IBAN in `formatted`. Its `bank_code`, `branch_code` and `account_number` remain a plausible everyday account.

Every safe result has `reserved` set and `bank_name` empty. A BIC has `0` as its 8th character, a test BIC by SWIFT's convention, which comes from secondary sources and may not hold outside FIN. An `unassigned_code` account has no BIC.

## Cards

A card's stream is `split("card").split(position)`, with `kind`, `network`, `number`, `dates` and `code` inside it, so another card leaves the first ones unchanged. `ownership` is a stream of its own, used only when `holding = surveyed`.

**There are no issuer ranges.** The number encodes the network and nothing else, although open copies of issuer tables exist. `issuer` copies the first account's bank name.

**A default card may be real.** These are estimates: the card count is Nilson's end-2023 figure as a secondary source gives it, 17.45 billion cards with Visa at 25%, so about 4.4 billion, and it assumes every Visa card has 16 digits. Visa's whole range has 10^14 numbers with a right Luhn digit, which is about 1 real card in 22,000 draws, or around 45 in a million. A number from one bank's range would be about 20 times likelier, which is why the library has no such ranges and never pairs a `network_range` number with a security code.

**`wrong_check` and Luhn.** A number failing Luhn is safe only if every card of the network passes it, and no network's own text was found to say so. The sandboxes' numbers all pass, and general descriptions of card numbers say the last digit is a Luhn digit. Two networks are marked `luhn` = `unsure` and get no `wrong_check`: Hipercard, which ActiveMerchant exempts from the checksum, and UnionPay, which Chromium and Interchange have both exempted while other software enforces it. Other networks' `wrong_check` numbers are drawn outside those two networks' ranges, because RuPay's 60 and 81 hold theirs. `card_valid` still requires Luhn for those two, so it can refuse a real card of theirs.

**Which network.** `card_networks.tsv` gives bands per country: main 8, common 2, rare 0.5. The default is Visa main, Mastercard main, American Express rare. A country's own rows replace the default, and rows for one kind replace its rows for every kind. With `test_number`, the draw is among networks with a number for that country, and then the country-free list. A network's ranges are drawn by `weight`: Mastercard's 51-55 is weighted 4 to 1 over 2221-2720, a judgement that the older range still holds most cards. When two networks of one country overlap, the number is redrawn to lie outside the other, up to 64 times.

**Dates.** Validity is 3, 4 or 5 years, a judgement since networks publish no rule. The card is the one running at the reference date, so `issued` is flat over the days whose card still runs then, and not before the holder's minimum age. A test number keeps its published expiry, with `issued` flat over the last 5 years, so its validity can reach many years. Braintree states no expiry, so its numbers use the drawn rule.

**Who holds what.** `ownership.tsv` holds Findex's shares for accounts, debit cards and credit cards, for all adults, 15 to 24, and 25 and over.

- **A credit-card share of 0 for 2024 is not data.** Findex reports 0 for the US, Germany, Japan, the UK, France, Australia, Canada, Korea and 35 others, whose 2021 values run from 40% to 70%. Its microdata shows the question went unasked (Greece has 0 valid and 1,004 missing answers). So each economy takes its latest positive value, with the year in the table. Some are old (Qatar and Syria, 2011). 40 countries with none take the subregion's population-weighted mean, marked `filled`.
- **A drawn kind** is debit or credit by the country's ratio of the two. Prepaid is drawn only when asked for.
- **`surveyed`** draws one chance for accounts and debit cards together, so a debit holder has an account wherever the shares allow, and a second for credit cards. All accounts or debit cards in one request come or go together. Findex has no share for prepaid, so a prepaid card is always held. A person of 15 to 24 uses that group's share, and everyone older uses 25 and over.

**Holder.** Upper-cased and folded to ASCII from Latin-1 and Latin Extended-A, keeping hyphens and apostrophes and dropping full stops. 26 characters at most, ISO/IEC 7813's name field by secondary sources since the standard is paid. Later given names shrink to initials first, then the first, then the end is cut. Any other character leaves `holder` empty rather than guessing.

## Rebuilding the Data

```
python scripts/fetch_raw.py
python scripts/prepare_countries.py
python scripts/prepare_iban.py
python scripts/prepare_schemes.py
python scripts/prepare_banks.py
python scripts/prepare_cards.py
python scripts/prepare_ownership.py
python scripts/check_finance.py
make embed
```

`prepare_iban`, `prepare_schemes` and `prepare_banks` must run in that order. A rebuild is byte-identical. Raw files stay out of the repository.

`scripts/validate_accounts.py` checks the library's output against implementations it does not share: schwifty, python-stdnum (LGPL, used only by this script, from a virtual environment, never shipped), and code written in the script from the national rules. It draws 1,000 people with two accounts each per country at three dates, plus a safe sample. On 2026-11-06 both tiers met the target, of 1,320,000 realistic and 420,000 safe accounts each.

| Oracle | Schemes reached |
| --- | --- |
| schwifty, IBAN structure and positions | 126 |
| schwifty, national check | 24 |
| stdnum, IBAN | 89 |
| stdnum, BIC | 135 |
| stdnum, routing number, CBU and New Zealand | 3 |
| Code in the script (RIB key of the territories, Russia's key, CLABE, NUBAN, New Zealand) | 31 |
| Nothing but structure | 89 |

Three disagreements were read and are not our errors:

- stdnum reduces New Zealand's E and G products with `% 9`, where Inland Revenue's text adds the digits twice. They differ on 18, 27, 36, 45, 54 and 63, so the script uses the text.
- stdnum's Belgian table lacks codes on the National Bank's register, such as 606.
- stdnum has no entries for the territories and the forms outside the registry.

## Known Limitations

- **China** (17.6% of the world's people) is on secondary sources: four flat banks from one guide, and no CNAPS check rule.
- **No named bank for 72 countries, 18.8% of people.** Their bank codes are drawn from the pattern.
- **Flat weights** in many lists, and a retail filter that works by name, so a person can bank at a small or specialised institution. Spain's list, with abbreviated names, shows this most.
- **`pe-cci` and `dz-rib`** ship with structure only, and no rule is known to us for their checks.
- **BF, ML, TG and the CEMAC keys** have no sample of their own; DZ and GW have conflicting lengths.
- **Currency is one value.** A person in Croatia in 2005 gets the euro, and Bulgaria's lev, Cuba's convertible peso and Zimbabwe's dollar are country-generator's current values, some of them since replaced.
- **Bank lists age.** Banks merge, and a list has no dates of its own.
- **New Zealand's banks 09, 26, 28, 29, 33 and 35** have check rules from stdnum's table where Inland Revenue's current table lists no such bank; example 3 of the text does use bank 26 with algorithm G.
- **Card ranges are secondary,** and BC Card is left out, since no range was found for it. Network shares per country are judgement, and so is the Mastercard weighting.
- **`wrong_check` rests on secondary evidence.** Only UnionPay and Hipercard were found to be in doubt, and no network's own text was read.
- **A prepaid card has no survey behind it,** and `surveyed` always gives one.
- **Licences.** SWIFT's clause on the registry is still to be read, and zengin-code and the Payments NZ register state no licence; only facts are used. See `LICENSE_DATA.txt`.
