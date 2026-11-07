# Usage Guide

This guide covers every feature of the finance-generator library. For a quick overview, see the [README](../README.md). For the full API reference, run `doxygen Doxyfile` from the repository root and open `doc/api/html/index.html`.

[TOC]

<!-- GitHub-rendered TOC (Doxygen uses [TOC] above instead) -->
<ul>
<li><a href="#quick-start">Quick Start</a></li>
<li><a href="#installation">Installation</a></li>
<li><a href="#loading-resources">Loading Resources</a></li>
<li><a href="#generating">Generating</a></li>
<li><a href="#safe-accounts">Safe Accounts</a></li>
<li><a href="#checking-a-number">Checking a Number</a></li>
<li><a href="#seeding-and-deterministic-generation">Seeding and Deterministic Generation</a></li>
<li><a href="#extending-data">Extending Data</a></li>
<li><a href="#replay-and-json">Replay and JSON</a></li>
<li><a href="#compiled-in-data">Compiled-in Data</a></li>
<li><a href="#thread-safety">Thread Safety</a></li>
<li><a href="#error-reference">Error Reference</a></li>
</ul>

## Quick Start

```cpp
#include <dasmig/fingen.hpp>
#include <iostream>

int main()
{
    auto& gen = dasmig::fng::instance();
    std::cout << gen.get_finance(dasmig::finance_request{}) << '\n';
}
```

## Installation

Copy `dasmig/fingen.hpp`, `dasmig/core.hpp` and the `dasmig/core/` directory into your include path, keeping them together. Supply `resources/` next to the program, or define `DASMIG_FINANCE_EMBED` to compile the `lite` tier in.

## Loading Resources

`instance()` loads the `lite` tier on first use (or `full` if there is no `lite`), from the compiled-in copy or from the first of `resources/`, `../resources`, `finance-generator/resources` and `../finance-generator/resources` that has it.

```cpp
dasmig::fng gen;                       // an empty, independent instance
bool ok = gen.load(dasmig::dataset::lite); // false when the tier is not found
gen.load(std::filesystem::path{"path/to/resources/lite"}); // a directory
gen.has_data();
gen.countries();            // every country and territory
gen.schemes("DE");          // {"iban-de"}
```

Each `load` replaces the data and every layer loaded before.

## Generating

```cpp
auto v = gen.get_finance(dasmig::finance_request{});      // draws a seed from the instance engine
auto w = gen.get_finance(dasmig::finance_request{}, 1234);  // deterministic; does not touch the engine
v.country_code;             // "DE"
v.birth_date;               // given or drawn
v.accounts.front().iban;    // "DE69613700860438549826"
```

A request says what to generate, and nothing is stored on the generator:

```cpp
using namespace std::chrono;
dasmig::finance_request q;
q.country = "BR";                         // any case; unset: drawn by population
q.birth_date = year{1980} / May / day{17}; // unset: drawn for ages 18 to 80
q.reference_date = year{2015} / June / day{1}; // the date the person is seen at
q.accounts = {dasmig::account_kind::current, dasmig::account_kind::savings}; // one account each
q.weighted = false;                       // flat over countries and banks
q.safe = true;                            // see below
```

A given field leaves every other stream's draws unchanged, and asking for another account leaves the first ones unchanged. A person below the scheme's minimum age has no account, which is not an error. A territory such as `YT` is drawn only when named.

Each account has:

| Field | Meaning |
| --- | --- |
| `scheme` | `"iban-de"`, `"us-aba"`, `"mx-clabe"`, `"generic-ke"` |
| `kind` | As asked for. A label, except in Brazil, whose IBAN carries it |
| `iban` | Electronic form; empty where the country has no IBAN form |
| `formatted` | Display form: `DE69 6137 0086 0438 5498 26`, `021000021 123456789` |
| `bank_code`, `branch_code` | The everyday code where a country has two; the branch is empty when there is none |
| `account_number` | What follows the bank and branch, with its own check digit |
| `bank_name`, `bic` | Empty when no listed bank has the code |
| `currency`, `currency_digits` | ISO 4217 and its minor unit |
| `opened` | From the minimum age to the reference date |
| `evidence` | `issuer`, `reference`, `secondary` or `generic`: how much is known of the form |
| `reserved` | The safe method was applied |

## Safe Accounts

Accounts drawn without `safe` are structurally valid and may match a real person's account. The library cannot tell. They are test data: never use them for payments, direct debits, to open accounts or to pass a check. The library looks nothing up.

`safe = true` applies each scheme's method, and **what it promises varies**:

| Method | What is made | `is_valid` | People, on `full` |
| --- | --- | --- | --- |
| `test_value` | A number a sandbox publishes | true | 9.1% |
| `unassigned_code` | The checks hold, and no bank has the code | false | 2.3% |
| `wrong_national` | The outer check holds, and the national check fails | false | 5.3% |
| `wrong_outer` | The check digits fail | false | 20.5% |
| `none` | A normal draw: **no promise** | true | 62.8% |

