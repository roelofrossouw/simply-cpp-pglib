-- sc_pglib 1.14 -> 1.15: an insurer's capital and reserving calculations (sc-fsc).
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.15'" to load this file. \quit

-- Cash flows durations[i] days away, of coupon_values[i], at the risk-free spot rate spot_rates[i] (a fraction
-- a year), valued at a risk premium (a fraction a year) and a spread (basis points), compounded daily over
-- 365.25 days: the sum of each coupon times (1 + (spot + premium + spread) / 365.25)^-days. The arrays must be
-- one-dimensional, the same length and without NULLs. The arguments are the goalseek extension's emv_cpp.
CREATE FUNCTION sc_fsc_emv(risk_prem double precision, spread double precision, durations integer[],
                           coupon_values double precision[], spot_rates double precision[]) RETURNS double precision
    AS 'MODULE_PATHNAME', 'sc_fsc_emv'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- The risk premium at which those cash flows are worth market_value, by Newton-Raphson from 0 with the exact
-- derivative; an error when it isn't within tolerance after max_iterations steps. The arguments are the
-- goalseek extension's solve_risk_prem.
CREATE FUNCTION sc_fsc_solve_risk_prem(market_value double precision, spread double precision, durations integer[],
                                       coupon_values double precision[], spot_rates double precision[],
                                       max_iterations integer DEFAULT 1000, tolerance double precision DEFAULT 1e-8)
    RETURNS double precision
    AS 'MODULE_PATHNAME', 'sc_fsc_solve_risk_prem'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
