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
constexpr auto DUMMY_PATH = "/dev/KSU_DUMMY";

#define PRINT_ERR cerr << strerror(errno) << ":"

struct dummy_dir {
private:
    bool success = false;

    void create_if_need() {
        if (mkdir(DUMMY_PATH, 0) == -1) {
            perror("mkdir /dev/KSU_DUMMY");
            return;
        }
        if (mount(SOURCE, DUMMY_PATH, "tmpfs", MS_RDONLY, "size=0") == -1) {
            perror("mount dummy");
            return;
        }
        success = true;
    }

public:
    optional<string> get_dummy_path() {
        if (!success)
            create_if_need();
        if (!success)
            return nullptr;
        return DUMMY_PATH;
    }

    ~dummy_dir() {
        if (success) {
            if (umount2(DUMMY_PATH, MNT_DETACH) == -1) {
                perror("umount dummy");
            }
            if (rmdir(DUMMY_PATH) == -1) {
                perror("rmdir dummy");
            }
        }
    }
};

bool mount_ro_overlay(const string &dest, const vector<string> &lowers) {
    auto tree = MountNode::fromProc("self");
    if (tree == nullptr) return false;
    auto mount_for_path = MountNode::findMountForPath(tree, dest);
    vector<MountNodePtr> mount_seq;
    vector<int> fds;
    struct stat s{};
    dummy_dir dummy;
    MountNode::findTopMostMountsUnderPath(mount_seq, mount_for_path, dest);
    for (auto &node: mount_seq) {
        node->print(cout);
        auto fd = open(node->mount_point.c_str(), O_RDWR | O_PATH);
        if (fd == -1) {
            std::perror("open");
            return false;
        }
        fds.push_back(fd);
    }
    // pause();
    bool first = true;
    for (int i = mount_seq.size() - 1; i >= 0; i--) {
        auto &node = mount_seq[i];
        auto &fd = fds[i];
        int lower_count = 0;
        string src, mount_point, lower_dir;
        ostringstream option_s, lower_dirs;
        if (first) {
            src = dest;
            mount_point = dest;
            first = false;
        } else {
            src = "/proc/self/fd/" + std::to_string(fd);
            mount_point = node->mount_point;
            if (stat(mount_point.c_str(), &s) == -1) {
                if (errno == ENOENT || errno == ENOTDIR) {
                    cout << "skip " << mount_point << " because it does not exists" << endl;
                    continue;
                } else {
                    PRINT_ERR << " while stat " << mount_point << endl;
                    return false;
                }
            }
            if (!S_ISDIR(s.st_mode)) {
                cout << "skip " << mount_point << " because it is not a dir" << endl;
                continue;
            }
        }
        for (auto & lower: lowers) {
            lower_dir = lower + mount_point;
            if (stat(lower_dir.c_str(), &s) == -1) {
                if (errno == ENOENT || errno == ENOTDIR) {
                    cout << mount_point << ": " << lower_dir << " does not exists" << endl;
                    continue;
                } else {
                    PRINT_ERR << " while stat " << lower_dir << endl;
                    return false;
                }
            } else if (!S_ISDIR(s.st_mode)) {
                cout << mount_point << ": " << lower_dir << " is not dir" << endl;
                continue;
            }
            lower_count++;
            if (lower_count > 1)
                lower_dirs << ":";
            lower_dirs << lower_dir;
        }
        string option;
        option_s << "lowerdir=";
        if (lower_count == 0) {
            cout << mount_point << ": " << " needs dummy" << endl;
            auto dummy_path = dummy.get_dummy_path();
            if (dummy_path.has_value()) {
                option_s << src << ":" << dummy_path.value();
            } else {
                return false;
            }
        } else {
            option_s << lower_dirs.str() << ":" << src;
        }
        option = option_s.str();
        cout << "mounting " << mount_point << ",option:" << option << endl;
        if (mount(SOURCE, mount_point.c_str(), "overlay", MS_RDONLY, option.c_str()) == - 1) {
            PRINT_ERR << " failed to mount " << mount_point << " option " << option << endl;
            cout << " trying fallback bind mount" << endl;
            if (mount(src.c_str(), mount_point.c_str(), nullptr, MS_BIND, nullptr) == -1) {
                PRINT_ERR << " fallback failed while bind mount " << src << "to" << mount_point << endl;
                return false;
            }
        }
    }
    for (auto &fd: fds) {
        close(fd);
    }
    return true;
}

bool umount_ro_overlay() {
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
