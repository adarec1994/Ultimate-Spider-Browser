#include <iostream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include "SpiderManTool.h"
#include "PackDirectory.h"
#include "Interface.h"
#include "NalIntegration.h"
#include "stb_image_write.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

static std::string AlsResolveHash(const SpiderManTool& tool, uint32_t hash) {
    if (!hash) return "None";
    auto found = tool.dictionary.find(hash);
    if (found != tool.dictionary.end()) return found->second;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", hash);
    return buf;
}

static void LoadBestDictionaryForPack(SpiderManTool& tool, const fs::path& packDir) {
    fs::path textDict = packDir / "string_hash_dictionary.txt";
    if (fs::exists(textDict)) {
        tool.LoadDictionary(textDict.string());
    } else if (fs::exists("string_hash_dictionary.txt")) {
        tool.LoadDictionary("string_hash_dictionary.txt");
    }

    if (tool.dictionary.empty()) {
        fs::path binaryDict = packDir / "string_hash_dictionary.bin";
        if (fs::exists(binaryDict)) {
            tool.LoadBinaryDictionary(binaryDict.string());
        } else if (fs::exists("string_hash_dictionary.bin")) {
            tool.LoadBinaryDictionary("string_hash_dictionary.bin");
        }
    }

    if (tool.dictionary.empty()) tool.LoadEmbeddedDictionary();
}

static int ExportPackGlbHeadless(const std::string& packArg) {
    fs::path packPath = fs::absolute(packArg);
    if (!fs::exists(packPath)) {
        std::cerr << "Pack does not exist: " << packPath.string() << std::endl;
        return 2;
    }

    SpiderManTool tool;
    fs::path packDir = packPath.parent_path();
    tool.searchPath = packDir.string();
    LoadBestDictionaryForPack(tool, packDir);

    try {
        for (const auto& entry : fs::recursive_directory_iterator(packDir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return (char)std::tolower(c);
            });
            if (ext == ".pcpack") tool.foundPacks.push_back(entry.path());
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to scan pack folder: " << e.what() << std::endl;
        return 3;
    }

    if (tool.foundPacks.empty()) tool.foundPacks.push_back(packPath);
    std::sort(tool.foundPacks.begin(), tool.foundPacks.end());
    tool.BuildGlobalTextureIndex();
    for (int i = 0; i < (int)tool.foundPacks.size(); ++i) {
        tool.BuildGlobalTextureIndexStep(i);
    }

    tool.ExtractPack(packPath.string(), true);
    return 0;
}

static int InspectMorphPackHeadless(const std::string& packArg) {
    const fs::path packPath = fs::absolute(packArg);
    std::ifstream file(packPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Pack does not exist: " << packPath.string() << std::endl;
        return 2;
    }

    const size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> blob(fileSize);
    file.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    if (!file.good()) {
        std::cerr << "Failed to read pack: " << packPath.string() << std::endl;
        return 3;
    }

    const PackDirectory dir = PackDirectory::Parse(blob);
    if (!dir.valid) {
        std::cerr << "Invalid directory: " << dir.error << std::endl;
        return 4;
    }

    auto printTl = [](const char* label, const std::vector<PackTlResource>& entries) {
        std::cout << label << " count=" << entries.size() << std::endl;
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            std::cout << "  [" << i << "] hash=0x" << std::hex << entry.nameHash
                      << " type=" << std::dec << static_cast<unsigned>(entry.type)
                      << " offset=0x" << std::hex << entry.offset
                      << " size=0x" << entry.size << std::dec << std::endl;
        }
    };
    printTl("MORPH_FILES", dir.morphFiles);
    printTl("MORPHS", dir.morphs);
    printTl("SCENE_ANIMS", dir.sceneAnims);
    std::map<uint32_t, size_t> resourceTypeCounts;
    for (const auto& resource : dir.resources) ++resourceTypeCounts[resource.type];
    std::cout << "RESOURCE_TYPES";
    for (const auto& [type, count] : resourceTypeCounts)
        std::cout << " " << ResourceKeyTypeName(type) << "(" << type << ")=" << count;
    std::cout << std::endl;

    auto readU32 = [&](size_t offset, uint32_t& value) {
        if (offset + sizeof(value) > blob.size()) return false;
        std::memcpy(&value, blob.data() + offset, sizeof(value));
        return true;
    };

    for (size_t resourceIndex = 0; resourceIndex < dir.resources.size(); ++resourceIndex) {
        const auto& resource = dir.resources[resourceIndex];
        uint32_t magic = 0;
        const bool haveMagic = readU32(resource.offset, magic);
        if (resource.type != RES_KEY_MORPH && resource.type != RES_KEY_MORPH_FILE_STRUCT &&
            resource.type != RES_KEY_ANIMATION && resource.type != RES_KEY_SCENE_ANIM &&
            resource.type != RES_KEY_CUT_SCENE && resource.type != RES_KEY_VISEME_STREAM &&
            resource.type != RES_KEY_ENTITY &&
            (!haveMagic || magic != 0x204D4350u)) {
            continue;
        }

        std::cout << "RESOURCE[" << resourceIndex << "] hash=0x" << std::hex
                  << resource.hash << " type=" << std::dec << resource.type
                  << " offset=0x" << std::hex << resource.offset
                  << " size=0x" << resource.size << std::dec;
        if (haveMagic && magic == 0x204D4350u) std::cout << " magic=PCM";
        std::cout << std::endl;

        if (resource.type == RES_KEY_CUT_SCENE || resource.type == RES_KEY_VISEME_STREAM) {
            std::cout << "  resource_words";
            for (size_t wordIndex = 0; wordIndex < 16; ++wordIndex) {
                uint32_t word = 0;
                if (!readU32(resource.offset + wordIndex * 4, word)) break;
                std::cout << " 0x" << std::hex << word;
            }
            std::cout << std::dec << std::endl;
        }

        if (resource.type == RES_KEY_ENTITY &&
            resource.offset <= blob.size() &&
            resource.size <= blob.size() - resource.offset) {
            std::vector<uint8_t> entityBytes(
                blob.begin() + resource.offset,
                blob.begin() + resource.offset + resource.size);
            const EntityBoneMapping mapping = ParseEntityBoneMapping(entityBytes);
            std::cout << "  entity_bone_mapping valid=" << (mapping.valid ? 1 : 0);
            if (!mapping.valid) {
                std::cout << " error=" << mapping.error << std::endl;
            } else {
                std::cout << " mesh_poses=" << mapping.meshPoseCount
                          << " logical_poses=" << mapping.logicalToMesh.size()
                          << " mesh_to_logical=";
                for (size_t mesh = 0; mesh < mapping.meshToLogical.size(); ++mesh) {
                    if (mesh) std::cout << ',';
                    std::cout << mapping.meshToLogical[mesh];
                }
                std::cout << std::endl;
            }
        }

        uint32_t version = 0, itemCount = 0, itemTableRel = 0, imageBaseRel = 0;
        if (!haveMagic || magic != 0x204D4350u ||
            !readU32(resource.offset + 4, version) ||
            !readU32(resource.offset + 8, itemCount) ||
            !readU32(resource.offset + 12, itemTableRel) ||
            !readU32(resource.offset + 16, imageBaseRel)) {
            continue;
        }
        const int64_t relocation = static_cast<int64_t>(resource.offset) - imageBaseRel;
        const int64_t itemTable = relocation + itemTableRel;
        std::cout << "  header version=0x" << std::hex << version << " items=" << std::dec
                  << itemCount << " table=0x" << std::hex << itemTable
                  << " image_base=0x" << imageBaseRel << " relocation=" << std::showbase
                  << relocation << std::noshowbase << std::dec << std::endl;
        if (itemTable < 0 || static_cast<uint64_t>(itemTable) + 12ull * itemCount > blob.size())
            continue;

        for (uint32_t itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
            const size_t itemOffset = static_cast<size_t>(itemTable) + 12u * itemIndex;
            const uint8_t itemType = blob[itemOffset + 3];
            uint32_t payloadRel = 0, auxRel = 0;
            readU32(itemOffset + 4, payloadRel);
            readU32(itemOffset + 8, auxRel);
            const int64_t payload = payloadRel ? relocation + payloadRel : 0;
            const int64_t aux = auxRel ? relocation + auxRel : 0;
            std::cout << "  item[" << itemIndex << "] type=" << unsigned(itemType)
                      << " payload=0x" << std::hex << payload << " aux=0x" << aux
                      << std::dec << std::endl;
            if (resource.type == RES_KEY_MESH && itemType == 2 && payload > 0 &&
                static_cast<uint64_t>(payload) + 16 <= blob.size()) {
                uint32_t sectionCount = 0, sectionTableRel = 0;
                readU32(static_cast<size_t>(payload) + 8, sectionCount);
                readU32(static_cast<size_t>(payload) + 12, sectionTableRel);
                const int64_t sectionTable = sectionTableRel ? relocation + sectionTableRel : 0;
                std::cout << "    mesh sections=" << sectionCount << " table=0x" << std::hex
                          << sectionTable << std::dec << std::endl;
                if (sectionCount <= 1024 && sectionTable > 0 &&
                    static_cast<uint64_t>(sectionTable) + 8ull * sectionCount <= blob.size()) {
                    for (uint32_t sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex) {
                        uint32_t sectionRel = 0;
                        readU32(static_cast<size_t>(sectionTable) + 8u * sectionIndex + 4, sectionRel);
                        const int64_t section = sectionRel ? relocation + sectionRel : 0;
                        if (section <= 0 || static_cast<uint64_t>(section) + 88 > blob.size()) continue;
                        uint32_t vertexCount = 0, vertexDataRel = 0, stride = 0, extensionRel = 0;
                        readU32(static_cast<size_t>(section) + 56, vertexCount);
                        readU32(static_cast<size_t>(section) + 60, vertexDataRel);
                        readU32(static_cast<size_t>(section) + 72, stride);
                        readU32(static_cast<size_t>(section) + 84, extensionRel);
                        std::cout << "      section[" << sectionIndex << "] at=0x" << std::hex
                                  << section << " vertices=" << std::dec << vertexCount
                                  << " stride=" << stride << " vertex_data=0x" << std::hex
                                  << (vertexDataRel ? relocation + vertexDataRel : 0)
                                  << " extension=0x" << (extensionRel ? relocation + extensionRel : 0);
                        const int64_t extension = extensionRel ? relocation + extensionRel : 0;
                        if (extension > 0 && static_cast<uint64_t>(extension) + 32 <= blob.size()) {
                            std::cout << " key=";
                            for (unsigned keyIndex = 0; keyIndex < 8; ++keyIndex) {
                                uint32_t word = 0;
                                readU32(static_cast<size_t>(extension) + 4u * keyIndex, word);
                                if (keyIndex) std::cout << ',';
                                std::cout << "0x" << word;
                            }
                        }
                        if (stride == 64) {
                            uint32_t paletteCount = 0, paletteRel = 0;
                            readU32(static_cast<size_t>(section) + 8, paletteCount);
                            readU32(static_cast<size_t>(section) + 12, paletteRel);
                            const int64_t paletteOffset = paletteRel ? relocation + paletteRel : 0;
                            std::cout << " bones=" << std::dec << paletteCount << " palette=";
                            if (paletteCount <= 256 && paletteOffset > 0 &&
                                static_cast<uint64_t>(paletteOffset) + 2ull * paletteCount <= blob.size()) {
                                for (uint32_t bone = 0; bone < paletteCount; ++bone) {
                                    uint16_t global = 0;
                                    std::memcpy(&global, blob.data() + paletteOffset + 2u * bone, 2);
                                    if (bone) std::cout << ',';
                                    std::cout << global;
                                }
                            }
                            int rawMin = 256, rawMax = -1;
                            uint64_t weightedInfluences = 0, invalidInfluences = 0;
                            const int64_t vertexData = vertexDataRel ? relocation + vertexDataRel : 0;
                            if (vertexData > 0 && vertexCount <= 100000 &&
                                static_cast<uint64_t>(vertexData) + 64ull * vertexCount <= blob.size()) {
                                for (uint32_t vertex = 0; vertex < vertexCount; ++vertex) {
                                    const size_t base = static_cast<size_t>(vertexData) + 64u * vertex;
                                    for (unsigned influence = 0; influence < 4; ++influence) {
                                        float rawFloat = 0.0f, weight = 0.0f;
                                        std::memcpy(&rawFloat, blob.data() + base + 32 + 4u * influence, 4);
                                        std::memcpy(&weight, blob.data() + base + 48 + 4u * influence, 4);
                                        if (weight <= 0.0f) continue;
                                        const int raw = static_cast<int>(rawFloat + 0.5f);
                                        rawMin = std::min(rawMin, raw);
                                        rawMax = std::max(rawMax, raw);
                                        ++weightedInfluences;
                                        if (paletteCount && (raw < 0 || static_cast<uint32_t>(raw) >= paletteCount))
                                            ++invalidInfluences;
                                    }
                                }
                            }
                            std::cout << " raw=" << rawMin << ".." << rawMax
                                      << " influences=" << weightedInfluences
                                      << " invalid=" << invalidInfluences;
                        }
                        std::cout << std::dec << std::endl;
                    }
                }
            }
            if (itemType != 3 || payload <= 0 || static_cast<uint64_t>(payload) + 20 > blob.size())
                continue;

            uint32_t nameRel = 0, setCount = 0, setsRel = 0, ownerRel = 0, extraRel = 0;
            readU32(static_cast<size_t>(payload) + 0, nameRel);
            readU32(static_cast<size_t>(payload) + 4, setCount);
            readU32(static_cast<size_t>(payload) + 8, setsRel);
            readU32(static_cast<size_t>(payload) + 12, ownerRel);
            readU32(static_cast<size_t>(payload) + 16, extraRel);
            std::cout << "    morph name_rel=0x" << std::hex << nameRel
                      << " set_count=" << std::dec << setCount
                      << " sets=0x" << std::hex << (setsRel ? relocation + setsRel : 0)
                      << " owner=0x" << (ownerRel ? relocation + ownerRel : 0)
                      << " extra=0x" << (extraRel ? relocation + extraRel : 0)
                      << std::dec << std::endl;

            const int64_t sets = setsRel ? relocation + setsRel : 0;
            if (sets <= 0 || static_cast<uint64_t>(sets) + 12ull * setCount > blob.size()) continue;
            for (uint32_t setIndex = 0; setIndex < setCount; ++setIndex) {
                uint32_t key = 0, targetCount = 0, targetsRel = 0;
                const size_t setOffset = static_cast<size_t>(sets) + 12u * setIndex;
                readU32(setOffset + 0, key);
                readU32(setOffset + 4, targetCount);
                readU32(setOffset + 8, targetsRel);
                std::cout << "      set[" << setIndex << "] key=0x" << std::hex << key
                          << " target_count=" << std::dec << targetCount
                          << " targets=0x" << std::hex
                          << (targetsRel ? relocation + targetsRel : 0) << std::dec << std::endl;
            }
        }
    }
    return 0;
}

