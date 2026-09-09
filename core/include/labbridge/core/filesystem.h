#pragma once

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 9
#include <experimental/filesystem>
namespace labbridge::core {
namespace fs = std::experimental::filesystem;
}
#else
#include <filesystem>
namespace labbridge::core {
namespace fs = std::filesystem;
}
#endif

namespace labbridge::core {

// 判断 path 是否位于 root 之内（含等于 root 本身），按路径分量逐段比较。
inline bool is_within(const fs::path& path, const fs::path& root) {
    auto path_part = path.begin();
    for (auto root_part = root.begin(); root_part != root.end();
         ++root_part, ++path_part) {
        if (path_part == path.end() || *path_part != *root_part) {
            return false;
        }
    }
    return true;
}

}  // namespace labbridge::core
