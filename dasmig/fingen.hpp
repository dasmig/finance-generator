// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Dasso Migotto

#ifndef DASMIG_FINGEN_HPP
#define DASMIG_FINGEN_HPP

#include "core.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef DASMIG_FINANCE_EMBED
#include "fingen_data.hpp"
#endif

/// @file fingen.hpp
/// @brief Finance Generator library: a person's bank accounts, consistent with
///        the country and the date, for C++23.
/// @author Diego Dasso Migotto (diegomigotto at hotmail dot com)
/// @see See doc/usage.md for the narrative tutorial and doc/data.md for what
///      each number is built from and what `safe` promises per scheme.
///
/// Numbers drawn without `safe` are structurally valid and may match a real
/// person's account. They are test data: never use them for payments, direct
/// debits or to pass a check. The library looks nothing up.

DASMIG_CORE_REQUIRE(1, 4);

namespace dasmig
{

// Members have {} initializers so designated initializers may leave them
// out without -Wmissing-designated-field-initializers warnings.
// NOLINTBEGIN(readability-redundant-member-init)

/// @brief The kinds of account a request can ask for. A label in 1.0, except in
///        Brazil, whose IBAN carries it.
enum class account_kind : std::uint8_t
{
    current, ///< A current or checking account.
    savings  ///< A savings account.
};

/// @brief What a finance call asks for.
///
/// Every generation call takes one; `finance_request{}` asks for the defaults.
/// Choices are never stored on the generator, so one call cannot change the
/// next. A given field skips its draw and leaves every other stream's draws
/// unchanged, and asking for another account leaves the first ones unchanged.
struct finance_request
{
    /// @brief The default reference date, the same as id-generator's.
    static constexpr std::chrono::year_month_day default_reference_date{
        std::chrono::year{2023}, std::chrono::month{7}, std::chrono::day{1}};

