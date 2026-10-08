#pragma once

#include <sys/types.h>
#include <compare>

namespace ElfBug
{
    // A file by device and inode, as stat and /proc/<pid>/maps report it.
    struct ImageId
    {
        dev_t dev = 0;
        ino_t ino = 0;

        [[nodiscard]] bool Known() const { return ino != 0; }
        auto operator<=>(const ImageId &) const = default;
    };
}
