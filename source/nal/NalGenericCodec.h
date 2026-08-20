#pragma once

#include "NalAnimCodec.h"
#include "NalSkeleton.h"
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct NalGenericDecodedAnimation {
    std::vector<std::array<float, 16>> default_world_matrices;
    std::vector<std::vector<std::array<float, 16>>> world_frames;
    std::vector<std::string> warnings;
    bool complete = false;
};

namespace NalGenericCodecDetail {

static inline size_t align_up(size_t value, size_t alignment) {
    if (alignment == 0) alignment = 1;
    return (value + alignment - 1) & ~(alignment - 1);
}

static inline bool enabled(const std::vector<uint32_t>& mask, uint32_t slot) {
    const size_t word = slot >> 5;
    return word < mask.size() && (mask[word] & (uint32_t{1} << (slot & 31))) != 0;
}

static inline std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static inline int sign_magnitude(uint32_t raw) {
    return (raw & 1u) ? -static_cast<int>(raw >> 1) : static_cast<int>(raw >> 1);
}

static inline int signed_three(uint32_t value) {
    int result = static_cast<int>(value & 7u);
    if ((result & 4) == 0) result -= 7;
    return result;
}

static inline int scalar_initial_delta(NalBitStream& bits, bool wide) {
    if (wide) {
        const bool full = bits.read_bits(1) != 0;
        if (full) return static_cast<int32_t>(bits.read_bits(32));
        return static_cast<int>(bits.read_bits(16)) - 0x8000;
    }

    const uint32_t peek = bits.peek_bits(12);
    if ((peek & 0x1Fu) != 0) {
        if ((peek & 1u) != 0) {
            const int shift = static_cast<int>((peek >> 1) & 0xFu) + 1;
            const int mantissa = signed_three(peek >> 5);
            bits.consume(8);
            return mantissa << shift;
        }
        const int result = static_cast<int>((peek >> 1) & 0xFu) - 8;
        bits.consume(5);
        return result;
    }

    const int shift = static_cast<int>((peek >> 5) & 0xFu) + 17;
    const int mantissa = signed_three(peek >> 9);
    bits.consume(12);
    return mantissa << shift;
}

static inline std::vector<int> decode_second_deltas(NalBitStream& bits,
                                                     int codec,
                                                     int count) {
    std::vector<int> result(std::max(0, count), 0);
    if (codec < 0 || codec >= 64) return result;
    int zeros = 0;
    for (int i = 0; i < count; ++i) {
        if (zeros != 0) {
            --zeros;
            continue;
        }
        const DecResult decoded = DECODER_TABLE[codec](bits);
        result[i] = decoded.val;
        zeros = std::max(0, decoded.run - 1);
    }
    return result;
}

static inline std::vector<float> decode_scalar(const uint8_t* data,
                                                size_t size,
                                                int frame_count,
                                                float scale) {
    std::vector<float> out(std::max(0, frame_count), 0.0f);
    if (frame_count <= 0 || size == 0) return out;

    NalBitStream bits(data, size);
    int codec = static_cast<int>(bits.read_bits(5));
    const bool negative_scale = scale < 0.0f;
    if (negative_scale) codec += 32;
    scale = std::fabs(scale);

    const bool base_is_full = bits.read_bits(1) != 0;
    const int base = base_is_full
        ? static_cast<int32_t>(bits.read_bits(32))
        : static_cast<int>(bits.read_bits(16)) - 0x8000;
    const int initial_delta = scalar_initial_delta(bits, negative_scale);

    out[0] = static_cast<float>(base) * scale;
    if (frame_count == 1) return out;
    int previous_previous = base;
    int previous = base + initial_delta;
    out[1] = static_cast<float>(previous) * scale;

    const auto seconds = decode_second_deltas(bits, codec, frame_count - 2);
    for (int frame = 2; frame < frame_count; ++frame) {
        const int current = seconds[frame - 2] + 2 * previous - previous_previous;
        out[frame] = static_cast<float>(current) * scale;
        previous_previous = previous;
        previous = current;
    }
    return out;
}

struct QuatAxisState {
    int codec = 0;
    int delta = 0;
    int bitpos = 0;
};

static inline int quat_initial_delta(NalBitStream& bits, bool wide) {
    if (wide) {
        const bool full = bits.read_bits(1) != 0;
        if (full) return static_cast<int32_t>(bits.read_bits(32));
        return static_cast<int>(bits.read_bits(16)) - 0x8000;
    }

    const uint32_t peek = bits.peek_bits(12);
    if ((peek & 0x1Fu) != 0) {
        if ((peek & 1u) != 0) {
            const int shift = static_cast<int>((peek >> 1) & 7u) + 1;
            const int mantissa = signed_three(peek >> 4);
            bits.consume(7);
            return mantissa << shift;
        }
        const int result = static_cast<int>((peek >> 1) & 0xFu) - 8;
        bits.consume(5);
        return result;
    }

    const int shift = static_cast<int>((peek >> 5) & 0xFu) + 9;
    const int mantissa = signed_three(peek >> 9);
    bits.consume(12);
    return mantissa << shift;
}

static inline void quat_normalize(float q[4]) {
    const float length2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (length2 <= 1.0e-30f) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    const float inverse = 1.0f / std::sqrt(length2);
    q[0] *= inverse; q[1] *= inverse; q[2] *= inverse; q[3] *= inverse;
}

static inline std::array<float, 4> quat_from_xyz(float x, float y, float z) {
    std::array<float, 4> q{x, y, z,
        std::sqrt(std::fabs(1.0f - (x * x + y * y + z * z)))};
    quat_normalize(q.data());
    return q;
}

static inline std::array<float, 4> quat_multiply(const std::array<float, 4>& a,
                                                 const std::array<float, 4>& b) {

    std::array<float, 4> out;
    nal_quat_mul(a.data(), b.data(), out.data());
    quat_normalize(out.data());
    return out;
}

static inline std::vector<std::array<float, 4>> decode_quaternion(
    const uint8_t* data, size_t size, int frame_count, float scale) {
    std::vector<std::array<float, 4>> out(std::max(0, frame_count));
    if (frame_count <= 0 || size == 0) return out;

    NalBitStream bits(data, size);
    const bool negative_scale = scale < 0.0f;
    scale = std::fabs(scale);
    constexpr int widths[4] = {3, 5, 8, 21};
    float initial[3] = {};
    QuatAxisState axes[3];

    for (int axis = 0; axis < 3; ++axis) {
        int explicit_skip = 0;
        if (axis < 2 && bits.read_bits(1) != 0)
            explicit_skip = static_cast<int>(bits.read_bits(11)) + 256;

        axes[axis].codec = static_cast<int>(bits.read_bits(5)) +
                           (negative_scale ? 32 : 0);
        const int width = widths[bits.read_bits(2) & 3u];
        const int base = sign_magnitude(bits.read_bits(width));
        initial[axis] = static_cast<float>(base) * scale * 0.25f;
        axes[axis].delta = quat_initial_delta(bits, negative_scale);
        axes[axis].bitpos = bits.bitpos;

        if (axis < 2) {
            if (explicit_skip != 0) {
                bits.bitpos += explicit_skip;
            } else {
                NalBitStream scan(data, size);
                scan.bitpos = axes[axis].bitpos;
                (void)decode_second_deltas(scan, axes[axis].codec,
                                           std::max(0, frame_count - 2));
                bits.bitpos = scan.bitpos;
            }
        }
    }

    std::array<float, 4> current = quat_from_xyz(initial[0], initial[1], initial[2]);
    out[0] = current;
    if (frame_count == 1) return out;

    int running_delta[3] = {axes[0].delta, axes[1].delta, axes[2].delta};
    current = quat_multiply(
        quat_from_xyz(running_delta[0] * scale,
                      running_delta[1] * scale,
                      running_delta[2] * scale), current);
    out[1] = current;

    std::vector<int> seconds[3];
    for (int axis = 0; axis < 3; ++axis) {
        NalBitStream axis_bits(data, size);
        axis_bits.bitpos = axes[axis].bitpos;
        seconds[axis] = decode_second_deltas(axis_bits, axes[axis].codec,
                                             std::max(0, frame_count - 2));
    }
    for (int frame = 2; frame < frame_count; ++frame) {
        for (int axis = 0; axis < 3; ++axis)
            running_delta[axis] += seconds[axis][frame - 2];
        current = quat_multiply(
            quat_from_xyz(running_delta[0] * scale,
                          running_delta[1] * scale,
                          running_delta[2] * scale), current);
        out[frame] = current;
    }
    return out;
}

static inline void identity(std::array<float, 16>& matrix) {
    matrix.fill(0.0f);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

static inline void multiply(const std::array<float, 16>& a,
                            const std::array<float, 16>& b,
                            std::array<float, 16>& out) {
    std::array<float, 16> result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result[column * 4 + row] =
                a[row] * b[column * 4] +
                a[4 + row] * b[column * 4 + 1] +
                a[8 + row] * b[column * 4 + 2] +
                a[12 + row] * b[column * 4 + 3];
        }
    }
    out = result;
}