    std::optional<std::string> country{};  ///< ISO alpha-2, any case; unset: drawn by population. A territory only by name.
    std::optional<std::chrono::year_month_day> birth_date{}; ///< Unset: drawn uniformly for ages 18 to 80.
    std::vector<account_kind> accounts{account_kind::current}; ///< The accounts asked for; empty: none.
    std::chrono::year_month_day reference_date{default_reference_date}; ///< The date the person is seen at.
    bool safe{false};     ///< Accounts nobody holds, by each scheme's method (doc/data.md).
    bool weighted{true};  ///< False: countries and banks are drawn uniformly.
};

/// @brief A bank account.
struct bank_account
{
    std::string scheme;          ///< "iban-de", "us-aba", "mx-clabe", "generic-ke".
    account_kind kind{account_kind::current}; ///< As asked for.
    std::string iban;            ///< Electronic form; empty where the country has no IBAN form.
    std::string formatted;       ///< Display form: "DE89 3704 0044 0532 0130 00".
    std::string bank_code;       ///< "37040044"; the everyday code where a country has two.
    std::string branch_code;     ///< Empty when the scheme has none.
    std::string account_number;  ///< What follows the bank and branch, with its own final check digit.
    std::string bank_name;       ///< Empty when safe, or when the code names no listed bank.
    std::string bic;             ///< "COBADEFFXXX"; empty when unknown. Under safe, a test BIC by SWIFT's convention.
    std::string currency;        ///< ISO 4217: "EUR".
    std::uint8_t currency_digits{2}; ///< Minor-unit digits.
    std::chrono::year_month_day opened{}; ///< From the minimum age to the reference date.
    std::string evidence;        ///< "issuer", "reference", "secondary" or "generic".
    bool reserved{false};        ///< The safe method was applied; doc/data.md says what each promises.
};

/// @brief The data tables, for layers.
enum class finance_table : std::uint8_t
{
    countries,     ///< countries.tsv
    schemes,       ///< schemes.tsv
    iban_formats,  ///< iban_formats.tsv
    banks,         ///< banks.tsv
    test_accounts, ///< test_accounts.tsv
    networks,      ///< networks.tsv
    card_networks, ///< card_networks.tsv
    test_cards,    ///< test_cards.tsv
    ownership      ///< ownership.tsv
};

// NOLINTEND(readability-redundant-member-init)

/// @cond INTERNAL
namespace fingen_detail
{

using day_t = std::int32_t;

[[nodiscard]] inline day_t to_day(const std::chrono::year_month_day& d)
{
    return static_cast<day_t>(std::chrono::sys_days{d}.time_since_epoch().count());
}

[[nodiscard]] inline std::chrono::year_month_day ymd_of(day_t d)
{
    return std::chrono::year_month_day{std::chrono::sys_days{std::chrono::days{d}}};
}

[[nodiscard]] inline day_t make_day(int y, unsigned m, unsigned d)
{
    return to_day(std::chrono::year_month_day{std::chrono::year{y}, std::chrono::month{m}, std::chrono::day{d}});
}

// The same day `years` later; Feb 29 in a year without one is the 28th.
[[nodiscard]] inline day_t anniversary(day_t day, int years)
{
    const auto v = ymd_of(day);
    const int y = static_cast<int>(v.year()) + years;
    const std::chrono::year_month_day same{std::chrono::year{y}, v.month(), v.day()};
    return same.ok() ? to_day(same) : make_day(y, static_cast<unsigned>(v.month()), 28);
}

[[nodiscard]] inline std::string pad_number(unsigned value, std::size_t width)
{
    std::string text = std::to_string(value);
    if (text.size() < width) text.insert(0, width - text.size(), '0');
    return text;
}

[[nodiscard]] inline std::string iso_date(const std::chrono::year_month_day& v)
{
    return pad_number(static_cast<unsigned>(static_cast<int>(v.year())), 4) + "-" +
           pad_number(static_cast<unsigned>(v.month()), 2) + "-" + pad_number(static_cast<unsigned>(v.day()), 2);
}

[[nodiscard]] inline std::string upper(std::string_view text)
{
    std::string out{text};
    for (char& c : out)
    {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

[[nodiscard]] inline std::string strip_separators(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        if (c != ' ' && c != '-') out.push_back(c);
    }
    return out;
}

[[nodiscard]] constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

[[nodiscard]] inline bool all_digits(std::string_view text)
{
    return !text.empty() && std::ranges::all_of(text, is_digit);
}

// A different digit, by a step of 1 to 9 drawn from the stream.
[[nodiscard]] inline char other_digit(char c, core::rng& r)
{
    const auto step = static_cast<unsigned>(core::bounded(r, 9)) + 1U;
    return static_cast<char>('0' + ((static_cast<unsigned>(c - '0') + step) % 10U));
}

// -- Germany: the Bundesbank's Pruefzifferberechnungsmethoden (June 2018) ------

// Weights from the right, repeated; digits outside `part` do not count.
[[nodiscard]] inline unsigned weigh_right(std::string_view part, std::span<const unsigned> weights)
{
    unsigned sum = 0;
    for (std::size_t k = 0; k < part.size(); ++k)
    {
        sum += static_cast<unsigned>(part[part.size() - 1 - k] - '0') * weights[k % weights.size()];
    }
    return sum;
}

// Method 06's reduction, which most of them share: 11 - remainder; 10 and 11 give 0.
[[nodiscard]] constexpr char mod11_modified(unsigned sum)
{
    const unsigned c = 11U - (sum % 11U);
    return static_cast<char>('0' + (c >= 10U ? 0U : c));
}

// Method 00: weights 2, 1 from the right, digit sum of each product, 10 - last digit.
[[nodiscard]] inline char mod10_alternating(std::string_view part)
{
    unsigned sum = 0;
    for (std::size_t k = 0; k < part.size(); ++k)
    {
        const unsigned p = static_cast<unsigned>(part[part.size() - 1 - k] - '0') * (k % 2 == 0 ? 2U : 1U);
        sum += p > 9U ? p - 9U : p;
    }
    return static_cast<char>('0' + ((10U - (sum % 10U)) % 10U));
}

inline constexpr std::array<unsigned, 6> w_2_7{2, 3, 4, 5, 6, 7};
inline constexpr std::array<unsigned, 7> w_2_8{2, 3, 4, 5, 6, 7, 8};
inline constexpr std::array<unsigned, 9> w_2_10{2, 3, 4, 5, 6, 7, 8, 9, 10};
inline constexpr std::array<unsigned, 9> w_20{2, 3, 4, 5, 6, 7, 8, 9, 3};
inline constexpr std::array<unsigned, 5> w_2_6{2, 3, 4, 5, 6};
inline constexpr std::array<unsigned, 3> w_371{3, 7, 1};
inline constexpr std::array<unsigned, 7> w_34{2, 4, 8, 5, 10, 9, 7};
inline constexpr std::array<unsigned, 6> w_38{2, 4, 8, 5, 10, 9};
inline constexpr std::array<unsigned, 6> w_91_2{7, 6, 5, 4, 3, 2};

[[nodiscard]] inline bool de_method_known(int method)
{
    constexpr std::array<int, 20> known{0, 1, 6, 9, 10, 13, 20, 24, 28, 32, 33, 34, 38, 60, 61, 63, 76, 88, 91, 99};
    return std::ranges::find(known, method) != known.end();
}

// "63" as 63; anything else, such as "A1", as -1.
[[nodiscard]] inline int de_method_number(std::string_view text)
{
    if (text.size() != 2 || !is_digit(text[0]) || !is_digit(text[1])) return -1;
    return ((text[0] - '0') * 10) + (text[1] - '0');
}

// The primary form of a method whose check digit moves when sub-account 00 is left out.
[[nodiscard]] inline bool de_valid_plain(int method, std::string_view a)
{
    switch (method)
    {
    case 9: return true;
    case 0: return mod10_alternating(a.substr(0, 9)) == a[9];
    case 1:
    {
        const unsigned sum = weigh_right(a.substr(0, 9), w_371);
        return static_cast<char>('0' + ((10U - (sum % 10U)) % 10U)) == a[9];
    }
    case 6: return mod11_modified(weigh_right(a.substr(0, 9), w_2_7)) == a[9];
    case 10: return mod11_modified(weigh_right(a.substr(0, 9), w_2_10)) == a[9];
    case 13: return mod10_alternating(a.substr(1, 6)) == a[7];
    case 20: return mod11_modified(weigh_right(a.substr(0, 9), w_20)) == a[9];
    case 24:
    {
        // Position 1's 3 to 6 count as 0; a 9 makes positions 1 to 3 count as 0.
        std::size_t start = 0;
        if (a[0] >= '3' && a[0] <= '6') start = 1;
        if (a[0] == '9') start = 3;
        while (start < 9 && a[start] == '0') ++start;
        unsigned sum = 0;
        for (std::size_t i = start; i < 9; ++i)
        {
            const unsigned w = static_cast<unsigned>((i - start) % 3) + 1U;
            sum += ((static_cast<unsigned>(a[i] - '0') * w) + w) % 11U;
        }
        return static_cast<char>('0' + (sum % 10U)) == a[9];
    }
    case 28: return mod11_modified(weigh_right(a.substr(0, 7), w_2_8)) == a[7];
    case 32: return mod11_modified(weigh_right(a.substr(3, 6), w_2_7)) == a[9];
    case 33: return mod11_modified(weigh_right(a.substr(4, 5), w_2_6)) == a[9];
    case 34: return mod11_modified(weigh_right(a.substr(0, 7), w_34)) == a[7];
    case 38: return mod11_modified(weigh_right(a.substr(3, 6), w_38)) == a[9];
    case 60: return mod10_alternating(a.substr(2, 7)) == a[9];
    case 61:
    {
        // Left to right 2, 1, ...; an account kind of 8 brings positions 9 and 10 in.
        unsigned sum = 0;
        const auto add = [&sum](char c, unsigned w)
        {
            const unsigned p = static_cast<unsigned>(c - '0') * w;
            sum += p > 9U ? p - 9U : p;
        };
        for (std::size_t i = 0; i < 7; ++i) add(a[i], i % 2 == 0 ? 2U : 1U);
        if (a[8] == '8')
        {
            add(a[8], 1U);
            add(a[9], 2U);
        }
        return static_cast<char>('0' + ((10U - (sum % 10U)) % 10U)) == a[7];
    }
    case 63: return a[0] == '0' && mod10_alternating(a.substr(1, 6)) == a[7];
    case 76:
    {
        constexpr std::string_view kinds = "046789";
        if (kinds.find(a[0]) == std::string_view::npos) return false;
        const unsigned remainder = weigh_right(a.substr(1, 6), w_2_7) % 11U;
        return remainder != 10U && static_cast<char>('0' + remainder) == a[7];
    }
    case 88:
        return a[2] == '9' ? mod11_modified(weigh_right(a.substr(2, 7), w_2_8)) == a[9]
                           : mod11_modified(weigh_right(a.substr(3, 6), w_2_7)) == a[9];
    case 91:
    {
        if (mod11_modified(weigh_right(a.substr(0, 6), w_2_7)) == a[6]) return true;
        if (mod11_modified(weigh_right(a.substr(0, 6), w_91_2)) == a[6]) return true;
        // Variant 3 takes every position but the check, from the right 2, 3, 4, then 5 to 10.
        constexpr std::array<unsigned, 10> v3{2, 3, 4, 0, 5, 6, 7, 8, 9, 10};
        if (mod11_modified(weigh_right(a, v3)) == a[6]) return true;
        return mod11_modified(weigh_right(a.substr(0, 6), w_38)) == a[6];
    }
    case 99:
    {
        if (a >= std::string_view{"0396000000"} && a <= std::string_view{"0499999999"}) return true;
        return mod11_modified(weigh_right(a.substr(0, 9), w_2_7)) == a[9];
    }
    default: return false;
    }
}

[[nodiscard]] inline bool de_valid(int method, std::string_view a)
{
    if (a.size() != 10 || !all_digits(a)) return false;
    if (de_valid_plain(method, a)) return true;
    // Sub-account 00 may be left out, moving the number two places to the right.
    const bool shifts = method == 13 || method == 76 || (method == 63 && a.starts_with("000"));
    if (shifts && a.starts_with("00"))
    {
        const std::string moved = std::string{a.substr(2)} + "00";
        return de_valid_plain(method, moved);
    }
    return false;
}

// The position a method's check digit is solved at; 9 for most.
[[nodiscard]] constexpr std::size_t de_check_index(int method)
{
    switch (method)
    {
    case 13: case 28: case 34: case 61: case 63: case 76: return 7;
    case 91: return 6;
    default: return 9;
    }
}

// Makes a drawn body valid; false when no digit does (redraw).
[[nodiscard]] inline bool de_fill(int method, std::string& a)
{
    if (method == 9) return true;
    if (method == 63) a[0] = '0';
    if (method == 76) a[0] = std::string_view{"046789"}[static_cast<std::size_t>(a[0] - '0') % 6];
    if (method == 24 && a[0] == '9' && a[3] == '0') a[3] = '1'; // "Die Stelle 4 ist ungleich 0"
    const std::size_t at = de_check_index(method);
    for (char d = '0'; d <= '9'; ++d)
    {
        a[at] = d;
        if (de_valid_plain(method, a)) return true;
    }
    return false;
}

// -- National checks inside a BBAN (design 4.2) --------------------------------

enum class check_kind : std::uint8_t
{
    none, rib_key, mod97_bban, mod97_variant, it_cin, cz, ru_key, de_method, es, pl, be, fi, no, ee, is
};

[[nodiscard]] inline std::optional<check_kind> parse_check(std::string_view text)
{
    constexpr std::array<std::pair<std::string_view, check_kind>, 15> names{{
        {"", check_kind::none}, {"rib_key", check_kind::rib_key}, {"mod97_bban", check_kind::mod97_bban},
        {"mod97_variant", check_kind::mod97_variant}, {"it_cin", check_kind::it_cin}, {"cz", check_kind::cz},
        {"ru_key", check_kind::ru_key}, {"de_method", check_kind::de_method}, {"es", check_kind::es},
        {"pl", check_kind::pl}, {"be", check_kind::be}, {"fi", check_kind::fi}, {"no", check_kind::no},
        {"ee", check_kind::ee}, {"is", check_kind::is},
    }};
    for (const auto& [name, kind] : names)
    {
        if (name == text) return kind;
    }
    return std::nullopt;
}

struct iban_format
{
    std::string country;
    core::pattern pattern;  // what the registry allows, for reading a number
    core::pattern drawn;    // the same with digits where it allows either: real accounts are mostly digits
    std::size_t length{0};
    int bank_from{-1}, bank_to{-1};
    int branch_from{-1}, branch_to{-1};
    int account_from{-1}, account_to{-1};
    int check_from{-1}, check_to{-1};
    check_kind check{check_kind::none};
};

// The shortest BBAN each check's fixed positions fit in; a layer below it is refused.
[[nodiscard]] constexpr std::size_t check_min_length(check_kind k)
{
    switch (k)
    {
    case check_kind::be: return 12;
    case check_kind::ee: return 16;
    case check_kind::es: case check_kind::cz: return 20;
    case check_kind::fi: return 14;
    case check_kind::no: return 11;
    case check_kind::pl: return 8;
    case check_kind::it_cin: return 23;
    case check_kind::ru_key: return 29;
    case check_kind::is: return 22;
    case check_kind::de_method: return 18;
    default: return 0;
    }
}

// Whether a check is one string at the format's check position, computed from the rest.
[[nodiscard]] constexpr bool check_is_positional(check_kind k)
{
    return k != check_kind::none && k != check_kind::cz && k != check_kind::de_method;
}

[[nodiscard]] inline std::string two_digits(unsigned value) { return pad_number(value, 2); }

// French letter values: A to I as 1 to 9, J to R as 1 to 9, S to Z as 2 to 9.
[[nodiscard]] constexpr unsigned rib_value(char c)
{
    if (is_digit(c)) return static_cast<unsigned>(c - '0');
    if (c >= 'A' && c <= 'I') return static_cast<unsigned>(c - 'A') + 1U;
    if (c >= 'J' && c <= 'R') return static_cast<unsigned>(c - 'J') + 1U;
    return static_cast<unsigned>(c - 'S') + 2U;
}

// Italy's CIN: odd positions (from 1) through the table, even ones by their value.
[[nodiscard]] inline char it_cin_letter(std::string_view body)
{
    constexpr std::array<unsigned, 26> odds{1,  0,  5,  7,  9,  13, 15, 17, 19, 21, 2,  4,  18,
                                            20, 11, 3,  6,  8,  12, 14, 16, 10, 22, 25, 24, 23};
    unsigned sum = 0;
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        const char c = body[i];
        const unsigned v = is_digit(c) ? static_cast<unsigned>(c - '0') : static_cast<unsigned>(c - 'A');
        sum += (i + 1) % 2 == 0 ? v : odds.at(v);
    }
    return static_cast<char>('A' + (sum % 26U));
}

// The sum of digits by weights from the left, stopping at the shorter.
[[nodiscard]] inline unsigned weigh_left(std::string_view part, std::span<const unsigned> w)
{
    unsigned sum = 0;
    for (std::size_t i = 0; i < part.size() && i < w.size(); ++i) sum += static_cast<unsigned>(part[i] - '0') * w[i];
    return sum;
}

// Norway's mod 11; nullopt when the body would need a 10.
[[nodiscard]] inline std::optional<std::string> no_digit(std::string_view part)
{
    constexpr std::array<unsigned, 10> w{5, 4, 3, 2, 7, 6, 5, 4, 3, 2};
    const unsigned c = 11U - (weigh_left(part, w) % 11U);
    if (c == 10U) return std::nullopt;
    return std::string(1, static_cast<char>('0' + (c % 11U)));
}

// The check string a positional check gives this BBAN; nullopt when the body
// admits none (a mod-11 remainder that needs a 10).
[[nodiscard]] inline std::optional<std::string> computed_check(check_kind k, std::string_view b, const iban_format& f)
{
    switch (k)
    {
    case check_kind::be:
    {
        unsigned r = 0;
        for (const char c : b.substr(0, 10)) r = ((r * 10U) + static_cast<unsigned>(c - '0')) % 97U;
        return two_digits(r == 0 ? 97U : r);
    }
    case check_kind::ee:
    {
        constexpr std::array<unsigned, 3> w{7, 3, 1};
        const unsigned sum = weigh_right(b.substr(2, 13), w);
        return std::string(1, static_cast<char>('0' + ((10U - (sum % 10U)) % 10U)));
    }
    case check_kind::es:
    {
        constexpr std::array<unsigned, 10> w{1, 2, 4, 8, 5, 10, 9, 7, 3, 6};
        const auto digit = [](unsigned sum)
        {
            const unsigned c = 11U - (sum % 11U);
            return static_cast<char>('0' + (c == 11U ? 0U : (c == 10U ? 1U : c)));
        };
        std::string out(2, '0');
        out[0] = digit(weigh_left(b.substr(0, 8), std::span<const unsigned>{w}.subspan(2)));
        out[1] = digit(weigh_left(b.substr(10, 10), w));
        return out;
    }
    case check_kind::fi: return std::string(1, core::luhn_digit(b.substr(0, 13)));
    case check_kind::no: return no_digit(b.substr(0, 10));
    case check_kind::pl:
    {
        constexpr std::array<unsigned, 7> w{3, 9, 7, 1, 3, 9, 7};
        return std::string(1, static_cast<char>('0' + ((10U - (weigh_left(b.substr(0, 7), w) % 10U)) % 10U)));
    }
    case check_kind::it_cin: return std::string(1, it_cin_letter(b.substr(1, 22)));
    case check_kind::mod97_bban:
    {
        const auto d = core::mod97_10_digits(b.substr(0, static_cast<std::size_t>(f.check_from)));
        return std::string{d[0], d[1]};
    }
    case check_kind::mod97_variant:
    {
        const auto d = core::mod97_10_digits(b.substr(0, static_cast<std::size_t>(f.check_from)));
        // 98 - r there, 97 - r here.
        return two_digits((static_cast<unsigned>(d[0] - '0') * 10U) + static_cast<unsigned>(d[1] - '0') - 1U);
    }
    case check_kind::rib_key:
    {
        unsigned r = 0;
        for (const char c : b.substr(0, static_cast<std::size_t>(f.check_from))) r = ((r * 10U) + rib_value(c)) % 97U;
        r = (r * 100U) % 97U;
        return two_digits(97U - r);
    }
    case check_kind::ru_key:
    {
        // 7, 1, 3 over the BIK's last three digits and the account, key taken as 0.
        constexpr std::array<unsigned, 3> w{7, 1, 3};
        unsigned sum = 0;
        for (std::size_t i = 0; i < 23; ++i)
        {
            const std::size_t at = i + 6;
            if (at != static_cast<std::size_t>(f.check_from)) sum += static_cast<unsigned>(b[at] - '0') * w.at(i % 3);
        }
        const std::size_t key_at = static_cast<std::size_t>(f.check_from) - 6;
        const unsigned need = (10U - (sum % 10U)) % 10U;
        // The key's weight is 7, 1 or 3, each with an inverse mod 10.
        constexpr std::array<unsigned, 3> inverse{3, 1, 7};
        return std::string(1, static_cast<char>('0' + ((need * inverse.at(key_at % 3)) % 10U)));
    }
    case check_kind::is:
    {
        constexpr std::array<unsigned, 8> w{3, 2, 7, 6, 5, 4, 3, 2};
        const unsigned r = weigh_left(b.substr(12, 8), w) % 11U;
        const unsigned c = r == 0 ? 0U : 11U - r;
        if (c == 10U) return std::nullopt;
        return std::string(1, static_cast<char>('0' + c));
    }
    default: return std::nullopt;
    }
}

// Whether a check's input holds the digits it reads, so computing it cannot misread a letter.
[[nodiscard]] inline bool check_input_ok(check_kind k, std::string_view b, const iban_format& f)
{
    switch (k)
    {
    case check_kind::be: return all_digits(b.substr(0, 12));
    case check_kind::ee: return all_digits(b.substr(0, 16));
    case check_kind::es: case check_kind::cz: return all_digits(b.substr(0, 20));
    case check_kind::fi: return all_digits(b.substr(0, 14));
    case check_kind::no: return all_digits(b.substr(0, 11));
    case check_kind::pl: return all_digits(b.substr(0, 8));
    case check_kind::ru_key: return all_digits(b.substr(0, 29));
    case check_kind::is: return all_digits(b.substr(12, 10));
    case check_kind::mod97_bban: case check_kind::mod97_variant: case check_kind::rib_key:
        return f.check_from > 0 && all_digits(b.substr(static_cast<std::size_t>(f.check_from), 2));
    default: return true;
    }
}

// The Czech and Slovak rule: prefix and account each weigh to 0 mod 11.
[[nodiscard]] inline bool cz_part_ok(std::string_view part)
{
    constexpr std::array<unsigned, 10> from_right{1, 2, 4, 8, 5, 10, 9, 7, 3, 6};
    return weigh_right(part, from_right) % 11U == 0;
}

[[nodiscard]] inline bool cz_fill_part(std::string& bban, std::size_t from, std::size_t size)
{
    for (char d = '0'; d <= '9'; ++d)
    {
        bban[from + size - 1] = d;
        if (cz_part_ok(std::string_view{bban}.substr(from, size))) return true;
    }
    return false;
}

// Makes a drawn BBAN satisfy its check; false when this body cannot (redraw).
[[nodiscard]] inline bool fill_check(check_kind k, std::string& bban, const iban_format& f, int method)
{
    switch (k)
    {
    case check_kind::none: return true;
    case check_kind::cz: return cz_fill_part(bban, 4, 6) && cz_fill_part(bban, 10, 10);
    case check_kind::de_method:
    {
        if (method < 0) return true;
        std::string account = bban.substr(8, 10);
        if (!de_fill(method, account)) return false;
        bban.replace(8, 10, account);
        return true;
    }
    default:
    {
        const auto check = computed_check(k, bban, f);
        if (!check) return false;
        bban.replace(static_cast<std::size_t>(f.check_from), check->size(), *check);
        return true;
    }
    }
}

// Whether a BBAN's national check holds. A German bank with no known method
// (method < 0) has nothing to check against.
[[nodiscard]] inline bool check_holds(check_kind k, std::string_view bban, const iban_format& f, int method)
{
    if (!check_input_ok(k, bban, f)) return false;
    switch (k)
    {
    case check_kind::none: return true;
    case check_kind::cz: return cz_part_ok(bban.substr(4, 6)) && cz_part_ok(bban.substr(10, 10));
    case check_kind::de_method: return method < 0 || !de_method_known(method) || de_valid(method, bban.substr(8, 10));
    default:
    {
        // Sources differ on 00 in positions 5 and 6: schwifty counts only the next four
        // digits, stdnum all of them. Either reading passes here, and none is drawn.
        if (k == check_kind::no && bban.substr(4, 2) == "00" && no_digit(bban.substr(6, 4)) == bban.substr(10, 1)) return true;
        const auto check = computed_check(k, bban, f);
        return check && bban.substr(static_cast<std::size_t>(f.check_from), check->size()) == *check;
    }
    }
}

// Breaks a BBAN's national check, leaving the rest; false when nothing can (method 09).
[[nodiscard]] inline bool spoil_check(check_kind k, std::string& bban, const iban_format& f, int method, core::rng& r)
{
    std::size_t at = 0;
    if (k == check_kind::cz) at = 19;
    else if (k == check_kind::de_method) at = 8 + de_check_index(method);
    else if (check_is_positional(k)) at = static_cast<std::size_t>(f.check_from);
    else return false;
    const char was = bban[at];
    if (is_digit(was))
    {
        // A step can land on another valid value in the loosest rules, so try them all.
        char c = other_digit(was, r);
        for (int tries = 0; tries < 10; ++tries)
        {
            bban[at] = c;
            if (c != was && !check_holds(k, bban, f, method)) return true;
            c = c == '9' ? '0' : static_cast<char>(c + 1);
        }
    }
    else
    {
        char c = was;
        for (int tries = 0; tries < 26; ++tries)
        {
            c = c == 'Z' ? 'A' : static_cast<char>(c + 1);
            bban[at] = c;
            if (c != was && !check_holds(k, bban, f, method)) return true;
        }
    }
    bban[at] = was;
    return false;
}

// -- National forms outside the registry (design 4.3) -----------------------------

// What a number is made of; apply_display and parse_display read and write these.
struct parts
{
    std::string iban, bank, branch, account, check, check1, check2, suffix;
};

[[nodiscard]] constexpr bool aba_prefix_ok(unsigned prefix)
{
    return prefix <= 12U || (prefix >= 21U && prefix <= 32U) || (prefix >= 61U && prefix <= 72U) || prefix == 80U;
}

[[nodiscard]] inline char aba_digit(std::string_view eight)
{
    constexpr std::array<unsigned, 8> w{3, 7, 1, 3, 7, 1, 3, 7};
    const unsigned sum = weigh_left(eight.substr(0, 8), w);
    return static_cast<char>('0' + ((10U - (sum % 10U)) % 10U));
}

[[nodiscard]] inline bool aba_valid(std::string_view code)
{
    if (code.size() != 9 || !all_digits(code)) return false;
    const unsigned prefix = (static_cast<unsigned>(code[0] - '0') * 10U) + static_cast<unsigned>(code[1] - '0');
    return aba_prefix_ok(prefix) && aba_digit(code) == code[8];
}

// Weights from the left, repeated, reduced as 10 - last digit: NUBAN (3, 7, 3), CLABE (3, 7, 1).
[[nodiscard]] inline char mod10_cycle(std::string_view part, std::span<const unsigned> w)
{
    unsigned sum = 0;
    for (std::size_t i = 0; i < part.size(); ++i) sum += static_cast<unsigned>(part[i] - '0') * w[i % w.size()];
    return static_cast<char>('0' + ((10U - (sum % 10U)) % 10U));
}

inline constexpr std::array<unsigned, 3> w_nuban{3, 7, 3};
inline constexpr std::array<unsigned, 3> w_clabe{3, 7, 1};

// Argentina's CBU: 3, 1, 7, 9 from the right, per group.
[[nodiscard]] inline char cbu_digit(std::string_view group)
{
    constexpr std::array<unsigned, 4> w{3, 1, 7, 9};
    const unsigned sum = weigh_right(group, w);
    return static_cast<char>('0' + ((10U - (sum % 10U)) % 10U));
}

// New Zealand: a weight set by bank, from Inland Revenue's Payroll Calculations and
// Business Rules, section 7. Over bank 2, branch 4, account 7 and suffix 3; sets E
// and G add the two digits of each product, and again those of the result.
struct nz_rule
{
    std::array<unsigned, 16> weights{};
    bool digit_sum{false};
    unsigned modulus{0};
    std::size_t check_at{12}; // the position with weight 1
};

[[nodiscard]] inline std::optional<nz_rule> nz_rule_for(std::string_view number)
{
    constexpr nz_rule a{{0, 0, 6, 3, 7, 9, 0, 10, 5, 8, 4, 2, 1, 0, 0, 0}, false, 11, 12};
    constexpr nz_rule b{{0, 0, 0, 0, 0, 0, 0, 10, 5, 8, 4, 2, 1, 0, 0, 0}, false, 11, 12};
    constexpr nz_rule d{{0, 0, 0, 0, 0, 0, 7, 6, 5, 4, 3, 2, 1, 0, 0, 0}, false, 11, 12};
    constexpr nz_rule e{{0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 4, 3, 2, 0, 0, 1}, true, 11, 15};
    constexpr nz_rule f{{0, 0, 0, 0, 0, 0, 1, 7, 3, 1, 7, 3, 1, 0, 0, 0}, false, 10, 12};
    constexpr nz_rule g{{0, 0, 0, 0, 0, 0, 1, 3, 7, 1, 3, 7, 1, 3, 7, 1}, true, 10, 15};
    const std::string_view bank = number.substr(0, 2);
    if (bank == "08") return d;
    if (bank == "09") return e;
    if (bank == "25" || bank == "33") return f;
    if (bank == "26" || bank == "28" || bank == "29") return g;
    constexpr std::array<std::string_view, 26> set_a{"01", "02", "03", "04", "05", "06", "10", "11", "12",
                                                     "13", "14", "15", "16", "17", "18", "19", "20", "21",
                                                     "22", "23", "24", "27", "30", "35", "38", "88"};
    if (std::ranges::find(set_a, bank) != set_a.end())
    {
        return number.substr(6, 7) >= std::string_view{"0990000"} ? b : a;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool nz_valid(std::string_view number)
{
    if (number.size() != 16 || !all_digits(number)) return false;
    const auto rule = nz_rule_for(number);
    if (!rule) return false;
    unsigned sum = 0;
    for (std::size_t i = 0; i < 16; ++i)
    {
        unsigned c = rule->weights.at(i) * static_cast<unsigned>(number[i] - '0');
        if (rule->digit_sum)
        {
            c = (c / 10U) + (c % 10U);
            c = (c / 10U) + (c % 10U);
        }
        sum += c;
    }
    return sum % rule->modulus == 0;
}

[[nodiscard]] inline std::string nz_number(const parts& p)
{
    std::string suffix = p.suffix;
    if (suffix.size() < 3) suffix.insert(0, 3 - suffix.size(), '0');
    return p.bank + p.branch + p.account + suffix;
}

enum class builder_kind : std::uint8_t
{
    iban, iban_br, iban_ru, national, bic_account, generic, us_aba, ng_nuban, mx_clabe, ar_cbu, nz_account
};

[[nodiscard]] inline std::optional<builder_kind> parse_builder(std::string_view text)
{
    constexpr std::array<std::pair<std::string_view, builder_kind>, 11> names{{
        {"iban", builder_kind::iban}, {"iban_br", builder_kind::iban_br}, {"iban_ru", builder_kind::iban_ru},
        {"national", builder_kind::national}, {"bic_account", builder_kind::bic_account},
        {"generic", builder_kind::generic}, {"us_aba", builder_kind::us_aba}, {"ng_nuban", builder_kind::ng_nuban},
        {"mx_clabe", builder_kind::mx_clabe}, {"ar_cbu", builder_kind::ar_cbu}, {"nz_account", builder_kind::nz_account},
    }};
    for (const auto& [name, kind] : names)
    {
        if (name == text) return kind;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool is_iban_builder(builder_kind b)
{
    return b == builder_kind::iban || b == builder_kind::iban_br || b == builder_kind::iban_ru;
}

// Fills a national form's check fields from its body; false when this body cannot.
[[nodiscard]] inline bool national_fill(builder_kind b, parts& p, bool listed_bank)
{
    switch (b)
    {
    case builder_kind::us_aba:
    {
        if (listed_bank) return true;
        p.bank[8] = aba_digit(p.bank);
        return aba_valid(p.bank);
    }
    case builder_kind::ng_nuban: p.check = std::string(1, mod10_cycle(p.bank + p.account, w_nuban)); return true;
    case builder_kind::mx_clabe: p.check = std::string(1, mod10_cycle(p.bank + p.branch + p.account, w_clabe)); return true;
    case builder_kind::ar_cbu:
        p.check1 = std::string(1, cbu_digit(p.bank + p.branch));
        p.check2 = std::string(1, cbu_digit(p.account));
        return true;
    case builder_kind::nz_account:
    {
        const auto rule = nz_rule_for(nz_number(p));
        if (!rule) return false;
        std::string& field = rule->check_at == 15 ? p.suffix : p.account;
        for (char d = '0'; d <= '9'; ++d)
        {
            field.back() = d;
            if (nz_valid(nz_number(p))) return true;
        }
        return false;
    }
    default: return true;
    }
}

[[nodiscard]] inline bool national_valid(builder_kind b, const parts& p)
{
    switch (b)
    {
    case builder_kind::us_aba: return aba_valid(p.bank);
    case builder_kind::ng_nuban: return p.check.size() == 1 && mod10_cycle(p.bank + p.account, w_nuban) == p.check[0];
    case builder_kind::mx_clabe:
        return p.check.size() == 1 && mod10_cycle(p.bank + p.branch + p.account, w_clabe) == p.check[0];
    case builder_kind::ar_cbu:
        return p.check1.size() == 1 && p.check2.size() == 1 && cbu_digit(p.bank + p.branch) == p.check1[0] &&
               cbu_digit(p.account) == p.check2[0];
    case builder_kind::nz_account: return nz_valid(nz_number(p));
    default: return true;
    }
}

// The wrong_outer method on a national form: exactly one check digit changes.
inline void national_spoil(builder_kind b, parts& p, core::rng& r)
{
    switch (b)
    {
    case builder_kind::us_aba: p.bank[8] = other_digit(p.bank[8], r); break;
    case builder_kind::ng_nuban: case builder_kind::mx_clabe: p.check[0] = other_digit(p.check[0], r); break;
    case builder_kind::ar_cbu: p.check2[0] = other_digit(p.check2[0], r); break;
    case builder_kind::nz_account:
    {
        const auto rule = nz_rule_for(nz_number(p));
        std::string& field = (rule && rule->check_at == 15) ? p.suffix : p.account;
        field.back() = other_digit(field.back(), r);
        break;
    }
    default: break;
    }
}

// -- Schemes -----------------------------------------------------------------------

enum class safe_method : std::uint8_t { test_value, unassigned_code, wrong_national, wrong_outer, none };

[[nodiscard]] inline std::optional<safe_method> parse_safe(std::string_view text)
{
    if (text == "test_value") return safe_method::test_value;
    if (text == "unassigned_code") return safe_method::unassigned_code;
    if (text == "wrong_national") return safe_method::wrong_national;
    if (text == "wrong_outer") return safe_method::wrong_outer;
    if (text == "none") return safe_method::none;
    return std::nullopt;
}

enum class token_kind : std::uint8_t { literal, iban, bank, branch, account, check, check1, check2, suffix };

struct token
{
    token_kind kind{token_kind::literal};
    std::string text; // a literal's text
};

// "{bank} {account}" as tokens; nullopt for an unknown name, which the caller reports.
[[nodiscard]] inline std::optional<std::vector<token>> parse_tokens(std::string_view display)
{
    constexpr std::array<std::pair<std::string_view, token_kind>, 8> names{{
        {"iban", token_kind::iban}, {"bank", token_kind::bank}, {"branch", token_kind::branch},
        {"account", token_kind::account}, {"check", token_kind::check}, {"check1", token_kind::check1},
        {"check2", token_kind::check2}, {"suffix", token_kind::suffix},
    }};
    std::vector<token> out;
    std::string literal;
    for (std::size_t i = 0; i < display.size();)
    {
        if (display[i] != '{')
        {
            literal.push_back(display[i++]);
            continue;
        }
        const auto end = display.find('}', i);
        if (end == std::string_view::npos) return std::nullopt;
        const std::string_view name = display.substr(i + 1, end - i - 1);
        if (!literal.empty())
        {
            out.push_back({.kind = token_kind::literal, .text = literal});
            literal.clear();
        }
        const auto found = std::ranges::find_if(names, [&](const auto& n) { return n.first == name; });
        if (found == names.end()) return std::nullopt;
        out.push_back({.kind = found->second, .text = {}});
        i = end + 1;
    }
    if (!literal.empty()) out.push_back({.kind = token_kind::literal, .text = literal});
    return out;
}

[[nodiscard]] inline std::string group_in_fours(std::string_view iban)
{
    std::string out;
    out.reserve(iban.size() + (iban.size() / 4));
    for (std::size_t i = 0; i < iban.size(); ++i)
    {
        if (i > 0 && i % 4 == 0) out.push_back(' ');
        out.push_back(iban[i]);
    }
    return out;
}

[[nodiscard]] inline std::string apply_display(std::span<const token> tokens, const parts& p)
{
    std::string out;
    out.reserve(40);
    for (const auto& t : tokens)
    {
        switch (t.kind)
        {
        case token_kind::literal: out += t.text; break;
        case token_kind::iban: out += group_in_fours(p.iban); break;
        case token_kind::bank: out += p.bank; break;
        case token_kind::branch: out += p.branch; break;
        case token_kind::account: out += p.account; break;
        case token_kind::check: out += p.check; break;
        case token_kind::check1: out += p.check1; break;
        case token_kind::check2: out += p.check2; break;
        case token_kind::suffix: out += p.suffix; break;
        }
    }
    return out;
}

struct bank_row
{
    std::string code;
    std::string name;
    std::string bic;
    std::string everyday;
    std::vector<std::string> branches;
    double weight{0.0};
    int method{-1};
};

struct bank_book
{
    std::vector<bank_row> rows;
    std::unordered_map<std::string, std::uint32_t> index;
    core::weighted_table<std::uint32_t> weights;
    std::vector<std::uint32_t> drawable; // rows with a positive weight, for unweighted draws
};

struct test_account
{
    std::string number;  // electronic form
    std::string shown;   // as published
    parts fields;
};

struct scheme
{
    std::string key;
    std::string country;
    builder_kind builder{builder_kind::generic};
    std::vector<token> display;
    safe_method safe{safe_method::none};
    std::string evidence;
    core::pattern bank_pattern;
    core::pattern branch_pattern;
    core::pattern account_pattern;
    core::pattern suffix_pattern;
    bool list_complete{false};
    int min_age{16};
    std::int32_t format{-1};      // into model::formats, for an IBAN builder
    std::uint32_t book{0};        // into model::books; a territory's is its parent's
    std::vector<test_account> tests;
};

struct country_row
{
    std::string code;
    std::string currency;
    std::uint8_t currency_digits{2};
    double population{0.0};
    std::uint32_t primary{0};
    bool territory{false};
    std::vector<std::uint32_t> schemes; // primary first
};

// Splits a stripped display form back into its fields; nullopt when it cannot be.
[[nodiscard]] inline std::optional<parts> parse_display(const scheme& s, std::string_view plain)
{
    const auto bounds = [&s](token_kind k) -> std::pair<std::size_t, std::size_t>
    {
        switch (k)
        {
        case token_kind::bank: return s.bank_pattern.lengths();
        case token_kind::branch: return s.branch_pattern.lengths();
        case token_kind::account: return s.account_pattern.lengths();
        case token_kind::suffix: return s.suffix_pattern.lengths();
        case token_kind::check: case token_kind::check1: case token_kind::check2: return {1, 1};
        default: return {0, 0};
        }
    };
    std::size_t fixed = 0;
    std::size_t variable_low = 0;
    std::size_t variable_high = 0;
    bool has_variable = false;
    for (const auto& t : s.display)
    {
        if (t.kind == token_kind::literal)
        {
            fixed += strip_separators(t.text).size();
            continue;
        }
        const auto [low, high] = bounds(t.kind);
        if (low == high)
        {
            fixed += low;
        }
        else
        {
            if (has_variable) return std::nullopt;
            has_variable = true;
            variable_low = low;
            variable_high = high;
        }
    }
    if (plain.size() < fixed) return std::nullopt;
    const std::size_t rest = plain.size() - fixed;
    if (has_variable ? (rest < variable_low || rest > variable_high) : rest != 0) return std::nullopt;

    parts p;
    std::size_t at = 0;
    for (const auto& t : s.display)
    {
        if (t.kind == token_kind::literal)
        {
            const std::string text = strip_separators(t.text);
            if (plain.substr(at, text.size()) != text) return std::nullopt;
            at += text.size();
            continue;
        }
        const auto [low, high] = bounds(t.kind);
        const std::size_t size = low == high ? low : rest;
        const std::string value{plain.substr(at, size)};
        at += size;
        switch (t.kind)
        {
        case token_kind::bank:
            if (!s.bank_pattern.matches(value)) return std::nullopt;
            p.bank = value;
            break;
        case token_kind::branch:
            if (!s.branch_pattern.matches(value)) return std::nullopt;
            p.branch = value;
            break;
        case token_kind::account:
            if (!s.account_pattern.matches(value)) return std::nullopt;
            p.account = value;
            break;
        case token_kind::suffix:
            if (!s.suffix_pattern.matches(value)) return std::nullopt;
            p.suffix = value;
            break;
        case token_kind::check:
            if (!all_digits(value)) return std::nullopt;
            p.check = value;
            break;
        case token_kind::check1:
            if (!all_digits(value)) return std::nullopt;
            p.check1 = value;
            break;
        case token_kind::check2:
            if (!all_digits(value)) return std::nullopt;
            p.check2 = value;
            break;
        default: break;
        }
    }
    return p;
}

} // namespace fingen_detail
/// @endcond

/// @brief Return type for finance generation.
///
/// Supports implicit conversion to std::string (the summary), streaming via
/// operator<< and JSON output via to_json().
class finance
{
  public:
    std::string country_code;                 ///< ISO alpha-2 code.
    std::chrono::year_month_day birth_date{}; ///< Echoed, given or drawn.
    std::vector<bank_account> accounts;       ///< The accounts held, as asked for.

    /// @brief Retrieve the random seed used to generate this finance.
    /// @return The call seed; pass it to get_finance(request, seed) with
    ///         the same request and data to replay this finance.
    [[nodiscard]] std::uint64_t seed() const { return _signature.seed; }

    /// @brief The seed and the data this finance was generated from.
    /// @return The replay signature; `data` is fng::data_hash() at the
    ///         time of generation.
    [[nodiscard]] const core::seed_signature& signature() const
    {
        return _signature;
    }

    /// @brief Human-readable summary string.
    /// @return For example `DE, EUR, DE89 3704 0044 0532 0130 00 (Commerzbank)`.
    [[nodiscard]] std::string to_string() const
    {
        std::string out = country_code;
        if (!accounts.empty()) out += ", " + accounts.front().currency;
        for (const auto& a : accounts)
        {
            out += ", " + a.formatted;
            if (!a.bank_name.empty()) out += " (" + a.bank_name + ")";
        }
        return out;
    }

    /// @brief Write this finance as a JSON object.
    /// @param w Writer positioned where a value is allowed.
    void to_json(core::json_writer& w) const
    {
        w.begin_object()
            .field("country_code", country_code)
            .field("birth_date", fingen_detail::iso_date(birth_date))
            .key("accounts")
            .begin_array();
        for (const auto& a : accounts)
        {
            w.begin_object()
                .field("scheme", a.scheme)
                .field("kind", a.kind == account_kind::current ? "current" : "savings")
                .field("iban", a.iban)
                .field("formatted", a.formatted)
                .field("bank_code", a.bank_code)
                .field("branch_code", a.branch_code)
                .field("account_number", a.account_number)
                .field("bank_name", a.bank_name)
                .field("bic", a.bic)
                .field("currency", a.currency)
                .field("currency_digits", static_cast<int>(a.currency_digits))
                .field("opened", fingen_detail::iso_date(a.opened))
                .field("evidence", a.evidence)
                .field("reserved", a.reserved)
                .end_object();
        }
        w.end_array().key("signature");
        _signature.to_json(w);
        w.end_object();
    }

    /// @brief This finance as a JSON document.
    /// @return Compact JSON, as doc/usage.md shows.
    [[nodiscard]] std::string to_json() const
    {
        core::json_writer w;
        to_json(w);
        return w.str();
    }

    /// @brief Implicit conversion to std::string.
    /// @return Summary string.
    operator std::string() const // NOLINT(hicpp-explicit-conversions)
    {
        return to_string();
    }

    /// @brief Stream the summary to an output stream.
    /// @param os Output stream.
    /// @param v The finance to stream.
    /// @return The stream.
    friend std::ostream& operator<<(std::ostream& os, const finance& v)
    {
        os << v.to_string();
        return os;
    }

  private:
    core::seed_signature _signature;
    friend class fng;
};

/// @brief Finance Generator: a person's accounts in the country's own form.
///
/// The call seed splits into `"country"`, `"birth_date"` and one stream per
/// account by position, so a given field or another account moves nothing else. Output is identical on every compiler and platform
/// (dasmig-core).
///
/// Can be used as a singleton via instance() or constructed independently.
///
/// @par Thread safety
/// Each instance is independent. Concurrent calls to get_finance(request) on
/// the **same** instance require external synchronisation; the seeded overload
/// is const and safe to share.
class fng
{
  public:
    /// @brief Default constructor: creates an empty generator with no data.
    fng() = default;

    fng(const fng&) = delete;            ///< Not copyable.
    fng& operator=(const fng&) = delete; ///< Not copyable.
    fng(fng&&) noexcept = default;       ///< Movable.
    fng& operator=(fng&&) noexcept = default; ///< Movable.
    ~fng() = default;                    ///< Destructor.

    /// @brief Access the global singleton instance.
    ///
    /// Loads the `lite` tier (or `full` if `lite` is missing) on first
    /// access, from compiled-in data or an auto-probed `resources/`.
    /// @return The singleton.
    static fng& instance()
    {
        static fng inst{auto_probe_tag{}};
        return inst;
    }

    // -- Generation -------------------------------------------------------

    /// @brief Generate a random finance.
    ///        Use finance_request{} for the defaults.
    /// @param request What to generate.
    /// @return The finance.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument If the request is invalid (see the
    ///         seeded overload).
    [[nodiscard]] finance get_finance(const finance_request& request)
    {
        return get_finance(request, draw_seed_());
    }

    /// @brief Generate a deterministic finance.
    ///
    /// An account the person cannot hold yet (below the scheme's minimum age)
    /// is left out, so batches over ages work.
    ///
    /// @param request What to generate.
    /// @param call_seed Seed; the same request, seed and data give the same
    ///        result. Does not touch the instance engine.
    /// @return The finance.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument For an unknown country; a birth date after
    ///         the reference date or before 1900; an invalid reference date.
    [[nodiscard]] finance get_finance(const finance_request& request, std::uint64_t call_seed) const
    {
        using namespace fingen_detail;
        require_data_();
        const core::rng call{call_seed};
        const day_t reference = reference_of_(request);
        const auto& country = _model.countries[country_of_(request, call)];
        const day_t birth = birth_of_(request, reference, call);

        finance v;
        v._signature = {.seed = call_seed, .data = data_hash()};
        v.country_code = country.code;
        v.birth_date = ymd_of(birth);
        v.accounts.reserve(request.accounts.size());
        for (std::size_t i = 0; i < request.accounts.size(); ++i)
        {
            auto account = make_account_(request.accounts[i], i, country, request, birth, reference, call);
            if (account) v.accounts.push_back(std::move(*account));
        }
        return v;
    }

    // -- Batch generation -------------------------------------------------

    /// @brief Generate a batch of finance results from a seed drawn like
    ///        get_finance() draws its own.
    ///
    /// Item `i` is `get_finance(request, core::item_seed(batch_seed, options.first + i))`
    /// for the drawn batch seed; see the seeded overload.
    ///
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness.
    /// @return The results, in index order.
    [[nodiscard]] std::vector<finance> get_finance_batch(const finance_request& request,
                                                  const core::batch_options& options)
    {
        return get_finance_batch(request, options, draw_seed_());
    }

    /// @brief Generate a batch of finance results, deterministically.
    ///
    /// Item `i` is exactly `get_finance(request, core::item_seed(batch_seed,
    /// options.first + i))`, so an item replays on its own, a range can be
    /// generated directly, two batches that follow each other equal one, and
    /// the output does not depend on `options.threads`.
    ///
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness (see
    ///        core::unique_rule; the rule must be made for `finance`).
    /// @param batch_seed Seed of the whole batch.
    /// @return The results, in index order.
    /// @throws std::invalid_argument If the request or options are invalid.
    /// @throws std::runtime_error If no data has been loaded, or a unique
    ///         batch runs out of attempts.
    [[nodiscard]] std::vector<finance> get_finance_batch(const finance_request& request,
                                                  const core::batch_options& options,
                                                  std::uint64_t batch_seed) const
    {
        return core::run_batch(options, batch_seed, [&](std::uint64_t item_seed)
                               { return get_finance(request, item_seed); });
    }

    /// @brief Stream a batch of finance results to a sink without holding the
    ///        whole batch.
    ///
    /// Generates the same items as get_finance_batch() and calls
    /// `sink(index, item)` in index order on the calling thread; `index`
    /// includes `options.first`.
    ///
    /// @tparam Sink Callable taking `(std::uint64_t, finance&)`; for example
    ///         core::jsonl_writer or core::csv_writer.
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness.
    /// @param batch_seed Seed of the whole batch.
    /// @param sink Receives the items.
    template <typename Sink>
    void for_each_finance(const finance_request& request, const core::batch_options& options,
                        std::uint64_t batch_seed, Sink&& sink) const
    {
        core::for_each_batch(options, batch_seed,
                             [&](std::uint64_t item_seed) { return get_finance(request, item_seed); },
                             std::forward<Sink>(sink));
    }

    /// @brief Stream a batch of finance results with a batch seed drawn like
    ///        get_finance() draws its own.
    /// @tparam Sink Callable taking `(std::uint64_t, finance&)`.
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness.
    /// @param sink Receives the items.
    template <typename Sink>
    void for_each_finance(const finance_request& request, const core::batch_options& options,
                        Sink&& sink)
    {
        for_each_finance(request, options, draw_seed_(), std::forward<Sink>(sink));
    }

    // -- Queries ----------------------------------------------------------

    /// @brief Every country and territory in the data.
    /// @return Alpha-2 codes in data order.
    [[nodiscard]] std::vector<std::string> countries() const
    {
        std::vector<std::string> out;
        out.reserve(_model.countries.size());
        for (const auto& c : _model.countries) out.push_back(c.code);
        return out;
    }

    /// @brief The account schemes of a country.
    /// @param country Alpha-2 code, any case.
    /// @return Scheme keys, primary first.
    /// @throws std::invalid_argument If the country is unknown.
    [[nodiscard]] std::vector<std::string> schemes(std::string_view country) const
    {
        std::vector<std::string> out;
        for (const auto index : _model.countries[country_index_(country)].schemes)
        {
            out.push_back(_model.schemes[index].key);
        }
        return out;
    }

    /// @brief Whether a text is a well-formed number of a scheme: its structure,
    ///        every check digit, and a known bank when the scheme's list is
    ///        complete.
    ///
    /// Takes the electronic or the display form. It looks nothing up, and cannot
    /// tell whether an account exists. A scheme's published test numbers count as
    /// well-formed. A safe number made with an unassigned code or a wrong check
    /// does not, which is what those methods are for.
    ///
    /// @param scheme Scheme key.
    /// @param text The number.
    /// @return Whether the text is valid.
    /// @throws std::invalid_argument If the scheme is unknown.
    [[nodiscard]] bool is_valid(std::string_view scheme, std::string_view text) const
    {
        const auto& s = scheme_or_throw_(scheme);
        const std::string plain = fingen_detail::strip_separators(text);
        for (const auto& t : s.tests)
        {
            if (t.number == plain) return true;
        }
        return number_valid_(_model, s, plain, true);
    }

    // -- Data management --------------------------------------------------

    /// @brief Check whether any data has been loaded.
    /// @return True when countries and schemes are loaded.
    [[nodiscard]] bool has_data() const { return !_model.countries.empty() && !_model.schemes.empty(); }

    /// @brief Load data from a resource directory, replacing all data and
    ///        layers loaded before.
    ///
    /// The directory must contain `countries.tsv` and `schemes.tsv`; the other
    /// tables are read when present. A path without them is ignored.
    ///
    /// @param dir Directory such as `resources/lite`.
    /// @throws core::tsv_error If a file is malformed; the data is then
    ///         unchanged.
    void load(const std::filesystem::path& dir)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec) ||
            !std::filesystem::is_regular_file(dir / file_name_(table::countries), ec) ||
            !std::filesystem::is_regular_file(dir / file_name_(table::schemes), ec))
        {
            return;
        }
        apply_(read_directory_(dir), core::layer_mode::replace, true);
    }

    /// @brief Load a dataset tier, replacing all data and layers loaded
    ///        before.
    ///
    /// Uses the compiled-in copy when the library is built with
    /// `DASMIG_FINANCE_EMBED`, otherwise searches `resources/`, `../resources`,
    /// `finance-generator/resources` and `../finance-generator/resources`.
    ///
    /// @param tier The tier.
    /// @return False when the tier was not found.
    /// @throws core::tsv_error If a file is malformed.
    [[nodiscard]] bool load(dataset tier)
    {
        const std::string tier_dir{core::tier_name(tier)};
        const auto marker = tier_dir + "/" + std::string{file_name_(table::countries)};
        const auto root = core::find_resources(library_name_, marker);
        documents docs;
        for (const auto t : all_tables_)
        {
            docs.at(index_(t)) = core::load_resource(tier_dir + "/" + std::string{file_name_(t)}, embedded_files_(), root);
        }
        if (!docs.at(index_(table::countries)) || !docs.at(index_(table::schemes))) return false;
        apply_(std::move(docs), core::layer_mode::replace, true);
        return true;
    }

    /// @brief Add data layers from a directory or a file.
    ///
    /// A directory may hold any of the table files; all are applied together
    /// or, if any fails, none. A file must have one of those names.
    ///
    /// @param path Directory or file.
    /// @param mode How the layers combine with the data loaded before.
    /// @throws core::tsv_error If a file cannot be read or a row is invalid;
    ///         the data is then unchanged.
    /// @throws std::invalid_argument If @p path is a file with another name,
    ///         or the tables do not fit together.
    void extend(const std::filesystem::path& path, core::layer_mode mode = core::layer_mode::override)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
        {
            apply_(read_directory_(path), mode, false);
            return;
        }
        for (const auto t : all_tables_)
        {
            if (path.filename() == file_name_(t))
            {
                (void)extend(t, core::tsv_document::load(path), mode);
                return;
            }
        }
        throw std::invalid_argument("extend: expected one of the finance tables (countries.tsv, schemes.tsv, "
                                    "banks.tsv, ...), got " + path.filename().string());
    }

    /// @brief Add a data layer to one table.
    ///
    /// Rows are keyed by the table's key columns (see doc/data.md). In the
    /// default override mode a row replaces the loaded row with the same key
    /// and new keys are added; a row with `disabled` set to 1 removes its key.
    ///
    /// @param which The table.
    /// @param doc TSV with that table's columns.
    /// @param mode How the layer combines with the data loaded before.
    /// @return What the layer did (rows added, replaced, removed, warnings).
    /// @throws core::tsv_error If a row is invalid; the data is unchanged.
    /// @throws std::invalid_argument If the tables no longer fit together.
    const core::layer_info& extend(finance_table which, core::tsv_document doc,
                                   core::layer_mode mode = core::layer_mode::override)
    {
        documents docs;
        docs.at(index_(which)).emplace(std::move(doc));
        apply_(std::move(docs), mode, false);
        return _tables.at(index_(which)).layers().back();
    }

    /// @brief Add a data layer to one table from TSV text (for example from
    ///        a WebAssembly host).
    /// @param which The table.
    /// @param text TSV with that table's columns.
    /// @param source Name used in errors.
    /// @param mode How the layer combines with the data loaded before.
    /// @return What the layer did.
    /// @throws core::tsv_error If a row is invalid; the data is unchanged.
    const core::layer_info& extend_from_string(finance_table which, std::string_view text, std::string source,
                                               core::layer_mode mode = core::layer_mode::override)
    {
        return extend(which, core::tsv_document::parse(text, std::move(source)), mode);
    }

    /// @brief The layers in effect for one table, in order.
    /// @param which The table.
    /// @return The layer records.
    [[nodiscard]] const std::vector<core::layer_info>& layers(finance_table which) const
    {
        return _tables.at(index_(which)).layers();
    }

    /// @brief Hash of the loaded data, recorded in each result's signature().
    /// @return A hash that changes when any table does; 0 when nothing is
    ///         loaded.
    [[nodiscard]] std::uint64_t data_hash() const
    {
        std::uint64_t h = 0;
        bool any = false;
        for (const auto& t : _tables)
        {
            any = any || !t.layers().empty();
            h = core::hash_combine(h, t.data_hash());
        }
        return any ? h : 0;
    }

  private:
    using table = finance_table;

    // -- Tables -----------------------------------------------------------

    static constexpr std::string_view library_name_ = "finance-generator";
    static constexpr std::size_t table_count_ = 9;
    static constexpr std::array<table, table_count_> all_tables_{
        table::countries,     table::schemes,  table::iban_formats, table::banks,    table::test_accounts,
        table::networks,      table::card_networks, table::test_cards, table::ownership};

    using documents = std::array<std::optional<core::tsv_document>, table_count_>;
    using tables = std::array<core::layered_table, table_count_>;

    static constexpr std::size_t index_(table t) { return static_cast<std::size_t>(t); }

    static constexpr std::string_view file_name_(table t)
    {
        constexpr std::array<std::string_view, table_count_> names{
            "countries.tsv", "schemes.tsv",       "iban_formats.tsv", "banks.tsv",    "test_accounts.tsv",
            "networks.tsv",  "card_networks.tsv", "test_cards.tsv",   "ownership.tsv"};
        return names.at(index_(t));
    }

    static core::table_schema schema_(table t)
    {
        switch (t)
        {
        case table::countries:
            return {.key = {"country_code"},
                    .required = {"population", "currency", "currency_digits", "primary_scheme"},
                    .optional = {"territory"}};
        case table::schemes:
            return {.key = {"key"},
                    .required = {"country", "builder", "display", "safe_method", "evidence"},
                    .optional = {"bank_pattern", "branch_pattern", "pattern", "check", "list_complete", "banks_from",
                                 "params", "source", "min_age"}};
        case table::iban_formats:
            return {.key = {"country_code"},
                    .required = {"bban_pattern", "length"},
                    .optional = {"bank_from", "bank_to", "branch_from", "branch_to", "account_from", "account_to",
                                 "check_from", "check_to", "check_builder", "sepa", "registered"}};
        case table::banks:
            return {.key = {"scheme", "bank_code"},
                    .required = {"name", "weight"},
                    .optional = {"bic", "branch_codes", "method", "everyday_code", "source"}};
        case table::test_accounts:
            return {.key = {"source", "scheme", "number"},
                    .required = {},
                    .optional = {"bank_code", "branch_code", "account_number"}};
        case table::networks:
            return {.key = {"network", "range_from"},
                    .required = {"range_to", "length", "lengths", "weight"},
                    .optional = {"validity_years", "min_age", "format", "source"}};
        case table::card_networks:
            return {.key = {"country_code", "kind", "network"}, .required = {"band", "evidence"}, .optional = {}};
        case table::test_cards:
            return {.key = {"source", "number"},
                    .required = {"network", "kind"},
                    .optional = {"country_code", "expires", "security_code"}};
        case table::ownership:
            return {.key = {"country_code", "group"},
                    .required = {"account", "debit", "credit"},
                    .optional = {"year", "filled"}};
        }
        return {};
    }

    static tables make_tables_()
    {
        return {core::layered_table{schema_(table::countries)},     core::layered_table{schema_(table::schemes)},
                core::layered_table{schema_(table::iban_formats)},  core::layered_table{schema_(table::banks)},
                core::layered_table{schema_(table::test_accounts)}, core::layered_table{schema_(table::networks)},
                core::layered_table{schema_(table::card_networks)}, core::layered_table{schema_(table::test_cards)},
                core::layered_table{schema_(table::ownership)}};
    }

    // -- The model built from the tables ------------------------------------

    struct model
    {
        std::vector<fingen_detail::country_row> countries;
        std::vector<fingen_detail::scheme> schemes;
        std::vector<fingen_detail::iban_format> formats;
        std::vector<fingen_detail::bank_book> books; // one per scheme, by scheme index
        std::unordered_map<std::string, std::uint32_t> country_index;
        std::unordered_map<std::string, std::uint32_t> scheme_index;
        core::weighted_table<std::uint32_t> drawn_countries; // by population, without territories
        std::vector<std::uint32_t> drawable_countries;       // the same, for unweighted draws
    };

    // Cells of optional columns, which a layer may leave out.
    static std::string_view cell_(const core::tsv_row& row, std::string_view column)
    {
        return row.has(column) ? row[column] : std::string_view{};
    }

    static int whole_(const core::tsv_row& row, std::string_view column, std::string_view text)
    {
        int v = 0;
        const auto* end = text.data() + text.size();
        const auto res = std::from_chars(text.data(), end, v);
        if (res.ec != std::errc{} || res.ptr != end || v < 0)
        {
            throw row.error(column, "expected a whole number, got '" + std::string{text} + "'");
        }
        return v;
    }

    static int position_(const core::tsv_row& row, std::string_view column)
    {
        const auto text = cell_(row, column);
        return text.empty() ? -1 : whole_(row, column, text);
    }

    static core::pattern pattern_(const core::tsv_row& row, std::string_view column)
    {
        try
        {
            return core::pattern::parse(cell_(row, column));
        }
        catch (const std::invalid_argument& e)
        {
            throw row.error(column, e.what());
        }
    }

    static fingen_detail::scheme parse_scheme_(const core::tsv_row& row)
    {
        using namespace fingen_detail;
        scheme s;
        s.key = std::string{row["key"]};
        s.country = upper(row["country"]);
        const auto builder = parse_builder(row["builder"]);
        if (!builder) throw row.error("builder", "unknown builder '" + std::string{row["builder"]} + "'");
        s.builder = *builder;
        auto tokens = parse_tokens(row["display"]);
        if (!tokens) throw row.error("display", "unreadable display '" + std::string{row["display"]} + "'");
        s.display = std::move(*tokens);
        const auto safe = parse_safe(row["safe_method"]);
        if (!safe) throw row.error("safe_method", "unknown method '" + std::string{row["safe_method"]} + "'");
        s.safe = *safe;
        s.evidence = std::string{row["evidence"]};
        s.bank_pattern = pattern_(row, "bank_pattern");
        s.branch_pattern = pattern_(row, "branch_pattern");
        s.account_pattern = pattern_(row, "pattern");
        // A national form's check is named after its builder; an IBAN's is one of the BBAN checks.
        if (!parse_check(cell_(row, "check")) && cell_(row, "check") != row["builder"])
        {
            throw row.error("check", "unknown check '" + std::string{cell_(row, "check")} + "'");
        }
        const auto params = cell_(row, "params");
        if (params.starts_with("suffix="))
        {
            try
            {
                s.suffix_pattern = core::pattern::parse(params.substr(7));
            }
            catch (const std::invalid_argument& e)
            {
                throw row.error("params", e.what());
            }
        }
        else if (!params.empty())
        {
            throw row.error("params", "unknown parameter '" + std::string{params} + "'");
        }
        s.list_complete = cell_(row, "list_complete") == "1";
        const auto age = cell_(row, "min_age");
        if (!age.empty()) s.min_age = whole_(row, "min_age", age);
        return s;
    }

    static fingen_detail::iban_format parse_format_(const core::tsv_row& row)
    {
        using namespace fingen_detail;
        iban_format f;
        f.country = upper(row["country_code"]);
        f.pattern = pattern_(row, "bban_pattern");
        std::string digits_only{row["bban_pattern"]};
        std::ranges::replace(digits_only, '*', '#');
        f.drawn = core::pattern::parse(digits_only);
        f.length = static_cast<std::size_t>(whole_(row, "length", row["length"]));
        const auto [low, high] = f.pattern.lengths();
        if (low != high || low + 4 != f.length)
        {
            throw row.error("bban_pattern", "gives " + std::to_string(low) + " to " + std::to_string(high) +
                                                " characters, which does not fit a length of " + std::to_string(f.length));
        }
        f.bank_from = position_(row, "bank_from");
        f.bank_to = position_(row, "bank_to");
        f.branch_from = position_(row, "branch_from");
        f.branch_to = position_(row, "branch_to");
        f.account_from = position_(row, "account_from");
        f.account_to = position_(row, "account_to");
        f.check_from = position_(row, "check_from");
        f.check_to = position_(row, "check_to");
        const auto require_span = [&row, size = low](int from, int to, std::string_view name)
        {
            if (from >= 0 && (to <= from || static_cast<std::size_t>(to) > size))
            {
                throw row.error(name, "the positions are outside the BBAN");
            }
        };
        require_span(f.bank_from, f.bank_to, "bank_from");
        require_span(f.branch_from, f.branch_to, "branch_from");
        require_span(f.account_from, f.account_to, "account_from");
        require_span(f.check_from, f.check_to, "check_from");
        const auto check = parse_check(cell_(row, "check_builder"));
        if (!check) throw row.error("check_builder", "unknown check '" + std::string{cell_(row, "check_builder")} + "'");
        f.check = *check;
        if (low < check_min_length(f.check)) throw row.error("check_builder", "the BBAN is too short for this check");
        if (check_is_positional(f.check) && f.check_from < 0)
        {
            throw row.error("check_from", "this check needs a position");
        }
        return f;
    }

    static fingen_detail::bank_row parse_bank_(const core::tsv_row& row)
    {
        using namespace fingen_detail;
        bank_row b;
        b.code = std::string{row["bank_code"]};
        b.name = std::string{row["name"]};
        if (b.name.empty()) throw row.error("name", "must not be empty");
        b.weight = row.get<double>("weight");
        if (!(b.weight >= 0.0)) throw row.error("weight", "must not be negative");
        b.bic = std::string{cell_(row, "bic")};
        if (!b.bic.empty() && b.bic.size() != 8 && b.bic.size() != 11) throw row.error("bic", "a BIC has 8 or 11 characters");
        b.everyday = std::string{cell_(row, "everyday_code")};
        const auto method = cell_(row, "method");
        if (!method.empty())
        {
            b.method = de_method_number(method);
            if (b.weight > 0.0 && !de_method_known(b.method))
            {
                throw row.error("method", "method '" + std::string{method} + "' is not implemented; a bank with one must have weight 0");
            }
        }
        const auto branches = cell_(row, "branch_codes");
        if (!branches.empty())
        {
            std::size_t at = 0;
            while (at <= branches.size())
            {
                const auto comma = branches.find(',', at);
                const auto end = comma == std::string_view::npos ? branches.size() : comma;
                b.branches.emplace_back(branches.substr(at, end - at));
                at = end + 1;
            }
        }
        return b;
    }

    // Row checks that need no other table; a bad layer stops here, with its line.
    static void validate_(table t, const core::tsv_row& row)
    {
        switch (t)
        {
        case table::countries:
        {
            const auto currency = row["currency"];
            if (currency.size() != 3) throw row.error("currency", "expected an ISO 4217 code");
            if (!(row.get<double>("population") >= 0.0)) throw row.error("population", "must not be negative");
            (void)whole_(row, "currency_digits", row["currency_digits"]);
            if (row["country_code"].size() != 2) throw row.error("country_code", "expected an alpha-2 code");
            break;
        }
        case table::schemes: (void)parse_scheme_(row); break;
        case table::iban_formats: (void)parse_format_(row); break;
        case table::banks: (void)parse_bank_(row); break;
        case table::test_accounts: break;
        case table::networks:
        {
            const auto from = row["range_from"];
            const auto to = row["range_to"];
            if (from.empty() || from.size() != to.size() || !fingen_detail::all_digits(from) || !fingen_detail::all_digits(to) || from > to)
            {
                throw row.error("range_to", "the range does not parse");
            }
            const int length = whole_(row, "length", row["length"]);
            if (length < 12 || length > 19) throw row.error("length", "a card number has 12 to 19 digits");
            break;
        }
        case table::card_networks:
        {
            const auto band = row["band"];
            if (band != "main" && band != "common" && band != "rare") throw row.error("band", "expected main, common or rare");
            break;
        }
        case table::test_cards:
            if (!core::luhn_valid(row["number"])) throw row.error("number", "fails Luhn");
            break;
        case table::ownership:
            for (const auto column : {"account", "debit", "credit"})
            {
                const auto share = row.get<double>(column);
                if (!(share >= 0.0 && share <= 1.0)) throw row.error(column, "a share is from 0 to 1");
            }
            break;
        }
    }

    // Whether a text in electronic or display form is a number of the scheme.
    [[nodiscard]] static bool number_valid_(const model& m, const fingen_detail::scheme& s, std::string_view plain, bool check_list)
    {
        using namespace fingen_detail;
        if (!is_iban_builder(s.builder))
        {
            const auto p = parse_display(s, plain);
            return p && national_valid(s.builder, *p);
        }
        const auto& f = m.formats[static_cast<std::size_t>(s.format)];
        if (!plain.starts_with(s.country))
        {
            if (s.builder == builder_kind::iban) return false;
            // Brazil's and Russia's everyday forms. Only Russia's has a check to read.
            const auto p = parse_display(s, plain);
            if (!p) return false;
            if (s.builder == builder_kind::iban_br) return true;
            const std::string bban = p->bank + p->account;
            return check_holds(f.check, bban, f, -1);
        }
        if (plain.size() != f.length || !core::iban_valid(plain)) return false;
        const std::string_view bban = plain.substr(4);
        if (!f.pattern.matches(bban)) return false;
        int method = -1;
        if (f.bank_from >= 0)
        {
            const auto& book = m.books[s.book];
            const auto found = book.index.find(std::string{
                bban.substr(static_cast<std::size_t>(f.bank_from), static_cast<std::size_t>(f.bank_to - f.bank_from))});
            if (found != book.index.end()) method = book.rows[found->second].method;
            else if (check_list && s.list_complete) return false;
        }
        return check_holds(f.check, bban, f, method);
    }

    static model build_model_(const tables& t)
    {
        using namespace fingen_detail;
        model m;

        std::unordered_map<std::string, std::int32_t> format_index;
        for (const auto& row : t.at(index_(table::iban_formats)).rows())
        {
            auto f = parse_format_(row);
            format_index[f.country] = static_cast<std::int32_t>(m.formats.size());
            m.formats.push_back(std::move(f));
        }

        const auto& scheme_rows = t.at(index_(table::schemes)).rows();
        m.schemes.reserve(scheme_rows.size());
        for (const auto& row : scheme_rows)
        {
            auto s = parse_scheme_(row);
            if (is_iban_builder(s.builder))
            {
                const auto found = format_index.find(s.country);
                if (found == format_index.end()) throw std::invalid_argument("scheme " + s.key + " has no IBAN format");
                s.format = found->second;
            }
            else if (s.builder == builder_kind::us_aba && s.bank_pattern.lengths() != std::pair<std::size_t, std::size_t>{9, 9})
            {
                throw std::invalid_argument("scheme " + s.key + ": a routing number has 9 digits");
            }
            m.scheme_index[s.key] = static_cast<std::uint32_t>(m.schemes.size());
            m.schemes.push_back(std::move(s));
        }
        m.books.resize(m.schemes.size());

        std::uint32_t at = 0;
        for (const auto& row : scheme_rows)
        {
            auto& s = m.schemes[at];
            s.book = at;
            const auto from = cell_(row, "banks_from");
            if (!from.empty())
            {
                const auto found = m.scheme_index.find(std::string{from});
                if (found == m.scheme_index.end()) throw std::invalid_argument("scheme " + s.key + " takes banks from unknown scheme " + std::string{from});
                s.book = found->second;
            }
            ++at;
        }

        for (const auto& row : t.at(index_(table::banks)).rows())
        {
            const auto found = m.scheme_index.find(std::string{row["scheme"]});
            if (found == m.scheme_index.end()) throw std::invalid_argument("banks: unknown scheme " + std::string{row["scheme"]});
            const auto& s = m.schemes[found->second];
            auto b = parse_bank_(row);
            if (is_iban_builder(s.builder))
            {
                const auto& f = m.formats[static_cast<std::size_t>(s.format)];
                if (f.bank_from < 0 || b.code.size() != static_cast<std::size_t>(f.bank_to - f.bank_from))
                {
                    throw std::invalid_argument("banks: " + b.code + " does not fit the bank position of " + s.key);
                }
            }
            else if (s.bank_pattern.lengths().second > 0 && !s.bank_pattern.matches(b.code))
            {
                throw std::invalid_argument("banks: " + b.code + " does not fit the bank pattern of " + s.key);
            }
            auto& book = m.books[found->second];
            const auto number = static_cast<std::uint32_t>(book.rows.size());
            book.index[b.code] = number;
            if (b.weight > 0.0)
            {
                book.weights.add(number, b.weight);
                book.drawable.push_back(number);
            }
            book.rows.push_back(std::move(b));
        }

        for (const auto& row : t.at(index_(table::test_accounts)).rows())
        {
            const auto found = m.scheme_index.find(std::string{row["scheme"]});
            if (found == m.scheme_index.end()) throw std::invalid_argument("test_accounts: unknown scheme " + std::string{row["scheme"]});
            auto& s = m.schemes[found->second];
            test_account a;
            a.shown = std::string{row["number"]};
            a.number = strip_separators(a.shown);
            a.fields.bank = std::string{cell_(row, "bank_code")};
            a.fields.branch = std::string{cell_(row, "branch_code")};
            a.fields.account = std::string{cell_(row, "account_number")};
            if (!number_valid_(m, s, a.number, false)) throw std::invalid_argument("test_accounts: " + a.shown + " fails the checks of " + s.key);
            s.tests.push_back(std::move(a));
        }

        for (const auto& s : m.schemes)
        {
            const bool needs_bank = s.builder == builder_kind::iban_br || s.builder == builder_kind::iban_ru;
            if (needs_bank && m.books[s.book].drawable.empty())
            {
                throw std::invalid_argument("scheme " + s.key + " builds on a bank's code and has no bank to draw");
            }
            const bool has_check = is_iban_builder(s.builder) || s.builder == builder_kind::us_aba ||
                                   s.builder == builder_kind::ng_nuban || s.builder == builder_kind::mx_clabe ||
                                   s.builder == builder_kind::ar_cbu || s.builder == builder_kind::nz_account;
            if (!has_check && (s.safe == safe_method::wrong_outer || s.safe == safe_method::wrong_national))
            {
                throw std::invalid_argument("scheme " + s.key + " has no check digit to get wrong");
            }
            if (s.safe == safe_method::test_value && s.tests.empty())
            {
                throw std::invalid_argument("scheme " + s.key + " uses test values but has none");
            }
            if (s.safe == safe_method::unassigned_code && !s.list_complete)
            {
                throw std::invalid_argument("scheme " + s.key + " draws an unassigned code from a list that is not complete");
            }
        }

        const auto& country_rows = t.at(index_(table::countries)).rows();
        m.countries.reserve(country_rows.size());
        for (const auto& row : country_rows)
        {
            country_row c;
            c.code = upper(row["country_code"]);
            c.currency = std::string{row["currency"]};
            c.currency_digits = static_cast<std::uint8_t>(whole_(row, "currency_digits", row["currency_digits"]));
            c.population = row.get<double>("population");
            c.territory = !cell_(row, "territory").empty();
            const auto primary = m.scheme_index.find(std::string{row["primary_scheme"]});
            if (primary == m.scheme_index.end())
            {
                throw std::invalid_argument("country " + c.code + " names unknown scheme " + std::string{row["primary_scheme"]});
            }
            c.primary = primary->second;
            c.schemes.push_back(c.primary);
            const auto number = static_cast<std::uint32_t>(m.countries.size());
            m.country_index[c.code] = number;
            if (!c.territory)
            {
                m.drawn_countries.add(number, c.population);
                m.drawable_countries.push_back(number);
            }
            m.countries.push_back(std::move(c));
        }
        for (std::uint32_t i = 0; i < m.schemes.size(); ++i)
        {
            const auto found = m.country_index.find(m.schemes[i].country);
            if (found == m.country_index.end())
            {
                throw std::invalid_argument("scheme " + m.schemes[i].key + " names unknown country " + m.schemes[i].country);
            }
            auto& list = m.countries[found->second].schemes;
            if (list.front() != i) list.push_back(i);
        }
        return m;
    }

    static documents read_directory_(const std::filesystem::path& dir)
    {
        documents docs;
        for (const auto t : all_tables_)
        {
            const auto file = dir / file_name_(t);
            std::error_code ec;
            if (std::filesystem::is_regular_file(file, ec)) docs.at(index_(t)) = core::tsv_document::load(file);
        }
        return docs;
    }

    void apply_(documents docs, core::layer_mode mode, bool reset)
    {
        tables next = _tables;
        for (const auto t : all_tables_)
        {
            auto& doc = docs.at(index_(t));
            auto& tbl = next.at(index_(t));
            if (!doc)
            {
                if (reset) tbl.clear();
                continue;
            }
            (void)tbl.extend(std::move(*doc), mode, [t](const core::tsv_row& row) { validate_(t, row); });
        }
        auto built = build_model_(next);
        _tables = std::move(next);
        _model = std::move(built);
    }

    // -- Requests ------------------------------------------------------------

    [[nodiscard]] std::uint32_t country_index_(std::string_view code) const
    {
        const auto it = _model.country_index.find(fingen_detail::upper(code));
        if (it == _model.country_index.end()) throw std::invalid_argument("unknown country '" + std::string{code} + "'");
        return it->second;
    }

    [[nodiscard]] const fingen_detail::scheme& scheme_or_throw_(std::string_view key) const
    {
        const auto it = _model.scheme_index.find(std::string{key});
        if (it == _model.scheme_index.end()) throw std::invalid_argument("unknown scheme '" + std::string{key} + "'");
        return _model.schemes[it->second];
    }

    static fingen_detail::day_t reference_of_(const finance_request& request)
    {
        using namespace fingen_detail;
        if (!request.reference_date.ok()) throw std::invalid_argument("invalid reference date");
        const day_t reference = to_day(request.reference_date);
        if (reference < make_day(1900, 1, 1) || reference > make_day(2090, 12, 31))
        {
            throw std::invalid_argument("the reference date must be from 1900-01-01 to 2090-12-31");
        }
        return reference;
    }

    static fingen_detail::day_t birth_of_(const finance_request& request, fingen_detail::day_t reference, const core::rng& call)
    {
        using namespace fingen_detail;
        const day_t earliest = make_day(1900, 1, 1);
        if (request.birth_date)
        {
            if (!request.birth_date->ok()) throw std::invalid_argument("invalid birth date");
            const day_t birth = to_day(*request.birth_date);
            if (birth > reference) throw std::invalid_argument("the birth date is after the reference date");
            if (birth < earliest) throw std::invalid_argument("the birth date is before 1900");
            return birth;
        }
        // Ages 18 to 80, flat: anniversaries count back from the reference date.
        const day_t hi = std::max(earliest, anniversary(reference, -18));
        const day_t lo = std::max(earliest, anniversary(reference, -81) + 1);
        auto r = call.split("birth_date");
        return core::uniform_int(r, lo, std::max(lo, hi));
    }

    [[nodiscard]] std::uint32_t country_of_(const finance_request& request, const core::rng& call) const
    {
        if (request.country) return country_index_(*request.country);
        if (_model.drawable_countries.empty()) throw std::invalid_argument("no country can be drawn");
        auto r = call.split("country");
        if (request.weighted) return _model.drawn_countries.pick(r);
        return _model.drawable_countries[static_cast<std::size_t>(core::bounded(r, _model.drawable_countries.size()))];
    }

    [[nodiscard]] static const fingen_detail::bank_row* pick_bank_(const fingen_detail::bank_book& book, core::rng& r, bool weighted)
    {
        if (book.drawable.empty()) return nullptr;
        if (weighted) return &book.rows[book.weights.pick(r)];
        return &book.rows[book.drawable[static_cast<std::size_t>(core::bounded(r, book.drawable.size()))]];
    }

    static std::string slice_(std::string_view text, int from, int to)
    {
        if (from < 0) return {};
        return std::string{text.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from))};
    }

