#pragma once
// From the sc-fsc module (modules/sc-fsc 1.0.0, commit 8abb0c7), folded into sc-pglib, its only user, on 2026-11-17.
// Financial calculations for an insurer's capital and reserving, as regulatory reporting needs them.
//
// Expected cash flows are valued by discounting each at its risk-free spot rate plus a risk premium and
// a spread, compounded daily; the risk premium that makes that value a given market value is solved
// for, by Newton-Raphson with the derivative exact (dual numbers, sc-core's dual.h).
//
//     const auto flows = sc::fsc::cash_flows({182, 365}, {50.0, 1050.0}, {0.0812, 0.0835});
//     const double value = sc::fsc::expected_market_value(flows, 0.012, sc::fsc::basis_points{25});
//     const auto found = sc::fsc::solve_risk_premium(flows, 1015.0, sc::fsc::basis_points{25});
//     found.risk_premium;    // the premium (a fraction a year) at which the flows are worth 1015
//
// Rates are nominal annual rates as fractions (0.085 is 8.5%), compounded daily over 365.25 days a year:
// a flow of amount A in d days is worth A (1 + (spot + premium + spread) / 365.25)^-d today.

#include <sc/core/dual.h>

#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace sc::fsc {
    // Days in a year, for daily compounding
    inline constexpr double days_a_year = 365.25;

    // A spread in basis points: 150 is 1.5% (0.015) a year
    struct basis_points {
        double value = 0;

        [[nodiscard]] constexpr double fraction() const { return value / 10'000; }
    };

    // An expected cash flow: when (days after the valuation date), how much, and the risk-free spot rate
    // for that term (a nominal annual rate as a fraction)
    struct cash_flow {
        int days = 0;
        double amount = 0;
        double spot_rate = 0;
    };

    // Cash flows from three lists of the same length, as a table's columns or a database's arrays give them.
    // Throws std::invalid_argument when the lengths differ or a value isn't finite.
    std::vector<cash_flow> cash_flows(std::span<const int> days, std::span<const double> amounts, std::span<const double> spot_rates);
    std::vector<cash_flow> cash_flows(std::initializer_list<int> days, std::initializer_list<double> amounts,
                                      std::initializer_list<double> spot_rates);

    // The cash flows' value today at a risk premium (a fraction a year) and a spread: the sum of each amount
    // discounted at (1 + (spot + premium + spread) / 365.25)^-days. No flows are worth 0.
    // Throws std::invalid_argument for a value that isn't finite, or a rate that leaves nothing to discount by
    // (1 + rate / 365.25 <= 0).
    [[nodiscard]] double expected_market_value(std::span<const cash_flow> flows, double risk_premium, basis_points spread);

    // The same, with its derivative to the risk premium: given sc::dual<double>{premium, 1}, its real part is the
    // value and its eps part how fast the value changes with the premium
    [[nodiscard]] dual<double> expected_market_value(std::span<const cash_flow> flows, const dual<double> &risk_premium, basis_points spread);

    struct solve_options {
        int max_iterations = 1000;
        // Done when the value is this close to the market value
        double tolerance = 1e-8;
        // The first guess at the premium
        double start = 0;
    };

    struct solution {
        double risk_premium = 0; // a fraction a year
        int iterations = 0;      // Newton steps taken
        double residual = 0;     // the value at that premium less the market value
    };

    // When the premium can't be found: Newton's steps didn't get within the tolerance, the value stopped
    // changing with the premium, or a step left the rates where nothing can be discounted
    class no_solution : public std::runtime_error {
    public:
        no_solution(const std::string &why, const double last_premium, const int iterations, const double residual)
            : std::runtime_error(why), last_premium(last_premium), iterations(iterations), residual(residual) {}

        double last_premium; // the last guess
        int iterations;
        double residual;
    };

    // The risk premium at which the cash flows are worth market_value, by Newton-Raphson from options.start.
    // Throws std::invalid_argument for inputs that aren't finite or options out of range, and no_solution
    // when it can't find one.
    [[nodiscard]] solution solve_risk_premium(std::span<const cash_flow> flows, double market_value, basis_points spread,
                                              const solve_options &options = {});
}
