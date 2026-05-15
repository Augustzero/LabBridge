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
