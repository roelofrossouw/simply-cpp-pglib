#include "folders.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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

        // A path below root, checked: its parts, and how the messages name it.
        struct checked {
            std::string shown;              // the path as given, a NUL as \0
            std::vector<std::string> parts; // below root
            bool trailing_slash = false;

            [[nodiscard]] std::runtime_error refuse(const std::string &why) const { return std::runtime_error{"\"" + shown + "\": " + why}; }
        };

        checked check(const std::string &root, const std::string_view path) {
            checked out;
            for (const char c: path) out.shown += c == '\0' ? std::string{"\\0"} : std::string(1, c);
            if (path.find('\0') != std::string_view::npos) throw out.refuse("it has a NUL");
            if (path.empty() || path.front() != '/') throw out.refuse("it isn't an absolute path");
            if (path != root && !(path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/'))
                throw out.refuse("it isn't " + root + " or a folder in it");

            // The parts below root; one trailing slash is allowed.
            std::string_view rest = path.substr(root.size());
            if (!rest.empty() && rest.back() == '/') {
                rest.remove_suffix(1);
                out.trailing_slash = true;
            }
            while (!rest.empty()) {
                rest.remove_prefix(1); // the '/'
                const auto slash = rest.find('/');
                const auto part = rest.substr(0, slash);
                if (part.empty()) throw out.refuse("it has an empty part (//)");
                if (part == "." || part == "..") throw out.refuse("it has a \"" + std::string{part} + "\" part");
                out.parts.emplace_back(part);
                rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
            }
            return out;
        }

        std::string joined(const std::string &root, const std::vector<std::string> &parts, const std::size_t count) {
            std::string out = root;
            for (std::size_t i = 0; i < count; ++i) out += "/" + parts[i];
            return out;
        }

        // A folder opened without following a link anywhere on the way: root, then the first count
        // parts (create: those missing made, 0755). Throws (what.refuse) for a link, a file in the
        // way, or one missing when not created.
        std::unique_ptr<descriptor> open_folder(const std::string &root, const checked &what, const std::size_t count, const bool create) {
            auto here = std::make_unique<descriptor>(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (here->fd < 0) throw what.refuse(root + " can't be opened: " + std::strerror(errno));
            for (std::size_t i = 0; i < count; ++i) {
                const auto &part = what.parts[i];
                const auto made = joined(root, what.parts, i + 1);
                bool created = false;
                if (create) {
                    if (::mkdirat(here->fd, part.c_str(), 0755) == 0) created = true;
                    else if (errno != EEXIST) throw what.refuse("making " + made + ": " + std::strerror(errno));
                }
                // Opened without following a link: a link (planted before or after mkdirat) fails here.
                auto next = std::make_unique<descriptor>(::openat(here->fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (next->fd < 0) {
                    // What's there, as itself (Linux says ELOOP for a link, macOS ENOTDIR).
                    const int opening = errno;
                    struct stat there{};
                    if (::fstatat(here->fd, part.c_str(), &there, AT_SYMLINK_NOFOLLOW) == 0) {
                        if (S_ISLNK(there.st_mode)) throw what.refuse(made + " is a symbolic link");
                        if (!S_ISDIR(there.st_mode)) throw what.refuse(made + " is there but isn't a folder");
                    }
                    if (opening == ENOENT) throw what.refuse(made + " isn't there");
                    throw what.refuse("opening " + made + ": " + std::strerror(opening));
                }
                struct stat seen{};
                if (::fstat(next->fd, &seen) != 0 || !S_ISDIR(seen.st_mode)) throw what.refuse(made + " isn't a folder");
                // The umask aside, as mkdir -p in a shell with umask 022 gives.
                if (created && ::fchmod(next->fd, 0755) != 0) throw what.refuse("setting " + made + "'s mode: " + std::strerror(errno));
                here = std::move(next);
            }
            return here;
        }

        // A regular file copied from one folder to another under a new name, then the original
        // removed: a move across filesystems. Written beside its new name first, then renamed.
        void copy_across(const checked &what, const int from_folder, const std::string &from_name, const int to_folder, const std::string &to_name) {
            descriptor source{::openat(from_folder, from_name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
            if (source.fd < 0) throw what.refuse(std::string{"opening it: "} + std::strerror(errno));
            const auto partial = "." + to_name + ".moving";
            descriptor target{::openat(to_folder, partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644)};
            if (target.fd < 0) throw what.refuse(std::string{"writing its copy: "} + std::strerror(errno));
            const auto fail = [&](const std::string &doing) {
                const int problem = errno;
                ::unlinkat(to_folder, partial.c_str(), 0);
                return what.refuse(doing + ": " + std::strerror(problem));
            };
            char buffer[1 << 16];
            for (;;) {
                const auto got = ::read(source.fd, buffer, sizeof buffer);
                if (got < 0) {
                    if (errno == EINTR) continue;
                    throw fail("reading it");
                }
                if (got == 0) break;
                for (ssize_t done = 0; done < got;) {
                    const auto wrote = ::write(target.fd, buffer + done, static_cast<std::size_t>(got - done));
                    if (wrote < 0) {
                        if (errno == EINTR) continue;
                        throw fail("writing its copy");
                    }
                    done += wrote;
                }
            }
            if (::fsync(target.fd) != 0) throw fail("writing its copy");
            if (::renameat(to_folder, partial.c_str(), to_folder, to_name.c_str()) != 0) throw fail("naming its copy");
            if (::unlinkat(from_folder, from_name.c_str(), 0) != 0) throw what.refuse(std::string{"copied, but not removed: "} + std::strerror(errno));
        }

        void list_into(const int folder, const std::string &path, std::vector<listed_file> &out) {
            descriptor walking{::dup(folder)};
            DIR *entries = walking.fd >= 0 ? ::fdopendir(walking.fd) : nullptr;
            if (!entries) throw std::runtime_error("\"" + path + "\": it can't be read: " + std::strerror(errno));
            walking.fd = -1; // closedir() closes it
            const std::unique_ptr<DIR, decltype(&::closedir)> closing{entries, &::closedir};
            while (const dirent *entry = ::readdir(entries)) {
                const std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                struct stat seen{};
                if (::fstatat(folder, name.c_str(), &seen, AT_SYMLINK_NOFOLLOW) != 0) continue; // gone meanwhile
                if (S_ISREG(seen.st_mode)) {
#ifdef __APPLE__
                    const auto &modified = seen.st_mtimespec;
#else
                    const auto &modified = seen.st_mtim;
#endif
                    out.push_back({path + "/" + name, static_cast<std::int64_t>(modified.tv_sec), static_cast<long>(modified.tv_nsec),
                                   static_cast<std::int64_t>(seen.st_size)});
                } else if (S_ISDIR(seen.st_mode)) {
                    descriptor inner{::openat(folder, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
                    if (inner.fd >= 0) list_into(inner.fd, path + "/" + name, out);
                }
                // Links, sockets and the like aren't files: left out, as find -type f does.
            }
        }
    }

    std::string make_folders(const std::string &root, const std::string_view path) {
        const auto what = check(root, path);
        (void) open_folder(root, what, what.parts.size(), true);
        return joined(root, what.parts, what.parts.size());
    }

    std::string move_path(const std::string &root, const std::string_view from, const std::string_view to) {
        // Both paths checked before either is looked at.
        const auto source = check(root, from);
        const auto target = check(root, to);
        if (source.parts.empty()) throw source.refuse("it's " + root + " itself");
        if (source.trailing_slash) throw source.refuse("name what's moved without a trailing slash");
        const auto source_folder = open_folder(root, source, source.parts.size() - 1, false);
        const auto &source_name = source.parts.back();
        struct stat moving{};
        if (::fstatat(source_folder->fd, source_name.c_str(), &moving, AT_SYMLINK_NOFOLLOW) != 0)
            throw source.refuse(errno == ENOENT ? "it isn't there" : std::string{"it can't be read: "} + std::strerror(errno));
        if (S_ISLNK(moving.st_mode)) throw source.refuse("it's a symbolic link");
        if (!S_ISREG(moving.st_mode) && !S_ISDIR(moving.st_mode)) throw source.refuse("it isn't a file or a folder");

        // Into a folder (a trailing slash, or a folder that's there), keeping its name, as mv does.
        std::unique_ptr<descriptor> target_folder;
        std::vector<std::string> target_parts = target.parts;
        bool into = target.trailing_slash || target.parts.empty();
        if (!into) {
            auto parent = open_folder(root, target, target.parts.size() - 1, false);
            struct stat there{};
            if (::fstatat(parent->fd, target.parts.back().c_str(), &there, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISLNK(there.st_mode)) throw target.refuse("it's a symbolic link");
                into = S_ISDIR(there.st_mode);
            }
            if (!into) target_folder = std::move(parent);
        }
        if (into) {
            target_folder = open_folder(root, target, target.parts.size(), false);
            target_parts.push_back(source_name);
        }
        const auto &target_name = target_parts.back();
        const auto moved = joined(root, target_parts, target_parts.size());

        if (::renameat(source_folder->fd, source_name.c_str(), target_folder->fd, target_name.c_str()) == 0) return moved;
        if (errno != EXDEV) throw source.refuse("moving it to " + moved + ": " + std::strerror(errno));
        if (!S_ISREG(moving.st_mode)) throw source.refuse("a folder can't be moved to another filesystem (" + moved + ")");
        copy_across(source, source_folder->fd, source_name, target_folder->fd, target_name);
        return moved;
    }

    std::vector<listed_file> list_files(const std::string &root, const std::string_view folder) {
        const auto what = check(root, folder);
        const auto opened = open_folder(root, what, what.parts.size(), false);
        std::vector<listed_file> out;
        list_into(opened->fd, joined(root, what.parts, what.parts.size()), out);
        std::ranges::sort(out, [](const listed_file &a, const listed_file &b) {
            if (a.seconds != b.seconds) return a.seconds < b.seconds;
            if (a.nanoseconds != b.nanoseconds) return a.nanoseconds < b.nanoseconds;
            return a.path < b.path;
        });
        return out;
    }
}
