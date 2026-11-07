#include "catch_amalgamated.hpp"
#include "../dasmig/fingen.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using dasmig::account_kind;
using dasmig::bank_account;
using dasmig::finance;
using dasmig::finance_request;
using dasmig::finance_table;
using dasmig::fng;
namespace core = dasmig::core;
namespace detail = dasmig::fingen_detail;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::filesystem::path resource_dir(const std::string& tier)
{
    for (const auto* base : {"resources", "../resources"})
    {
        const auto candidate = std::filesystem::path{base} / tier;
        if (std::filesystem::is_directory(candidate)) return candidate;
    }
    FAIL("no resources directory");
    return {};
}

static std::chrono::year_month_day date(int y, unsigned m, unsigned d)
{
    return std::chrono::year_month_day{std::chrono::year{y}, std::chrono::month{m}, std::chrono::day{d}};
}

static const fng& shared_generator()
{
    static const fng gen = []
    {
        fng loaded;
        loaded.load(resource_dir("lite"));
        return loaded;
    }();
    return gen;
}

static const fng& shared_full_generator()
{
    static const fng gen = []
    {
        fng loaded;
        loaded.load(resource_dir("full"));
        return loaded;
    }();
    return gen;
}

static finance_request in(const std::string& country)
{
    finance_request q;
    q.country = country;
    return q;
}

// What the tests read back from the tables.
struct table_view
{
    std::map<std::string, std::string> primary;   // country to scheme
    std::map<std::string, std::string> currency;  // country to currency
    std::map<std::string, double> population;     // the countries a draw can land on
    std::map<std::string, std::string> safe;      // scheme to method
    std::map<std::string, std::string> builder;   // scheme to builder
    std::map<std::string, std::string> book_of;   // scheme to the scheme whose banks it takes
    std::map<std::string, std::size_t> length;    // country to IBAN length
    std::map<std::string, std::array<int, 4>> at; // country to bank and branch positions
    std::map<std::string, std::map<std::string, double>> banks; // scheme to code to weight
    std::set<std::string> tests;                  // published numbers, stripped
};

static int position(const core::tsv_row& row, std::string_view column)
{
    return row[column].empty() ? -1 : std::stoi(std::string{row[column]});
}

static const table_view& view(const char* tier)
{
    static std::map<std::string, table_view> views;
    auto [it, fresh] = views.try_emplace(tier);
    if (!fresh) return it->second;
    auto& v = it->second;
    const auto dir = resource_dir(tier);
    const auto countries = core::tsv_document::load(dir / "countries.tsv");
    for (const auto& row : countries.rows())
    {
        const std::string code{row["country_code"]};
        v.primary[code] = row["primary_scheme"];
        v.currency[code] = row["currency"];
        if (row["territory"].empty()) v.population[code] = row.get<double>("population");
    }
    const auto schemes = core::tsv_document::load(dir / "schemes.tsv");
    for (const auto& row : schemes.rows())
    {
        const std::string key{row["key"]};
        v.safe[key] = row["safe_method"];
        v.builder[key] = row["builder"];
        v.book_of[key] = row["banks_from"].empty() ? key : std::string{row["banks_from"]};
    }
    const auto formats = core::tsv_document::load(dir / "iban_formats.tsv");
    for (const auto& row : formats.rows())
    {
        const std::string code{row["country_code"]};
        v.length[code] = row.get<std::size_t>("length");
        v.at[code] = {position(row, "bank_from"), position(row, "bank_to"), position(row, "branch_from"), position(row, "branch_to")};
    }
    const auto banks = core::tsv_document::load(dir / "banks.tsv");
    for (const auto& row : banks.rows()) v.banks[std::string{row["scheme"]}][std::string{row["bank_code"]}] = row.get<double>("weight");
    const auto tests = core::tsv_document::load(dir / "test_accounts.tsv");
    for (const auto& row : tests.rows()) v.tests.insert(detail::strip_separators(row["number"]));
    return v;
}

static std::string slice(std::string_view text, int from, int to)
{
    return from < 0 ? std::string{} : std::string{text.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from))};
}