    // A bank's BIC with a 0 as the second location character: SWIFT's test convention.
    static std::string test_bic_(std::string bic)
    {
        if (bic.size() >= 8) bic[7] = '0';
        return bic;
    }

    // Iceland's account carries the holder's kennitala: date, two digits, check, century.
    static void write_holder_id_(std::string& bban, fingen_detail::day_t birth)
    {
        using namespace fingen_detail;
        const auto v = ymd_of(birth);
        const int year = static_cast<int>(v.year());
        const std::string date = pad_number(static_cast<unsigned>(v.day()), 2) +
                                 pad_number(static_cast<unsigned>(v.month()), 2) +
                                 pad_number(static_cast<unsigned>(year % 100), 2);
        bban.replace(12, 6, date);
        bban[21] = year >= 2000 ? '0' : '9';
    }

    // One draw of a BBAN, its national check not yet filled.
    [[nodiscard]] static std::string draw_bban_(const fingen_detail::scheme& s, const fingen_detail::iban_format& f,
                                                const fingen_detail::bank_row* bank, account_kind kind,
                                                fingen_detail::day_t birth, fingen_detail::parts& p, core::rng& r)
    {
        using namespace fingen_detail;
        if (s.builder == builder_kind::iban_br)
        {
            // ISPB, a 0 and the branch, the account padded to 10, its type and the holder's position.
            p.branch = s.branch_pattern.draw(r);
            p.account = s.account_pattern.draw(r);
            std::string padded = p.account;
            padded.insert(0, 10 - padded.size(), '0');
            return bank->code + "0" + p.branch + padded + (kind == account_kind::savings ? "P1" : "C1");
        }
        if (s.builder == builder_kind::iban_ru) return bank->code + s.account_pattern.draw(r);
        std::string bban = f.drawn.draw(r);
        if (bank != nullptr) bban.replace(static_cast<std::size_t>(f.bank_from), bank->code.size(), bank->code);
        if (bank != nullptr && !bank->branches.empty() && f.branch_from >= 0)
        {
            const auto at = static_cast<std::size_t>(core::bounded(r, bank->branches.size()));
            bban.replace(static_cast<std::size_t>(f.branch_from), bank->branches[at].size(), bank->branches[at]);
        }
        if (f.check == check_kind::is) write_holder_id_(bban, birth);
        if (f.check == check_kind::no && bban[4] == '0' && bban[5] == '0') bban[4] = '1'; // see check_holds
        return bban;
    }

