#pragma once

#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <memory>
#include <functional>

#include <sys/sysmacros.h>

#ifdef ANDROID

#pragma once

template <typename T>
class reversed_container {
public:
    reversed_container(T &base) : base(base) {}
    decltype(std::declval<T>().rbegin()) begin() { return base.rbegin(); }
    decltype(std::declval<T>().crbegin()) begin() const { return base.crbegin(); }
    decltype(std::declval<T>().crbegin()) cbegin() const { return base.crbegin(); }
    decltype(std::declval<T>().rend()) end() { return base.rend(); }
    decltype(std::declval<T>().crend()) end() const { return base.crend(); }
    decltype(std::declval<T>().crend()) cend() const { return base.crend(); }
private:
    T &base;
};

template <typename T>
reversed_container<T> reversed(T &base) {
    return reversed_container<T>(base);
}

#else
#include <ranges>
#define reversed std::ranges::reverse_view
#endif


struct MountNode;

using MountNodePtr = std::shared_ptr<MountNode>;
using mount_id_t = int;

struct MountNode {
    mount_id_t id = -1;
    mount_id_t parent_id = -1;
    dev_t st_dev = 0;
    std::string root;
    std::string mount_point;
    std::string mount_options;
    std::vector<std::string> optional_fields;
    std::string fs_type;
    std::string source;
    std::string super_options;
    std::vector<MountNodePtr> children;

    void print(std::ostream &os) const {
        os  << id << " " << parent_id << " "
            << major(st_dev) << ":" << minor(st_dev) << " "
            << root << " "
            << mount_point << " "
            << mount_options << " ";
        for (auto &field: optional_fields) {
            os << field << " ";
        }
        os  << "- "
            << fs_type << " "
            << source << " "
            << super_options
            << std::endl;
    }

    void print_tree(std::ostream &os) const {
        print_tree(0, os);
    }

    //NOLINTNEXTLINE
    void print_tree(int level, std::ostream &os) const {
        os << std::string(level, ' ');
        print(os);
        for (auto &c: children) {
            c->print_tree(level + 1, os);
        }
    }

    /**
     * Find the top-most (effective) mount point for given path
     * @param self
     * @param path
     * @return
     */
    static MountNodePtr findMountForPath(const MountNodePtr &self, const std::string &path) {
        if (path.starts_with(self->mount_point)) {
            for (auto & node : reversed(self->children)) {
                auto new_result = findMountForPath(node, path);
                if (new_result != nullptr) {
                    return new_result;
                }
            }
            return self;
        } else {
            return nullptr;
        }
    }

    /**
     * Find all top-most child mounts for path.
     * @param self
     * @param path normalized path
     * @return List of mounts (in reverse order)
     */
    static std::vector<MountNodePtr> findChildMountForPath(const MountNodePtr &self, const std::string &path) {
        std::vector<MountNodePtr> childs;
        findChildMountForPath(childs, self, path);
        return childs;
    }

    static void findChildMountForPath(std::vector<MountNodePtr> &children, const MountNodePtr &self, const std::string &path) {
        auto mount_for_path = findMountForPath(self, path);
        if (mount_for_path == nullptr) return;
        for (auto & node : reversed(mount_for_path->children)) {
            if (node->mount_point.starts_with(path + "/")) {
                children.push_back(findMountForPath(node, node->mount_point));
            }
        }
    }

    static void findTopMostMountsUnderPath(std::vector<MountNodePtr> &seq, const MountNodePtr &self, const std::string &path) {
        if (self != nullptr) {
            auto children = self->findChildMountForPath(self, path);
            for (auto &c: children) {
                findTopMostMountsUnderPath(seq, c, c->mount_point);
            }
            seq.push_back(self);
        }
    }

    static MountNodePtr createMountTree(std::ifstream &file);

    static MountNodePtr fromProc(const std::string& name) {
        std::string file_name = "/proc/" + name + "/mountinfo";
        std::ifstream f{file_name};
        return createMountTree(f);
    }

    static void traversal(const MountNodePtr &node, const std::function<void(const MountNodePtr &)> &fn) {
        if (node != nullptr) {
            for (auto &n: reversed(node->children)) {
                traversal(n, fn);
            }
            fn(node);
        }
    }
};

MountNodePtr MountNode::createMountTree(std::ifstream &file) {
    // TODO: handle the case paths containing space (\040)
    std::string s;
    std::map<mount_id_t, MountNodePtr> node_map;
    bool is_valid = true;
    {
        int state = 0;
        std::string::size_type pos;
        MountNodePtr node;
        unsigned int major, minor;
        while (is_valid && file >> s) {
            switch (state) {
                case 0: // mount id
                    node = std::make_shared<MountNode>();
                    node->id = stoi(s);
                    break;
                case 1: // parent Id
                    node->parent_id = stoi(s);
                    break;
                case 2: // major:minor
                    pos = s.find(':');
                    if (pos == std::string::npos) {
                        is_valid = false;
                        break;
                    }
                    major = stoi(s.substr(0, pos));
                    minor = stoi(s.substr(pos + 1));
                    node->st_dev = makedev(major, minor);
                    break;
                case 3: // root
                    node->root = s;
                    break;
                case 4: // mount point
                    node->mount_point = s;
                    break;
                case 5: // mount options
                    node->mount_options = s;
                    break;
                case 6: // optional fields or separator
                    if (s == "-") {
                        // separator
                    } else {
                        // optional fields
                        // there may be more than one field
                        // for example, shared and slave mount
                        node->optional_fields.push_back(s);
                        continue;
                    }
                    break;
                case 7: // filesystem type
                    node->fs_type = s;
                    break;
                case 8: // mount source
                    node->source = s;
                    break;
                case 9: // super options
                    node->super_options = s;

                    // finalize
                    node_map.emplace(node->id, node);
                    node.reset();
                    state = 0;
                    continue;
                default: // error
                    is_valid = false;
            }
            state++;
        }
    }
    if (!is_valid) return nullptr;
    MountNodePtr root;
    for (const auto &[_, node]: node_map) {
        auto parent = node_map.find(node->parent_id);
        if (parent != node_map.end()) {
            parent->second->children.push_back(node);
        }
        if (root == nullptr && (
                node->id == node->parent_id
                || parent == node_map.end())) {
            root = node;
        }
    }
    return root;
}