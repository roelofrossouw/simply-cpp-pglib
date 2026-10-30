#include "folders.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace sc_pglib {
    namespace {
        struct descriptor {
            int fd = -1;
            explicit descriptor(const int opened) : fd(opened) {}
            descriptor(const descriptor &) = delete;
            descriptor &operator=(const descriptor &) = delete;
            ~descriptor() {
                if (fd >= 0) ::close(fd);
            }
        };
    }

    std::string make_folders(const std::string &root, const std::string_view path) {
        std::string shown; // the path as the messages name it, a NUL as \0
        for (const char c: path) shown += c == '\0' ? std::string{"\\0"} : std::string(1, c);
        const auto refuse = [&](const std::string &why) -> std::runtime_error { return std::runtime_error{"\"" + shown + "\": " + why}; };
        if (path.find('\0') != std::string_view::npos) throw refuse("it has a NUL");
        if (path.empty() || path.front() != '/') throw refuse("it isn't an absolute path");
        if (path != root && !(path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/'))
            throw refuse("it isn't " + root + " or a folder in it");

        // The parts below root; one trailing slash is allowed.
        std::string_view rest = path.substr(root.size());
        if (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
        std::vector<std::string> parts;
        while (!rest.empty()) {
            rest.remove_prefix(1); // the '/'
            const auto slash = rest.find('/');
            const auto part = rest.substr(0, slash);
            if (part.empty()) throw refuse("it has an empty part (//)");
            if (part == "." || part == "..") throw refuse("it has a \"" + std::string{part} + "\" part");
            parts.emplace_back(part);
            rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
        }

        auto here = std::make_unique<descriptor>(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (here->fd < 0) throw refuse(root + " can't be opened: " + std::strerror(errno));
        std::string made = root;
        for (const auto &part: parts) {
            made += "/" + part;
            bool created = false;
            if (::mkdirat(here->fd, part.c_str(), 0755) == 0) created = true;
            else if (errno != EEXIST) throw refuse("making " + made + ": " + std::strerror(errno));
            // Opened without following a link: a link (planted before or after mkdirat) fails here.
            auto next = std::make_unique<descriptor>(::openat(here->fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (next->fd < 0) {
                // What's there, as itself (Linux says ELOOP for a link, macOS ENOTDIR).
                const int opening = errno;
                struct stat there{};
                if (::fstatat(here->fd, part.c_str(), &there, AT_SYMLINK_NOFOLLOW) == 0) {
                    if (S_ISLNK(there.st_mode)) throw refuse(made + " is a symbolic link");
                    if (!S_ISDIR(there.st_mode)) throw refuse(made + " is there but isn't a folder");
                }
                throw refuse("opening " + made + ": " + std::strerror(opening));
            }
            struct stat seen{};
            if (::fstat(next->fd, &seen) != 0 || !S_ISDIR(seen.st_mode)) throw refuse(made + " isn't a folder");
            // The umask aside, as mkdir -p in a shell with umask 022 gives.
            if (created && ::fchmod(next->fd, 0755) != 0) throw refuse("setting " + made + "'s mode: " + std::strerror(errno));
            here = std::move(next);
        }
        return made;
    }
}
