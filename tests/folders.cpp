// sc_mkdir's folder walk (folders.h) in a scratch tree: made, there already, refused.
#include "folders.h"

#include "sc_test.h"

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
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

    fs::remove_all(scratch);
    TEST_SUMMARY();
}
