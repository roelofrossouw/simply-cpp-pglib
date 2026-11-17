#include "fsc.h"

#include <cmath>
#include <string>

namespace sc::fsc {
    namespace {
        void check_finite(const double value, const char *what) {
            if (!std::isfinite(value)) throw std::invalid_argument(std::string("sc::fsc: ") + what + " isn't a finite number");
        }

        // The value, plain or with derivatives: one formula for both
        template<typename Premium>
        Premium value_of(std::span<const cash_flow> flows, const Premium &risk_premium, const double spread) {
            Premium total = 0.0;
            for (const auto &flow: flows) {
                const Premium growth = (risk_premium + (flow.spot_rate + spread)) / days_a_year + 1.0;
                double base;
                if constexpr (std::is_floating_point_v<Premium>) base = growth;
                else base = growth.real;
                if (!(base > 0))
                    throw std::invalid_argument("sc::fsc: a rate of " + std::to_string((base - 1) * days_a_year) +
                                                " leaves nothing to discount by (1 + rate / 365.25 must be more than 0)");
                using std::pow;
                total += pow(growth, -flow.days) * flow.amount;
            }
            return total;
        }

        void check(std::span<const cash_flow> flows, const basis_points spread) {
            check_finite(spread.value, "the spread");
            for (const auto &flow: flows) {
                check_finite(flow.amount, "a cash flow's amount");
                check_finite(flow.spot_rate, "a cash flow's spot rate");
            }
        }
    }

    std::vector<cash_flow> cash_flows(std::span<const int> days, std::span<const double> amounts, std::span<const double> spot_rates) {
        if (days.size() != amounts.size() || days.size() != spot_rates.size())
            throw std::invalid_argument("sc::fsc: the cash flows' days, amounts and spot rates aren't the same length (" +
                                        std::to_string(days.size()) + ", " + std::to_string(amounts.size()) + " and " +
                                        std::to_string(spot_rates.size()) + ")");
        std::vector<cash_flow> flows;
        flows.reserve(days.size());
        for (std::size_t i = 0; i < days.size(); ++i) {
            check_finite(amounts[i], "a cash flow's amount");
            check_finite(spot_rates[i], "a cash flow's spot rate");
            flows.push_back({days[i], amounts[i], spot_rates[i]});
        }
        return flows;
    }

    std::vector<cash_flow> cash_flows(std::initializer_list<int> days, std::initializer_list<double> amounts,
                                      std::initializer_list<double> spot_rates) {
        return cash_flows(std::span(days.begin(), days.size()), std::span(amounts.begin(), amounts.size()),
                          std::span(spot_rates.begin(), spot_rates.size()));
    }

    double expected_market_value(std::span<const cash_flow> flows, const double risk_premium, const basis_points spread) {
        check_finite(risk_premium, "the risk premium");
        check(flows, spread);
        return value_of(flows, risk_premium, spread.fraction());
    }

    dual<double> expected_market_value(std::span<const cash_flow> flows, const dual<double> &risk_premium, const basis_points spread) {
        check_finite(risk_premium.real, "the risk premium");
        check_finite(risk_premium.eps, "the risk premium's derivative");
        check(flows, spread);
        return value_of(flows, risk_premium, spread.fraction());
    }

    solution solve_risk_premium(std::span<const cash_flow> flows, const double market_value, const basis_points spread,
                                const solve_options &options) {
        check_finite(market_value, "the market value");
        check_finite(options.start, "the first guess");
        check(flows, spread);
        if (options.max_iterations < 1) throw std::invalid_argument("sc::fsc: max_iterations must be at least 1");
        if (!(options.tolerance > 0) || !std::isfinite(options.tolerance))
            throw std::invalid_argument("sc::fsc: the tolerance must be a number more than 0");

        // Newton-Raphson: each step moves the premium by the difference over the slope, both from one dual evaluation
        double premium = options.start;
        auto evaluate = [&](const double at) {
            try {
                return value_of(flows, dual<double>{at, 1}, spread.fraction()) - dual<double>{market_value};
            } catch (const std::invalid_argument &problem) {
                throw no_solution(std::string(problem.what()) + " at a premium of " + std::to_string(at), at, 0, NAN);
            }
        };
        const auto first = evaluate(premium);
        double difference = first.real;
        double slope = first.eps;
        int iterations = 0;
        while (!(std::fabs(difference) <= options.tolerance)) {
            if (iterations == options.max_iterations)
                throw no_solution("sc::fsc: no risk premium within " + std::to_string(options.tolerance) + " of the market value after " +
                                  std::to_string(iterations) + " iterations (the value is " + std::to_string(difference) + " off at a premium of " +
                                  std::to_string(premium) + ")", premium, iterations, difference);
            if (!(std::fabs(slope) > 0) || !std::isfinite(slope) || !std::isfinite(difference))
                throw no_solution("sc::fsc: the value stopped changing with the premium at " + std::to_string(premium) +
                                  ", so there's no next step", premium, iterations, difference);
            premium -= difference / slope;
            ++iterations;
            try {
                const auto next = evaluate(premium);
                difference = next.real;
                slope = next.eps;
            } catch (no_solution &failed) {
                failed.iterations = iterations;
                throw;
            }
        }
        return {premium, iterations, difference};
    }
}
