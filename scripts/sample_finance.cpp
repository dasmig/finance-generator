// Draws accounts for scripts/validate_accounts.py and writes one row per account.
//
//   sample_finance <resource dir> <people per country and date> <accounts.tsv>
//
// Every country and territory, realistic at three dates and safe at the default
// one. Each person holds a current and a savings account, so Brazil's account
// type is in the sample too.

#include "../dasmig/fingen.hpp"

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

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "usage: sample_finance <resource dir> <count> <accounts.tsv>\n";
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
    return out.good() ? 0 : 1;
}
