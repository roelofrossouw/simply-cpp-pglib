#pragma once
// Files and folders kept to one folder tree (sc_mkdir, sc_move, sc_list_files), without PostgreSQL,
// so they can be tested on their own. Every path is root or below it: absolute, with no empty, "."
// or ".." parts (one trailing slash is fine). Nothing on the way may be a symbolic link: each part is
// opened without following one, so none can lead out of root, whatever is planted there meanwhile.
// Each throws std::runtime_error naming the path and why.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sc_pglib {
    // mkdir -p: path and its missing parents made (mode 0755, whatever the umask), given back without
    // a trailing slash. A folder that's there already is fine; anything else that's there (a file)
    // isn't.
    std::string make_folders(const std::string &root, std::string_view path);

    // mv: a file or folder from renamed to, or into to when to ends with a slash or is a folder that's
    // there (keeping its name); a file at to is replaced. to's folder must be there. Across
    // filesystems a file is copied (0644), then removed. Gives back where it is now.
    std::string move_path(const std::string &root, std::string_view from, std::string_view to);

    struct listed_file {
        std::string path;
        std::int64_t seconds = 0; // modified, since 1970-01-01 UTC
        long nanoseconds = 0;
        std::int64_t size = 0;
    };

    // find -type f: the files in folder and the folders in it (links left out), oldest first (then by
    // path).
    std::vector<listed_file> list_files(const std::string &root, std::string_view folder);
}
