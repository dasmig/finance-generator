#include "../dasmig/fingen.hpp"
#include <cstdint>
#include <iostream>

int main()
{
    // The singleton loads the lite tier (compiled in, or from resources/).
    auto& gen = dasmig::fng::instance();
    if (!gen.has_data())
    {
        std::cerr << "Could not load the finance data.\n";
        return 1;
    }

    std::cout << "Random people, drawn by population:\n";
    for (int i = 0; i < 5; ++i)
    {
        std::cout << "  " << gen.get_finance(dasmig::finance_request{}) << '\n';
    }

    std::cout << "\nA person in Germany, as JSON, from a fixed seed:\n";
    dasmig::finance_request germany;
    germany.country = "DE";
    std::cout << "  " << gen.get_finance(germany, std::uint64_t{42}).to_json() << '\n';

    std::cout << "\nTwo accounts in Brazil, whose IBAN carries the account type:\n";
    dasmig::finance_request brazil;
    brazil.country = "BR";
    brazil.accounts = {dasmig::account_kind::current, dasmig::account_kind::savings};
    for (const auto& account : gen.get_finance(brazil, std::uint64_t{42}).accounts)
    {
        std::cout << "  " << account.iban << "  " << account.formatted << "  " << account.bank_name << '\n';
    }

    std::cout << "\nSafe accounts, by each scheme's method (doc/data.md):\n";
    for (const auto* country : {"DE", "IT", "US", "KE"})
    {
        dasmig::finance_request safe;
        safe.country = country;
        safe.safe = true;
        const auto account = gen.get_finance(safe, std::uint64_t{42}).accounts.front();
        std::cout << "  " << country << ' ' << account.formatted << "  valid: "
                  << (gen.is_valid(account.scheme, account.formatted) ? "yes" : "no") << '\n';
    }

    std::cout << "\nA card from a sandbox's published numbers, which safe uses:\n";
    dasmig::finance_request carded;
    carded.country = "DE";
    carded.safe = true;
    carded.given_names = "Maria";
    carded.surname = "Garcia";
    carded.cards = {dasmig::card_kind::debit};
    const auto card = gen.get_finance(carded, std::uint64_t{42}).cards.front();
    std::cout << "  " << card.network << ' ' << card.formatted << "  " << card.holder << "  from " << card.test_source
              << ", valid: " << (gen.card_valid(card.number) ? "yes" : "no") << '\n';

    std::cout << "\nA number with a wrong check digit, which no card has:\n";
    dasmig::finance_request wrong;
    wrong.country = "JP";
    wrong.cards = {dasmig::card_kind::credit};
    wrong.card_numbers = dasmig::card_source::wrong_check;
    const auto refused = gen.get_finance(wrong, std::uint64_t{42}).cards.front();
    std::cout << "  " << refused.network << ' ' << refused.formatted << ", valid: " << (gen.card_valid(refused.number) ? "yes" : "no") << '\n';

    std::cout << "\nWith a layer that adds a bank to Andorra:\n";
    gen.extend_from_string(dasmig::finance_table::banks,
                           "scheme\tbank_code\tname\tweight\niban-ad\t9999\tBanc de Prova\t100000\n", "example.tsv");
    dasmig::finance_request andorra;
    andorra.country = "AD";
    for (std::uint64_t seed = 0; seed < 3; ++seed)
    {
        std::cout << "  " << gen.get_finance(andorra, seed) << '\n';
    }
    return 0;
}