struct HeadlessPcmSection {
    uint32_t vertexCount = 0;
    uint32_t vertexOffset = 0;
    uint32_t stride = 0;
};

static bool ReadHeadlessPcmSections(const std::vector<uint8_t>& pcm,
                                    std::vector<HeadlessPcmSection>& sections,
                                    std::string& error) {
    auto u32 = [&](size_t offset, uint32_t& value) {
        if (offset > pcm.size() || 4 > pcm.size() - offset) return false;
        std::memcpy(&value, pcm.data() + offset, 4);
        return true;
    };
    uint32_t magic = 0, itemCount = 0, table = 0, imageBase = 0;
    if (!u32(0, magic) || magic != UsmMorph::PCM_MAGIC || !u32(8, itemCount) ||
        !u32(12, table) || !u32(16, imageBase) || table < imageBase) {
        error = "invalid PCM header";
        return false;
    }
    table -= imageBase;
    if (itemCount > 4096 || table > pcm.size() ||
        12ull * itemCount > pcm.size() - table) {
        error = "PCM item table is out of range";
        return false;
    }
    size_t lod = 0;
    for (uint32_t item = 0; item < itemCount; ++item) {
        const size_t record = table + 12u * item;
        if (pcm[record + 3] != 2) continue;
        uint32_t relative = 0;
        u32(record + 4, relative);
        if (relative >= imageBase && relative - imageBase < pcm.size()) {
            lod = relative - imageBase;
            break;
        }
    }
    uint32_t sectionCount = 0, sectionTable = 0;
    if (!lod || !u32(lod + 8, sectionCount) || !u32(lod + 12, sectionTable) ||
        sectionTable < imageBase) {
        error = "PCM has no valid LOD section table";
        return false;
    }
    sectionTable -= imageBase;
    if (sectionCount > 4096 || sectionTable > pcm.size() ||
        8ull * sectionCount > pcm.size() - sectionTable) {
        error = "PCM LOD section table is out of range";
        return false;
    }
    sections.resize(sectionCount);
    for (uint32_t sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex) {
        uint32_t sectionOffset = 0;
        u32(sectionTable + 8u * sectionIndex + 4, sectionOffset);
        if (sectionOffset < imageBase || sectionOffset - imageBase > pcm.size() ||
            76 > pcm.size() - (sectionOffset - imageBase)) {
            error = "PCM mesh section is out of range";
            return false;
        }
        sectionOffset -= imageBase;
        auto& section = sections[sectionIndex];
        u32(sectionOffset + 56, section.vertexCount);
        u32(sectionOffset + 60, section.vertexOffset);
        u32(sectionOffset + 72, section.stride);
        if (section.vertexOffset < imageBase) {
            error = "PCM vertex pointer precedes its image base";
            return false;
        }
        section.vertexOffset -= imageBase;
        const uint64_t byteCount = static_cast<uint64_t>(section.vertexCount) * section.stride;
        if (section.stride < 12 || section.vertexOffset > pcm.size() ||
            byteCount > pcm.size() - section.vertexOffset) {
            error = "PCM vertex payload is out of range";
            return false;
        }
    }
    return true;
}

static int ValidateMorphPacksHeadless(const std::string& packDirArg) {
    const fs::path packDir = fs::absolute(packDirArg);
    if (!fs::is_directory(packDir)) {
        std::cerr << "Pack directory does not exist: " << packDir.string() << std::endl;
        return 2;
    }

    SpiderManTool names;
    LoadBestDictionaryForPack(names, packDir);

    uint64_t packCount = 0, morphFileCount = 0, setCount = 0;
    uint64_t sectionRecordCount = 0, positionStreamCount = 0, changedVertexCount = 0;
    uint64_t neutralVertexCount = 0, externalMeshCount = 0, failureCount = 0;
    uint64_t visemeStreamCount = 0, visemeFrameCount = 0, visemeWeightCount = 0;
    for (const auto& directoryEntry : fs::directory_iterator(packDir)) {
        if (!directoryEntry.is_regular_file()) continue;
        std::string extension = directoryEntry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension != ".pcpack") continue;
        ++packCount;

        std::ifstream input(directoryEntry.path(), std::ios::binary | std::ios::ate);
        if (!input) { ++failureCount; continue; }
        const size_t fileSize = static_cast<size_t>(input.tellg());
        input.seekg(0);
        std::vector<uint8_t> blob(fileSize);
        input.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
        const PackDirectory directory = PackDirectory::Parse(blob);
        if (!directory.valid) { ++failureCount; continue; }

        const bool packHasMorph = std::any_of(directory.resources.begin(), directory.resources.end(),
            [](const PackResourceEntry& resource) { return resource.type == RES_KEY_MORPH; });
        for (const auto& animationResource : directory.resources) {
            if (animationResource.type != RES_KEY_VISEME_STREAM) continue;
            ++visemeStreamCount;
            if (animationResource.offset > blob.size() ||
                animationResource.size > blob.size() - animationResource.offset) {
                ++failureCount;
                std::cerr << "[VISEME FAIL] " << directoryEntry.path().filename().string()
                          << " resource extent is out of range" << std::endl;
                continue;
            }
            std::vector<uint8_t> streamBytes(
                blob.begin() + animationResource.offset,
                blob.begin() + animationResource.offset + animationResource.size);
            const UsmViseme::Stream stream = UsmViseme::Parse(streamBytes);
            if (!stream.valid) {
                ++failureCount;
                std::cerr << "[VISEME FAIL] " << directoryEntry.path().filename().string()
                          << " hash=0x" << std::hex << animationResource.hash << std::dec
                          << ": " << (stream.warnings.empty() ? "parser rejected resource" :
                                      stream.warnings.front()) << std::endl;
                continue;
            }
            visemeFrameCount += stream.frame_count;
            visemeWeightCount += stream.weights.size();
            if (packHasMorph) {
                const auto nameIt = names.dictionary.find(animationResource.hash);
                std::cout << "[MORPH TIMELINE] " << directoryEntry.path().filename().string()
                          << " hash=0x" << std::hex << animationResource.hash << std::dec;
                if (nameIt != names.dictionary.end()) std::cout << " name=" << nameIt->second;
                std::cout << " channels=" << stream.channel_count
                          << " format=" << stream.format
                          << " frames=" << stream.frame_count
                          << " fps=" << stream.sample_rate
                          << " field_10=" << stream.field_10 << std::endl;
            }
        }

        for (const auto& morphResource : directory.resources) {
            if (morphResource.type != RES_KEY_MORPH) continue;
            ++morphFileCount;
            const auto nameIt = names.dictionary.find(morphResource.hash);
            std::cout << "[MORPH RESOURCE] " << directoryEntry.path().filename().string()
                      << " hash=0x" << std::hex << morphResource.hash << std::dec;
            if (nameIt != names.dictionary.end()) std::cout << " name=" << nameIt->second;
            std::cout << std::endl;
            auto fail = [&](const std::string& message) {
                ++failureCount;
                std::cerr << "[MORPH FAIL] " << directoryEntry.path().filename().string()
                          << " hash=0x" << std::hex << morphResource.hash << std::dec
                          << ": " << message << std::endl;
            };
            if (morphResource.offset > blob.size() ||
                morphResource.size > blob.size() - morphResource.offset) {
                fail("resource extent is out of range");
                continue;
            }
            std::vector<uint8_t> morphBytes(
                blob.begin() + morphResource.offset,
                blob.begin() + morphResource.offset + morphResource.size);
            const UsmMorph::File morph = UsmMorph::Parse(morphBytes);
            if (!morph.valid) {
                fail(morph.warnings.empty() ? "parser rejected resource" : morph.warnings.front());
                continue;
            }

            const PackResourceEntry* meshResource = nullptr;
            for (const auto& candidate : directory.resources) {
                if (candidate.type == RES_KEY_MESH && candidate.hash == morphResource.hash) {
                    meshResource = &candidate;
                    break;
                }
            }
            if (!meshResource || meshResource->offset > blob.size() ||
                meshResource->size > blob.size() - meshResource->offset) {

                ++externalMeshCount;
                continue;
            }
            std::vector<uint8_t> pcmBytes(blob.begin() + meshResource->offset,
                                          blob.begin() + meshResource->offset + meshResource->size);
            std::vector<HeadlessPcmSection> pcmSections;
            std::string pcmError;
            if (!ReadHeadlessPcmSections(pcmBytes, pcmSections, pcmError)) {
                fail(pcmError);
                continue;
            }

            bool resourceFailed = false;
            setCount += morph.sets.size();
            for (size_t targetIndex = 0; targetIndex < morph.sets.size(); ++targetIndex) {
                const auto& target = morph.sets[targetIndex];
                if (target.sections.size() != pcmSections.size()) {
                    fail("target " + std::to_string(targetIndex) + " section count differs from PCM");
                    resourceFailed = true;
                    break;
                }
                sectionRecordCount += target.sections.size();
                for (size_t sectionIndex = 0; sectionIndex < target.sections.size(); ++sectionIndex) {
                    const auto& section = target.sections[sectionIndex];
                    const auto& pcmSection = pcmSections[sectionIndex];

                    if (section.vertex_count != 0 &&
                        section.vertex_count != pcmSection.vertexCount) {
                        fail("target " + std::to_string(targetIndex) + " section " +
                             std::to_string(sectionIndex) + " vertex count differs from PCM");
                        resourceFailed = true;
                        break;
                    }
                    if (section.position.values.empty()) continue;
                    ++positionStreamCount;
                    for (uint8_t changed : section.position.changed) changedVertexCount += changed != 0;
                    if (targetIndex != 0) continue;

                    for (size_t vertex = 0; vertex < section.position.values.size(); ++vertex) {
                        const size_t pcmPosition = pcmSection.vertexOffset + vertex * pcmSection.stride;
                        if (std::memcmp(section.position.values[vertex].data(),
                                        pcmBytes.data() + pcmPosition, 12) != 0) {
                            fail("neutral position bits differ at section " +
                                 std::to_string(sectionIndex) + " vertex " +
                                 std::to_string(vertex));
                            resourceFailed = true;
                            break;
                        }
                        ++neutralVertexCount;
                    }
                    if (resourceFailed) break;
                }
                if (resourceFailed) break;
            }
        }
    }

    return failureCount == 0 ? 0 : 1;
}

