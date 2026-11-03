// sc_oneweb's C++ side (oneweb.h) without PostgreSQL or the network: the settings file, the
// signature (the README's test vector), the body made compact and 1web's answers read.
#include "oneweb.h"

#include "sc_test.h"

#include <stdexcept>
#include <string>

namespace {
    using namespace sc_pglib::oneweb;

    bool refused(const std::string &text, const std::string &database, const std::string &why) {
        try {
            (void) settings_for(parse_ini(text), database, [](const std::string &) { return std::string(40, 'k'); });
        } catch (const std::invalid_argument &problem) {
            if (std::string{problem.what()}.find(why) != std::string::npos) return true;
            std::printf("   (said: %s)\n", problem.what());
        }
        return false;
    }

    response answered(const long status, const std::string &body) {
        response r;
        r.status = status;
        r.body = body;
        return r;
    }
}

int main() {
    const std::string key(32, 'k');
    const std::string file = "; settings\n"
                             "[default]\n"
                             "url = https://example.com/interface/index.php/   ; the main site\n"
                             "key_file = /etc/sc-pglib/oneweb.key\n"
                             "timeout_ms = 20000\n"
                             "on_error = error\n"
                             "\n"
                             "[database whd_12345]\n"
                             "url = https://whd-12345.example.com/interface/index.php/\n"
                             "on_error = warning\n";
    const auto read_key = [](const std::string &path) {
        if (path != "/etc/sc-pglib/oneweb.key") throw std::invalid_argument{"no " + path};
        return std::string{"0123456789abcdef0123456789abcdef\n"};
    };

    SECTION("The section for the database, the rest from [default]");
    {
        const auto ini = parse_ini(file);
        const auto main = settings_for(ini, "1web", read_key);
        CHECK_EQ(main.section, std::string{"default"});
        CHECK_EQ(main.url, std::string{"https://example.com/interface/index.php/"});
        CHECK_EQ(main.key, std::string{"0123456789abcdef0123456789abcdef"}); // the line end trimmed
        CHECK_EQ(main.key_file, std::string{"/etc/sc-pglib/oneweb.key"});
        CHECK_EQ(main.timeout_ms, 20000L);
        CHECK_EQ(main.connect_timeout_ms, 5000L);
        CHECK(!main.warn);
        const auto branch = settings_for(ini, "whd_12345", read_key);
        CHECK_EQ(branch.section, std::string{"database whd_12345"});
        CHECK_EQ(branch.url, std::string{"https://whd-12345.example.com/interface/index.php/"});
        CHECK_EQ(branch.timeout_ms, 20000L);
        CHECK(branch.warn);
        const auto own = settings_for(parse_ini("[database only]\nurl = https://example.net/\nkey = " + key + "\n"), "only", read_key);
        CHECK_EQ(own.key, key);
        CHECK(own.key_file.empty());
        CHECK_EQ(parse_ini("").size(), std::size_t{0});
    }

    SECTION("Not set up, or set up wrongly");
    {
        CHECK(refused("[database other]\nurl = https://example.com/\nkey = " + key, "1web", "no [database 1web] or [default] section"));
        CHECK(refused("[default]\nkey = " + key, "1web", "no url"));
        CHECK(refused("[default]\nurl = http://example.com/\nkey = " + key, "1web", "url must be https://"));
        CHECK(refused("[default]\nurl = https://example.com\nkey = " + key, "1web", "end with \"/\""));
        CHECK(refused("[default]\nurl = https://example.com/", "1web", "no key or key_file"));
        CHECK(refused("[default]\nurl = https://example.com/\nkey = short", "1web", "at least 32 bytes"));
        CHECK(refused("[default]\nurl = https://example.com/\nkey = " + key + "\nkey_file = /x", "1web", "both set"));
        CHECK(refused("[default]\nurl = https://example.com/\nkey = " + key + "\ntimeout_ms = 999", "1web", "timeout_ms must be a whole number from 1000"));
        CHECK(refused("[default]\nurl = https://example.com/\nkey = " + key + "\ntimeout_ms = 5s", "1web", "timeout_ms must be"));
        CHECK(refused("[default]\nurl = https://example.com/\nkey = " + key + "\non_error = maybe", "1web", "on_error must be"));
        CHECK(refused("[default]\nurl = https://example.com/\nkey = " + key + "\nproxy = x", "1web", "line 4: unknown setting \"proxy\""));
        CHECK(refused("[default]\nurl = https://example.com/\nurl = https://example.net/", "1web", "line 3: url is there twice"));
        CHECK(refused("[server x]\n", "1web", "line 1: a section is [default] or [database <name>]"));
        CHECK(refused("[default]\n[default]\n", "1web", "[default] is there twice"));
        CHECK(refused("url = https://example.com/\n", "1web", "before any section"));
        CHECK(refused("[default]\njust words\n", "1web", "line 2: not \"key = value\""));
    }

    SECTION("Names");
    {
        CHECK(valid_name("ping") && valid_name("id_lookup") && valid_name("a") && valid_name("v2_lookup"));
        CHECK(valid_name(std::string(64, 'a')) && !valid_name(std::string(65, 'a')));
        CHECK(!valid_name("") && !valid_name("Ping") && !valid_name("_x") && !valid_name("1x") && !valid_name("a-b") && !valid_name("../x") && !valid_name("a/b"));
    }

    SECTION("The body: jsonb's text without the spaces between");
    {
        CHECK_EQ(compact_json(R"({"a": 1, "b": [1, 2, {"c": "x, y: z"}], "d": "say \"hi\", \\ ok"})"),
                 std::string{R"({"a":1,"b":[1,2,{"c":"x, y: z"}],"d":"say \"hi\", \\ ok"})"});
        CHECK_EQ(compact_json("{}"), std::string{"{}"});
    }

    SECTION("The signature: the README's test vector");
    {
        CHECK_EQ(signature("0123456789abcdef0123456789abcdef", "ping", "1700000000", "00112233445566778899aabbccddeeff", "1web", R"({"hello":"world"})"),
                 std::string{"47000de7c86714a083c28418a3a8836059e07399f871fafdc5954c38ba3fdcda"});
        const auto a = nonce(), b = nonce();
        CHECK(a.size() == 32 && a != b && a.find_first_not_of("0123456789abcdef") == std::string::npos);
    }

    SECTION("Answers");
    {
        auto got = read_answer(answered(200, R"({"ok": true, "data": {"name": "Ann"}})"), 1000);
        CHECK(got.ok && !got.null);
        got = read_answer(answered(200, R"({"ok":true,"data":null})"), 1000);
        CHECK(got.ok && got.null);
        got = read_answer(answered(200, R"({"ok":false,"error":"no such person"})"), 1000);
        CHECK(!got.ok);
        CHECK_EQ(got.error, std::string{"no such person"});
        got = read_answer(answered(403, R"({"ok":false,"error":"Wrong signature"})"), 1000);
        CHECK_EQ(got.error, std::string{"HTTP 403: Wrong signature"});
        got = read_answer(answered(500, "Internal\ntrouble"), 1000);
        CHECK_EQ(got.error, std::string{"HTTP 500: Internal trouble"});
        got = read_answer(answered(200, "<html>" + std::string(300, 'x')), 10000);
        CHECK(!got.ok && got.error.starts_with("HTTP 200: <html>xxx") && got.error.ends_with("x...") && got.error.size() == 10 + 200 + 3);
        got = read_answer(answered(200, R"({"data": 1})"), 1000);
        CHECK(!got.ok && got.error.starts_with("HTTP 200: {\"data\""));
        got = read_answer(answered(200, R"({"ok": true})"), 1000);
        CHECK(!got.ok && got.error.find("without data") != std::string::npos);
        got = read_answer(answered(302, ""), 1000);
        CHECK_EQ(got.error, std::string{"HTTP 302: (no body)"});
        response none;
        none.failure = "Couldn't connect to server";
        CHECK_EQ(read_answer(none, 1000).error, std::string{"Couldn't connect to server"});
        CHECK_EQ(excerpt("caf\xc3\xa9 \xff!"), std::string{"caf\xc3\xa9 ?!"});
        CHECK_EQ(excerpt("\xc3\xa9\xc3\xa9\xc3\xa9", 2), std::string{"\xc3\xa9\xc3\xa9..."});
    }

    TEST_SUMMARY();
}