`none` covers 88 countries, including China, India, Indonesia and Japan, whose forms have no check digit and whose lists are incomplete. Check [`doc/data.md`](data.md#safe) for your country before relying on `safe`.

## Checking a Number

```cpp
gen.is_valid("iban-de", "DE89 3704 0044 0532 0130 00"); // the display or electronic form
```

`is_valid` checks the structure, every check digit, and a known bank when the scheme's list is complete. It looks nothing up, and true does not mean an account exists. An unknown scheme throws.

## Seeding and Deterministic Generation

- `get_finance(request, seed)` gives the same result for the same request, seed and data, on every platform, without advancing the instance engine.
- `get_finance(request)` draws a fresh seed from the instance engine, initially seeded from system entropy. There are no generator-level `seed()` or `unseed()` controls; for deterministic sequences, keep a `dasmig::core::rng` in the caller and pass its next value as the seed.
- `v.seed()` is the seed a result was generated from: `get_finance(dasmig::finance_request{}, v.seed())` replays it with the same request and data.

## Extending Data

The shipped data is a base layer. Files with the same columns as a table can be loaded on top of it to add rows, change them or remove them, without editing the shipped file. [`doc/data.md`](data.md#columns) gives each table's key.

```cpp
// Override mode (the default): a row replaces the loaded row with its key;
// new keys are added. A file must be named after its table; a directory may hold several.
gen.extend("mods/banks.tsv");
gen.extend("mods/");

// From text, for example supplied by a WebAssembly host.
gen.extend_from_string(dasmig::finance_table::banks,
                       "scheme\tbank_code\tname\tweight\niban-ad\t9999\tMy Bank\t10\n", "scenario");

// Remove rows: a row with disabled set to 1.
gen.extend_from_string(dasmig::finance_table::banks,
                       "scheme\tbank_code\tname\tweight\tdisabled\niban-ad\t9999\tx\t1\t1\n", "cleanup");

// Add mode refuses to touch keys that are already loaded.
gen.extend("mods/banks.tsv", dasmig::core::layer_mode::add);
```

- Every value in a layer is checked before anything changes. A bad layer throws `dasmig::core::tsv_error` with the file, line and column, or `std::invalid_argument` when the tables no longer fit together, and the data stays as it was.
- `layers(table)` lists what is loaded: source, mode, rows added, replaced and removed, and warnings such as unknown columns.
- `load(...)` replaces the base and drops every layer.

## Replay and JSON

Every result records the seed and the data it came from:

```cpp
auto v = gen.get_finance(dasmig::finance_request{});
v.signature().seed; // the call seed
v.signature().data; // gen.data_hash() when it was generated
```

`to_json()` gives the result as JSON, the form the WebAssembly layer and the game consume. Seeds are strings because JavaScript numbers cannot hold every 64-bit value. `to_json(writer)` writes into an existing `dasmig::core::json_writer`, for composing larger documents.

## Compiled-in Data

Define `DASMIG_FINANCE_EMBED` before including the header to compile the `lite` tier into the program. `load(dataset::lite)` and `instance()` then use it and need no files. The compiled-in data has the same `data_hash()` as the file, so seeds replay the same either way.

`dasmig/fingen_data.hpp` is generated from `resources/lite` by `make embed`; CI checks it is up to date.

## Thread Safety

Each `fng` instance is independent. Concurrent calls to `get_finance(request)` on the **same** instance require external synchronisation (e.g. a `std::mutex`).

## Batch Generation

Every seeded getter has batch forms: `get_finance_batch` return a `std::vector`, and `for_each_finance` streams the items to a sink in index order without holding the batch.

```cpp
auto& gen = dasmig::fng::instance();
const auto request = dasmig::finance_request{};

// 1,000 results from one batch seed, on four threads.
auto items = gen.get_finance_batch(request, {.count = 1000, .threads = 4}, 42);

// Item 1 replays on its own: the batch seed and the index give its seed.
auto again = gen.get_finance(request, dasmig::core::item_seed(42, 1));

// A million results to a file as JSON Lines, never all in memory.
std::ofstream out{"items.jsonl"};
dasmig::core::jsonl_writer lines{out};
gen.for_each_finance(request, {.count = 1'000'000, .threads = 0}, 42, lines);
```

- **Item seeds.** Item `i` is exactly `get_finance(request, core::item_seed(batch_seed, first + i))`. A range can be generated directly (`first = 1000` starts at item 1000), two batches that follow each other equal one, and the output is the same for any `threads` (0 means the hardware count). Without a `batch_seed` one is drawn like the single getter draws its own.
- **Options.** `core::batch_options` has `count`, `first`, `threads` and `unique`. The request describes one item; the options describe the batch.
- **Uniqueness.** `.unique = core::unique_rule::by<finance>(key)` regenerates an item whose key already appeared in the batch, with a deterministic retry seed, and throws `std::runtime_error` naming the item when the attempts run out.
- **Sinks.** `core::jsonl_writer` writes one `to_json()` per line, `core::csv_writer` flattens each result's JSON into dotted columns. Any callable `(std::uint64_t index, finance&)` works.
- Batch calls use the same const getters from several threads, which the thread-safety rules above allow; the CI runs the batch tests under ThreadSanitizer.

## Error Reference

| Exception | When |
|-----------|------|
| `std::runtime_error` | Any generation call before data is loaded |
| `std::runtime_error` | A unique batch runs out of attempts; a scheme's number cannot be drawn in 64 tries |
| `std::invalid_argument` | An unknown country or scheme; a birth date after the reference date or before 1900; an invalid date; a reference date outside 1900 to 2090; a layer that leaves the tables not fitting together (the data is unchanged) |
| `dasmig::core::tsv_error` | A malformed table or layer (message gives file, line and column); the data is unchanged |