static int InspectAnimationMorphHeadless(const std::string& packArg) {
    const fs::path packPath = fs::absolute(packArg);
    if (!fs::exists(packPath)) {
        std::cerr << "Pack does not exist: " << packPath.string() << std::endl;
        return 2;
    }

    SpiderManTool tool;
    const fs::path packDir = packPath.parent_path();
    tool.searchPath = packDir.string();
    LoadBestDictionaryForPack(tool, packDir);
    for (const auto& entry : fs::directory_iterator(packDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext == ".pcpack") tool.foundPacks.push_back(entry.path());
    }
    std::sort(tool.foundPacks.begin(), tool.foundPacks.end());
    tool.BuildGlobalSkeletonIndex();
    tool.OpenPCPack(packPath.string());

    std::cout << "SKELETON_CANDIDATES count=" << tool.skeletonCandidates.size() << std::endl;
    for (size_t skeletonIndex = 0; skeletonIndex < tool.skeletonCandidates.size(); ++skeletonIndex) {
        const auto& candidate = tool.skeletonCandidates[skeletonIndex];
        if (!candidate.data) continue;
        const auto& skeleton = *candidate.data;
        std::cout << "  skeleton[" << skeletonIndex << "] name=" << candidate.name
                  << " hash=0x" << std::hex << candidate.hash << std::dec
                  << " kind=" << skeleton.skeleton_kind
                  << " slots=" << skeleton.generic_component_slot_count
                  << " pose_size=" << skeleton.generic_pose_size << std::endl;
        if (!skeleton.generic_default_pose.empty()) {
            std::cout << "    generic_default_pose=";
            for (uint8_t byte : skeleton.generic_default_pose) {
                static constexpr char digits[] = "0123456789abcdef";
                std::cout << digits[byte >> 4] << digits[byte & 15];
            }
            std::cout << std::endl;
        }
        for (const auto& [boneIndex, boneName] : skeleton.bone_map) {
            const auto parentIt = skeleton.parent_map.find(boneIndex);
            const int parentIndex = parentIt == skeleton.parent_map.end() ? -1 : parentIt->second;
            std::cout << "    bone[" << boneIndex << "] name=" << boneName
                      << " parent=" << parentIndex << std::endl;
        }
        for (size_t componentIndex = 0; componentIndex < skeleton.components.size(); ++componentIndex) {
            const auto& component = skeleton.components[componentIndex];
            std::cout << "    component[" << componentIndex << "] slot="
                      << component.component_index << " type=" << component.type_name
                      << " hash=0x" << std::hex << component.type_id << std::dec
                      << " flags=0x" << std::hex << component.component_flags << std::dec
                      << " bones=" << component.bone_indices.size()
                      << " skel_bytes=" << component.raw_skel_block.size()
                      << " pose_bytes=" << component.raw_default_pose_block.size() << std::endl;
            if (component.type_id == NalCompType::ArbitraryPO) {
                std::cout << "      arb_header=";
                for (uint32_t value : component.arb_header) std::cout << value << ',';
                std::cout << " eval_order=";
                for (uint32_t value : component.arb_eval_order) std::cout << value << ',';
                std::cout << std::endl;
                for (size_t nodeIndex = 0; nodeIndex < component.arb_nodes.size(); ++nodeIndex) {
                    const auto& node = component.arb_nodes[nodeIndex];
                    std::cout << "      arb_node[" << nodeIndex << "] name=" << node.name
                              << " matrix=" << node.my_matrix_ix
                              << " parent=" << node.parent_matrix_ix
                              << " quat=" << node.quat_ix
                              << " pos=" << node.pos_ix
                              << " quat_anim=" << node.is_quat_anim
                              << " pos_anim=" << node.is_pos_anim << std::endl;
                }
            }
            if (component.type_id == NalCompType::Tentacles) {
                std::cout << "      tentacle_default=";
                for (size_t valueIndex = 0;
                     valueIndex < component.default_pose.tentacle_values.size(); ++valueIndex) {
                    if (valueIndex) std::cout << ',';
                    std::cout << component.default_pose.tentacle_values[valueIndex];
                }
                std::cout << std::endl;
            }
        }
        for (size_t infoIndex = 0; infoIndex < skeleton.generic_component_infos.size(); ++infoIndex) {
            const auto& info = skeleton.generic_component_infos[infoIndex];
            std::cout << "    info[" << infoIndex << "] type=" << info.type_name
                      << " hash=0x" << std::hex << info.type_hash << std::dec
                      << " first=" << info.first_component
                      << " count=" << info.component_count
                      << " pose_offset=" << info.pose_offset << std::endl;
        }
    }

    if (!tool.loadedAnimFile) {
        std::cout << "ANIMATIONS count=0" << std::endl;
        return 0;
    }
    std::cout << "ANIMATIONS count=" << tool.loadedAnimFile->animations.size() << std::endl;
    for (size_t animationIndex = 0;
         animationIndex < tool.loadedAnimFile->animations.size(); ++animationIndex) {
        const auto& animation = tool.loadedAnimFile->animations[animationIndex];
        std::cout << "  animation[" << animationIndex << "] name=" << animation.name
                  << " frames=" << animation.frame_count
                  << " t_scale=" << animation.t_scale
                  << " current_time=" << animation.current_time
                  << " generic=" << (animation.is_gen_anim() ? 1 : 0)
                  << " decoded=" << (animation.generic_decoded.complete ? 1 : 0)
                  << " warnings=" << animation.generic_decoded.warnings.size();
        if (animation.skeleton) std::cout << " skeleton=" << animation.skeleton->name;
        std::cout << std::endl;
        for (size_t componentIndex = 0; componentIndex < animation.components.size(); ++componentIndex) {
            const auto& component = animation.components[componentIndex];
            std::cout << "    component[" << componentIndex << "] comp_ix=" << component.comp_ix
                      << " slot=" << component.slot_ix << " type=0x" << std::hex
                      << component.type_hash << " mask=0x" << component.mask << std::dec
                      << " tracks=" << component.ntracks
                      << " codecs=" << component.codec_ixs.size()
                      << " encoded_bytes=" << component.encoded_data.size()
                      << " decoded_frames=" << component.decoded.frames.size();
            if (!component.decode_error.empty())
                std::cout << " error=" << component.decode_error;
            if (component.comp_ix == NalComp::TENTACLE && !component.decoded.frames.empty()) {
                float minimum = std::numeric_limits<float>::max();
                float maximum = std::numeric_limits<float>::lowest();
                for (const auto& frame : component.decoded.frames) {
                    for (float value : frame) {
                        minimum = std::min(minimum, value);
                        maximum = std::max(maximum, value);
                    }
                }
                std::cout << " value_range=" << minimum << ".." << maximum;
            }
            std::cout << std::endl;
        }
        for (const auto& warning : animation.generic_decoded.warnings)
            std::cout << "    warning: " << warning << std::endl;
    }
    return 0;
}

static float AlsBlendTimeForState(uint16_t flags, float animDuration) {
    if (flags & 0x0020) return 0.0f;
    if (flags & 0x0040) return 0.13333f;
    if (flags & 0x0080) return 0.26666f;
    if (flags & 0x0100) {
        const float t = animDuration * 0.2f;
        return t < 0.26666f ? t : 0.26666f;
    }
    return 0.0f;
}

