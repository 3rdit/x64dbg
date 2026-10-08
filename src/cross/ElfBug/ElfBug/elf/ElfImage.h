#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ElfBug
{
    inline uint64_t ElfPageFloor(const uint64_t value)
    {
        return value & ~static_cast<uint64_t>(0xfff);
    }

    inline uint64_t ElfPageCeil(const uint64_t value)
    {
        return ElfPageFloor(value + 0xfff);
    }

    struct ElfSegment
    {
        uint64_t vaddr = 0;
        uint64_t memsz = 0;
        uint64_t offset = 0;
        uint64_t filesz = 0;
        uint32_t flags = 0;

        [[nodiscard]] uint64_t End() const { return vaddr + std::max(memsz, filesz); }
    };

    struct ElfSection
    {
        std::string name;
        uint64_t addr = 0;
        uint64_t size = 0;
    };

    // A file offset; a tracee reader reads base + offset, valid only where the image is mapped linearly.
    using ElfReader = std::function<bool(uint64_t offset, void* buffer, size_t size)>;

    class ElfImage
    {
    public:
        enum class Parts
        {
            Headers,
            HeadersAndSections,
        };

        static std::optional<ElfImage> Parse(const ElfReader & read, Parts parts);

        [[nodiscard]] const std::vector<ElfSegment> & Segments() const { return mSegments; }
        [[nodiscard]] const std::vector<ElfSection> & Sections() const { return mSections; }
        [[nodiscard]] std::span<const std::byte> HeaderBytes() const { return mHeaderBytes; }
        [[nodiscard]] std::optional<uint64_t> LoadBias(uint64_t mappingStart, uint64_t mappingOffset) const;
        [[nodiscard]] uint64_t End() const;

    private:
        std::vector<ElfSegment> mSegments;
        std::vector<ElfSection> mSections;
        std::vector<std::byte> mHeaderBytes;
    };
}