static inline void set_rotation(const float* q, std::array<float, 16>& matrix) {

    float x = -q[0], y = -q[1], z = -q[2], w = q[3];
    const float xx = x*x, yy = y*y, zz = z*z;
    const float xy = x*y, xz = x*z, yz = y*z;
    const float wx = w*x, wy = w*y, wz = w*z;
    matrix[0] = 1.0f - 2.0f * (yy + zz);
    matrix[1] = 2.0f * (xy + wz);
    matrix[2] = 2.0f * (xz - wy);
    matrix[3] = 0.0f;
    matrix[4] = 2.0f * (xy - wz);
    matrix[5] = 1.0f - 2.0f * (xx + zz);
    matrix[6] = 2.0f * (yz + wx);
    matrix[7] = 0.0f;
    matrix[8] = 2.0f * (xz + wy);
    matrix[9] = 2.0f * (yz - wx);
    matrix[10] = 1.0f - 2.0f * (xx + yy);
    matrix[11] = 0.0f;
    matrix[15] = 1.0f;
}

static inline bool build_world_matrices(const NalSkeletonData& skeleton,
                                        const std::vector<uint8_t>& pose,
                                        std::vector<std::array<float, 16>>& matrices,
                                        std::string& error) {
    matrices.resize(skeleton.generic_matrix_count);
    for (auto& matrix : matrices) identity(matrix);
    size_t animated_cursor = 0;
    size_t constant_cursor = 0;
    size_t command_cursor = 0;

    auto source = [&](bool constant, size_t byte_count) -> const uint8_t* {
        const auto& bytes = constant ? skeleton.generic_const_pose : pose;
        size_t& cursor = constant ? constant_cursor : animated_cursor;
        if (cursor > bytes.size() || byte_count > bytes.size() - cursor) return nullptr;
        const uint8_t* result = bytes.data() + cursor;
        cursor += byte_count;
        return result;
    };

    while (command_cursor < skeleton.generic_matrix_commands.size()) {
        const uint8_t raw_opcode = skeleton.generic_matrix_commands[command_cursor++];
        const uint8_t opcode = raw_opcode & 0x7Fu;
        const bool constant = (raw_opcode & 0x80u) != 0;
        if (opcode == 0 || opcode == 1 || opcode == 2) {
            if (command_cursor >= skeleton.generic_matrix_commands.size()) {
                error = "Generic matrix command is truncated";
                return false;
            }
            const uint8_t destination = skeleton.generic_matrix_commands[command_cursor++];
            if (destination >= matrices.size()) {
                error = "Generic matrix command destination is out of range";
                return false;
            }
            if (opcode == 0) {
                const uint8_t* position = source(constant, 12);
                if (!position) { error = "Generic position source is out of range"; return false; }
                std::memcpy(&matrices[destination][12], position, 12);
            } else if (opcode == 1) {
                const uint8_t* quaternion = source(constant, 16);
                if (!quaternion) { error = "Generic quaternion source is out of range"; return false; }
                set_rotation(reinterpret_cast<const float*>(quaternion), matrices[destination]);
            } else {
                const uint8_t* po = source(constant, 28);
                if (!po) { error = "Generic PO source is out of range"; return false; }
                set_rotation(reinterpret_cast<const float*>(po), matrices[destination]);
                std::memcpy(&matrices[destination][12], po + 16, 12);
            }
            continue;
        }

        if (opcode == 12) {
            if (command_cursor + 2 > skeleton.generic_matrix_commands.size()) {
                error = "Generic parent command is truncated";
                return false;
            }
            const uint8_t parent = skeleton.generic_matrix_commands[command_cursor++];
            const uint8_t child = skeleton.generic_matrix_commands[command_cursor++];
            if (parent >= matrices.size() || child >= matrices.size()) {
                error = "Generic parent command index is out of range";
                return false;
            }
            multiply(matrices[parent], matrices[child], matrices[child]);
            continue;
        }

        error = "Unsupported generic matrix opcode " + std::to_string(raw_opcode);
        return false;
    }
    return true;
}

}