static std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static int DumpAlsHeadless(const std::string& packArg, const std::string& outArg) {
    const fs::path packPath = fs::absolute(packArg);
    if (!fs::exists(packPath)) {
        std::cerr << "Pack does not exist: " << packPath.string() << std::endl;
        return 2;
    }

    SpiderManTool tool;
    const fs::path packDir = packPath.parent_path();
    tool.searchPath = packDir.string();
    LoadBestDictionaryForPack(tool, packDir);
    for (const auto& entry : fs::directory_iterator(packDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext == ".pcpack") tool.foundPacks.push_back(entry.path());
    }
    std::sort(tool.foundPacks.begin(), tool.foundPacks.end());
    tool.BuildGlobalSkeletonIndex();
    tool.OpenPCPack(packPath.string());

    if (tool.animationStateMachines.empty()) {
        std::cerr << "No ALS resource in pack" << std::endl;
        return 3;
    }

    struct AnimInfo { bool looping = false; float duration = 0.f; std::string name; bool found = false; };
    std::map<uint32_t, AnimInfo> animByHash;
    if (tool.loadedAnimFile) {
        for (const auto& anim : tool.loadedAnimFile->animations) {
            AnimInfo info;
            info.looping  = anim.is_looping();
            info.duration = anim.t_scale;
            info.name     = anim.name;
            info.found    = true;
            animByHash[anim.name_hash] = info;
        }
    }

    auto name = [&](uint32_t hash) { return AlsResolveHash(tool, hash); };

    std::ostringstream out;
    out << std::boolalpha;
    out << "{\n  \"pack\": \"" << JsonEscape(packPath.filename().string()) << "\",\n";
    out << "  \"animations_in_pack\": " << animByHash.size() << ",\n";
    out << "  \"machines\": [\n";

    auto emitRules = [&](const std::vector<UsmAls::Rule>& rules, const char* label,
                         const std::string& indent, bool trailingComma) {
        out << indent << "\"" << label << "\": [";
        for (size_t i = 0; i < rules.size(); ++i) {
            const auto& rule = rules[i];
            out << (i ? ",\n" : "\n") << indent << "  {";
            out << "\"order\": " << rule.priority;
            out << ", \"action\": " << rule.action.type;
            if (rule.kind == UsmAls::RuleKind::Layer) {
                out << ", \"layer\": " << rule.conditionA;
                out << ", \"match\": \"" << JsonEscape(name(
                        static_cast<uint32_t>(rule.conditionB))) << "\"";
            }
            if (rule.kind == UsmAls::RuleKind::Incoming && rule.conditionA) {
                out << ", \"from\": \"" << JsonEscape(name(
                        static_cast<uint32_t>(rule.conditionA))) << "\"";
                out << ", \"from_is_category\": " << (rule.conditionB != 0);
            }
            out << ", \"target\": \"" << JsonEscape(name(rule.action.target)) << "\"";
            if (rule.trigger)
                out << ", \"trigger\": \"" << JsonEscape(name(rule.trigger)) << "\"";
            if (!rule.action.destinations.empty()) {
                out << ", \"destinations\": [";
                for (size_t d = 0; d < rule.action.destinations.size(); ++d) {
                    const auto& dest = rule.action.destinations[d];
                    if (d) out << ", ";
                    out << "{\"state\": \"" << JsonEscape(name(dest.target))
                        << "\", \"weight\": " << dest.weight << "}";
                }
                out << "]";
            }
            if (!rule.filters.empty()) {
                out << ", \"filters\": [";
                for (size_t f = 0; f < rule.filters.size(); ++f) {
                    const auto& filter = rule.filters[f];
                    if (f) out << ", ";
                    out << "{\"param\": " << filter.parameter
                        << ", \"min\": " << filter.minimum
                        << ", \"max\": " << filter.maximum
                        << ", \"kind\": \"" << (filter.parameter >= 91 ? "internal" : "external")
                        << "\"}";
                }
                out << "]";
            }
            if (rule.hasPostAction) out << ", \"post_action\": true";
            out << "}";
        }
        out << (rules.empty() ? "]" : "\n" + indent + "]") << (trailingComma ? ",\n" : "\n");
    };

    for (size_t m = 0; m < tool.animationStateMachines.size(); ++m) {
        const auto& file = tool.animationStateMachines[m];
        for (size_t g = 0; g < file.machines.size(); ++g) {
            const auto& machine = file.machines[g];
            out << "    {\n      \"base_layer\": " << machine.baseLayer
                << ",\n      \"layer_type\": " << machine.layerType
                << ",\n      \"state_count\": " << machine.states.size()
                << ",\n      \"category_count\": " << machine.categories.size()
                << ",\n      \"states\": [\n";
            for (size_t s = 0; s < machine.states.size(); ++s) {
                const auto& state = machine.states[s];
                auto found = animByHash.find(state.animation);
                const AnimInfo info = (found != animByHash.end()) ? found->second : AnimInfo{};
                out << "        {\n";
                out << "          \"state\": \"" << JsonEscape(name(state.id)) << "\",\n";
                out << "          \"category\": \"" << JsonEscape(name(state.category)) << "\",\n";
                out << "          \"animation\": \"" << JsonEscape(name(state.animation)) << "\",\n";
                out << "          \"anim_resolved\": " << info.found << ",\n";
                out << "          \"anim_name\": \"" << JsonEscape(info.name) << "\",\n";
                out << "          \"looping\": " << info.looping << ",\n";
                out << "          \"duration\": " << info.duration << ",\n";
                out << "          \"flags\": " << state.flags << ",\n";
                out << "          \"blend_time\": "
                    << AlsBlendTimeForState(state.flags, info.duration) << ",\n";
                out << "          \"biped_physics\": " << ((state.flags & 0x0800) != 0) << ",\n";
                out << "          \"retrigger_same_category\": "
                    << ((state.flags & 0x0400) != 0) << ",\n";
                out << "          \"transition_groups\": [";
                for (size_t t = 0; t < state.transitionGroups.size(); ++t) {
                    if (t) out << ", ";
                    out << state.transitionGroups[t];
                }
                out << "],\n";
                emitRules(state.implicitRules, "implicit", "          ", true);
                emitRules(state.explicitRules, "explicit", "          ", true);
                emitRules(state.layerRules,    "layer",    "          ", false);
                out << "        }" << (s + 1 < machine.states.size() ? ",\n" : "\n");
            }
            out << "      ],\n      \"categories\": [\n";
            for (size_t c = 0; c < machine.categories.size(); ++c) {
                const auto& category = machine.categories[c];
                out << "        {\n";
                out << "          \"category\": \"" << JsonEscape(name(category.id)) << "\",\n";
                out << "          \"flags\": " << category.flags << ",\n";
                out << "          \"force_entry\": " << ((category.flags & 0x2) != 0) << ",\n";
                out << "          \"cold_start_state\": \""
                    << JsonEscape(name(category.label)) << "\",\n";
                out << "          \"force_default_state\": \""
                    << JsonEscape(name(category.forcedState)) << "\",\n";
                out << "          \"force_conditions\": [";
                for (size_t f = 0; f < category.forceConditions.size(); ++f) {
                    const auto& alter = category.forceConditions[f];
                    if (f) out << ", ";
                    out << "{\"mode\": " << alter.mode
                        << ", \"value\": " << alter.value
                        << ", \"param\": \"" << JsonEscape(name(alter.parameter)) << "\"}";
                }
                out << "],\n";
                out << "          \"transition_groups\": [";
                for (size_t t = 0; t < category.transitionGroups.size(); ++t) {
                    if (t) out << ", ";
                    out << category.transitionGroups[t];
                }
                out << "],\n";
                emitRules(category.implicitRules, "implicit", "          ", true);
                emitRules(category.explicitRules, "explicit", "          ", true);
                emitRules(category.incomingRules, "incoming", "          ", true);
                emitRules(category.layerRules,    "layer",    "          ", false);
                out << "        }" << (c + 1 < machine.categories.size() ? ",\n" : "\n");
            }
            out << "      ],\n      \"transition_groups\": [\n";
            for (size_t t = 0; t < machine.transitionGroups.size(); ++t) {
                const auto& group = machine.transitionGroups[t];
                out << "        {\n";
                out << "          \"index\": " << t << ",\n";
                out << "          \"nested_groups\": " << group.transitionGroups.size() << ",\n";
                emitRules(group.implicitRules, "implicit", "          ", true);
                emitRules(group.explicitRules, "explicit", "          ", true);
                emitRules(group.layerRules,    "layer",    "          ", false);
                out << "        }" << (t + 1 < machine.transitionGroups.size() ? ",\n" : "\n");
            }

            const bool lastMachine =
                (m + 1 == tool.animationStateMachines.size()) && (g + 1 == file.machines.size());
            out << "      ]\n    }" << (lastMachine ? "\n" : ",\n");
        }
    }
    out << "  ],\n  \"meta_animations\": [\n";
    bool firstMeta = true;
    for (const auto& file : tool.animationStateMachines) {
        for (const auto& meta : file.metaAnimations) {
            if (!firstMeta) out << ",\n";
            firstMeta = false;
            out << "    {\"kind\": " << static_cast<int>(meta.kind)
                << ", \"type\": " << meta.type
                << ", \"name\": \"" << JsonEscape(meta.name)
                << "\", \"hash\": \"" << JsonEscape(name(meta.hash))
                << "\", \"keys\": [";
            for (size_t k = 0; k < meta.animationKeys.size(); ++k) {
                if (k) out << ", ";
                out << "\"" << JsonEscape(name(meta.animationKeys[k])) << "\"";
            }
            out << "]}";
        }
    }
    out << (firstMeta ? "" : "\n") << "  ],\n  \"params\": {\n";
    {
        std::map<std::string, std::string> merged;
        for (const auto& file : tool.animationStateMachines) {
            for (const auto& param : file.params) {
                std::ostringstream value;
                if (param.type == 1) value << param.ivalue;
                else if (param.type == 0) value << param.fvalue;
                else value << "\"type" << param.type << "\"";
                merged[AlsResolveHash(tool, param.name)] = value.str();
            }
        }
        size_t written = 0;
        for (const auto& entry : merged) {
            out << "    \"" << JsonEscape(entry.first) << "\": " << entry.second
                << (++written < merged.size() ? ",\n" : "\n");
        }
    }
    out << "  }\n}\n";

    std::ofstream stream(fs::absolute(outArg), std::ios::binary);
    if (!stream) {
        std::cerr << "Cannot write " << outArg << std::endl;
        return 4;
    }
    stream << out.str();
    stream.close();

    size_t states = 0, categories = 0, metas = 0;
    for (const auto& file : tool.animationStateMachines) {
        for (const auto& machine : file.machines) {
            states += machine.states.size();
            categories += machine.categories.size();
        }
        metas += file.metaAnimations.size();
    }
    std::cout << "states=" << states << " categories=" << categories
              << " meta_anims=" << metas << " anims_in_pack=" << animByHash.size()
              << " -> " << fs::absolute(outArg).string() << std::endl;
    return 0;
}

