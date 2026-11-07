// Benchmarks for finance-generator; see bench.hpp for the options.

#include "../dasmig/fingen.hpp"
#include "bench.hpp"

int main(int argc, char** argv)
{
    dasmig::bench::runner run{"finance-generator", argc, argv};

    run.load("load", [&] {
        dasmig::fng gen;
        return gen.load(run.tier());
    });

    dasmig::fng gen;
    if (!gen.load(run.tier())) return run.missing_tier();

    run.calls("get_finance", [&](std::uint64_t seed) { return gen.get_finance({}, seed); });
    run.calls("get_finance to_json", [&](std::uint64_t seed) {
        return gen.get_finance({}, seed).to_json();
    });

    dasmig::finance_request with_card;
    with_card.cards = {dasmig::card_kind::debit};
    run.calls("get_finance with a card", [&](std::uint64_t seed) { return gen.get_finance(with_card, seed); });
    dasmig::finance_request with_test_card = with_card;
    with_test_card.card_numbers = dasmig::card_source::test_number;
    run.calls("get_finance with a test card", [&](std::uint64_t seed) { return gen.get_finance(with_test_card, seed); });

    run.batches("get_finance_batch", [&](std::size_t count, std::size_t threads, std::uint64_t seed) {
        return gen.get_finance_batch({}, {.count = count, .threads = threads}, seed);
    });

    return run.finish();
}
