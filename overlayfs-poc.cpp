#include "mount-scan.h"

#include <optional>

#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <sstream>

using namespace std;

constexpr auto SOURCE = "KSU";

#define PRINT_ERR cerr << strerror(errno) << ":"

bool mount_ro_overlay(const string &dest, const vector<string> &lowers) {
    auto tree = MountNode::fromProc("self");
    if (tree == nullptr) return false;
    auto mount_for_path = MountNode::findMountForPath(tree, dest);
    vector<MountNodePtr> mount_seq;
    vector<int> fds;
    struct stat s{};
    MountNode::findTopMostMountsUnderPath(mount_seq, mount_for_path, dest);
    for (auto &node: mount_seq) {
        node->print(cout);
        auto fd = open(node->mount_point.c_str(), O_PATH);
        if (fd == -1) {
            std::perror("open");
            return false;
        }
        fds.push_back(fd);
    }
    bool first = true;
    for (int i = mount_seq.size() - 1; i >= 0; i--) {
        auto &node = mount_seq[i];
        auto &fd = fds[i];
        int lower_count = 0;
        string src, mount_point, lower_dir;
        ostringstream option_s, lower_dirs;
        bool stock_is_dir = false;
        // path on lower overlayfs is dir
        bool modified_is_dir = false;
        if (first) {
            mount_point = dest; // node->mount_point might not be the path we need (e.g. the mount point of /system is / )
            src = dest;
        } else {
            mount_point = node->mount_point;
            src = "/proc/self/fd/" + std::to_string(fd);
        }
        if (stat(mount_point.c_str(), &s) == -1) {
            if (errno == ENOENT || errno == ENOTDIR) {
                cout << "skip " << mount_point << " because it does not exists" << endl;
                first = false; // although root dir is unlikely not exists
                continue;
            } else {
                PRINT_ERR << " while stat " << mount_point << endl;
                return false;
            }
        }
        if (S_ISDIR(s.st_mode)) {
            modified_is_dir = true;
        }
        if (fstat(fd, &s) == -1) {
            PRINT_ERR << " while stat fd " << fd << endl;
            return false;
        }
        if (S_ISDIR(s.st_mode)) {
            stock_is_dir = true;
        }

        for (auto & lower: lowers) {
            lower_dir = lower + mount_point;
            if (stat(lower_dir.c_str(), &s) == -1) {
                if (errno == ENOENT || errno == ENOTDIR) {
                    cout << mount_point << ": module " << lower_dir << " does not exists" << endl;
                    continue;
                } else {
                    PRINT_ERR << " while stat " << lower_dir << endl;
                    return false;
                }
            }
            // some module's root is not dir, which is invalid
            if (!S_ISDIR(s.st_mode) && first) {
                cout << lower_dir << " is an invalid module" << endl;
                continue;
            }
            lower_count++;
            if (lower_count > 1)
                lower_dirs << ":";
            lower_dirs << lower_dir;
        }
        // no module modifications, bind mount!
        if (lower_count == 0) {
            // we don't need to mount anything if root is not modified
            if (first) {
                cout << "no valid modules, skip" << endl;
                return true;
            }
            cout << "no module modifies " << mount_point << " , bind mount" << endl;
            if (mount(src.c_str(), mount_point.c_str(), nullptr, MS_BIND, nullptr) == -1) {
                PRINT_ERR << " bind mount " << src << " to " << mount_point << " because nothing get modified" << endl;
                return false;
            }
        } else if (stock_is_dir && modified_is_dir) {
            string option;
            option_s << "lowerdir=";
            option_s << lower_dirs.str() << ":" << src;
            option = option_s.str();
            cout << "mounting " << mount_point << ",option:" << option << endl;
            // some filesystem (e.g. vfat) cannot be the lowerdir of overlayfs
            if (mount(SOURCE, mount_point.c_str(), "overlay", MS_RDONLY, option.c_str()) == - 1) {
                PRINT_ERR << " failed to mount " << mount_point << " option " << option << endl;
                cout << " trying fallback bind mount" << endl;
                if (mount(src.c_str(), mount_point.c_str(), nullptr, MS_BIND, nullptr) == -1) {
                    PRINT_ERR << " fallback failed while bind mount " << src << "to" << mount_point << endl;
                    return false;
                }
            }
        }
        if (first) {
            first = false;
        }
    }
    for (auto &fd: fds) {
        close(fd);
    }
    return true;
}

bool umount_ro_overlay() {
    // TODO: only unmount top-level mount point is okay
    auto tree = MountNode::fromProc("self");
    bool success = true;
    MountNode::traversal(tree, [&](auto &node) {
        if (node->source == SOURCE) {
            string reason = "umounting " + node->mount_point;
            cout << reason << endl;
            if (umount2(node->mount_point.c_str(), MNT_DETACH) == -1) {
                std::perror(reason.c_str());
                success = false;
            }
        }
    });
    return success;
}

#include <string_view>

int main(int argc, char **argv) {
    if (argc >= 3) {
        vector<string> modules;
        for (int i = 2; i < argc; i++) {
            modules.emplace_back(argv[i]);
        }
        auto result = mount_ro_overlay(argv[1], modules);
        cout << (result ? "mount success" : "mount failed") << endl;
    } else if (argc == 2) {
        if (argv[1] == "--revert"sv) {
            auto result = umount_ro_overlay();
            cout << (result ? "umount success" : "umount failed") << endl;
        } else if (argv[1] == "--tree"sv) {
            auto tree = MountNode::fromProc("self");
            tree->print_tree(cout);
        }
    }
    return 0;
}