static int DumpParamsHeadless(const std::string& packArg, const std::string& outArg) {
    const fs::path packPath = fs::absolute(packArg);
    if (!fs::exists(packPath)) {
        std::cerr << "Pack does not exist: " << packPath.string() << std::endl;
        return 2;
    }

    SpiderManTool tool;
    const fs::path packDir = packPath.parent_path();
    tool.searchPath = packDir.string();
    LoadBestDictionaryForPack(tool, packDir);
    tool.OpenPCPack(packPath.string());

    if (!tool.currentDir.valid || tool.pcPackData.empty()) {
        std::cerr << "Pack directory did not parse" << std::endl;
        return 3;
    }

    std::ostringstream out;
    out << "{\n  \"blocks\": [\n";
    size_t blockCount = 0, totalRecords = 0;

    for (const auto& resource : tool.currentDir.resources) {
        if (resource.type != RES_KEY_BASE_AI || resource.size < 12) continue;
        if (resource.offset > tool.pcPackData.size() ||
            resource.size > tool.pcPackData.size() - resource.offset) continue;

        const uint8_t* base = tool.pcPackData.data() + resource.offset;

        size_t bestStart = 0, bestRun = 0;
        for (size_t start = 0; start + 12 <= resource.size && start < 256; start += 4) {
            size_t run = 0;
            uint32_t prev = 0;
            bool first = true;
            for (size_t p = start; p + 12 <= resource.size; p += 12) {
                uint32_t hash = 0, type = 0;
                std::memcpy(&hash, base + p, 4);
                std::memcpy(&type, base + p + 8, 4);
                if (type > 1) break;
                if (!first && hash <= prev) break;
                prev = hash;
                first = false;
                ++run;
            }
            if (run > bestRun) { bestRun = run; bestStart = start; }
        }
        if (bestRun < 4) continue;

        if (blockCount) out << ",\n";
        out << "    {\n      \"resource\": \"" << JsonEscape(AlsResolveHash(tool, resource.hash))
            << "\",\n      \"records\": " << bestRun
            << ",\n      \"params\": {\n";
        for (size_t i = 0; i < bestRun; ++i) {
            const size_t p = bestStart + i * 12;
            uint32_t hash = 0, type = 0, raw = 0;
            std::memcpy(&hash, base + p, 4);
            std::memcpy(&raw, base + p + 4, 4);
            std::memcpy(&type, base + p + 8, 4);
            float asFloat = 0.f;
            std::memcpy(&asFloat, &raw, 4);
            out << "        \"" << JsonEscape(AlsResolveHash(tool, hash)) << "\": ";
            if (type == 1) out << static_cast<int32_t>(raw);
            else           out << asFloat;
            out << (i + 1 < bestRun ? ",\n" : "\n");
        }
        out << "      }\n    }";
        ++blockCount;
        totalRecords += bestRun;
    }
    out << "\n  ]\n}\n";

    std::ofstream stream(fs::absolute(outArg), std::ios::binary);
    if (!stream) { std::cerr << "Cannot write " << outArg << std::endl; return 4; }
    stream << out.str();
    stream.close();

    std::cout << "param blocks=" << blockCount << " records=" << totalRecords
              << " -> " << fs::absolute(outArg).string() << std::endl;
    return 0;
}

static int DumpTentacleCurvesHeadless(const std::string& packArg, const std::string& outArg) {
    const fs::path packPath = fs::absolute(packArg);
    if (!fs::exists(packPath)) {
        std::cerr << "Pack does not exist: " << packPath.string() << std::endl;
        return 2;
    }

    SpiderManTool tool;
    const fs::path packDir = packPath.parent_path();
    tool.searchPath = packDir.string();
    LoadBestDictionaryForPack(tool, packDir);
    for (const auto& entry : fs::directory_iterator(packDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext == ".pcpack") tool.foundPacks.push_back(entry.path());
    }
    std::sort(tool.foundPacks.begin(), tool.foundPacks.end());
    tool.BuildGlobalSkeletonIndex();
    tool.OpenPCPack(packPath.string());

    if (!tool.loadedAnimFile) {
        std::cerr << "No animation file in pack" << std::endl;
        return 3;
    }

    static const char* kChainNames[5] = {
        "UpLeftTent", "UpRightTent", "LowLeftTent", "LowRightTent", "Tongue"};
    static const char* kTrackNames[3] = {"Diameter", "Activity", "Pull"};

    std::ofstream out(outArg);
    if (!out) {
        std::cerr << "Cannot write: " << outArg << std::endl;
        return 4;
    }

    out << "{\n  \"pack\": \"" << packPath.stem().string() << "\",\n";
    out << "  \"chains\": [\"UpLeftTent\", \"UpRightTent\", \"LowLeftTent\", "
           "\"LowRightTent\", \"Tongue\"],\n";
    out << "  \"animations\": [\n";

    size_t written = 0;
    for (const auto& animation : tool.loadedAnimFile->animations) {
        int tentacleIndex = -1;
        for (size_t ci = 0; ci < animation.components.size(); ++ci) {
            if (animation.components[ci].comp_ix == NalComp::TENTACLE &&
                !animation.components[ci].decoded.frames.empty()) {
                tentacleIndex = static_cast<int>(ci);
                break;
            }
        }
        if (tentacleIndex < 0) continue;
        const auto& frames = animation.components[tentacleIndex].decoded.frames;

        if (written++) out << ",\n";
        out << "    {\n      \"name\": \"" << animation.name << "\",\n";
        out << "      \"frames\": " << frames.size() << ",\n";
        out << "      \"curves\": {\n";
        for (int chain = 0; chain < 5; ++chain) {
            for (int track = 0; track < 3; ++track) {
                const size_t valueIndex = static_cast<size_t>(chain) * 3 + track;
                out << "        \"" << kChainNames[chain] << "_" << kTrackNames[track] << "\": [";
                for (size_t frame = 0; frame < frames.size(); ++frame) {
                    if (frame) out << ", ";
                    out << (valueIndex < frames[frame].size() ? frames[frame][valueIndex] : 0.0f);
                }
                out << (chain == 4 && track == 2 ? "]\n" : "],\n");
            }
        }
        out << "      }\n    }";
    }
    out << "\n  ]\n}\n";
    std::cout << "Wrote " << written << " animations with tentacle tracks to "
              << outArg << std::endl;
    return 0;
}

static int ScanAnimationFailuresHeadless(const std::string& packDirArg) {
    const fs::path packDir = fs::absolute(packDirArg);
    if (!fs::is_directory(packDir)) return 2;
    SpiderManTool tool;
    tool.searchPath = packDir.string();
    LoadBestDictionaryForPack(tool, packDir);
    for (const auto& entry : fs::directory_iterator(packDir)) {
        if (!entry.is_regular_file()) continue;
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension == ".pcpack") tool.foundPacks.push_back(entry.path());
    }
    std::sort(tool.foundPacks.begin(), tool.foundPacks.end());
    tool.BuildGlobalSkeletonIndex();
    uint64_t animationCount = 0, genericWarningCount = 0, componentFailureCount = 0;
    for (const auto& packPath : tool.foundPacks) {
        tool.OpenPCPack(packPath.string());
        if (!tool.loadedAnimFile) continue;
        for (const auto& animation : tool.loadedAnimFile->animations) {
            ++animationCount;
            if (animation.is_gen_anim() && !animation.generic_decoded.complete) {
                ++genericWarningCount;
                std::cout << "[GENERIC WARN] " << packPath.filename().string()
                          << " / " << animation.name;
                for (const auto& warning : animation.generic_decoded.warnings)
                    std::cout << " | " << warning;
                std::cout << std::endl;
            }
            for (const auto& component : animation.components) {
                if (component.decode_error.empty()) continue;
                ++componentFailureCount;
                std::cout << "[COMPONENT FAIL] " << packPath.filename().string()
                          << " / " << animation.name << " / " << component.comp_ix
                          << " | " << component.decode_error << std::endl;
            }
        }
    }
    return componentFailureCount == 0 ? 0 : 21;
}

