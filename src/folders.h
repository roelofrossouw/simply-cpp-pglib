#pragma once
// mkdir -p kept to one folder tree (sc_mkdir), without PostgreSQL, so it can be tested on its own.

#include <string>
#include <string_view>

namespace sc_pglib {
    // Makes path and its missing parents (mode 0755, whatever the umask), each inside root, and
    // gives it back without a trailing slash. path is root itself or below it: absolute, with no
    // empty, "." or ".." parts (one trailing slash is fine). Nothing on the way may be a symbolic
    // link: each part is opened without following one, so none can lead out of root, whatever is
    // planted there meanwhile. A folder that's there already is fine; anything else that's there
    // (a file) isn't. Throws std::runtime_error naming the path and why.
    std::string make_folders(const std::string &root, std::string_view path);
}
