// Draws accounts for scripts/validate_accounts.py and writes one row per account.
//
//   sample_finance <resource dir> <people per country and date> <accounts.tsv> <cards.tsv> [<examples.tsv>]
//
// With examples, also prints each `scheme<TAB>number` line that is_valid refuses.
//
// Every country and territory, realistic at three dates and safe at the default
// one. Each person holds a current and a savings account, so Brazil's account
// type is in the sample too. Cards take a fifth as many people, each holding one of
// each kind and one drawn, from each source.

#include "../dasmig/fingen.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <string>

namespace
{

using namespace std::chrono;

constexpr auto realistic_dates = {year{2005} / June / day{1}, year{2015} / June / day{1}, year{2023} / July / day{1}};
constexpr auto safe_dates = {year{2023} / July / day{1}};
// Sandboxes' published expiries run from 2029 to 2034, so a late date is in the sample too.
constexpr auto test_dates = {year{2023} / July / day{1}, year{2031} / March / day{31}};

std::string iso(const year_month_day& d)
{
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(d.year()), static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()));
}

void sample(const dasmig::fng& gen, int count, bool safe, const auto& dates, std::ofstream& out)
{
    for (const year_month_day reference : dates)
    {
        for (const auto& country : gen.countries())
        {
            dasmig::finance_request q;
            q.country = country;
            q.accounts = {dasmig::account_kind::current, dasmig::account_kind::savings};
            q.reference_date = reference;
            q.safe = safe;
            for (int i = 0; i < count; ++i)
            {
                const auto seed = (static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ULL) + 1U;
                const auto v = gen.get_finance(q, seed);
                for (const auto& a : v.accounts)
                {
                    const auto& text = a.iban.empty() ? a.formatted : a.iban;
                    out << (safe ? "1" : "0") << '\t' << country << '\t' << a.scheme << '\t'
                        << (a.kind == dasmig::account_kind::savings ? "savings" : "current") << '\t' << a.iban << '\t'
                        << a.formatted << '\t' << a.bank_code << '\t' << a.branch_code << '\t' << a.account_number << '\t'
                        << a.bic << '\t' << a.bank_name << '\t' << a.currency << '\t' << iso(v.birth_date) << '\t'
                        << iso(a.opened) << '\t' << iso(reference) << '\t' << (gen.is_valid(a.scheme, text) ? "1" : "0")
                        << '\t' << (gen.is_valid(a.scheme, a.formatted) ? "1" : "0") << '\n';
                }
            }
        }
    }
}

const char* source_name(dasmig::card_source s)
{
    switch (s)
    {
    case dasmig::card_source::test_number: return "test_number";
    case dasmig::card_source::wrong_check: return "wrong_check";
    default: return "network_range";
    }
}

const char* kind_name(dasmig::card_kind k)
{
    switch (k)
    {
    case dasmig::card_kind::credit: return "credit";
    case dasmig::card_kind::prepaid: return "prepaid";
    default: return "debit";
    }
}

void sample_cards(const dasmig::fng& gen, int count, dasmig::card_source source, const auto& dates, std::ofstream& out)
{
    for (const year_month_day reference : dates)
    {
        for (const auto& country : gen.countries())
        {
            dasmig::finance_request q;
            q.country = country;
            q.accounts = {};
            q.cards = {std::nullopt, dasmig::card_kind::debit, dasmig::card_kind::credit, dasmig::card_kind::prepaid};
            q.card_numbers = source;
            q.reference_date = reference;
            for (int i = 0; i < count; ++i)
            {
                const auto seed = (static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ULL) + 1U;
                const auto v = gen.get_finance(q, seed);
                for (const auto& c : v.cards)
                {
                    out << source_name(source) << '\t' << country << '\t' << c.network << '\t' << kind_name(c.kind) << '\t'
                        << c.number << '\t' << c.formatted << '\t' << c.security_code << '\t' << c.test_source << '\t'
                        << (c.reserved ? "1" : "0") << '\t' << iso(v.birth_date) << '\t' << iso(c.issued) << '\t'
                        << std::format("{:04}-{:02}", static_cast<int>(c.expires.year()), static_cast<unsigned>(c.expires.month()))
                        << '\t' << iso(reference) << '\t' << (gen.card_valid(c.number) ? "1" : "0") << '\n';
                }
            }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 5 && argc != 6)
    {
        std::cerr << "usage: sample_finance <resource dir> <count> <accounts.tsv> <cards.tsv> [<examples.tsv>]\n";
        return 2;
    }
    dasmig::fng gen;
    gen.load(std::filesystem::path{argv[1]});
    if (!gen.has_data())
    {
        std::cerr << "no data in " << argv[1] << '\n';
        return 1;
    }
    std::ofstream out{argv[3], std::ios::binary};
    out << "safe\tcountry\tscheme\tkind\tiban\tformatted\tbank\tbranch\taccount\tbic\tbank_name\tcurrency\tbirth\topened\treference\town_number\town_formatted\n";
    const int count = std::atoi(argv[2]);
    sample(gen, count, false, realistic_dates, out);
    sample(gen, count, true, safe_dates, out);

    std::ofstream cards{argv[4], std::ios::binary};
    cards << "source\tcountry\tnetwork\tkind\tnumber\tformatted\tsecurity_code\ttest_source\treserved\tbirth\tissued\texpires\treference\town_valid\n";
    const int holders = std::max(count / 5, 1);
    sample_cards(gen, holders, dasmig::card_source::network_range, realistic_dates, cards);
    sample_cards(gen, holders, dasmig::card_source::wrong_check, safe_dates, cards);
    sample_cards(gen, holders, dasmig::card_source::test_number, test_dates, cards);
    if (argc == 6)
    {
        std::ifstream examples{argv[5]};
        std::string line;
        while (std::getline(examples, line))
        {
            const auto tab = line.find('\t');
            if (!gen.is_valid(line.substr(0, tab), line.substr(tab + 1))) std::cout << "refused\t" << line << '\n';
        }
    }
    return out.good() && cards.good() ? 0 : 1;
}
