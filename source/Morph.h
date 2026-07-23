#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace UsmMorph {

constexpr uint32_t PCM_MAGIC = 0x204D4350u;
constexpr uint32_t PCM_VERSION = 0x00000601u;
constexpr size_t SECTION_RECORD_SIZE = 0x88;
constexpr size_t SECTION_STREAM_COUNT = 32;

struct SparseVec3Stream {
    std::vector<std::array<float, 3>> values;
    std::vector<uint8_t> changed;
    size_t encoded_bytes = 0;
};

struct SectionTarget {
    uint32_t vertex_count = 0;
    uint32_t channel_mask = 0;
    std::array<uint32_t, SECTION_STREAM_COUNT> stream_offsets{};
    SparseVec3Stream position;
};

struct TargetSet {
    uint32_t kind = 0;
    std::vector<SectionTarget> sections;
};

struct File {
    bool valid = false;
    uint32_t version = 0;
    uint32_t name_hash = 0;
    std::string name;
    std::vector<TargetSet> sets;
    std::vector<std::string> warnings;
};

namespace Detail {

static inline bool read_u16(const std::vector<uint8_t>& bytes, size_t offset, uint16_t& value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return true;
}

static inline bool read_u32(const std::vector<uint8_t>& bytes, size_t offset, uint32_t& value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return true;
}

static inline bool resolve(uint32_t relative, uint32_t image_base, size_t size, size_t& absolute) {
    if (!relative) return false;
    const int64_t resolved = static_cast<int64_t>(relative) - static_cast<int64_t>(image_base);
    if (resolved < 0 || static_cast<uint64_t>(resolved) >= size) return false;
    absolute = static_cast<size_t>(resolved);
    return true;
}

static inline bool decode_sparse_vec3(const std::vector<uint8_t>& bytes,
                                      size_t stream_offset,
                                      uint32_t vertex_count,
                                      SparseVec3Stream& out,
                                      std::string& error) {
    out.values.assign(vertex_count, {0.0f, 0.0f, 0.0f});
    out.changed.assign(vertex_count, 0);
    size_t source = stream_offset;
    uint64_t destination = 0;
    uint64_t guard = 0;
    for (;;) {
        uint16_t changed_count = 0, skipped_count = 0;
        if (!read_u16(bytes, source, changed_count) ||
            !read_u16(bytes, source + 2, skipped_count)) {
            error = "Sparse morph run header is truncated";
            return false;
        }
        source += 4;
        if (destination + changed_count > vertex_count) {
            error = "Sparse morph changed run exceeds its mesh section";
            return false;
        }
        const uint64_t value_bytes = static_cast<uint64_t>(changed_count) * 12ull;
        if (value_bytes > std::numeric_limits<size_t>::max() ||
            source > bytes.size() || value_bytes > bytes.size() - source) {
            error = "Sparse morph float3 payload is truncated";
            return false;
        }
        for (uint32_t i = 0; i < changed_count; ++i) {
            std::array<float, 3> value{};
            std::memcpy(value.data(), bytes.data() + source + static_cast<size_t>(i) * 12, 12);
            out.values[static_cast<size_t>(destination) + i] = value;
            out.changed[static_cast<size_t>(destination) + i] = 1;
        }
        source += static_cast<size_t>(value_bytes);
        destination += changed_count;
        if (skipped_count == 0) break;
        if (destination + skipped_count > vertex_count) {
            error = "Sparse morph skipped run exceeds its mesh section";
            return false;
        }
        destination += skipped_count;
        if (++guard > static_cast<uint64_t>(vertex_count) + 1) {
            error = "Sparse morph run chain does not terminate";
            return false;
        }
    }
    if (destination != vertex_count) {
        error = "Sparse morph run chain does not cover the complete mesh section";
        return false;
    }
    out.encoded_bytes = source - stream_offset;
    return true;
}

}