// What must hold of every realistic account.
static void require_coherent(const fng& gen, const table_view& v, const finance& f, const bank_account& a, std::chrono::year_month_day reference)
{
    CAPTURE(a.scheme, a.iban, a.formatted, a.bank_code);
    REQUIRE(a.scheme == v.primary.at(f.country_code));
    REQUIRE(a.currency == v.currency.at(f.country_code));
    REQUIRE_FALSE(a.reserved);
    REQUIRE(gen.is_valid(a.scheme, a.formatted));

    const auto& builder = v.builder.at(a.scheme);
    const auto& book_key = v.book_of.at(a.scheme);
    const bool has_book = v.banks.contains(book_key);
    if (!a.iban.empty())
    {
        REQUIRE(a.iban.starts_with(f.country_code));
        REQUIRE(a.iban.size() == v.length.at(f.country_code));
        REQUIRE(core::iban_valid(a.iban));
        REQUIRE(gen.is_valid(a.scheme, a.iban));
        if (builder == "iban")
        {
            const auto& at = v.at.at(f.country_code);
            const std::string_view bban = std::string_view{a.iban}.substr(4);
            REQUIRE(a.bank_code == slice(bban, at[0], at[1]));
            REQUIRE(a.branch_code == slice(bban, at[2], at[3]));
            REQUIRE(a.iban.ends_with(a.account_number));
            if (has_book) REQUIRE(v.banks.at(book_key).contains(a.bank_code));
        }
    }
    else if (has_book && builder != "bic_account" && builder != "generic")
    {
        REQUIRE(v.banks.at(book_key).contains(a.bank_code));
    }
    if (has_book && !a.iban.empty() && builder == "iban") REQUIRE(v.banks.at(book_key).at(a.bank_code) > 0.0);

    const auto opened = std::chrono::sys_days{a.opened};
    REQUIRE(opened <= std::chrono::sys_days{reference});
    REQUIRE(opened >= std::chrono::sys_days{detail::ymd_of(detail::anniversary(detail::to_day(f.birth_date), 16))});
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

TEST_CASE("load(dataset::lite) loads every table", "[loading]")
{
    fng gen;
    gen.load(resource_dir("lite"));
    REQUIRE(gen.has_data());
    REQUIRE(gen.countries().size() == view("lite").primary.size());
    REQUIRE(gen.data_hash() != 0);
    for (const auto t : {finance_table::countries, finance_table::schemes, finance_table::iban_formats, finance_table::banks,
                         finance_table::test_accounts, finance_table::networks, finance_table::card_networks,
                         finance_table::test_cards, finance_table::ownership})
    {
        REQUIRE(gen.layers(t).size() == 1);
        REQUIRE(gen.layers(t).front().added > 0);
    }
}

TEST_CASE("the full tier loads the same countries and schemes with more banks", "[loading]")
{
    const auto& full = shared_full_generator();
    const auto& lite = shared_generator();
    REQUIRE(full.countries() == lite.countries());
    REQUIRE(full.data_hash() != lite.data_hash());
    REQUIRE(full.layers(finance_table::banks).front().added > lite.layers(finance_table::banks).front().added);
}

TEST_CASE("a path without the tables is a no-op", "[loading]")
{
    fng gen;
    gen.load(std::filesystem::path{"does/not/exist"});
    REQUIRE_FALSE(gen.has_data());
    REQUIRE(gen.data_hash() == 0);
    REQUIRE_THROWS_AS(gen.get_finance(finance_request{}), std::runtime_error);
    REQUIRE_THROWS_AS(gen.get_finance(finance_request{}, 1), std::runtime_error);
}

TEST_CASE("the singleton loads data on first use", "[loading]")
{
    REQUIRE(fng::instance().has_data());
}

TEST_CASE("load(dataset) serves the same data as the files", "[loading]")
{
    fng gen;
    REQUIRE(gen.load(dasmig::dataset::lite));
    REQUIRE(gen.data_hash() == shared_generator().data_hash());
    REQUIRE(gen.get_finance(finance_request{}, 5).to_json() == shared_generator().get_finance(finance_request{}, 5).to_json());
}

TEST_CASE("a moved generator keeps its data", "[loading]")
{
    fng gen;
    gen.load(resource_dir("lite"));
    const auto before = gen.get_finance(in("DE"), 3).to_json();
    fng moved{std::move(gen)};
    REQUIRE(moved.get_finance(in("DE"), 3).to_json() == before);
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

TEST_CASE("the same seed gives the same finance, across instances", "[seed]")
{
    const auto& gen = shared_generator();
    fng other;
    other.load(resource_dir("lite"));
    for (std::uint64_t seed = 0; seed < 200; ++seed)
    {
        REQUIRE(gen.get_finance(finance_request{}, seed).to_json() == other.get_finance(finance_request{}, seed).to_json());
    }
}

TEST_CASE("a drawn finance replays from its seed", "[seed]")
{
    fng gen;
    gen.load(resource_dir("lite"));
    const auto drawn = gen.get_finance(finance_request{});
    REQUIRE(gen.get_finance(finance_request{}, drawn.seed()).to_json() == drawn.to_json());
    REQUIRE(drawn.signature().data == gen.data_hash());
}

// ---------------------------------------------------------------------------
// Builders: the authorities' own examples
// ---------------------------------------------------------------------------

TEST_CASE("the German methods reproduce the Bundesbank's examples", "[builders][de]")
{
    struct method_case
    {
        int method;
        std::vector<std::string_view> valid;
        std::vector<std::string_view> invalid;
    };
    const auto pad = [](std::string_view text) { return std::string(10 - text.size(), '0') + std::string{text}; };
    const std::vector<method_case> cases{
        {0, {"9290701", "539290858", "1501824", "1501832"}, {}},
        {6, {"94012341", "5073321010"}, {}},
        {10, {"12345008", "87654008"}, {}},
        {13, {"0123456600", "0001234566"}, {}},
        {24, {"138301", "1306118605", "3307118608", "9307118603"}, {}},
        {28, {"19999000", "9130000201"}, {}},
        {32, {"9141405", "1709107983", "0122116979", "0121114867", "9030101192", "9245500460"}, {}},
        {33, {"48658", "84956"}, {}},
        {34, {"9913000700", "9914001000"}, {}},
        {38, {"191919", "1100660"}, {}},
        {61, {"2063099200", "0260760481"}, {}},
        {63, {"0123456600", "0001234566"}, {"1123456600", "0001234567"}},
        {76, {"0006543200", "9012345600", "7876543100", "0012345600", "0000123456", "0000005009"}, {"1234567890", "0000123457", "0000005000"}},
        {88, {"2525259", "1000500", "90013000", "92525253", "99913003"}, {}},
        {91, {"2974118000", "5281741000", "9952810000", "2974117000", "5281770000", "9952812000", "8840019000", "8840050000",
              "8840087000", "8840045000", "8840012000", "8840055000", "8840080000"},
         {"8840017000", "8840023000", "8840041000", "8840014000", "8840026000", "8840011000", "8840025000", "8840062000",
          "8840010000", "8840057000"}},
        {99, {"0068007003", "0847321750", "0396000000", "0499999999"}, {"0395999999", "0500000000"}},
    };
    for (const auto& c : cases)
    {
        CAPTURE(c.method);
        for (const auto text : c.valid)
        {
            CAPTURE(text);
            REQUIRE(detail::de_valid(c.method, pad(text)));
        }
        for (const auto text : c.invalid)
        {
            CAPTURE(text);
            REQUIRE_FALSE(detail::de_valid(c.method, pad(text)));
        }
    }
    // Method 09 has no check at all.
    REQUIRE(detail::de_valid(9, "1234567890"));
    REQUIRE_FALSE(detail::de_method_known(11));
    REQUIRE(detail::de_method_number("A1") == -1);
    REQUIRE(detail::de_method_number("63") == 63);
}

TEST_CASE("filling a German account makes it valid under its method", "[builders][de]")
{
    core::rng r{7};
    for (const int method : {0, 1, 6, 9, 10, 13, 20, 24, 28, 32, 33, 34, 38, 60, 61, 63, 76, 88, 91, 99})
    {
        CAPTURE(method);
        int filled = 0;
        for (int i = 0; i < 400; ++i)
        {
            std::string account = core::pattern::parse("#{10}").draw(r);
            if (!detail::de_fill(method, account)) continue;
            ++filled;
            CAPTURE(account);
            REQUIRE(detail::de_valid(method, account));
        }
        REQUIRE(filled > 200);
    }
}

TEST_CASE("IBANs with a national check pass, and one changed digit fails", "[builders]")
{
    const auto& gen = shared_generator();
    // The registry's and the authorities' examples, none of them in test_accounts.tsv.
    const std::vector<std::pair<std::string, std::string>> samples{
        {"iban-fr", "FR7630006000011234567890189"},
        {"iban-it", "IT60X0542811101000000123456"},
        {"iban-sm", "SM86U0322509800000000270100"},
        {"iban-es", "ES9121000418450200051332"},
        {"iban-pt", "PT50000201231234567890154"},
        {"iban-be", "BE68539007547034"},
        {"iban-pl", "PL61109010140000071219812874"},
        {"iban-cz", "CZ6508000000192000145399"},
        {"iban-ba", "BA391290079401028494"},
        {"iban-rs", "RS35260005601001611379"},
        {"iban-me", "ME25505000012345678951"},
        {"iban-mk", "MK07250120000058984"},
        {"iban-tl", "TL380080012345678910157"},
        {"iban-xk", "XK051212012345678906"},
        {"iban-mr", "MR1300020001010000123456753"},
        {"iban-tn", "TN5910006035183598478831"},
        {"iban-is", "IS140159260076545510730339"},
        {"iban-ru", "RU0304452522540817810538091310419"},
        {"iban-ci", "CI93CI0080111301134291200589"},
        {"iban-ma", "MA64011519000001205000534921"},
    };
    for (const auto& [scheme, number] : samples)
    {
        CAPTURE(scheme, number);
        REQUIRE(gen.is_valid(scheme, number));
        // The group form too.
        REQUIRE(gen.is_valid(scheme, detail::group_in_fours(number)));
        // A wrong outer check.
        std::string wrong = number;
        wrong[3] = wrong[3] == '9' ? '0' : static_cast<char>(wrong[3] + 1);
        REQUIRE_FALSE(gen.is_valid(scheme, wrong));
    }
}

TEST_CASE("national forms reproduce their authorities' examples", "[builders]")
{
    const auto& gen = shared_generator();
    SECTION("the ABA routing number")
    {
        REQUIRE(detail::aba_valid("021000021"));
        REQUIRE_FALSE(detail::aba_valid("021000022"));
        REQUIRE_FALSE(detail::aba_valid("131000022")); // no such prefix
        REQUIRE(gen.is_valid("us-aba", "021000021 123456789"));
        REQUIRE_FALSE(gen.is_valid("us-aba", "021000022 123456789"));
    }
    SECTION("the CLABE")
    {
        REQUIRE(gen.is_valid("mx-clabe", "002010077777777771"));
        REQUIRE(gen.is_valid("mx-clabe", "002 010 07777777777 1"));
        REQUIRE_FALSE(gen.is_valid("mx-clabe", "002010077777777772"));
    }
    SECTION("the NUBAN")
    {
        REQUIRE(gen.is_valid("ng-nuban", "011 0000014579"));
        REQUIRE(gen.is_valid("ng-nuban", "058 0123456785"));
        REQUIRE_FALSE(gen.is_valid("ng-nuban", "011 0000014570"));
    }
    SECTION("the CBU, per group")
    {
        REQUIRE(gen.is_valid("ar-cbu", "2850590940090418135201"));
        REQUIRE_FALSE(gen.is_valid("ar-cbu", "2850590840090418135201"));
        REQUIRE_FALSE(gen.is_valid("ar-cbu", "2850590940090418135202"));
    }
    SECTION("New Zealand: Inland Revenue's three examples")
    {
        REQUIRE(detail::nz_valid("0109020068389000"));
        REQUIRE(detail::nz_valid("0865231954512001"));
        REQUIRE(detail::nz_valid("2626000320871032"));
        REQUIRE(gen.is_valid("nz-account", "01-0902-0068389-00"));
        REQUIRE(gen.is_valid("nz-account", "08-6523-1954512-001"));
        REQUIRE(gen.is_valid("nz-account", "26-2600-0320871-032"));
        REQUIRE(gen.is_valid("nz-account", "01-0242-0100194-00"));
        REQUIRE_FALSE(gen.is_valid("nz-account", "01-0242-0100195-00"));
        REQUIRE_FALSE(gen.is_valid("nz-account", "99-0242-0100194-00")); // no such bank
    }
    SECTION("New Zealand adds the digits of a product twice, so 18 counts as 9")
    {
        // G: 6 * 3 = 18 counts 9, and a last digit of 1 makes 10.
        REQUIRE(detail::nz_valid("2626000600000001"));
        REQUIRE_FALSE(detail::nz_valid("2626000600000000"));
        // E: 9 * 5 = 45 counts 9, and a last digit of 2 makes 11.
        REQUIRE(detail::nz_valid("0900000009000002"));
    }
    SECTION("Russia's key, from the BIK's last three digits and the account")
    {
        REQUIRE(gen.is_valid("iban-ru", "044525225 40817810538091310419"));
        REQUIRE_FALSE(gen.is_valid("iban-ru", "044525225 40817810638091310419"));
        // The same weights on a correspondent account, whose prefix is 0 and the BIK's
        // fifth and sixth digits: Sberbank's, at BIK 044525225.
        unsigned sum = 0;
        const std::string body = "025" + std::string{"30101810400000000225"};
        for (std::size_t i = 0; i < body.size(); ++i) sum += static_cast<unsigned>(body[i] - '0') * std::array<unsigned, 3>{7, 1, 3}.at(i % 3);
        REQUIRE(sum % 10 == 0);
    }
    SECTION("Brazil's everyday form is structure only")
    {
        REQUIRE(gen.is_valid("iban-br", "001 1234 1234567"));
        REQUIRE_FALSE(gen.is_valid("iban-br", "001 1234 123456"));
    }
    SECTION("a display form with a literal must have it")
    {
        REQUIRE(gen.is_valid("in-ifsc", "HDFC0000123 123456789"));
        REQUIRE_FALSE(gen.is_valid("in-ifsc", "HDFC1000123 123456789"));
    }
}

TEST_CASE("is_valid refuses other text and unknown schemes", "[builders]")
{
    const auto& gen = shared_generator();
    REQUIRE_THROWS_AS(gen.is_valid("iban-zz", "x"), std::invalid_argument);
    REQUIRE_FALSE(gen.is_valid("iban-fr", ""));
    REQUIRE_FALSE(gen.is_valid("iban-fr", "DE89370400440532013000"));
    REQUIRE_FALSE(gen.is_valid("iban-fr", "FR763000600001123456789018"));
    REQUIRE_FALSE(gen.is_valid("us-aba", "banana"));
    REQUIRE_FALSE(gen.is_valid("generic-af", "12345"));
    REQUIRE(gen.is_valid("generic-af", "1234567890"));
}

TEST_CASE("a bank outside a complete list fails, and published test numbers pass", "[builders]")
{
    const auto& full = shared_full_generator();
    // Commerzbank's code, in the German list.
    REQUIRE(full.is_valid("iban-de", "DE89370400440532013000"));
    // The test number passes whatever its digits say.
    REQUIRE(shared_generator().is_valid("au-bsb", "000000 000123456"));
    // No German bank has the code 00000000: the outer check holds, the list does not.
    const auto digits = core::iban_check_digits("DE", "000000000532013000");
    const std::string unknown = std::string{"DE"} + digits[0] + digits[1] + "000000000532013000";
    REQUIRE(core::iban_valid(unknown));
    REQUIRE_FALSE(full.is_valid("iban-de", unknown));
}

// ---------------------------------------------------------------------------
// Coherence
// ---------------------------------------------------------------------------

static void coherence(const fng& gen, const char* tier, int people)
{
    const auto& v = view(tier);
    for (const auto reference : {date(2005, 6, 1), date(2015, 6, 1), date(2023, 7, 1)})
    {
        for (const auto& country : gen.countries())
        {
            finance_request q = in(country);
            q.reference_date = reference;
            q.accounts = {account_kind::current, account_kind::savings};
            for (int i = 0; i < people; ++i)
            {
                const auto f = gen.get_finance(q, static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ULL);
                REQUIRE(f.country_code == country);
                REQUIRE(f.accounts.size() == 2);
                for (const auto& a : f.accounts) require_coherent(gen, v, f, a, reference);
            }
        }
    }
}

TEST_CASE("every account holds together on the lite tier", "[coherence]")
{
    coherence(shared_generator(), "lite", 300);
}

TEST_CASE("every account holds together on the full tier", "[coherence]")
{
    coherence(shared_full_generator(), "full", 60);
}

TEST_CASE("every account holds together, at length", "[.exhaustive]")
{
    const auto& gen = shared_full_generator();
    const auto& v = view("full");
    for (std::uint64_t seed = 0; seed < 100000; ++seed)
    {
        const auto f = gen.get_finance(finance_request{}, seed);
        for (const auto& a : f.accounts) require_coherent(gen, v, f, a, finance_request::default_reference_date);
    }
}

TEST_CASE("the kinds of number decode to their fields", "[coherence]")
{
    const auto& gen = shared_generator();
    SECTION("Brazil's IBAN carries the bank's ISPB, the branch, the account and its type")
    {
        finance_request q = in("BR");
        q.accounts = {account_kind::current, account_kind::savings};
        for (std::uint64_t seed = 0; seed < 200; ++seed)
        {
            const auto f = gen.get_finance(q, seed);
            for (const auto& a : f.accounts)
            {
                CAPTURE(a.iban, a.formatted);
                const std::string_view bban = std::string_view{a.iban}.substr(4);
                REQUIRE(a.bank_code.size() == 3);
                REQUIRE(bban[8] == '0');
                REQUIRE(bban.substr(9, 4) == a.branch_code);
                REQUIRE(std::string(10 - a.account_number.size(), '0') + a.account_number == bban.substr(13, 10));
                REQUIRE(bban.substr(23) == (a.kind == account_kind::savings ? "P1" : "C1"));
                REQUIRE(a.formatted == a.bank_code + " " + a.branch_code + " " + a.account_number);
            }
        }
    }
    SECTION("Russia's holds the BIK and the 20-digit account of a person, in roubles")
    {
        for (std::uint64_t seed = 0; seed < 200; ++seed)
        {
            const auto a = gen.get_finance(in("RU"), seed).accounts.front();
            REQUIRE(a.account_number.size() == 20);
            REQUIRE(a.account_number.starts_with("40817810"));
            REQUIRE(a.iban.substr(4) == a.bank_code + a.account_number);
            REQUIRE(a.formatted == a.bank_code + " " + a.account_number);
        }
    }
    SECTION("Iceland's holds the holder's kennitala")
    {
        finance_request q = in("IS");
        q.birth_date = date(1975, 3, 9);
        for (std::uint64_t seed = 0; seed < 100; ++seed)
        {
            const auto a = gen.get_finance(q, seed).accounts.front();
            REQUIRE(a.iban.substr(16, 6) == "090375");
            REQUIRE(a.iban[25] == '9');
        }
        q.birth_date = date(2003, 12, 1);
        q.reference_date = date(2023, 7, 1);
        REQUIRE(gen.get_finance(q, 1).accounts.front().iban.substr(16, 6) == "011203");
        REQUIRE(gen.get_finance(q, 1).accounts.front().iban[25] == '0');
    }
    SECTION("IBANs take digits where the registry allows either, and letters still read")
    {
        for (std::uint64_t seed = 0; seed < 200; ++seed)
        {
            const auto a = gen.get_finance(in("FR"), seed).accounts.front();
            REQUIRE(detail::all_digits(a.iban.substr(2)));
            REQUIRE(a.iban.starts_with("FR76"));
        }
        REQUIRE(gen.is_valid("iban-fr", "FR1420041010050500013M02606"));
    }
    SECTION("Germany draws only banks whose method is implemented")
    {
        for (std::uint64_t seed = 0; seed < 300; ++seed)
        {
            const auto a = shared_full_generator().get_finance(in("DE"), seed).accounts.front();
            REQUIRE(a.bank_name.size() > 0);
        }
    }
    SECTION("a formatted IBAN is the electronic one in fours")
    {
        const auto a = gen.get_finance(in("DE"), 1).accounts.front();
        REQUIRE(a.formatted == detail::group_in_fours(a.iban));
        REQUIRE(detail::strip_separators(a.formatted) == a.iban);
    }
}

// ---------------------------------------------------------------------------
// Safe
// ---------------------------------------------------------------------------

static void safe_as_declared(const fng& gen, const char* tier, int people)
{
    const auto& v = view(tier);
    std::map<std::string, int> seen;
    for (const auto& country : gen.countries())
    {
        finance_request q = in(country);
        q.safe = true;
        const auto& scheme = v.primary.at(country);
        const auto& method = v.safe.at(scheme);
        for (int i = 0; i < people; ++i)
        {
            const auto f = gen.get_finance(q, static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ULL);
            const auto& a = f.accounts.front();
            CAPTURE(scheme, method, a.iban, a.formatted, a.bic);
            ++seen[method];
            REQUIRE(a.reserved);
            REQUIRE(a.bank_name.empty());
            if (!a.bic.empty()) REQUIRE(a.bic[7] == '0');
            const std::string& text = a.iban.empty() ? a.formatted : a.iban;
            const std::string plain = detail::strip_separators(text);
            if (method == "test_value")
            {
                REQUIRE(v.tests.contains(plain));
                REQUIRE(gen.is_valid(scheme, text));
            }
            else if (method == "unassigned_code")
            {
                REQUIRE(core::iban_valid(a.iban));
                REQUIRE_FALSE(v.banks.at(v.book_of.at(scheme)).contains(a.bank_code));
                REQUIRE(a.bic.empty());
                REQUIRE_FALSE(gen.is_valid(scheme, text));
            }
            else if (method == "wrong_national")
            {
                REQUIRE(core::iban_valid(a.iban));
                REQUIRE_FALSE(gen.is_valid(scheme, text));
            }
            else if (method == "wrong_outer")
            {
                if (!a.iban.empty())
                {
                    REQUIRE_FALSE(core::iban_valid(a.iban));
                    const int digits = std::stoi(a.iban.substr(2, 2));
                    REQUIRE(digits >= 2);
                    REQUIRE(digits <= 98);
                }
                REQUIRE_FALSE(gen.is_valid(scheme, text));
                REQUIRE_FALSE(gen.is_valid(scheme, a.formatted));
            }
            else
            {
                REQUIRE(method == "none");
                REQUIRE(gen.is_valid(scheme, text));
            }
        }
    }
    for (const auto* method : {"test_value", "unassigned_code", "wrong_national", "wrong_outer", "none"})
    {
        CAPTURE(method);
        REQUIRE(seen[method] > 0);
    }
}

TEST_CASE("safe accounts are as each scheme's method declares, on the lite tier", "[safe]")
{
    safe_as_declared(shared_generator(), "lite", 100);
}

TEST_CASE("safe accounts are as each scheme's method declares, on the full tier", "[safe]")
{
    safe_as_declared(shared_full_generator(), "full", 40);
}

TEST_CASE("a realistic number is not marked reserved, and safe is per call", "[safe]")
{
    const auto& gen = shared_generator();
    finance_request q = in("IT");
    q.safe = true;
    const auto safe = gen.get_finance(q, 9).accounts.front();
    const auto real = gen.get_finance(in("IT"), 9).accounts.front();
    REQUIRE(safe.reserved);
    REQUIRE_FALSE(real.reserved);
    REQUIRE(safe.bank_name.empty());
    REQUIRE_FALSE(real.bank_name.empty());
    REQUIRE(safe.iban != real.iban);
}

TEST_CASE("an unassigned code stays unassigned after a check digit inside it is filled", "[safe]")
{
    // Poland's check is the last digit of its bank code.
    const auto& gen = shared_full_generator();
    const auto& book = view("full").banks.at("iban-pl");
    finance_request q = in("PL");
    q.safe = true;
    for (std::uint64_t seed = 0; seed < 20000; ++seed)
    {
        const auto a = gen.get_finance(q, seed).accounts.front();
        CAPTURE(a.iban);
        REQUIRE_FALSE(book.contains(a.bank_code));
        REQUIRE_FALSE(gen.is_valid("iban-pl", a.iban));
    }
}

TEST_CASE("Norway's 00 accounts are read either way and never drawn", "[builders]")
{
    const auto& gen = shared_generator();
    // By the short reading: 5, 4, 3, 2 over 8665 gives 7.
    REQUIRE(gen.is_valid("iban-no", "NO3978810086657"));
    // And by the full reading of the same digits, with its own check digit.
    const auto full = detail::no_digit("7881008665");
    REQUIRE(full.has_value());
    const auto digits = core::iban_check_digits("NO", "7881008665" + *full);
    REQUIRE(gen.is_valid("iban-no", std::string{"NO"} + digits[0] + digits[1] + "7881008665" + *full));
    for (std::uint64_t seed = 0; seed < 5000; ++seed)
    {
        const auto a = gen.get_finance(in("NO"), seed).accounts.front();
        REQUIRE(a.iban.substr(8, 2) != "00");
    }
}

TEST_CASE("Germany's method 09 has no check to spoil, and falls to the outer one", "[safe]")
{
    fng gen;
    gen.load(resource_dir("full"));
    gen.extend_from_string(finance_table::schemes,
                           "key\tcountry\tbuilder\tdisplay\tsafe_method\tevidence\tcheck\tlist_complete\n"
                           "iban-de\tDE\tiban\t{iban}\twrong_national\tissuer\tde_method\t1\n",
                           "mine");
    gen.extend_from_string(finance_table::banks, "scheme\tbank_code\tname\tweight\tmethod\niban-de\t10000000\tTest\t1000000000\t09\n", "mine");
    finance_request q = in("DE");
    q.safe = true;
    for (std::uint64_t seed = 0; seed < 100; ++seed)
    {
        const auto a = gen.get_finance(q, seed).accounts.front();
        if (a.bank_code == "10000000") REQUIRE_FALSE(core::iban_valid(a.iban));
        REQUIRE_FALSE(gen.is_valid("iban-de", a.iban));
    }
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

TEST_CASE("a given input is honoured and moves nothing else", "[requests]")
{
    const auto& gen = shared_generator();
    for (std::uint64_t seed = 0; seed < 100; ++seed)
    {
        const auto base = gen.get_finance(finance_request{}, seed);

        finance_request same_country = in(base.country_code);
        const auto with_country = gen.get_finance(same_country, seed);
        REQUIRE(with_country.to_json() == base.to_json());

        finance_request same_birth;
        same_birth.birth_date = base.birth_date;
        REQUIRE(gen.get_finance(same_birth, seed).to_json() == base.to_json());

        // Another country leaves the birth date where it was.
        const auto other = gen.get_finance(in(base.country_code == "DE" ? "FR" : "DE"), seed);
        REQUIRE(other.birth_date == base.birth_date);
    }
}

TEST_CASE("another account leaves the first ones unchanged", "[requests]")
{
    const auto& gen = shared_generator();
    finance_request one = in("DE");
    finance_request three = one;
    three.accounts = {account_kind::current, account_kind::savings, account_kind::current};
    for (std::uint64_t seed = 0; seed < 100; ++seed)
    {
        const auto a = gen.get_finance(one, seed);
        const auto b = gen.get_finance(three, seed);
        REQUIRE(b.accounts.size() == 3);
        REQUIRE(a.accounts.front().iban == b.accounts.front().iban);
        REQUIRE(a.accounts.front().opened == b.accounts.front().opened);
        REQUIRE(b.accounts[0].iban != b.accounts[1].iban);
        REQUIRE(b.accounts[1].kind == account_kind::savings);
    }
}

TEST_CASE("the country is any case, and a territory only by name", "[requests]")
{
    const auto& gen = shared_generator();
    REQUIRE(gen.get_finance(in("de"), 1).country_code == "DE");
    REQUIRE(gen.get_finance(in("YT"), 1).accounts.front().iban.starts_with("YT"));
    REQUIRE(gen.schemes("de") == std::vector<std::string>{"iban-de"});
    REQUIRE_THROWS_AS(gen.schemes("ZZ"), std::invalid_argument);
    const auto& v = view("lite");
    for (std::uint64_t seed = 0; seed < 5000; ++seed)
    {
        REQUIRE(v.population.contains(gen.get_finance(finance_request{}, seed).country_code));
    }
}

TEST_CASE("bad requests are refused", "[requests]")
{
    const auto& gen = shared_generator();
    REQUIRE_THROWS_AS(gen.get_finance(in("ZZ"), 1), std::invalid_argument);
    finance_request q;
    q.birth_date = date(2024, 1, 1);
    REQUIRE_THROWS_AS(gen.get_finance(q, 1), std::invalid_argument);
    q.birth_date = date(1899, 12, 31);
    REQUIRE_THROWS_AS(gen.get_finance(q, 1), std::invalid_argument);
    q.birth_date = date(2023, 2, 30);
    REQUIRE_THROWS_AS(gen.get_finance(q, 1), std::invalid_argument);
    finance_request r;
    r.reference_date = date(2023, 2, 30);
    REQUIRE_THROWS_AS(gen.get_finance(r, 1), std::invalid_argument);
    r.reference_date = date(1899, 12, 31);
    REQUIRE_THROWS_AS(gen.get_finance(r, 1), std::invalid_argument);
    r.reference_date = date(2091, 1, 1);
    REQUIRE_THROWS_AS(gen.get_finance(r, 1), std::invalid_argument);
}

TEST_CASE("ages and minimum ages", "[requests]")
{
    const auto& gen = shared_generator();
    SECTION("no account asked, none given")
    {
        finance_request q = in("DE");
        q.accounts.clear();
        const auto f = gen.get_finance(q, 1);
        REQUIRE(f.accounts.empty());
        REQUIRE(f.country_code == "DE");
        REQUIRE(f.to_string() == "DE");
    }
    SECTION("a child has no account until the minimum age, then from that day")
    {
        finance_request q = in("DE");
        q.birth_date = date(2010, 3, 15);
        q.reference_date = date(2026, 3, 14);
        REQUIRE(gen.get_finance(q, 1).accounts.empty());
        q.reference_date = date(2026, 3, 15);
        const auto f = gen.get_finance(q, 1);
        REQUIRE(f.accounts.size() == 1);
        REQUIRE(f.accounts.front().opened == date(2026, 3, 15));
    }
    SECTION("a layer changes a scheme's minimum age")
    {
        fng own;
        own.load(resource_dir("lite"));
        own.extend_from_string(finance_table::schemes,
                               "key\tcountry\tbuilder\tdisplay\tsafe_method\tevidence\tcheck\tmin_age\n"
                               "iban-de\tDE\tiban\t{iban}\ttest_value\tissuer\tde_method\t21\n",
                               "mine");
        finance_request q = in("DE");
        q.birth_date = date(2000, 1, 1);
        q.reference_date = date(2020, 12, 31);
        REQUIRE(own.get_finance(q, 1).accounts.empty());
        q.reference_date = date(2021, 1, 1);
        REQUIRE(own.get_finance(q, 1).accounts.size() == 1);
    }
    SECTION("drawn ages run from 18 to 80, and the first of the range is reached")
    {
        int youngest = 200;
        int oldest = 0;
        const auto ref = finance_request::default_reference_date;
        for (std::uint64_t seed = 0; seed < 20000; ++seed)
        {
            const auto born = gen.get_finance(finance_request{}, seed).birth_date;
            int age = static_cast<int>(ref.year()) - static_cast<int>(born.year());
            if (std::chrono::month_day{born.month(), born.day()} > std::chrono::month_day{ref.month(), ref.day()}) --age;
            youngest = std::min(youngest, age);
            oldest = std::max(oldest, age);
        }
        REQUIRE(youngest == 18);
        REQUIRE(oldest == 80);
    }
    SECTION("the reference date moves what is drawn back, as far as 1900")
    {
        finance_request q = in("DE");
        q.reference_date = date(1900, 1, 1);
        const auto f = gen.get_finance(q, 1);
        REQUIRE(f.birth_date == date(1900, 1, 1));
        REQUIRE(f.accounts.empty());
    }
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

TEST_CASE("countries follow population, and are flat when asked", "[stats]")
{
    const auto& gen = shared_generator();
    const auto& v = view("lite");
    double total = 0.0;
    for (const auto& [code, people] : v.population) total += people;
    constexpr int draws = 20000;
    std::map<std::string, int> weighted;
    std::map<std::string, int> flat;
    finance_request uniform;
    uniform.weighted = false;
    for (std::uint64_t seed = 0; seed < draws; ++seed)
    {
        ++weighted[gen.get_finance(finance_request{}, seed).country_code];
        ++flat[gen.get_finance(uniform, seed).country_code];
    }
    for (const auto* code : {"IN", "CN", "US", "BR"})
    {
        CAPTURE(code);
        REQUIRE(weighted[code] == Catch::Approx(draws * v.population.at(code) / total).margin(draws * 0.015));
    }
    REQUIRE(flat["IN"] == Catch::Approx(static_cast<double>(draws) / static_cast<double>(v.population.size())).margin(draws * 0.005));
    REQUIRE(flat["TV"] > 0);
}

TEST_CASE("banks follow their weights, and are flat when asked", "[stats]")
{
    const auto& gen = shared_full_generator();
    const auto& banks = view("full").banks.at("nz-account");
    double total = 0.0;
    for (const auto& [code, weight] : banks) total += weight;
    constexpr int draws = 20000;
    std::map<std::string, int> counted;
    std::map<std::string, int> flat;
    finance_request uniform = in("NZ");
    uniform.weighted = false;
    for (std::uint64_t seed = 0; seed < draws; ++seed)
    {
        ++counted[gen.get_finance(in("NZ"), seed).accounts.front().bank_code];
        ++flat[gen.get_finance(uniform, seed).accounts.front().bank_code];
    }
    for (const auto* code : {"03", "01", "38"})
    {
        CAPTURE(code);
        REQUIRE(counted[code] == Catch::Approx(draws * banks.at(code) / total).margin(draws * 0.015));
        REQUIRE(flat[code] == Catch::Approx(static_cast<double>(draws) / static_cast<double>(banks.size())).margin(draws * 0.015));
    }
}

TEST_CASE("a bank of weight 0 is known and never drawn", "[stats]")
{
    const auto& gen = shared_full_generator();
    const auto& banks = view("full").banks.at("iban-gb");
    std::set<std::string> kept;
    for (const auto& [code, weight] : banks)
    {
        if (weight == 0.0) kept.insert(code);
    }
    REQUIRE_FALSE(kept.empty());
    for (std::uint64_t seed = 0; seed < 5000; ++seed)
    {
        REQUIRE_FALSE(kept.contains(gen.get_finance(in("GB"), seed).accounts.front().bank_code));
    }
}

// ---------------------------------------------------------------------------
// Batch
// ---------------------------------------------------------------------------

TEST_CASE("batch items equal single calls, and threads, ranges and streaming agree", "[batch]")
{
    const auto& gen = shared_generator();
    constexpr std::uint64_t batch_seed = 77;
    const auto items = gen.get_finance_batch(finance_request{}, {.count = 500}, batch_seed);
    REQUIRE(items.size() == 500);
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        REQUIRE(items[i].to_json() == gen.get_finance(finance_request{}, core::item_seed(batch_seed, i)).to_json());
    }
    const auto threaded = gen.get_finance_batch(finance_request{}, {.count = 500, .threads = 4}, batch_seed);
    const auto range = gen.get_finance_batch(finance_request{}, {.count = 100, .first = 400}, batch_seed);
    for (std::size_t i = 0; i < 500; ++i) REQUIRE(threaded[i].to_json() == items[i].to_json());
    for (std::size_t i = 0; i < 100; ++i) REQUIRE(range[i].to_json() == items[400 + i].to_json());

    std::vector<std::string> streamed;
    gen.for_each_finance(finance_request{}, {.count = 500, .threads = 2}, batch_seed,
                         [&](std::uint64_t index, finance& item)
                         {
                             REQUIRE(index == streamed.size());
                             streamed.push_back(item.to_json());
                         });
    REQUIRE(streamed.size() == 500);
    for (std::size_t i = 0; i < 500; ++i) REQUIRE(streamed[i] == items[i].to_json());
}

TEST_CASE("a unique batch has no repeats, and refuses more than the list holds", "[batch]")
{
    const auto& gen = shared_generator();
    const auto by_iban = core::unique_rule::by<finance>([](const finance& v) { return v.accounts.front().iban; });
    const auto items = gen.get_finance_batch(in("DE"), {.count = 500, .unique = by_iban}, 3);
    std::set<std::string> seen;
    for (const auto& v : items) seen.insert(v.accounts.front().iban);
    REQUIRE(seen.size() == 500);

    const auto by_country = core::unique_rule::by<finance>([](const finance& v) { return v.country_code; });
    REQUIRE_THROWS_AS(gen.get_finance_batch(in("DE"), {.count = 2, .unique = by_country}, 3), std::runtime_error);
}

TEST_CASE("a batch with a drawn seed works, and the instance's engine moves", "[batch]")
{
    fng gen;
    gen.load(resource_dir("lite"));
    const auto a = gen.get_finance_batch(finance_request{}, {.count = 3});
    const auto b = gen.get_finance_batch(finance_request{}, {.count = 3});
    REQUIRE(a.size() == 3);
    REQUIRE(a.front().seed() != b.front().seed());
    int seen = 0;
    gen.for_each_finance(finance_request{}, {.count = 3}, [&](std::uint64_t, finance&) { ++seen; });
    REQUIRE(seen == 3);
}

// ---------------------------------------------------------------------------
// Conversion and JSON
// ---------------------------------------------------------------------------

TEST_CASE("string conversion and streaming give the summary", "[conversion]")
{
    const auto& gen = shared_generator();
    const auto f = gen.get_finance(in("DE"), 5);
    const auto& a = f.accounts.front();
    const std::string s = f;
    std::ostringstream os;
    os << f;
    REQUIRE(s == "DE, EUR, " + a.formatted + " (" + a.bank_name + ")");
    REQUIRE(os.str() == s);
}

TEST_CASE("to_json and signature", "[json]")
{
    const auto& gen = shared_generator();
    const auto f = gen.get_finance(in("DE"), 99);
    REQUIRE(f.signature().seed == 99);
    REQUIRE(f.seed() == 99);
    REQUIRE(f.signature().data == gen.data_hash());
    const auto json = f.to_json();
    REQUIRE(json.starts_with(R"({"country_code":"DE","birth_date":")"));
    REQUIRE(json.find(R"("scheme":"iban-de")") != std::string::npos);
    REQUIRE(json.find(R"("kind":"current")") != std::string::npos);
    REQUIRE(json.find(R"("currency":"EUR","currency_digits":2)") != std::string::npos);
    REQUIRE(json.find(R"("reserved":false)") != std::string::npos);
    REQUIRE(json.find(R"("signature":{"seed":"0000000000000063")") != std::string::npos);
    core::json_writer w;
    w.begin_array();
    f.to_json(w);
    w.end_array();
    REQUIRE(w.str() == "[" + json + "]");
}

// ---------------------------------------------------------------------------
// Known answers: the same seed and data give these exact results on every
// compiler and platform. A change here is a change of output and needs a major
// version.
// ---------------------------------------------------------------------------

static const std::uint64_t kat_data_hash = 0x0ebc91db592625c7ULL;

TEST_CASE("known answers for fixed seeds", "[kat]")
{
    fng gen;
    gen.load(resource_dir("lite"));
    REQUIRE(gen.data_hash() == kat_data_hash);

    REQUIRE(gen.get_finance(finance_request{}, std::uint64_t{1}).to_json() ==
            R"x({"country_code":"ES","birth_date":"1958-05-06","accounts":[{"scheme":"iban-es","kind":"current","iban":"ES9001551612513854922663","formatted":"ES90 0155 1612 5138 5492 2663","bank_code":"0155","branch_code":"1612","account_number":"513854922663","bank_name":"BANCO DO BRASIL","bic":"BRASESMM","currency":"EUR","currency_digits":2,"opened":"1983-12-28","evidence":"issuer","reserved":false}],"signature":{"seed":"0000000000000001","data":"0ebc91db592625c7"}})x");
    finance_request two = in("DE");
    two.accounts = {account_kind::current, account_kind::savings};
    REQUIRE(gen.get_finance(two, std::uint64_t{3}).to_json() ==
            R"x({"country_code":"DE","birth_date":"1961-10-16","accounts":[{"scheme":"iban-de","kind":"current","iban":"DE77870700240795672554","formatted":"DE77 8707 0024 0795 6725 54","bank_code":"87070024","branch_code":"","account_number":"0795672554","bank_name":"Deutsche Bank","bic":"DEUTDEDBCHE","currency":"EUR","currency_digits":2,"opened":"1991-01-31","evidence":"issuer","reserved":false},{"scheme":"iban-de","kind":"savings","iban":"DE85870700000695345904","formatted":"DE85 8707 0000 0695 3459 04","bank_code":"87070000","branch_code":"","account_number":"0695345904","bank_name":"Deutsche Bank","bic":"DEUTDE8CXXX","currency":"EUR","currency_digits":2,"opened":"1985-02-27","evidence":"issuer","reserved":false}],"signature":{"seed":"0000000000000003","data":"0ebc91db592625c7"}})x");
    finance_request safe = in("IT");
    safe.safe = true;
    REQUIRE(gen.get_finance(safe, std::uint64_t{5}).to_json() ==
            R"x({"country_code":"IT","birth_date":"1966-05-29","accounts":[{"scheme":"iban-it","kind":"current","iban":"IT74D0312675571305157708832","formatted":"IT74 D031 2675 5713 0515 7708 832","bank_code":"03126","branch_code":"75571","account_number":"305157708832","bank_name":"","bic":"","currency":"EUR","currency_digits":2,"opened":"1983-04-14","evidence":"issuer","reserved":true}],"signature":{"seed":"0000000000000005","data":"0ebc91db592625c7"}})x");

    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::uint64_t s = 0; s < 10000; ++s)
    {
        for (const char c : gen.get_finance(finance_request{}, s).to_json())
        {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ULL;
        }
    }
    REQUIRE(h == 0x542f089d4d659eefULL);
}

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

static const char* const bank_header = "scheme\tbank_code\tname\tweight\tbic\n";

TEST_CASE("a layer adds a bank, which is then drawn", "[layers]")
{
    fng gen;
    gen.load(resource_dir("lite"));
    const auto hash = gen.data_hash();
    const auto& info = gen.extend_from_string(finance_table::banks, std::string{bank_header} + "iban-ad\t9999\tMy Bank\t1000000\tMYBKADADXXX\n", "mine");
    REQUIRE(info.added == 1);
    REQUIRE(gen.data_hash() != hash);
    REQUIRE(gen.layers(finance_table::banks).size() == 2);
    int mine = 0;
    for (std::uint64_t seed = 0; seed < 200; ++seed)
    {
        const auto a = gen.get_finance(in("AD"), seed).accounts.front();
        REQUIRE(core::iban_valid(a.iban));
        if (a.bank_code == "9999")
        {
            ++mine;
            REQUIRE(a.bank_name == "My Bank");
            REQUIRE(a.bic == "MYBKADADXXX");
        }
    }
    REQUIRE(mine > 150);

    // Reloading the base drops every layer.
    gen.load(resource_dir("lite"));
    REQUIRE(gen.data_hash() == hash);
}

TEST_CASE("a directory layer adds a country with a national scheme and a test number", "[layers]")
{
    fng gen;
    gen.load(resource_dir("lite"));
    const auto hash = gen.data_hash();
    const auto tmp = std::filesystem::temp_directory_path() / "fng_layer_dir";
    std::filesystem::create_directories(tmp);
    {
        std::ofstream{tmp / "countries.tsv"} << "country_code\tpopulation\tcurrency\tcurrency_digits\tprimary_scheme\nXA\t1000\tXAA\t0\txa-num\n";
        std::ofstream{tmp / "schemes.tsv"} << "key\tcountry\tbuilder\tdisplay\tsafe_method\tevidence\tbank_pattern\tpattern\n"
                                            << "xa-num\tXA\tnational\t{bank}-{account}\ttest_value\treference\t#{3}\t#{6,8}\n";
        std::ofstream{tmp / "test_accounts.tsv"} << "source\tscheme\tnumber\tbank_code\taccount_number\nmine\txa-num\t000-123456\t000\t123456\n";
    }
    gen.extend(tmp);
    std::filesystem::remove_all(tmp);

    REQUIRE(gen.data_hash() != hash);
    REQUIRE(gen.schemes("XA") == std::vector<std::string>{"xa-num"});
    const auto f = gen.get_finance(in("XA"), 4);
    const auto& a = f.accounts.front();
    REQUIRE(a.currency == "XAA");
    REQUIRE(a.currency_digits == 0);
    REQUIRE(a.iban.empty());
    REQUIRE(gen.is_valid("xa-num", a.formatted));
    REQUIRE(a.formatted == a.bank_code + "-" + a.account_number);

    finance_request q = in("XA");
    q.safe = true;
    const auto safe = gen.get_finance(q, 4).accounts.front();
    REQUIRE(safe.formatted == "000-123456");
    REQUIRE(safe.bank_code == "000");
    REQUIRE(safe.account_number == "123456");
    REQUIRE(safe.reserved);
}

TEST_CASE("a file layer is named after its table", "[layers]")
{
    fng gen;
    gen.load(resource_dir("lite"));
    const auto tmp = std::filesystem::temp_directory_path() / "fng_layer_file";
    std::filesystem::create_directories(tmp);
    {
        std::ofstream{tmp / "banks.tsv"} << bank_header << "iban-ad\t9999\tMy Bank\t1\t\n";
        std::ofstream{tmp / "other.tsv"} << "x\ty\n";
    }
    gen.extend(tmp / "banks.tsv");
    REQUIRE(gen.layers(finance_table::banks).size() == 2);
    REQUIRE_THROWS_AS(gen.extend(tmp / "other.tsv"), std::invalid_argument);
    std::filesystem::remove_all(tmp);
}

TEST_CASE("a disabled row removes its bank", "[layers]")
{
    fng gen;
    gen.load(resource_dir("lite"));
    gen.extend_from_string(finance_table::banks, std::string{bank_header} + "iban-ad\t9999\tMy Bank\t1000000\t\n", "add");
    const auto& info = gen.extend_from_string(finance_table::banks, "scheme\tbank_code\tname\tweight\tdisabled\niban-ad\t9999\tx\t1\t1\n", "remove");
    REQUIRE(info.removed == 1);
    for (std::uint64_t seed = 0; seed < 200; ++seed) REQUIRE(gen.get_finance(in("AD"), seed).accounts.front().bank_code != "9999");
}

TEST_CASE("a bad layer changes nothing", "[layers]")
{
    fng gen;
    gen.load(resource_dir("lite"));
    const auto hash = gen.data_hash();
    const auto rejected_tsv = [&](finance_table table, const std::string& text)
    {
        CAPTURE(text);
        REQUIRE_THROWS_AS(gen.extend_from_string(table, text, "bad"), core::tsv_error);
        REQUIRE(gen.data_hash() == hash);
    };
    const auto rejected = [&](finance_table table, const std::string& text)
    {
        CAPTURE(text);
        REQUIRE_THROWS_AS(gen.extend_from_string(table, text, "bad"), std::invalid_argument);
        REQUIRE(gen.data_hash() == hash);
    };
    const std::string schemes = "key\tcountry\tbuilder\tdisplay\tsafe_method\tevidence\tcheck\tpattern\n";
    SECTION("rows that cannot be read")
    {
        rejected_tsv(finance_table::schemes, schemes + "x\tDE\tmagic\t{account}\tnone\treference\t\t#{3}\n");
        rejected_tsv(finance_table::schemes, schemes + "x\tDE\tnational\t{nonsense}\tnone\treference\t\t#{3}\n");
        rejected_tsv(finance_table::schemes, schemes + "x\tDE\tnational\t{account}\tmaybe\treference\t\t#{3}\n");
        rejected_tsv(finance_table::schemes, schemes + "x\tDE\tnational\t{account}\tnone\treference\tmod1000\t#{3}\n");
        rejected_tsv(finance_table::schemes, schemes + "x\tDE\tnational\t{account}\tnone\treference\t\t#{\n");
        rejected_tsv(finance_table::banks, std::string{bank_header} + "iban-ad\t9999\t\t1\t\n");
        rejected_tsv(finance_table::banks, std::string{bank_header} + "iban-ad\t9999\tName\t-1\t\n");
        rejected_tsv(finance_table::banks, std::string{bank_header} + "iban-ad\t9999\tName\t1\tSHORT\n");
        rejected_tsv(finance_table::banks, "scheme\tbank_code\tname\tweight\tmethod\niban-de\t10000000\tName\t1\tA1\n");
        rejected_tsv(finance_table::countries, "country_code\tpopulation\tcurrency\tcurrency_digits\tprimary_scheme\nXA\t1\tEURO\t2\tiban-de\n");
        rejected_tsv(finance_table::iban_formats, "country_code\tbban_pattern\tlength\nXA\t#{10}\t20\n");
        rejected_tsv(finance_table::iban_formats, "country_code\tbban_pattern\tlength\tbank_from\tbank_to\nXA\t#{10}\t14\t8\t12\n");
        rejected_tsv(finance_table::iban_formats, "country_code\tbban_pattern\tlength\tcheck_builder\nXA\t#{10}\t14\tes\n");
        rejected_tsv(finance_table::iban_formats, "country_code\tbban_pattern\tlength\tcheck_builder\nXA\t#{22}\t26\tmod97_bban\n");
        rejected_tsv(finance_table::test_cards, "source\tnumber\tnetwork\tkind\nmine\t4242424242424241\tvisa\tany\n");
        rejected_tsv(finance_table::networks, "network\trange_from\trange_to\tlength\tlengths\tweight\nx\t5\t4\t16\t16\t1\n");
        rejected_tsv(finance_table::card_networks, "country_code\tkind\tnetwork\tband\tevidence\nDE\tdebit\tvisa\tusual\tjudgement\n");
        rejected_tsv(finance_table::ownership, "country_code\tgroup\taccount\tdebit\tcredit\nDE\tall\t1.5\t0\t0\n");
    }
    SECTION("tables that no longer fit together")
    {
        rejected(finance_table::schemes, schemes + "x\tZZ\tnational\t{account}\tnone\treference\t\t#{3}\n");
        rejected(finance_table::schemes, schemes + "x\tDE\tnational\t{account}\twrong_outer\treference\t\t#{3}\n");
        rejected(finance_table::schemes, schemes + "x\tDE\tnational\t{account}\ttest_value\treference\t\t#{3}\n");
        rejected(finance_table::schemes, schemes + "x\tDE\tnational\t{account}\tunassigned_code\treference\t\t#{3}\n");
        rejected(finance_table::schemes, schemes + "x\tXA\tiban\t{iban}\tnone\treference\t\t\n");
        rejected(finance_table::banks, std::string{bank_header} + "no-such\t9999\tName\t1\t\n");
        rejected(finance_table::banks, std::string{bank_header} + "iban-ad\t99\tName\t1\t\n");
        rejected(finance_table::banks, std::string{bank_header} + "iban-ao\t99\tName\t1\t\n");
        rejected(finance_table::banks, std::string{bank_header} + "au-bsb\t12\tName\t1\t\n");
        rejected(finance_table::countries, "country_code\tpopulation\tcurrency\tcurrency_digits\tprimary_scheme\nXA\t1\tEUR\t2\tno-such\n");
        rejected(finance_table::test_accounts, "source\tscheme\tnumber\nmine\tiban-de\tDE00370400440532013000\n");
        rejected(finance_table::test_accounts, "source\tscheme\tnumber\nmine\tno-such\t1\n");
    }
}

TEST_CASE("a replace layer needs the rest of the tables to still fit", "[layers]")
{
    fng gen;
    gen.load(resource_dir("lite"));
    REQUIRE_THROWS_AS(gen.extend_from_string(finance_table::schemes,
                                             "key\tcountry\tbuilder\tdisplay\tsafe_method\tevidence\tpattern\n"
                                             "only\tDE\tnational\t{account}\tnone\treference\t#{3}\n",
                                             "mine", core::layer_mode::replace),
                      std::invalid_argument);
}
