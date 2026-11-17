// Cash flows valued at a risk premium (against values computed by hand), the derivative against finite
// differences, the risk premium solved back from a value, and every input refused that should be.
#include "fsc/fsc.h"

#include "sc_test.h"

#include <cmath>
#include <limits>

namespace {
    using sc::fsc::basis_points;

    // The textbook formula, flow by flow, to check against
    double by_hand(const std::vector<sc::fsc::cash_flow> &flows, const double premium, const double spread_bp) {
        double total = 0;
        for (const auto &flow: flows) total += flow.amount * std::pow(1 + (flow.spot_rate + premium + spread_bp / 10'000) / 365.25, -flow.days);
        return total;
    }

    template<typename Call>
    bool refused(Call call) {
        try {
            call();
        } catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    }
}

int main() {
    SECTION("Values: one flow and several, against values computed elsewhere");
    {
        // 1000 in a year at 8% + 1% + 50bp: 1000 (1 + 0.095 / 365.25)^-365
        const auto one = sc::fsc::cash_flows({365}, {1000.0}, {0.08});
        CHECK_NEAR(sc::fsc::expected_market_value(one, 0.01, basis_points{50}), 909.4432934744606, 1e-9);

        const auto two = sc::fsc::cash_flows({182, 365}, {50.0, 1050.0}, {0.0812, 0.0835});
        CHECK_NEAR(sc::fsc::expected_market_value(two, 0.012, basis_points{25}), 999.7296555481444, 1e-9);
        CHECK_NEAR(sc::fsc::expected_market_value(two, 0.012, basis_points{25}), by_hand(two, 0.012, 25), 1e-9);

        // A flow today is worth itself; no flows are worth nothing
        CHECK_NEAR(sc::fsc::expected_market_value(sc::fsc::cash_flows({0}, {123.0}, {0.09}), 0.5, basis_points{}), 123.0, 1e-12);
        CHECK_EQ(sc::fsc::expected_market_value({}, 0.01, basis_points{}), 0.0);
        CHECK_EQ(basis_points{150}.fraction(), 0.015);
    }

    SECTION("The derivative to the premium, against finite differences");
    {
        std::vector<sc::fsc::cash_flow> flows;
        for (int year = 1; year <= 30; ++year) flows.push_back({static_cast<int>(year * 365.25), year == 30 ? 1100.0 : 100.0, 0.07 + year * 0.001});
        const double premium = 0.015;
        const auto exact = sc::fsc::expected_market_value(flows, sc::dual<double>{premium, 1}, basis_points{40});
        const double h = 1e-6;
        const double numeric = (sc::fsc::expected_market_value(flows, premium + h, basis_points{40}) -
                                sc::fsc::expected_market_value(flows, premium - h, basis_points{40})) / (2 * h);
        CHECK_NEAR(exact.real, by_hand(flows, premium, 40), 1e-8);
        CHECK_NEAR(exact.eps, numeric, std::fabs(numeric) * 1e-6);
        CHECK_LT(exact.eps, 0.0); // a higher premium is worth less
    }

    SECTION("The premium solved back from a value");
    {
        const auto flows = sc::fsc::cash_flows({91, 182, 273, 365, 730}, {30.0, 30.0, 30.0, 30.0, 1030.0}, {0.075, 0.077, 0.079, 0.08, 0.083});
        for (const double premium: {-0.02, 0.0, 0.0123, 0.05, 0.25}) {
            const double value = sc::fsc::expected_market_value(flows, premium, basis_points{35});
            const auto found = sc::fsc::solve_risk_premium(flows, value, basis_points{35});
            CHECK_NEAR(found.risk_premium, premium, 1e-9);
            CHECK(std::fabs(found.residual) <= 1e-8);
            CHECK_LT(found.iterations, 20); // Newton converges quadratically here
        }
        // Already there: no steps
        const double value = sc::fsc::expected_market_value(flows, 0.03, basis_points{});
        CHECK_EQ(sc::fsc::solve_risk_premium(flows, value, basis_points{}, {.start = 0.03}).iterations, 0);
        // A tighter tolerance, and another first guess
        const auto tight = sc::fsc::solve_risk_premium(flows, 1000.0, basis_points{10}, {.tolerance = 1e-11, .start = 0.1});
        CHECK(std::fabs(tight.residual) <= 1e-11);
        CHECK_NEAR(sc::fsc::expected_market_value(flows, tight.risk_premium, basis_points{10}), 1000.0, 1e-10);
    }

    SECTION("No solution: said so, with how far it got");
    {
        const auto flows = sc::fsc::cash_flows({365}, {1000.0}, {0.08});
        // Too few steps
        try {
            (void) sc::fsc::solve_risk_premium(flows, 500.0, basis_points{}, {.max_iterations = 1});
            CHECK(false);
        } catch (const sc::fsc::no_solution &failed) {
            CHECK_EQ(failed.iterations, 1);
            CHECK(std::fabs(failed.residual) > 1e-8);
            CHECK(std::string(failed.what()).find("after 1 iterations") != std::string::npos);
        }
        // Positive flows are never worth a negative amount: the steps run off
        bool unsolved = false;
        try {
            (void) sc::fsc::solve_risk_premium(flows, -10.0, basis_points{});
        } catch (const sc::fsc::no_solution &) {
            unsolved = true;
        }
        CHECK(unsolved);
        // No flows: the value doesn't change with the premium
        bool flat = false;
        try {
            (void) sc::fsc::solve_risk_premium({}, 100.0, basis_points{});
        } catch (const sc::fsc::no_solution &failed) {
            flat = std::string(failed.what()).find("stopped changing") != std::string::npos;
        }
        CHECK(flat);
    }

    SECTION("Inputs refused");
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        const std::vector<int> days = {30, 60};
        const std::vector<double> amounts = {1.0, 2.0};
        const std::vector<double> one_rate = {0.05};
        CHECK(refused([&] { (void) sc::fsc::cash_flows(days, amounts, one_rate); }));
        CHECK(refused([&] { (void) sc::fsc::cash_flows({1}, {nan}, {0.05}); }));
        CHECK(refused([&] { (void) sc::fsc::cash_flows({1}, {1.0}, {inf}); }));
        const auto flows = sc::fsc::cash_flows({365}, {1000.0}, {0.08});
        CHECK(refused([&] { (void) sc::fsc::expected_market_value(flows, nan, basis_points{}); }));
        CHECK(refused([&] { (void) sc::fsc::expected_market_value(flows, 0.01, basis_points{inf}); }));
        CHECK(refused([&] { (void) sc::fsc::expected_market_value(flows, -400.0, basis_points{}); })); // 1 + rate / 365.25 < 0
        CHECK(refused([&] { (void) sc::fsc::solve_risk_premium(flows, nan, basis_points{}); }));
        CHECK(refused([&] { (void) sc::fsc::solve_risk_premium(flows, 900.0, basis_points{}, {.max_iterations = 0}); }));
        CHECK(refused([&] { (void) sc::fsc::solve_risk_premium(flows, 900.0, basis_points{}, {.tolerance = 0}); }));
        CHECK(refused([&] { (void) sc::fsc::solve_risk_premium(flows, 900.0, basis_points{}, {.start = inf}); }));
    }

    TEST_SUMMARY();
}