static inline File Parse(const std::vector<uint8_t>& bytes) {
    using namespace Detail;
    File result;
    uint32_t magic = 0, item_count = 0, item_table_rel = 0, image_base = 0;
    if (!read_u32(bytes, 0, magic) || magic != PCM_MAGIC ||
        !read_u32(bytes, 4, result.version) || result.version != PCM_VERSION ||
        !read_u32(bytes, 8, item_count) ||
        !read_u32(bytes, 12, item_table_rel) ||
        !read_u32(bytes, 16, image_base)) {
        result.warnings.push_back("Invalid PCMORPH 0x601 header");
        return result;
    }
    size_t item_table = 0;
    if (item_count == 0 || item_count > 4096 ||
        !resolve(item_table_rel, image_base, bytes.size(), item_table) ||
        item_table > bytes.size() || static_cast<uint64_t>(item_count) * 12ull > bytes.size() - item_table) {
        result.warnings.push_back("PCMORPH item table is out of range");
        return result;
    }

    size_t root = 0;
    for (uint32_t item = 0; item < item_count; ++item) {
        const size_t record = item_table + static_cast<size_t>(item) * 12;
        if (bytes[record + 3] != 3) continue;
        uint32_t root_rel = 0;
        read_u32(bytes, record + 4, root_rel);
        if (resolve(root_rel, image_base, bytes.size(), root)) break;
    }
    if (!root || root > bytes.size() || 20 > bytes.size() - root) {
        result.warnings.push_back("PCMORPH has no valid type-3 root");
        return result;
    }

    uint32_t name_rel = 0, set_count = 0, sets_rel = 0;
    read_u32(bytes, root + 0, name_rel);
    read_u32(bytes, root + 4, set_count);
    read_u32(bytes, root + 8, sets_rel);
    size_t name_offset = 0;
    if (resolve(name_rel, image_base, bytes.size(), name_offset) && name_offset + 4 <= bytes.size()) {
        read_u32(bytes, name_offset, result.name_hash);
        const size_t begin = name_offset + 4;
        size_t end = begin;
        while (end < bytes.size() && end - begin < 28 && bytes[end]) ++end;
        result.name.assign(reinterpret_cast<const char*>(bytes.data() + begin), end - begin);
    }

    size_t sets_offset = 0;
    if (set_count == 0 || set_count > 65536 ||
        !resolve(sets_rel, image_base, bytes.size(), sets_offset) ||
        sets_offset > bytes.size() || static_cast<uint64_t>(set_count) * 12ull > bytes.size() - sets_offset) {
        result.warnings.push_back("PCMORPH target-set table is out of range");
        return result;
    }

    result.sets.reserve(set_count);
    for (uint32_t set_index = 0; set_index < set_count; ++set_index) {
        const size_t set_record = sets_offset + static_cast<size_t>(set_index) * 12;
        uint32_t section_count = 0, sections_rel = 0;
        TargetSet set;
        read_u32(bytes, set_record + 0, set.kind);
        read_u32(bytes, set_record + 4, section_count);
        read_u32(bytes, set_record + 8, sections_rel);
        size_t sections_offset = 0;
        if (section_count > 4096 ||
            !resolve(sections_rel, image_base, bytes.size(), sections_offset) ||
            sections_offset > bytes.size() ||
            static_cast<uint64_t>(section_count) * SECTION_RECORD_SIZE > bytes.size() - sections_offset) {
            result.warnings.push_back("PCMORPH set " + std::to_string(set_index) +
                                      " section table is out of range");
            return result;
        }
        set.sections.resize(section_count);
        for (uint32_t section_index = 0; section_index < section_count; ++section_index) {
            const size_t section_record = sections_offset +
                static_cast<size_t>(section_index) * SECTION_RECORD_SIZE;
            auto& section = set.sections[section_index];
            read_u32(bytes, section_record + 0, section.vertex_count);
            read_u32(bytes, section_record + 4, section.channel_mask);
            for (size_t stream = 0; stream < SECTION_STREAM_COUNT; ++stream)
                read_u32(bytes, section_record + 8 + stream * 4, section.stream_offsets[stream]);

            if ((section.channel_mask & 1u) && section.stream_offsets[0]) {
                size_t stream_offset = 0;
                std::string error;
                if (!resolve(section.stream_offsets[0], image_base, bytes.size(), stream_offset) ||
                    !decode_sparse_vec3(bytes, stream_offset, section.vertex_count,
                                        section.position, error)) {
                    result.warnings.push_back("PCMORPH set " + std::to_string(set_index) +
                        " section " + std::to_string(section_index) + ": " +
                        (error.empty() ? "position stream offset is out of range" : error));
                    return result;
                }
            }
        }
        result.sets.push_back(std::move(set));
    }
    result.valid = true;
    return result;
}

}
