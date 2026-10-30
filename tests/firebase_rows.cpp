// A Firebase foreign table's rows from a fetched tree, without PostgreSQL or the network: the path
// narrowed by leading known keys, later known keys filtering, nodes that aren't objects passed
// over, fields by name or path, and values as column text.
#include "firebase_rows.h"

#include "sc_test.h"

using namespace sc_pglib::firebase_rows;
using known_keys = std::vector<std::optional<std::string>>;

namespace {
    // Shaped like truckassist's "device" tree: account, then IMEI; a timestamp beside the devices.
    const json tree = json::parse(R"({
        "42": {"timestamp": "2026-10-30T12:00:00",
               "350000000000001": {"lat": -26.2, "lng": 28.0, "online": 1, "reg_nr": "ABC 123 GP", "temp1": "", "gps": {"sat": 9}},
               "350000000000002": {"lat": -33.9, "lng": 18.4, "online": 0, "reg_nr": ""}},
        "43": {"350000000000003": {"lat": -29.8, "lng": 31.0, "ignition": "on"}},
        "44": "not a node"
    })");

    std::vector<std::string> row_keys(const std::vector<row> &rows) {
        std::vector<std::string> out;
        for (const auto &each: rows) out.push_back(each.keys.at(0) + "/" + each.keys.at(1));
        return out;
    }
}

int main() {
    SECTION("The path: the leading known keys only");
    {
        CHECK_EQ(fetch_path("device", {std::nullopt, std::nullopt}), std::string{"device"});
        CHECK_EQ(fetch_path("device", {"42", std::nullopt}), std::string{"device/42"});
        CHECK_EQ(fetch_path("device", {"42", "350000000000001"}), std::string{"device/42/350000000000001"});
        CHECK_EQ(fetch_path("device", {std::nullopt, "350000000000001"}), std::string{"device"}); // a later one filters instead
        CHECK_EQ(leading_known({"a", std::nullopt, "c"}), std::size_t{1});
    }

    SECTION("Every device: object nodes at the last level; the timestamp and the string passed over");
    {
        CHECK(row_keys(rows(tree, {std::nullopt, std::nullopt})) ==
              (std::vector<std::string>{"42/350000000000001", "42/350000000000002", "43/350000000000003"}));
    }

    SECTION("One account, fetched below its key; one device; a later key filtering");
    {
        CHECK(row_keys(rows(tree["42"], {"42", std::nullopt})) == (std::vector<std::string>{"42/350000000000001", "42/350000000000002"}));
        CHECK(row_keys(rows(tree["42"]["350000000000002"], {"42", "350000000000002"})) == (std::vector<std::string>{"42/350000000000002"}));
        CHECK(row_keys(rows(tree, {std::nullopt, "350000000000003"})) == (std::vector<std::string>{"43/350000000000003"}));
        CHECK(rows(tree, {std::nullopt, "nope"}).empty());
        CHECK(rows(json(nullptr), {"99", std::nullopt}).empty()); // nothing at the path
        CHECK(rows(tree["42"]["timestamp"], {"42", "timestamp"}).empty()); // a value, not a node
    }

    SECTION("Fields and their text");
    {
        const auto &device = tree["42"]["350000000000001"];
        CHECK(text_of(*field(device, "lat"), false) == std::optional<std::string>{"-26.2"});
        CHECK(text_of(*field(device, "reg_nr"), false) == std::optional<std::string>{"ABC 123 GP"});
        CHECK(text_of(*field(device, "reg_nr"), true) == std::optional<std::string>{"\"ABC 123 GP\""});
        CHECK(text_of(*field(device, "gps/sat"), false) == std::optional<std::string>{"9"});
        CHECK(text_of(*field(device, "gps"), false) == std::optional<std::string>{"{\"sat\":9}"});
        CHECK(text_of(json(true), false) == std::optional<std::string>{"true"});
        CHECK(!text_of(json(nullptr), false).has_value());
        CHECK(field(device, "missing") == nullptr);
        CHECK(field(device, "lat/deeper") == nullptr);
    }

    SECTION("Lists");
    {
        CHECK(split_list(" account_id, imei ,") == (std::vector<std::string>{"account_id", "imei"}));
        CHECK(split_list("").empty());
    }

    TEST_SUMMARY();
}