    // Writes what an IBAN's electronic form breaks into onto the account.
    static void set_iban_fields_(bank_account& out, const fingen_detail::scheme& s, const fingen_detail::iban_format& f,
                                 const fingen_detail::bank_row* bank, const fingen_detail::parts& p)
    {
        using namespace fingen_detail;
        const std::string_view bban = std::string_view{out.iban}.substr(4);
        if (s.builder == builder_kind::iban_br)
        {
            out.bank_code = bank->everyday.empty() ? bank->code : bank->everyday;
            out.branch_code = p.branch;
            out.account_number = p.account;
            return;
        }
        if (s.builder == builder_kind::iban_ru)
        {
            out.bank_code = slice_(bban, f.bank_from, f.bank_to);
            out.account_number = std::string{bban.substr(9)};
            return;
        }
        out.bank_code = slice_(bban, f.bank_from, f.bank_to);
        out.branch_code = slice_(bban, f.branch_from, f.branch_to);
        const int after = std::max({0, f.bank_to, f.branch_to});
        out.account_number = std::string{bban.substr(static_cast<std::size_t>(after))};
    }

    void fill_iban_(bank_account& out, const fingen_detail::scheme& s, account_kind kind, const finance_request& request,
                    fingen_detail::day_t birth, core::rng& bank_r, core::rng& number_r) const
    {
        using namespace fingen_detail;
        const auto& f = _model.formats[static_cast<std::size_t>(s.format)];
        const auto& book = _model.books[s.book];
        const bool unassigned = request.safe && s.safe == safe_method::unassigned_code;
        const bank_row* bank = unassigned ? nullptr : pick_bank_(book, bank_r, request.weighted);
        const int method = bank != nullptr ? bank->method : -1;

        parts p;
        std::string bban;
        bool made = false;
        for (int attempt = 0; attempt < max_redraws_ && !made; ++attempt)
        {
            bban = draw_bban_(s, f, bank, kind, birth, p, number_r);
            // After the check: Poland's sits inside the bank code.
            made = fill_check(f.check, bban, f, method) && !(unassigned && book.index.contains(slice_(bban, f.bank_from, f.bank_to)));
        }
        if (!made) throw std::runtime_error("could not draw a number for scheme " + s.key);

        bool wrong_outer = request.safe && s.safe == safe_method::wrong_outer;
        if (request.safe && s.safe == safe_method::wrong_national) wrong_outer = !spoil_check(f.check, bban, f, method, number_r);

        auto digits = core::iban_check_digits(s.country, bban);
        if (wrong_outer)
        {
            // Over 02 to 98, whose values are distinct modulo 97, so it can only fail.
            const unsigned right = (static_cast<unsigned>(digits[0] - '0') * 10U) + static_cast<unsigned>(digits[1] - '0');
            const unsigned step = static_cast<unsigned>(core::bounded(number_r, 95)) + 1U;
            const unsigned wrong = 2U + ((right - 2U + step) % 97U);
            digits = {static_cast<char>('0' + (wrong / 10U)), static_cast<char>('0' + (wrong % 10U))};
        }
        out.iban = s.country;
        out.iban.push_back(digits[0]);
        out.iban.push_back(digits[1]);
        out.iban += bban;

        set_iban_fields_(out, s, f, bank, p);
        p.iban = out.iban;
        p.bank = out.bank_code;
        p.account = out.account_number;
        // Brazil's everyday form has no check to carry the method, so a safe number shows its IBAN.
        out.formatted = request.safe && s.builder == builder_kind::iban_br ? group_in_fours(out.iban) : apply_display(s.display, p);
        if (bank != nullptr)
        {
            out.bic = request.safe ? test_bic_(bank->bic) : bank->bic;
            if (!request.safe) out.bank_name = bank->name;
        }
    }

