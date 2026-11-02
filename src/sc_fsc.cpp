// An insurer's capital and reserving calculations (sc::fsc), with the arguments of the goalseek extension
// they replace, so a query only changes the function's name:
//
//   sc_fsc_emv(risk_prem, spread, durations, coupon_values, spot_rates)
//       the cash flows' expected market value at a risk premium
//   sc_fsc_solve_risk_prem(market_value, spread, durations, coupon_values, spot_rates [, max_iterations [, tolerance]])
//       the risk premium at which they're worth market_value (Newton-Raphson, exact derivatives)
//
// A cash flow is durations[i] days away, of coupon_values[i], at the spot rate spot_rates[i]; rates are
// fractions a year (0.085), the spread is in basis points, compounding daily over 365.25 days. The three
// arrays must be one-dimensional, of the same length and without NULLs; an error says which isn't.
//
// As pg_glue.h has it: the arrays are read and checked first (PostgreSQL, which may raise), the calculation
// runs in C++ on plain data and reports a pending error, raised once the C++ objects are gone.

#include <cmath>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include <sc_fsc.h>

#include "pg_glue.h"

extern "C" {
#include <catalog/pg_type.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>

PG_FUNCTION_INFO_V1(sc_fsc_emv);
PG_FUNCTION_INFO_V1(sc_fsc_solve_risk_prem);
}

namespace {
    // A one-dimensional array without NULLs, as plain values in PostgreSQL's memory. PostgreSQL code: raises.
    template<typename Value>
    int array_values(ArrayType *array, const Oid type, const char *name, const char *function, Value *&values) {
        if (ARR_NDIM(array) > 1)
            ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), errmsg("%s: %s must be a one-dimensional array", function, name)));
        int16 length;
        bool by_value;
        char align;
        get_typlenbyvalalign(type, &length, &by_value, &align);
        Datum *elements = nullptr;
        bool *nulls = nullptr;
        int count = 0;
        deconstruct_array(array, type, length, by_value, align, &elements, &nulls, &count);
        values = static_cast<Value *>(palloc(sizeof(Value) * Max(count, 1)));
        for (int i = 0; i < count; ++i) {
            if (nulls[i])
                ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("%s: %s[%d] is NULL", function, name, i + 1)));
            if constexpr (std::is_same_v<Value, int32>) values[i] = DatumGetInt32(elements[i]);
            else values[i] = DatumGetFloat8(elements[i]);
        }
        return count;
    }

    // The cash flows from the three array arguments at first (durations, coupon_values, spot_rates). PostgreSQL code: raises.
    struct flow_arrays {
        int count = 0;
        int32 *days = nullptr;
        double *amounts = nullptr;
        double *spot_rates = nullptr;
    };

    flow_arrays read_flows(FunctionCallInfo fcinfo, const int first, const char *function) {
        flow_arrays flows;
        flows.count = array_values(PG_GETARG_ARRAYTYPE_P(first), INT4OID, "durations", function, flows.days);
        const int amounts = array_values(PG_GETARG_ARRAYTYPE_P(first + 1), FLOAT8OID, "coupon_values", function, flows.amounts);
        const int rates = array_values(PG_GETARG_ARRAYTYPE_P(first + 2), FLOAT8OID, "spot_rates", function, flows.spot_rates);
        if (amounts != flows.count || rates != flows.count)
            ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                            errmsg("%s: durations, coupon_values and spot_rates must be the same length, not %d, %d and %d", function,
                                   flows.count, amounts, rates)));
        return flows;
    }

    // The value at a premium. Never raises.
    double value_at(const flow_arrays &arrays, const double premium, const double spread, sc_pglib::pending_error &error) noexcept {
        try {
            const auto flows = sc::fsc::cash_flows(std::span<const int>(arrays.days, arrays.count),
                                                   std::span<const double>(arrays.amounts, arrays.count),
                                                   std::span<const double>(arrays.spot_rates, arrays.count));
            return sc::fsc::expected_market_value(flows, premium, sc::fsc::basis_points{spread});
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_fsc_emv: %s", problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_fsc_emv: unexpected C++ exception");
        }
        return 0;
    }

    // The premium for a market value. Never raises.
    double premium_for(const flow_arrays &arrays, const double market_value, const double spread, const int max_iterations,
                       const double tolerance, sc_pglib::pending_error &error) noexcept {
        try {
            const auto flows = sc::fsc::cash_flows(std::span<const int>(arrays.days, arrays.count),
                                                   std::span<const double>(arrays.amounts, arrays.count),
                                                   std::span<const double>(arrays.spot_rates, arrays.count));
            return sc::fsc::solve_risk_premium(flows, market_value, sc::fsc::basis_points{spread},
                                               {.max_iterations = max_iterations, .tolerance = tolerance}).risk_premium;
        } catch (const sc::fsc::no_solution &problem) {
            error.set(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE, ERROR, "sc_fsc_solve_risk_prem: %s", problem.what());
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_fsc_solve_risk_prem: %s", problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_fsc_solve_risk_prem: unexpected C++ exception");
        }
        return 0;
    }
}

// sc_fsc_emv(risk_prem float8, spread float8, durations int[], coupon_values float8[], spot_rates float8[]) -> float8
extern "C" Datum sc_fsc_emv(PG_FUNCTION_ARGS) {
    const double premium = PG_GETARG_FLOAT8(0);
    const double spread = PG_GETARG_FLOAT8(1);
    const flow_arrays flows = read_flows(fcinfo, 2, "sc_fsc_emv");
    sc_pglib::pending_error error;
    const double value = value_at(flows, premium, spread, error);
    error.raise();
    PG_RETURN_FLOAT8(value);
}

// sc_fsc_solve_risk_prem(market_value float8, spread float8, durations int[], coupon_values float8[], spot_rates float8[],
//                        max_iterations int, tolerance float8) -> float8
extern "C" Datum sc_fsc_solve_risk_prem(PG_FUNCTION_ARGS) {
    const double market_value = PG_GETARG_FLOAT8(0);
    const double spread = PG_GETARG_FLOAT8(1);
    const flow_arrays flows = read_flows(fcinfo, 2, "sc_fsc_solve_risk_prem");
    const int max_iterations = PG_NARGS() > 5 ? PG_GETARG_INT32(5) : 1000;
    const double tolerance = PG_NARGS() > 6 ? PG_GETARG_FLOAT8(6) : 1e-8;
    sc_pglib::pending_error error;
    const double premium = premium_for(flows, market_value, spread, max_iterations, tolerance, error);
    error.raise();
    PG_RETURN_FLOAT8(premium);
}
