// sc_http_get's allow list (http_allowed.h), without PostgreSQL.
#include "http_allowed.h"

#include "sc_test.h"

#include <stdexcept>

int main() {
    using sc_pglib::http_allowed;
    using sc_pglib::http_prefixes;

    SECTION("The default list parses, and holds Webfleet's /extern");
    {
        const auto defaults = http_prefixes(sc_pglib::default_http_urls);
        CHECK_EQ(defaults.size(), std::size_t{6});
        CHECK(http_allowed("https://csv.webfleet.com/extern?lang=en&action=x", defaults));
        CHECK(http_allowed("https://CSV.Webfleet.com:443/extern", defaults));
        CHECK(!http_allowed("https://csv.webfleet.com/externx", defaults));
        CHECK(!http_allowed("https://csv.webfleet.com/", defaults));
        CHECK(!http_allowed("http://csv.webfleet.com/extern", defaults));
    }

    SECTION("A host on its own allows any path; a path, itself and below");
    {
        const auto list = http_prefixes("https://api.example.com, https://data.example.com/tracking/");
        CHECK(http_allowed("https://api.example.com", list));
        CHECK(http_allowed("https://api.example.com/anything/at/all?x=1", list));
        CHECK(http_allowed("https://data.example.com/tracking", list));
        CHECK(http_allowed("https://data.example.com/tracking/a/b", list));
        CHECK(http_allowed("https://data.example.com/tracking?x=1", list));
        CHECK(!http_allowed("https://data.example.com/trackingx", list));
        CHECK(!http_allowed("https://data.example.com/", list));
        CHECK(!http_allowed("https://api.example.com:8443/", list));
        CHECK(!http_allowed("https://api.example.com.evil.example/", list));
    }

    SECTION("Nothing that could step out of a prefix");
    {
        const auto list = http_prefixes("https://data.example.com/tracking");
        CHECK(!http_allowed("https://data.example.com/tracking/../admin", list));
        CHECK(!http_allowed("https://data.example.com/tracking/./x", list));
        CHECK(!http_allowed("https://data.example.com/tracking/%2e%2e/admin", list));
        CHECK(!http_allowed("https://data.example.com/tracking%2Fx", list));
        CHECK(!http_allowed("https://user:pw@data.example.com/tracking", list));
        CHECK(!http_allowed("https://data.example.com@evil.example/tracking", list));
        CHECK(!http_allowed("ftp://data.example.com/tracking", list));
        CHECK(!http_allowed("data.example.com/tracking", list));
        CHECK(!http_allowed("https://data.example.com/tracking", {}));
    }

    SECTION("A setting that isn't URLs is refused");
    {
        bool refused = false;
        try {
            (void) http_prefixes("https://ok.example.com, not-a-url");
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        CHECK(refused);
        CHECK(http_prefixes("").empty());
        CHECK_EQ(http_prefixes(" https://a.example.com\n https://b.example.com ,").size(), std::size_t{2});
    }
    TEST_SUMMARY();
}