static inline NalGenericDecodedAnimation nal_decode_generic_animation(
    const std::vector<uint8_t>& blob,
    size_t animation_offset,
    int frame_count,
    const NalSkeletonData& skeleton) {
    using namespace NalGenericCodecDetail;
    NalGenericDecodedAnimation result;
    if (animation_offset + 0x80 > blob.size() || frame_count <= 0) {
        result.warnings.push_back("Generic animation header is truncated");
        return result;
    }
    if (skeleton.generic_component_slot_count == 0 ||
        skeleton.generic_default_pose.empty() ||
        skeleton.generic_matrix_commands.empty()) {
        result.warnings.push_back("Generic skeleton runtime payload is incomplete");
        return result;
    }

    auto u32 = [&](size_t offset, uint32_t& value) -> bool {
        if (offset + 4 > blob.size()) return false;
        std::memcpy(&value, blob.data() + offset, 4);
        return true;
    };
    uint32_t header_size = 0, header_align = 1, chunk_count = 0;
    uint32_t frames_per_chunk = 0, frame_stride = 0, frame_align = 1;
    if (!u32(animation_offset + 0x54, header_size) ||
        !u32(animation_offset + 0x58, header_align) ||
        !u32(animation_offset + 0x64, chunk_count) ||
        !u32(animation_offset + 0x68, frames_per_chunk) ||
        !u32(animation_offset + 0x70, frame_stride) ||
        !u32(animation_offset + 0x74, frame_align) ||
        chunk_count == 0 || frames_per_chunk == 0) {
        result.warnings.push_back("Generic animation chunk header is invalid");
        return result;
    }

    const size_t mask_offset = (animation_offset + 0x83u) & ~size_t(3);
    const size_t mask_word_count = (skeleton.generic_component_slot_count + 31u) / 32u;
    if (mask_offset + mask_word_count * 4 > blob.size()) {
        result.warnings.push_back("Generic animation component mask is truncated");
        return result;
    }
    std::vector<uint32_t> mask(mask_word_count);
    std::memcpy(mask.data(), blob.data() + mask_offset, mask_word_count * 4);

    size_t header_cursor = align_up(mask_offset + mask_word_count * 4, header_align);
    const size_t header_begin = header_cursor;
    if (header_begin + header_size > blob.size()) {
        result.warnings.push_back("Generic animation codec header is truncated");
        return result;
    }

    struct InfoRuntime {
        const NalSkeletonData::GenericComponentInfo* info = nullptr;
        float shared_scale = 1.0f;
    };
    std::vector<InfoRuntime> infos;
    infos.reserve(skeleton.generic_component_infos.size());
    for (const auto& info : skeleton.generic_component_infos) {
        const std::string type = lower(info.type_name);
        InfoRuntime runtime{&info, 1.0f};
        if (type.find("usmevent") == std::string::npos &&
            type.find("usmmorph") == std::string::npos) {
            header_cursor = align_up(header_cursor, 4);
            if (header_cursor + 4 > header_begin + header_size) {
                result.warnings.push_back("Generic component scale header is truncated");
                return result;
            }
            std::memcpy(&runtime.shared_scale, blob.data() + header_cursor, 4);
            header_cursor += 4;
        }
        if (type.find("trajectory") != std::string::npos) {
            for (uint32_t i = 0; i < info.component_count; ++i) {
                const uint32_t slot = info.first_component + i;
                if (enabled(mask, slot)) header_cursor += 28;
            }
        } else if (type.find("usmevent") != std::string::npos) {
            header_cursor = align_up(header_cursor, 2);
            for (uint32_t i = 0; i < info.component_count; ++i) {
                const uint32_t slot = info.first_component + i;
                if (!enabled(mask, slot)) continue;
                if (header_cursor + 4 > header_begin + header_size) {
                    result.warnings.push_back("Generic event header is truncated");
                    return result;
                }
                uint16_t record_count = 0;
                std::memcpy(&record_count, blob.data() + header_cursor + 2, 2);
                size_t object_size = 4;
                size_t record = header_cursor + 4;
                for (uint16_t r = 0; r < record_count; ++r) {
                    if (record + 4 > header_begin + header_size) break;
                    const size_t record_size = 12 + 4 * blob[record + 3];
                    object_size += record_size;
                    record += record_size;
                }

                header_cursor += 4 + object_size;
            }
        }
        if (header_cursor > header_begin + header_size) {
            result.warnings.push_back("Generic codec header consumed beyond its byte extent");
            return result;
        }
        infos.push_back(runtime);
    }

    const size_t chunk_table = align_up(header_begin + header_size, 4);
    const size_t chunk_data = chunk_table + static_cast<size_t>(chunk_count) * 4;
    if (chunk_data > blob.size()) {
        result.warnings.push_back("Generic animation chunk table is truncated");
        return result;
    }

    std::string matrix_error;
    if (!build_world_matrices(skeleton, skeleton.generic_default_pose,
                              result.default_world_matrices, matrix_error)) {
        result.warnings.push_back(matrix_error);
        return result;
    }

    result.world_frames.reserve(frame_count);
    int output_frame = 0;
    for (uint32_t chunk_index = 0;
         chunk_index < chunk_count && output_frame < frame_count;
         ++chunk_index) {
        uint32_t relative_chunk = 0;
        std::memcpy(&relative_chunk, blob.data() + chunk_table + chunk_index * 4, 4);
        size_t stream_cursor = align_up(chunk_data + relative_chunk, frame_align);
        const int this_frame_count = std::min<int>(frames_per_chunk, frame_count - output_frame);
        std::vector<std::vector<uint8_t>> poses(this_frame_count, skeleton.generic_default_pose);

        auto take_stream = [&](const uint8_t*& data, size_t& size) -> bool {
            if (stream_cursor >= blob.size()) return false;
            const uint8_t byte_count = blob[stream_cursor++];
            if (stream_cursor + byte_count > blob.size()) return false;
            data = blob.data() + stream_cursor;
            size = byte_count;
            stream_cursor += byte_count;
            return true;
        };

        for (const auto& runtime : infos) {
            const auto& info = *runtime.info;
            const std::string type = lower(info.type_name);
            for (uint32_t local = 0; local < info.component_count; ++local) {
                const uint32_t slot = info.first_component + local;
                if (!enabled(mask, slot)) continue;
                if (slot >= skeleton.generic_component_scales.size()) {
                    result.warnings.push_back("Generic component scale index is out of range");
                    return result;
                }
                const float scale = skeleton.generic_component_scales[slot] * runtime.shared_scale;
                const bool is_po = type.find("positionorient") != std::string::npos ||
                                   type.find("trajectory") != std::string::npos;
                const size_t pose_offset = static_cast<size_t>(info.pose_offset) +
                                           static_cast<size_t>(local) *
                                           (is_po ? 28u :
                                            type.find("quatern") != std::string::npos ? 16u :
                                            type.find("float3") != std::string::npos ? 12u : 4u);

                if (is_po) {
                    std::vector<float> positions[3];
                    for (int axis = 0; axis < 3; ++axis) {
                        const uint8_t* bytes = nullptr; size_t byte_count = 0;
                        if (!take_stream(bytes, byte_count)) {
                            result.warnings.push_back("Generic PO scalar stream is truncated");
                            return result;
                        }
                        positions[axis] = decode_scalar(bytes, byte_count, this_frame_count, scale);
                    }
                    const uint8_t* bytes = nullptr; size_t byte_count = 0;
                    if (!take_stream(bytes, byte_count)) {
                        result.warnings.push_back("Generic PO quaternion stream is truncated");
                        return result;
                    }
                    const auto quaternions = decode_quaternion(bytes, byte_count,
                                                               this_frame_count, scale);
                    for (int frame = 0; frame < this_frame_count; ++frame) {
                        if (pose_offset + 28 > poses[frame].size()) {
                            result.warnings.push_back("Generic PO pose offset is out of range");
                            return result;
                        }
                        std::memcpy(poses[frame].data() + pose_offset,
                                    quaternions[frame].data(), 16);
                        float position[3] = {positions[0][frame], positions[1][frame], positions[2][frame]};
                        std::memcpy(poses[frame].data() + pose_offset + 16, position, 12);
                    }
                } else if (type.find("quatern") != std::string::npos) {
                    const uint8_t* bytes = nullptr; size_t byte_count = 0;
                    if (!take_stream(bytes, byte_count)) {
                        result.warnings.push_back("Generic quaternion stream is truncated");
                        return result;
                    }
                    const auto values = decode_quaternion(bytes, byte_count,
                                                          this_frame_count, scale);
                    for (int frame = 0; frame < this_frame_count; ++frame) {
                        if (pose_offset + 16 > poses[frame].size()) {
                            result.warnings.push_back("Generic quaternion pose offset is out of range");
                            return result;
                        }
                        std::memcpy(poses[frame].data() + pose_offset, values[frame].data(), 16);
                    }
                } else if (type.find("float3") != std::string::npos) {
                    std::vector<float> values[3];
                    for (int axis = 0; axis < 3; ++axis) {
                        const uint8_t* bytes = nullptr; size_t byte_count = 0;
                        if (!take_stream(bytes, byte_count)) {
                            result.warnings.push_back("Generic float3 stream is truncated");
                            return result;
                        }
                        values[axis] = decode_scalar(bytes, byte_count, this_frame_count, scale);
                    }
                    for (int frame = 0; frame < this_frame_count; ++frame) {
                        if (pose_offset + 12 > poses[frame].size()) {
                            result.warnings.push_back("Generic float3 pose offset is out of range");
                            return result;
                        }
                        float value[3] = {values[0][frame], values[1][frame], values[2][frame]};
                        std::memcpy(poses[frame].data() + pose_offset, value, 12);
                    }
                } else if (type.find("float1") != std::string::npos) {
                    const uint8_t* bytes = nullptr; size_t byte_count = 0;
                    if (!take_stream(bytes, byte_count)) {
                        result.warnings.push_back("Generic float1 stream is truncated");
                        return result;
                    }
                    const auto values = decode_scalar(bytes, byte_count, this_frame_count, scale);
                    for (int frame = 0; frame < this_frame_count; ++frame) {
                        if (pose_offset + 4 > poses[frame].size()) {
                            result.warnings.push_back("Generic float1 pose offset is out of range");
                            return result;
                        }
                        std::memcpy(poses[frame].data() + pose_offset, &values[frame], 4);
                    }
                } else if (type.find("usmevent") != std::string::npos) {
                    stream_cursor = align_up(stream_cursor, 4);
                    uint32_t byte_count = 0;
                    if (stream_cursor + 4 > blob.size()) {
                        result.warnings.push_back("Generic event stream length is truncated");
                        return result;
                    }
                    std::memcpy(&byte_count, blob.data() + stream_cursor, 4);
                    stream_cursor += 4;
                    if (stream_cursor + byte_count > blob.size()) {
                        result.warnings.push_back("Generic event stream is truncated");
                        return result;
                    }

                    size_t cursor = stream_cursor;
                    int run = 0; uint8_t value = 0;
                    for (int frame = 0; frame < this_frame_count; ++frame) {
                        if (run == 0 && cursor + 2 <= stream_cursor + byte_count) {
                            run = blob[cursor++];
                            value = blob[cursor++];
                        }
                        if (pose_offset < poses[frame].size()) poses[frame][pose_offset] = value;
                        if (run > 0) --run;
                    }
                    stream_cursor += byte_count;
                } else if (type.find("usmmorph") != std::string::npos) {
                    result.warnings.push_back("USMMorph generic tracks are preserved but do not drive skeleton matrices");
                } else {
                    result.warnings.push_back("Unsupported generic component type '" + info.type_name + "'");
                    return result;
                }
            }
        }

        for (int frame = 0; frame < this_frame_count; ++frame) {
            std::vector<std::array<float, 16>> world;
            if (!build_world_matrices(skeleton, poses[frame], world, matrix_error)) {
                result.warnings.push_back(matrix_error);
                return result;
            }
            result.world_frames.push_back(std::move(world));
            ++output_frame;
        }
    }

    if (static_cast<int>(result.world_frames.size()) != frame_count) {
        result.warnings.push_back("Generic animation decoded fewer frames than declared");
        return result;
    }
    result.complete = true;
    return result;
}