    void fill_national_(bank_account& out, const fingen_detail::scheme& s, const finance_request& request,
                        core::rng& bank_r, core::rng& number_r) const
    {
        using namespace fingen_detail;
        const bank_row* bank = pick_bank_(_model.books[s.book], bank_r, request.weighted);
        parts p;
        bool made = false;
        for (int attempt = 0; attempt < max_redraws_ && !made; ++attempt)
        {
            p.bank = bank != nullptr ? bank->code : s.bank_pattern.draw(number_r);
            if (bank != nullptr && !bank->branches.empty())
            {
                p.branch = bank->branches[static_cast<std::size_t>(core::bounded(number_r, bank->branches.size()))];
            }
            else
            {
                p.branch = s.branch_pattern.draw(number_r);
            }
            p.account = s.account_pattern.draw(number_r);
            p.suffix = s.suffix_pattern.draw(number_r);
            made = national_fill(s.builder, p, bank != nullptr);
        }
        if (!made) throw std::runtime_error("could not draw a number for scheme " + s.key);
        if (request.safe && (s.safe == safe_method::wrong_outer || s.safe == safe_method::wrong_national))
        {
            national_spoil(s.builder, p, number_r);
        }
        out.bank_code = p.bank;
        out.branch_code = p.branch;
        out.account_number = p.account + p.check + p.check2 + p.suffix;
        out.formatted = apply_display(s.display, p);
        if (bank != nullptr)
        {
            out.bic = request.safe ? test_bic_(bank->bic) : bank->bic;
            if (!request.safe) out.bank_name = bank->name;
        }
    }

