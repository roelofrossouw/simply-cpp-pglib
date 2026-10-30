// sc_mkdir's folder walk (folders.h) in a scratch tree: made, there already, refused.
#include "folders.h"

#include "sc_test.h"

#include <sys/stat.h>

#include <filesystem>
#include <chrono>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>

namespace {
    template<typename Work>
    bool refused_by(Work work, const std::string &why) {
        try {
            work();
        } catch (const std::runtime_error &problem) {
            return std::string{problem.what()}.find(why) != std::string::npos;
        }
        return false;
    }

    bool refused_move(const std::string &root, const std::string &from, const std::string &to, const std::string &why) {
        return refused_by([&] { (void) sc_pglib::move_path(root, from, to); }, why);
    }

    bool refused_list(const std::string &root, const std::string &folder, const std::string &why) {
        return refused_by([&] { (void) sc_pglib::list_files(root, folder); }, why);
    }

    bool refused(const std::string &root, const std::string &path, const std::string &why) {
        try {
            (void) sc_pglib::make_folders(root, path);
        } catch (const std::runtime_error &problem) {
            return std::string{problem.what()}.find(why) != std::string::npos;
        }
        return false;
    }
}

int main() {
    namespace fs = std::filesystem;
    const auto scratch = fs::temp_directory_path() / ("sc-pglib-folders-" + std::to_string(std::random_device{}()));
    const auto root = (scratch / "output").string();
    fs::create_directories(root);
    ::umask(0077); // as the server runs: the folders are 0755 all the same

    SECTION("Made with their parents, 0755; there already is fine; a trailing slash");
    {
        CHECK_EQ(sc_pglib::make_folders(root, root + "/omart/2026/Oct/"), root + "/omart/2026/Oct");
        CHECK(fs::is_directory(root + "/omart/2026/Oct"));
        struct stat seen{};
        CHECK(::stat((root + "/omart/2026").c_str(), &seen) == 0 && (seen.st_mode & 07777) == 0755);
        CHECK_EQ(sc_pglib::make_folders(root, root + "/omart/2026/Oct"), root + "/omart/2026/Oct");
        CHECK_EQ(sc_pglib::make_folders(root, root), root);
        CHECK_EQ(sc_pglib::make_folders(root, root + "/"), root);
        // A folder that's there keeps its mode.
        fs::permissions(root + "/omart", fs::perms::owner_all);
        (void) sc_pglib::make_folders(root, root + "/omart/2027");
        CHECK(::stat((root + "/omart").c_str(), &seen) == 0 && (seen.st_mode & 07777) == 0700);
    }

    SECTION("Refused: outside, relative, empty, . and .. parts");
    {
        CHECK(refused(root, "relative/path", "isn't an absolute path"));
        CHECK(refused(root, "", "isn't an absolute path"));
        CHECK(refused(root, scratch.string(), "or a folder in it"));
        CHECK(refused(root, root + "x/y", "or a folder in it"));
        CHECK(refused(root, "/tmp/x", "or a folder in it"));
        CHECK(refused(root, root + "/a/../../x", "\"..\" part"));
        CHECK(refused(root, root + "/a/./b", "\".\" part"));
        CHECK(refused(root, root + "/a//b", "empty part"));
        CHECK(refused(root, root + "//", "empty part"));
        CHECK(refused(root, root + std::string{"/a\0b", 4}, "NUL"));
        CHECK(!fs::exists(scratch / "x"));
    }

    SECTION("Refused: a symbolic link on the way, in or out; a file in the way");
    {
        fs::create_directories(scratch / "elsewhere");
        fs::create_directory_symlink(scratch / "elsewhere", root + "/out");
        CHECK(refused(root, root + "/out/x", "is a symbolic link"));
        CHECK(!fs::exists(scratch / "elsewhere" / "x"));
        fs::create_directory_symlink(root + "/omart", root + "/in");
        CHECK(refused(root, root + "/in/x", "is a symbolic link"));
        CHECK(refused(root, root + "/out", "is a symbolic link"));
        std::ofstream{root + "/file"} << "x";
        CHECK(refused(root, root + "/file/x", "isn't a folder"));
    }

    SECTION("Moved: renamed, into a folder (a trailing slash, or one that's there), a file replaced");
    {
        fs::create_directories(root + "/dsn/processed");
        std::ofstream{root + "/dsn/a.txt"} << "a";
        std::ofstream{root + "/dsn/b.txt"} << "b";
        CHECK_EQ(sc_pglib::move_path(root, root + "/dsn/a.txt", root + "/dsn/processed/"), root + "/dsn/processed/a.txt");
        CHECK(fs::exists(root + "/dsn/processed/a.txt") && !fs::exists(root + "/dsn/a.txt"));
        CHECK_EQ(sc_pglib::move_path(root, root + "/dsn/b.txt", root + "/dsn/processed"), root + "/dsn/processed/b.txt");
        CHECK_EQ(sc_pglib::move_path(root, root + "/dsn/processed/b.txt", root + "/dsn/renamed.txt"), root + "/dsn/renamed.txt");
        std::ofstream{root + "/dsn/c.txt"} << "c";
        CHECK_EQ(sc_pglib::move_path(root, root + "/dsn/c.txt", root + "/dsn/renamed.txt"), root + "/dsn/renamed.txt");
        std::ifstream replaced{root + "/dsn/renamed.txt"};
        const std::string content{std::istreambuf_iterator<char>{replaced}, std::istreambuf_iterator<char>{}};
        CHECK_EQ(content, std::string{"c"});
        // A folder moves too.
        CHECK_EQ(sc_pglib::move_path(root, root + "/dsn/processed", root + "/dsn/done"), root + "/dsn/done");
    }

    SECTION("Not moved: missing, a link, outside, root itself, a missing target folder");
    {
        CHECK(refused_move(root, root + "/dsn/none.txt", root + "/dsn/x", "isn't there"));
        CHECK(refused_move(root, root + "/out", root + "/dsn/x", "symbolic link"));
        CHECK(refused_move(root, root + "/dsn/renamed.txt", scratch.string() + "/x", "or a folder in it"));
        CHECK(refused_move(root, scratch.string() + "/elsewhere", root + "/x", "or a folder in it"));
        CHECK(refused_move(root, root, root + "/x", "itself"));
        CHECK(refused_move(root, root + "/dsn/renamed.txt", root + "/nowhere/x", "nowhere isn't there"));
        CHECK(refused_move(root, root + "/dsn/renamed.txt", root + "/out/x", "is a symbolic link"));
        CHECK(!fs::exists(scratch / "elsewhere" / "x"));
    }

    SECTION("Listed: files only, links left out, oldest first, folders below too");
    {
        fs::create_directories(root + "/queue/123");
        std::ofstream{root + "/queue/new.txt"} << "new";
        std::ofstream{root + "/queue/123/old.txt"} << "old!";
        fs::last_write_time(root + "/queue/123/old.txt", fs::last_write_time(root + "/queue/new.txt") - std::chrono::hours{2});
        fs::create_symlink(root + "/queue/new.txt", root + "/queue/link.txt");
        const auto files = sc_pglib::list_files(root, root + "/queue/");
        CHECK_EQ(files.size(), std::size_t{2});
        if (files.size() == 2) {
            CHECK_EQ(files[0].path, root + "/queue/123/old.txt");
            CHECK_EQ(files[0].size, std::int64_t{4});
            CHECK_EQ(files[1].path, root + "/queue/new.txt");
            CHECK(files[0].seconds < files[1].seconds);
        }
        CHECK(refused_list(root, root + "/out", "is a symbolic link"));
        CHECK(refused_list(root, root + "/queue/none", "isn't there"));
    }

    fs::remove_all(scratch);
    TEST_SUMMARY();
}
