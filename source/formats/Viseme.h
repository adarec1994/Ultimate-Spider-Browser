#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace UsmViseme {

constexpr uint32_t RETAIL_CHANNEL_COUNT = 15;
constexpr uint32_t RETAIL_FORMAT = 11;
constexpr uint32_t RETAIL_SAMPLE_RATE = 30;
constexpr size_t HEADER_SIZE = 24;

struct Stream {
    bool valid = false;
    uint32_t resource_hash = 0;
    std::string name;
    uint32_t channel_count = 0;
    uint32_t format = 0;
    uint32_t frame_count = 0;
    uint32_t sample_rate = 0;
    float field_10 = 0.0f;
    uint32_t serialized_data_pointer = 0;
    std::vector<float> weights;
    std::vector<std::string> warnings;

    const float* frame(uint32_t index) const {
        if (!valid || index >= frame_count || channel_count == 0) return nullptr;
        const size_t offset = static_cast<size_t>(index) * channel_count;
        return offset <= weights.size() && channel_count <= weights.size() - offset
            ? weights.data() + offset : nullptr;
    }
};

static inline Stream Parse(const std::vector<uint8_t>& bytes) {
    Stream result;
    if (bytes.size() < HEADER_SIZE) {
        result.warnings.push_back("Viseme stream header is truncated");
        return result;
    }
    std::memcpy(&result.channel_count, bytes.data() + 0, 4);
    std::memcpy(&result.format, bytes.data() + 4, 4);
    std::memcpy(&result.frame_count, bytes.data() + 8, 4);
    std::memcpy(&result.sample_rate, bytes.data() + 12, 4);
    std::memcpy(&result.field_10, bytes.data() + 16, 4);
    std::memcpy(&result.serialized_data_pointer, bytes.data() + 20, 4);

    if (result.channel_count == 0 || result.channel_count > 4096 ||
        result.frame_count == 0 || result.frame_count > 1000000 ||
        result.sample_rate == 0 || result.sample_rate > 10000) {
        result.warnings.push_back("Viseme stream header values are out of range");
        return result;
    }
    const uint64_t value_count = static_cast<uint64_t>(result.channel_count) * result.frame_count;
    const uint64_t expected_size = HEADER_SIZE + value_count * sizeof(float);
    if (value_count > std::numeric_limits<size_t>::max() / sizeof(float) ||
        expected_size != bytes.size()) {
        result.warnings.push_back("Viseme stream byte extent does not match its dense float matrix");
        return result;
    }
    result.weights.resize(static_cast<size_t>(value_count));
    std::memcpy(result.weights.data(), bytes.data() + HEADER_SIZE,
                result.weights.size() * sizeof(float));
    for (float value : result.weights) {
        if (!std::isfinite(value)) {
            result.warnings.push_back("Viseme stream contains a non-finite weight");
            result.weights.clear();
            return result;
        }
    }
    result.valid = true;
    return result;
}

}
