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

    run.batches("get_finance_batch", [&](std::size_t count, std::size_t threads, std::uint64_t seed) {
        return gen.get_finance_batch({}, {.count = count, .threads = threads}, seed);
    });

    return run.finish();
}