static int ValidateAllPacksHeadless(const std::string& packDirArg) {
    fs::path packDir = fs::absolute(packDirArg);
    if (!fs::is_directory(packDir)) {
        std::cerr << "Pack directory does not exist: " << packDir.string() << std::endl;
        return 2;
    }

    SpiderManTool tool;
    tool.searchPath = packDir.string();
    LoadBestDictionaryForPack(tool, packDir);

    try {
        for (const auto& entry : fs::directory_iterator(packDir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return (char)std::tolower(c);
            });
            if (ext == ".pcpack") tool.foundPacks.push_back(entry.path());
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to scan pack folder: " << e.what() << std::endl;
        return 3;
    }
    std::sort(tool.foundPacks.begin(), tool.foundPacks.end());
    if (tool.foundPacks.empty()) {
        std::cerr << "No PCPACK files found in: " << packDir.string() << std::endl;
        return 4;
    }

    tool.BuildGlobalTextureIndex();
    for (int i = 0; i < (int)tool.foundPacks.size(); ++i)
        tool.BuildGlobalTextureIndexStep(i);
    tool.BuildGlobalSkeletonIndex();

    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path scratchDir = fs::temp_directory_path() /
        ("usm_pack_validation_" + std::to_string(stamp));
    std::error_code fsError;
    fs::create_directories(scratchDir, fsError);
    if (fsError) {
        std::cerr << "Failed to create validation directory: " << fsError.message() << std::endl;
        return 5;
    }

    uint64_t resourceCount = 0;
    uint64_t skeletonCount = 0;
    uint64_t animationCount = 0;
    uint64_t componentCount = 0;
    uint64_t decodedFrameCount = 0;
    uint64_t decodedValueCount = 0;
    uint64_t pcmCount = 0;
    uint64_t convertedPcmCount = 0;
    uint64_t nonRenderablePcmCount = 0;
    uint64_t morphAnimationCount = 0;
    uint64_t warningCount = 0;
    uint64_t failureCount = 0;

    for (size_t packIndex = 0; packIndex < tool.foundPacks.size(); ++packIndex) {
        const fs::path& packPath = tool.foundPacks[packIndex];
        try {
            tool.OpenPCPack(packPath.string());
            if (!tool.currentDir.valid) {
                std::cerr << "[FAIL] invalid directory: " << packPath.filename().string()
                          << " (" << tool.currentDir.error << ")" << std::endl;
                ++failureCount;
                continue;
            }

            resourceCount += tool.currentDir.resources.size();
            for (const auto& resource : tool.currentDir.resources) {
                if ((uint64_t)resource.offset + resource.size > tool.pcPackData.size()) {
                    std::cerr << "[FAIL] resource outside pack: " << packPath.filename().string()
                              << " offset=" << resource.offset << " size=" << resource.size << std::endl;
                    ++failureCount;
                }
            }

            skeletonCount += tool.skeletonCandidates.size();
            for (const auto& candidate : tool.skeletonCandidates) {
                if (!candidate.data) {
                    ++failureCount;
                    continue;
                }
                warningCount += candidate.data->warnings.size();
            }

            if (tool.loadedAnimFile) {
                warningCount += tool.loadedAnimFile->warnings.size();
                animationCount += tool.loadedAnimFile->animations.size();
                for (const auto& animation : tool.loadedAnimFile->animations) {
                    warningCount += animation.warnings.size();
                    componentCount += animation.components.size();
                    if (animation.is_gen_anim()) {
                        bool hasMorphTrack = false;
                        for (const auto& warning : animation.generic_decoded.warnings) {
                            if (warning.find("USMMorph generic tracks") != std::string::npos) {
                                hasMorphTrack = true;
                                break;
                            }
                        }
                        if (hasMorphTrack) {
                            ++morphAnimationCount;
                        }
                        if (!animation.generic_decoded.complete) {

                            std::cerr << "[WARN] non-renderable generic animation: "
                                      << packPath.filename().string() << " / "
                                      << animation.name << std::endl;
                            for (const auto& warning : animation.generic_decoded.warnings)
                                std::cerr << "       " << warning << std::endl;
                        }
                        decodedFrameCount += animation.generic_decoded.world_frames.size();
                        for (const auto& frame : animation.generic_decoded.world_frames) {
                            for (const auto& matrix : frame) {
                                decodedValueCount += matrix.size();
                                for (float value : matrix) {
                                    if (!std::isfinite(value)) {
                                        std::cerr << "[FAIL] non-finite generic matrix: "
                                                  << packPath.filename().string() << " / "
                                                  << animation.name << std::endl;
                                        ++failureCount;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    for (const auto& component : animation.components) {
                        if (!component.decode_error.empty()) {
                            std::cerr << "[FAIL] " << packPath.filename().string() << " / "
                                      << animation.name << " component " << component.comp_ix
                                      << ": " << component.decode_error << std::endl;
                            ++failureCount;
                        }
                        decodedFrameCount += component.decoded.frames.size();
                        for (const auto& frame : component.decoded.frames) {
                            decodedValueCount += frame.size();
                            for (float value : frame) {
                                if (!std::isfinite(value)) {
                                    std::cerr << "[FAIL] non-finite decoded value: "
                                              << packPath.filename().string() << " / "
                                              << animation.name << std::endl;
                                    ++failureCount;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            for (const auto& fileEntry : tool.entries) {
                if (!fileEntry.isPcm) continue;
                ++pcmCount;
                if ((uint64_t)fileEntry.offset + fileEntry.size > tool.pcPackData.size()) {
                    std::cerr << "[FAIL] PCM outside pack: " << packPath.filename().string()
                              << " / " << fileEntry.name << std::endl;
                    ++failureCount;
                    continue;
                }

                tool.SelectSkeletonForMesh(fileEntry.name, fileEntry.hash);
                fs::path probePath = scratchDir / fs::path(fileEntry.name).filename();
                probePath.replace_extension(".glb");
                fs::remove(probePath, fsError);
                fsError.clear();

                std::vector<uint8_t> pcmData(
                    tool.pcPackData.begin() + fileEntry.offset,
                    tool.pcPackData.begin() + fileEntry.offset + fileEntry.size);
                tool.ConvertPCM(pcmData, probePath.string());

                if (fs::exists(probePath, fsError) && fs::file_size(probePath, fsError) >= 20) {
                    ++convertedPcmCount;
                } else {

                    ++nonRenderablePcmCount;
                }
                fs::remove(probePath, fsError);
                fsError.clear();
            }
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << packPath.filename().string() << ": " << e.what() << std::endl;
            ++failureCount;
        } catch (...) {
            std::cerr << "[FAIL] " << packPath.filename().string() << ": unknown exception" << std::endl;
            ++failureCount;
        }
    }

    fs::remove(scratchDir, fsError);
    return failureCount == 0 ? 0 : 10;
}

static int TestPreviewTabCacheHeadless() {
    SpiderManTool tool;
    tool.previewTabs.resize(2);
    tool.previewTabs[0].id = 1;
    tool.previewTabs[0].label = "first.pcm";
    tool.previewTabs[1].id = 2;
    tool.previewTabs[1].label = "second.pcm";
    tool.activePreviewTab = 0;

    RenderMesh firstMesh{};
    firstMesh.vao = 101;
    firstMesh.vbo = 102;
    firstMesh.ebo = 103;
    tool.previewMeshes.push_back(firstMesh);
    tool.proceduralTentacleMesh.vao = 111;
    tool.proceduralTentacleMesh.vbo = 112;
    tool.proceduralTentacleMesh.ebo = 113;
    tool.isModelLoaded = true;
    tool.isModelPreview = true;
    tool.camPos[0] = 11.0f;
    tool.animPlaybackTime = 3.25f;
    tool.loadedSkeletonName = "first_skeleton";
    tool.loadedVisemeStreams.resize(1);
    tool.loadedVisemeStreams[0].name = "first_viseme";
    tool.selectedVisemeIndex = 0;
    tool.StoreActivePreviewTab();

    RenderMesh secondMesh{};
    secondMesh.vao = 201;
    secondMesh.vbo = 202;
    secondMesh.ebo = 203;
    auto& second = tool.previewTabs[1];
    second.hasCachedState = true;
    second.modelLoaded = true;
    second.modelPreview = true;
    second.meshes.push_back(secondMesh);
    second.proceduralTentacleMesh.vao = 211;
    second.proceduralTentacleMesh.vbo = 212;
    second.proceduralTentacleMesh.ebo = 213;
    second.camPos[0] = 22.0f;
    second.animPlaybackTime = 7.5f;
    second.loadedSkeletonName = "second_skeleton";
    second.loadedVisemeStreams.resize(1);
    second.loadedVisemeStreams[0].name = "second_viseme";
    second.selectedVisemeIndex = 0;

    tool.ActivatePreviewTab(1);
    const bool secondRestored = tool.previewMeshes.size() == 1 &&
        tool.previewMeshes[0].vao == 201 && tool.proceduralTentacleMesh.vao == 211 &&
        tool.camPos[0] == 22.0f &&
        tool.animPlaybackTime == 7.5f && tool.loadedSkeletonName == "second_skeleton" &&
        tool.selectedVisemeIndex == 0 && tool.loadedVisemeStreams.size() == 1 &&
        tool.loadedVisemeStreams[0].name == "second_viseme";

    tool.camPos[0] = 23.0f;
    tool.animPlaybackTime = 8.25f;
    tool.ActivatePreviewTab(0);
    const bool firstRestored = tool.previewMeshes.size() == 1 &&
        tool.previewMeshes[0].vao == 101 && tool.proceduralTentacleMesh.vao == 111 &&
        tool.camPos[0] == 11.0f &&
        tool.animPlaybackTime == 3.25f && tool.loadedSkeletonName == "first_skeleton" &&
        tool.selectedVisemeIndex == 0 && tool.loadedVisemeStreams.size() == 1 &&
        tool.loadedVisemeStreams[0].name == "first_viseme";

    tool.ActivatePreviewTab(1);
    const bool secondPreserved = tool.previewMeshes.size() == 1 &&
        tool.previewMeshes[0].vao == 201 && tool.proceduralTentacleMesh.vao == 211 &&
        tool.camPos[0] == 23.0f &&
        tool.animPlaybackTime == 8.25f && tool.loadedVisemeStreams.size() == 1 &&
        tool.loadedVisemeStreams[0].name == "second_viseme";

    const bool ok = secondRestored && firstRestored && secondPreserved;
    return ok ? 0 : 11;
}

static int TestVisemePlaybackHeadless(const std::string& packArg) {
    const fs::path packPath = fs::absolute(packArg);
    if (!fs::exists(packPath)) {
        std::cerr << "Pack does not exist: " << packPath.string() << std::endl;
        return 2;
    }

    SpiderManTool tool;
    tool.searchPath = packPath.parent_path().string();
    LoadBestDictionaryForPack(tool, packPath.parent_path());
    tool.OpenPCPack(packPath.string());

    const PackResourceEntry* morphResource = nullptr;
    for (const auto& resource : tool.currentDir.resources) {
        if (resource.type == RES_KEY_MORPH) {
            morphResource = &resource;
            break;
        }
    }
    if (!morphResource || tool.loadedVisemeStreams.empty()) {
        std::cerr << "Pack has no local PCMORPH/viseme pair" << std::endl;
        return 12;
    }
    tool.LoadMorphForCurrentModel(morphResource->hash);
    if (!tool.loadedMorphFile.valid) return 13;

    int streamIndex = 0;
    for (int index = 0; index < static_cast<int>(tool.loadedVisemeStreams.size()); ++index) {
        if (tool.loadedVisemeStreams[index].name.find("_EDD_") != std::string::npos) {
            streamIndex = index;
            break;
        }
    }
    const auto& stream = tool.loadedVisemeStreams[streamIndex];
    if (tool.loadedMorphFile.sets.size() <= stream.channel_count) return 14;
    tool.selectedAnimIndex = -1;
    tool.selectedVisemeIndex = streamIndex;
    tool.isAnimPlaying = true;
    tool.animPlaybackTime = 0.0f;
    tool.UpdateAnimationPlayback(0.0f);

    auto weightsMatch = [&](uint32_t frame) {
        const float* source = stream.frame(frame);
        if (!source) return false;
        for (uint32_t channel = 0; channel < stream.channel_count; ++channel) {
            if (std::memcmp(&tool.morphTargetWeights[channel + 1],
                            &source[channel], sizeof(float)) != 0) return false;
        }
        return true;
    };
    const bool frameZeroExact = weightsMatch(0);

    tool.animPlaybackTime = 1.5f / static_cast<float>(stream.sample_rate);
    tool.UpdateAnimationPlayback(0.0f);
    const bool frameOneExact = stream.frame_count < 2 ||
        (tool.currentAnimFrame == 1 && weightsMatch(1));

    tool.animPlaybackTime = static_cast<float>(stream.frame_count) /
                            static_cast<float>(stream.sample_rate);
    tool.UpdateAnimationPlayback(1.0f / static_cast<float>(stream.sample_rate));
    const bool stoppedNeutral = !tool.isAnimPlaying &&
        std::all_of(tool.morphTargetWeights.begin(), tool.morphTargetWeights.end(),
                    [](float value) { return value == 0.0f; });

    const bool ok = frameZeroExact && frameOneExact && stoppedNeutral;
    return ok ? 0 : 15;
}

#ifdef _WIN32
static bool StdStreamAlreadyConnected(DWORD which) {
    HANDLE handle = GetStdHandle(which);
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    DWORD type = GetFileType(handle);
    return type == FILE_TYPE_CHAR || type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}
#endif

static void AttachParentConsoleIfPresent() {
#ifdef _WIN32
    const bool haveOut = StdStreamAlreadyConnected(STD_OUTPUT_HANDLE);
    const bool haveErr = StdStreamAlreadyConnected(STD_ERROR_HANDLE);
    const bool haveIn = StdStreamAlreadyConnected(STD_INPUT_HANDLE);
    if (haveOut && haveErr && haveIn) return;

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

    FILE* reopened = nullptr;
    if (!haveOut) freopen_s(&reopened, "CONOUT$", "w", stdout);
    if (!haveErr) freopen_s(&reopened, "CONOUT$", "w", stderr);
    if (!haveIn) freopen_s(&reopened, "CONIN$", "r", stdin);

    std::ios::sync_with_stdio(true);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();
#endif
}

int main(int argc, char** argv) {
    AttachParentConsoleIfPresent();

    if (argc >= 3 && std::string(argv[1]) == "--export-pack-glb") {
        return ExportPackGlbHeadless(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--validate-all-packs") {
        return ValidateAllPacksHeadless(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--inspect-morph-pack") {
        return InspectMorphPackHeadless(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--inspect-animation-morph") {
        return InspectAnimationMorphHeadless(argv[2]);
    }
    if (argc >= 4 && std::string(argv[1]) == "--dump-params") {
        return DumpParamsHeadless(argv[2], argv[3]);
    }
    if (argc >= 4 && std::string(argv[1]) == "--dump-als") {
        return DumpAlsHeadless(argv[2], argv[3]);
    }
    if (argc >= 4 && std::string(argv[1]) == "--dump-tentacle-curves") {
        return DumpTentacleCurvesHeadless(argv[2], argv[3]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--scan-animation-failures") {
        return ScanAnimationFailuresHeadless(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--validate-morph-packs") {
        return ValidateMorphPacksHeadless(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--test-viseme-playback") {
        return TestVisemePlaybackHeadless(argv[2]);
    }
    if (argc >= 2 && std::string(argv[1]) == "--test-preview-tab-cache") {
        return TestPreviewTabCacheHeadless();
    }

    const bool testWorldInstancingGl =
        argc >= 2 && std::string(argv[1]) == "--test-world-instancing-gl";
    const bool testMorphRenderGl =
        argc >= 3 && std::string(argv[1]) == "--test-morph-render-gl";
    const bool captureModelAnimationGl =
        argc >= 5 && std::string(argv[1]) == "--capture-model-animation-gl";

    if (!glfwInit()) return 1;
    if (testWorldInstancingGl || testMorphRenderGl || captureModelAnimationGl)
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    const char* glsl_version = "#version 130";
    GLFWwindow* window = glfwCreateWindow(1024, 768, "Ultimate Spider-Browser", NULL, NULL);
    if (!window) return 1;

#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(window);
    HICON hIcon = (HICON)LoadImage(GetModuleHandle(NULL), "IDI_ICON1", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (hIcon) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }
#endif

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        return -1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    SpiderManTool tool;

    if (captureModelAnimationGl) {
        const fs::path packPath = fs::absolute(argv[2]);
        const fs::path outputPath = fs::absolute(argv[3]);
        const std::string animationName = argv[4];
        const int requestedFrame = argc >= 6 ? std::max(0, std::atoi(argv[5])) : 0;
        std::string requestedModel = argc >= 7 ? argv[6] : packPath.stem().string();
        const float requestedOrbitDegrees = argc >= 8 ? std::strtof(argv[7], nullptr) : 0.0f;
        std::transform(requestedModel.begin(), requestedModel.end(), requestedModel.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (!fs::exists(packPath)) return 22;

        tool.InitModelPreview();
        tool.searchPath = packPath.parent_path().string();
        LoadBestDictionaryForPack(tool, packPath.parent_path());
        for (const auto& entry : fs::directory_iterator(packPath.parent_path())) {
            if (!entry.is_regular_file()) continue;
            std::string extension = entry.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (extension == ".pcpack") tool.foundPacks.push_back(entry.path());
        }
        std::sort(tool.foundPacks.begin(), tool.foundPacks.end());

        for (int packIndex = 0; packIndex < static_cast<int>(tool.foundPacks.size()); ++packIndex) {
            tool.BuildGlobalTextureIndexStep(packIndex);
        }
        tool.OpenPCPack(packPath.string());

        int modelIndex = -1;
        int firstModelIndex = -1;
        for (int index = 0; index < static_cast<int>(tool.entries.size()); ++index) {
            if (!tool.entries[index].isPcm) continue;
            if (firstModelIndex < 0) firstModelIndex = index;

            std::string candidate = tool.entries[index].name;
            std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            const fs::path candidatePath(candidate);
            if (!requestedModel.empty() &&
                (candidate == requestedModel || candidatePath.stem().string() == requestedModel)) {
                modelIndex = index;
                break;
            }
            if (!requestedModel.empty() && modelIndex < 0 &&
                candidate.find(requestedModel) != std::string::npos) {
                modelIndex = index;
            }
        }
        if (modelIndex < 0) modelIndex = firstModelIndex;
        if (modelIndex < 0) return 23;
        tool.LoadModelToGL(modelIndex);
        if (std::isfinite(requestedOrbitDegrees) && requestedOrbitDegrees != 0.0f) {
            constexpr float kPi = 3.14159265358979323846f;
            const float angle = requestedOrbitDegrees * kPi / 180.0f;
            const float offsetX = tool.camPos[0] - tool.modelCenter[0];
            const float offsetZ = tool.camPos[2] - tool.modelCenter[2];
            tool.camPos[0] = tool.modelCenter[0] + offsetX * std::cos(angle) - offsetZ * std::sin(angle);
            tool.camPos[2] = tool.modelCenter[2] + offsetX * std::sin(angle) + offsetZ * std::cos(angle);
            tool.camFront[0] = tool.modelCenter[0] - tool.camPos[0];
            tool.camFront[1] = tool.modelCenter[1] - tool.camPos[1];
            tool.camFront[2] = tool.modelCenter[2] - tool.camPos[2];
            const float frontLength = std::sqrt(tool.camFront[0] * tool.camFront[0] +
                tool.camFront[1] * tool.camFront[1] + tool.camFront[2] * tool.camFront[2]);
            if (frontLength > 0.0001f) {
                tool.camFront[0] /= frontLength;
                tool.camFront[1] /= frontLength;
                tool.camFront[2] /= frontLength;
            }
        }

        if (animationName == "scan") {
            struct VertexBufferCopy {
                std::vector<float> interleaved;
            };
            std::vector<VertexBufferCopy> copies(tool.previewMeshes.size());
            for (size_t section = 0; section < tool.previewMeshes.size(); ++section) {
                const auto& mesh = tool.previewMeshes[section];
                const size_t floatCount = (mesh.positions.size() / 3) * 20;
                copies[section].interleaved.resize(floatCount);
                glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
                glGetBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(floatCount * sizeof(float)),
                    copies[section].interleaved.data());
            }
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            struct ScanResult {
                std::string animation;
                int frame = 0;
                int section = 0;
                uint16_t edgeA = 0;
                uint16_t edgeB = 0;
                float edgeRatio = 0.0f;
            };
            std::vector<ScanResult> results;
            tool.captureEvaluatedSkinMatrices = true;
            tool.skipDrawAfterPoseEvaluation = true;
            tool.selectedVisemeIndex = -1;
            if (tool.loadedAnimFile) {
                for (int animationIndex = 0;
                     animationIndex < static_cast<int>(tool.loadedAnimFile->animations.size());
                     ++animationIndex) {
                    const auto& animation = tool.loadedAnimFile->animations[animationIndex];
                    const int frameCount = animation.playback_frame_count();
                    float animationMax = 0.0f;
                    int animationMaxFrame = 0, animationMaxSection = 0;
                    uint16_t animationEdgeA = 0, animationEdgeB = 0;
                    tool.selectedAnimIndex = animationIndex;
                    for (int frame = 0; frame < frameCount; ++frame) {
                        tool.currentAnimFrame = frame;
                        tool.animFrameFraction = 0.0f;
                        tool.RenderModelPreview();
                        if (!tool.evaluatedSkinningActive ||
                            tool.evaluatedGlobalBoneMatrices.empty()) continue;

                        for (size_t section = 0; section < tool.previewMeshes.size(); ++section) {
                            const auto& mesh = tool.previewMeshes[section];
                            const auto& source = copies[section].interleaved;
                            const size_t vertexCount = mesh.positions.size() / 3;
                            if (source.size() != vertexCount * 20) continue;
                            std::vector<std::array<float, 3>> deformed(vertexCount);
                            for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
                                const float* v = source.data() + vertex * 20;
                                float out[3] = {0.0f, 0.0f, 0.0f};
                                float usedWeight = 0.0f;
                                for (int influence = 0; influence < 4; ++influence) {
                                    const float weight = v[12 + influence];
                                    if (weight <= 0.0f) continue;
                                    const int local = static_cast<int>(v[8 + influence] + 0.5f);
                                    int global = local;
                                    if (!mesh.bonePalette.empty()) {
                                        if (local < 0 || local >= static_cast<int>(mesh.bonePalette.size()))
                                            continue;
                                        global = mesh.bonePalette[local];
                                    }
                                    if (global < 0 ||
                                        static_cast<size_t>(global * 16 + 15) >=
                                            tool.evaluatedGlobalBoneMatrices.size()) continue;
                                    const float* matrix =
                                        tool.evaluatedGlobalBoneMatrices.data() + global * 16;
                                    out[0] += weight * (matrix[0] * v[0] + matrix[4] * v[1] +
                                                       matrix[8] * v[2] + matrix[12]);
                                    out[1] += weight * (matrix[1] * v[0] + matrix[5] * v[1] +
                                                       matrix[9] * v[2] + matrix[13]);
                                    out[2] += weight * (matrix[2] * v[0] + matrix[6] * v[1] +
                                                       matrix[10] * v[2] + matrix[14]);
                                    usedWeight += weight;
                                }
                                if (usedWeight < 0.999f) {
                                    out[0] += (1.0f - usedWeight) * v[0];
                                    out[1] += (1.0f - usedWeight) * v[1];
                                    out[2] += (1.0f - usedWeight) * v[2];
                                }
                                deformed[vertex] = {out[0], out[1], out[2]};
                            }

                            float sectionMax = 0.0f;
                            uint16_t sectionEdgeA = 0, sectionEdgeB = 0;
                            auto edge = [&](uint16_t a, uint16_t b) {
                                if (a >= vertexCount || b >= vertexCount || a == b) return;
                                const float* pa = source.data() + static_cast<size_t>(a) * 20;
                                const float* pb = source.data() + static_cast<size_t>(b) * 20;
                                const float bx = pa[0] - pb[0], by = pa[1] - pb[1], bz = pa[2] - pb[2];
                                const float baseLength = std::sqrt(bx * bx + by * by + bz * bz);
                                if (baseLength < 1e-5f) return;
                                const auto& da = deformed[a];
                                const auto& db = deformed[b];
                                const float dx = da[0] - db[0], dy = da[1] - db[1], dz = da[2] - db[2];
                                const float ratio =
                                    std::sqrt(dx * dx + dy * dy + dz * dz) / baseLength;
                                if (ratio > sectionMax) {
                                    sectionMax = ratio;
                                    sectionEdgeA = a;
                                    sectionEdgeB = b;
                                }
                            };
                            if (mesh.mode == GL_TRIANGLES) {
                                for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                                    edge(mesh.indices[i], mesh.indices[i + 1]);
                                    edge(mesh.indices[i + 1], mesh.indices[i + 2]);
                                    edge(mesh.indices[i + 2], mesh.indices[i]);
                                }
                            } else {
                                for (size_t i = 0; i + 2 < mesh.indices.size(); ++i) {
                                    edge(mesh.indices[i], mesh.indices[i + 1]);
                                    edge(mesh.indices[i + 1], mesh.indices[i + 2]);
                                    edge(mesh.indices[i + 2], mesh.indices[i]);
                                }
                            }
                            if (sectionMax > animationMax) {
                                animationMax = sectionMax;
                                animationMaxFrame = frame;
                                animationMaxSection = static_cast<int>(section);
                                animationEdgeA = sectionEdgeA;
                                animationEdgeB = sectionEdgeB;
                            }
                        }
                    }
                    results.push_back({animation.name, animationMaxFrame,
                                       animationMaxSection, animationEdgeA,
                                       animationEdgeB, animationMax});
                }
            }
            return 0;
        }

        int animationIndex = animationName == "none" ? -2 : -1;
        if (animationIndex != -2 && tool.loadedAnimFile) {
            for (int index = 0;
                 index < static_cast<int>(tool.loadedAnimFile->animations.size()); ++index) {
                if (tool.loadedAnimFile->animations[index].name == animationName) {
                    animationIndex = index;
                    break;
                }
            }
        }
        if (animationIndex == -1) return 24;
        const NalAnimEntry* animation = animationIndex >= 0
            ? &tool.loadedAnimFile->animations[animationIndex]
            : nullptr;
        tool.selectedAnimIndex = animationIndex;
        tool.selectedVisemeIndex = -1;
        tool.currentAnimFrame = animation
            ? std::min(requestedFrame, std::max(0, animation->playback_frame_count() - 1))
            : 0;
        tool.animFrameFraction = 0.0f;
        tool.animPlaybackTime = static_cast<float>(tool.currentAnimFrame) / 30.0f;
        tool.isAnimPlaying = false;
        tool.showSkeleton = true;
        tool.captureEvaluatedSkinMatrices = true;
        tool.RenderModelPreview();

        constexpr int width = 3840;
        constexpr int height = 2160;
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        std::vector<uint8_t> flipped(pixels.size());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, tool.modelFbo);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (int row = 0; row < height; ++row) {
            std::memcpy(flipped.data() + static_cast<size_t>(row) * width * 4,
                        pixels.data() + static_cast<size_t>(height - 1 - row) * width * 4,
                        static_cast<size_t>(width) * 4);
        }
        const int wrote = stbi_write_png(outputPath.string().c_str(), width, height, 4,
                                         flipped.data(), width * 4);
        return wrote ? 0 : 25;
    }

    if (testMorphRenderGl) {
        const fs::path packPath = fs::absolute(argv[2]);
        if (!fs::exists(packPath)) return 16;
        tool.InitModelPreview();
        tool.searchPath = packPath.parent_path().string();
        LoadBestDictionaryForPack(tool, packPath.parent_path());
        tool.OpenPCPack(packPath.string());

        uint32_t morphHash = 0;
        uint32_t requestedMorphHash = 0;
        if (argc >= 4) {
            try { requestedMorphHash = static_cast<uint32_t>(std::stoul(argv[3], nullptr, 0)); }
            catch (...) { return 17; }
        }
        for (const auto& resource : tool.currentDir.resources) {
            if (resource.type == RES_KEY_MORPH &&
                (!requestedMorphHash || resource.hash == requestedMorphHash)) {
                morphHash = resource.hash;
                break;
            }
        }
        int modelIndex = -1;
        for (int index = 0; index < static_cast<int>(tool.entries.size()); ++index) {
            if (tool.entries[index].isPcm && tool.entries[index].hash == morphHash) {
                modelIndex = index;
                break;
            }
        }
        if (!morphHash || modelIndex < 0 || tool.loadedVisemeStreams.empty()) return 17;
        tool.LoadModelToGL(modelIndex);
        if (!tool.loadedMorphFile.valid || tool.previewMeshes.empty()) return 18;

        int streamIndex = -1;
        uint32_t sampleFrame = 0;
        for (int candidate = 0;
             candidate < static_cast<int>(tool.loadedVisemeStreams.size()) && streamIndex < 0;
             ++candidate) {
            const auto& stream = tool.loadedVisemeStreams[candidate];
            for (uint32_t frame = 0; frame < stream.frame_count && streamIndex < 0; ++frame) {
                const float* weights = stream.frame(frame);
                for (uint32_t channel = 0; weights && channel < stream.channel_count; ++channel) {
                    if (weights[channel] != 0.0f && channel + 1 < tool.loadedMorphFile.sets.size()) {
                        streamIndex = candidate;
                        sampleFrame = frame;
                        break;
                    }
                }
            }
        }
        if (streamIndex < 0) return 19;

        const auto& stream = tool.loadedVisemeStreams[streamIndex];
        tool.selectedAnimIndex = -1;
        tool.selectedVisemeIndex = streamIndex;
        tool.isAnimPlaying = true;
        tool.animPlaybackTime = (static_cast<float>(sampleFrame) + 0.25f) /
                                static_cast<float>(stream.sample_rate);
        tool.UpdateAnimationPlayback(0.0f);
        while (glGetError() != GL_NO_ERROR) {}
        tool.ApplyMorphTargets();

        bool cpuChanged = false;
        bool gpuExact = false;
        for (const auto& mesh : tool.previewMeshes) {
            if (!mesh.vbo || mesh.morphVertexData.empty() || mesh.positions.size() % 3 != 0) continue;
            const size_t vertexCount = mesh.positions.size() / 3;
            for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
                const float* cpu = mesh.morphVertexData.data() + vertex * 20;
                const float* base = mesh.positions.data() + vertex * 3;
                if (cpu[0] == base[0] && cpu[1] == base[1] && cpu[2] == base[2]) continue;
                cpuChanged = true;
                float gpu[3] = {};
                glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
                glGetBufferSubData(GL_ARRAY_BUFFER,
                    static_cast<GLintptr>(vertex * 20 * sizeof(float)), sizeof(gpu), gpu);
                gpuExact = std::memcmp(gpu, cpu, sizeof(gpu)) == 0;
                break;
            }
            if (cpuChanged) break;
        }
        const GLenum glError = glGetError();
        const bool ok = cpuChanged && gpuExact && glError == GL_NO_ERROR;
        return ok ? 0 : 20;
    }

    if (testWorldInstancingGl) {
        tool.InitModelPreview();

        GLint shaderLinked = GL_FALSE;
        glGetProgramiv(tool.modelProgram, GL_LINK_STATUS, &shaderLinked);
        const bool instancingFunctionsLoaded =
            glDrawElementsInstanced != nullptr && glVertexAttribDivisor != nullptr;

        RenderMesh mesh;
        struct TestVertex { float x, y, z; float r, g, b, a; };
        const TestVertex vertices[3] = {
            {-0.5f, -0.5f, 0.0f, 1, 1, 1, 1},
            { 0.5f, -0.5f, 0.0f, 1, 1, 1, 1},
            { 0.0f,  0.5f, 0.0f, 1, 1, 1, 1}
        };
        const uint16_t indices[3] = {0, 1, 2};
        glGenVertexArrays(1, &mesh.vao);
        glGenBuffers(1, &mesh.vbo);
        glGenBuffers(1, &mesh.ebo);
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TestVertex), (void*)0);
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(TestVertex),
                              (void*)(sizeof(float) * 3));
        glBindVertexArray(0);
        mesh.indexCount = 3;

        for (int i = 0; i < 2; ++i) {
            RenderMesh::Instance instance;
            instance.transform[0] = instance.transform[5] =
                instance.transform[10] = instance.transform[15] = 1.0f;
            instance.transform[12] = (float)i;
            mesh.instances.push_back(instance);
        }

        GLint instanceBufferBytes = 0;
        GLenum drawError = GL_INVALID_OPERATION;
        if (instancingFunctionsLoaded) {
            tool.RefreshInstanceBuffer(mesh);
            glBindBuffer(GL_ARRAY_BUFFER, mesh.instanceVbo);
            glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &instanceBufferBytes);

            float identity[16] = {0};
            identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
            glUseProgram(tool.modelProgram);
            glUniformMatrix4fv(glGetUniformLocation(tool.modelProgram, "model"), 1, GL_FALSE, identity);
            glUniformMatrix4fv(glGetUniformLocation(tool.modelProgram, "view"), 1, GL_FALSE, identity);
            glUniformMatrix4fv(glGetUniformLocation(tool.modelProgram, "projection"), 1, GL_FALSE, identity);
            glUniform1i(glGetUniformLocation(tool.modelProgram, "useSkinning"), 0);
            glUniform1i(glGetUniformLocation(tool.modelProgram, "useInstancing"), 1);
            glUniform1i(glGetUniformLocation(tool.modelProgram, "hasTexture"), 0);
            glUniform1i(glGetUniformLocation(tool.modelProgram, "isWater"), 0);
            glUniform1i(glGetUniformLocation(tool.modelProgram, "debugTransparent"), 0);
            glUniform1f(glGetUniformLocation(tool.modelProgram, "selectedInstanceIndex"), 1.0f);
            while (glGetError() != GL_NO_ERROR) {}
            glBindVertexArray(mesh.vao);
            glDrawElementsInstanced(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr, 2);
            drawError = glGetError();
        }

        const bool bufferOk = instanceBufferBytes == (GLint)(2 * (17 * sizeof(float)));
        const bool ok = shaderLinked == GL_TRUE && instancingFunctionsLoaded &&
                        mesh.instanceDrawCount == 2 && bufferOk && drawError == GL_NO_ERROR;
        return ok ? 0 : 12;
    }

    tool.LoadConfig();

    if (fs::exists("string_hash_dictionary.txt")) tool.LoadDictionary("string_hash_dictionary.txt");
    if (tool.dictionary.empty() && fs::exists("string_hash_dictionary.bin")) tool.LoadBinaryDictionary("string_hash_dictionary.bin");
    if (tool.dictionary.empty()) tool.LoadEmbeddedDictionary();

    tool.onLoadProgress = [&]() {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        RenderUI(tool);
        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        tool.UpdateAnimationPlayback(ImGui::GetIO().DeltaTime);

        RenderUI(tool);

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        if (tool.wantLoadWorld) {
            tool.wantLoadWorld = false;
            tool.LoadAllWorldGeometries();
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