    void fill_test_value_(bank_account& out, const fingen_detail::scheme& s, core::rng& number_r) const
    {
        using namespace fingen_detail;
        const auto& t = s.tests[static_cast<std::size_t>(core::bounded(number_r, s.tests.size()))];
        if (is_iban_builder(s.builder))
        {
            const auto& f = _model.formats[static_cast<std::size_t>(s.format)];
            out.iban = t.number;
            set_iban_fields_(out, s, f, nullptr, t.fields);
            out.formatted = group_in_fours(out.iban);
        }
        else
        {
            out.bank_code = t.fields.bank;
            out.branch_code = t.fields.branch;
            out.account_number = t.fields.account;
            out.formatted = t.shown;
        }
        const auto& book = _model.books[s.book];
        const auto found = book.index.find(out.bank_code);
        if (found != book.index.end()) out.bic = test_bic_(book.rows[found->second].bic);
    }

    [[nodiscard]] std::optional<bank_account> make_account_(account_kind kind, std::size_t position,
                                                            const fingen_detail::country_row& country,
                                                            const finance_request& request, fingen_detail::day_t birth,
                                                            fingen_detail::day_t reference, const core::rng& call) const
    {
        using namespace fingen_detail;
        const auto& s = _model.schemes[country.primary];
        const day_t earliest = anniversary(birth, s.min_age);
        if (earliest > reference) return std::nullopt;

        const core::rng stream = call.split("account").split(static_cast<std::uint64_t>(position));
        auto bank_r = stream.split("bank");
        auto number_r = stream.split("number");
        auto opened_r = stream.split("opened");

        bank_account out;
        out.scheme = s.key;
        out.kind = kind;
        out.currency = country.currency;
        out.currency_digits = country.currency_digits;
        out.evidence = s.evidence;
        out.reserved = request.safe;
        out.opened = ymd_of(core::uniform_int(opened_r, earliest, reference));
        if (request.safe && s.safe == safe_method::test_value) fill_test_value_(out, s, number_r);
        else if (is_iban_builder(s.builder)) fill_iban_(out, s, kind, request, birth, bank_r, number_r);
        else fill_national_(out, s, request, bank_r, number_r);
        return out;
    }

    // -- State ------------------------------------------------------------

    static constexpr int max_redraws_ = 64;

    tables _tables{make_tables_()};
    model _model;
    core::rng _engine{core::rng::from_entropy()};

    struct auto_probe_tag {};

    explicit fng(auto_probe_tag /*tag*/)
    {
        if (!load(dataset::lite)) (void)load(dataset::full);
    }

    // -- Helpers ----------------------------------------------------------

    static std::span<const core::embedded_file> embedded_files_()
    {
#ifdef DASMIG_FINANCE_EMBED
        return embedded::fingen::files;
#else
        return {};
#endif
    }

    std::uint64_t draw_seed_() { return _engine(); }

    void require_data_() const
    {
        if (!has_data())
        {
            throw std::runtime_error("No finance data loaded. Call load() first.");
        }
    }
};

} // namespace dasmig

#endif // DASMIG_FINGEN_HPP
