#include "AnimationStateMachine.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace UsmAls {
namespace {

class Parser {
public:
    Parser(const uint8_t* bytes, size_t byteCount, uint32_t hash)
        : data(bytes), size(byteCount) {
        result.resourceHash = hash;
    }

    File parse() {
        if (!data || size < 0x1C) fail("ALS resource is too small");
        cursor = 0x1C;
        parseMachineVector(0, true);
        if (pointer(0x14)) {
            const size_t object = read(0x40, 0);
            if (u32(object) != 484) fail("ALS base state machine has an unsupported type");
            result.machines.push_back(parseMachine(object, true));
        }
        if (pointer(0x18)) parseMetaTable();
        if (cursor != size) fail("ALS resource has unconsumed packed data");
        result.bytesConsumed = cursor;
        result.valid = !result.machines.empty();
        if (!result.valid) result.error = "ALS resource contains no state machines";
        return result;
    }

private:
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t cursor = 0;
    File result;

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error(message);
    }

    void require(size_t offset, size_t count) const {
        if (offset > size || count > size - offset) fail("ALS resource ended while reading packed data");
    }

    uint32_t u32(size_t offset) const {
        require(offset, 4);
        uint32_t value = 0;
        std::memcpy(&value, data + offset, 4);
        return value;
    }

    int32_t i32(size_t offset) const {
        return static_cast<int32_t>(u32(offset));
    }

    uint16_t u16(size_t offset) const {
        require(offset, 2);
        uint16_t value = 0;
        std::memcpy(&value, data + offset, 2);
        return value;
    }

    float f32(size_t offset) const {
        require(offset, 4);
        float value = 0.0f;
        std::memcpy(&value, data + offset, 4);
        return value;
    }

    bool pointer(size_t offset) const {
        return u32(offset) != 0;
    }

    int count(size_t vectorOffset) const {
        const int value = i32(vectorOffset + 4);
        if (value < 0 || value > 100000) fail("ALS vector count is invalid");
        return value;
    }

    bool vectorPresent(size_t vectorOffset) const {
        return pointer(vectorOffset + 8) && count(vectorOffset) > 0;
    }

    void align(size_t alignment) {
        const size_t mask = alignment - 1;
        cursor = (cursor + mask) & ~mask;
        if (cursor > size) fail("ALS alignment exceeded the resource size");
    }

    void deductiveAlign() {
        const size_t original = cursor;
        while (cursor < size && data[cursor] == 0xA1) ++cursor;
        if ((cursor & 3) != 0) {
            const size_t rounded = cursor & ~size_t(3);
            if (rounded < original) fail("ALS packed alignment is invalid");
            cursor = rounded;
        }
    }

    size_t read(size_t countValue, size_t alignment) {
        if (alignment) align(alignment); else deductiveAlign();
        require(cursor, countValue);
        const size_t offset = cursor;
        cursor += countValue;
        return offset;
    }

    void readPointerTable(size_t vectorOffset) {
        if (vectorPresent(vectorOffset)) read(static_cast<size_t>(count(vectorOffset)) * 4, 4);
    }

    std::vector<int> parseIntVector(size_t vectorOffset) {
        std::vector<int> values;
        if (!vectorPresent(vectorOffset)) return values;
        const int total = count(vectorOffset);
        const size_t block = read(static_cast<size_t>(total) * 4, 4);
        values.reserve(total);
        for (int index = 0; index < total; ++index) values.push_back(i32(block + static_cast<size_t>(index) * 4));
        return values;
    }

    void parseParamBlockPointer(size_t pointerOffset) {
        if (!pointer(pointerOffset)) return;
        const size_t block = read(0xC, 4);
        if (!pointer(block + 4)) return;
        const size_t array = read(0x18, 4);
        parseParamDataVector(array);
    }

    void parseParamDataVector(size_t vectorOffset) {
        if (!vectorPresent(vectorOffset)) return;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        for (int index = 0; index < total; ++index) {
            const size_t object = read(0xC, 4);
            switch (i32(object + 4)) {
                case 3: read(32, 4); break;
                case 4: read(12, 4); break;
                case 5: read(8, 4); break;
                default: break;
            }
        }
    }

    void parseMachineVector(size_t vectorOffset, bool layers) {
        if (!vectorPresent(vectorOffset)) return;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        for (int index = 0; index < total; ++index) {
            const size_t object = read(layers ? 0x48 : 0x40, 0);
            const uint32_t type = u32(object);
            if ((layers && type != 483) || (!layers && type != 484)) fail("ALS state machine type is unsupported");
            result.machines.push_back(parseMachine(object, false));
        }
    }

    Machine parseMachine(size_t object, bool baseLayer) {
        Machine machine;
        machine.baseLayer = baseLayer;
        if (!baseLayer) machine.layerType = i32(object + 0x44);
        parseStateVector(object + 4, machine);
        parseCategoryVector(object + 0x18, machine);
        parseTransitionGroupVector(object + 0x2C, machine);
        return machine;
    }

    void parseStateVector(size_t vectorOffset, Machine& machine) {
        if (!vectorPresent(vectorOffset)) return;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        machine.states.reserve(total);
        for (int index = 0; index < total; ++index) {
            const size_t base = read(0x14, 0);
            const uint32_t type = u32(base);
            size_t objectSize = 0;
            if (type == 532) objectSize = 0x54;
            if (type == 530) objectSize = 0x58;
            if (!objectSize) fail("ALS state type is unsupported");
            read(objectSize - 0x14, 1);
            State state;
            state.id = u32(base + 4);
            state.category = u32(base + 8);
            state.flags = u16(base + 0xC);
            state.animation = u32(base + 0x14);
            parseParamBlockPointer(base + 0x10);
            state.transitionGroups = parseIntVector(base + 0x18);
            state.implicitRules = parseBasicRuleVector(base + 0x28, RuleKind::Implicit, 0x24);
            state.explicitRules = parseBasicRuleVector(base + 0x3C, RuleKind::Explicit, 0x28);
            if (pointer(base + 0x50)) {
                const size_t rules = read(0x14, 4);
                state.layerRules = parseLayerRuleVector(rules);
            }
            machine.states.push_back(std::move(state));
        }
    }

    void parseCategoryVector(size_t vectorOffset, Machine& machine) {
        if (!vectorPresent(vectorOffset)) return;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        machine.categories.reserve(total);
        for (int index = 0; index < total; ++index) {
            const size_t base = read(0x10, 0);
            if (u32(base) != 531) fail("ALS category type is unsupported");
            read(0x7C - 0x10, 1);
            Category category;
            category.id = u32(base + 4);
            category.flags = i32(base + 8);
            category.label = u32(base + 0x10);
            category.forcedState = u32(base + 0x14);
            parseParamBlockPointer(base + 0xC);
            category.forceConditions = parseAlterVector(base + 0x18);
            category.transitionGroups = parseIntVector(base + 0x2C);
            category.implicitRules = parseBasicRuleVector(base + 0x3C, RuleKind::Implicit, 0x24);
            category.explicitRules = parseBasicRuleVector(base + 0x50, RuleKind::Explicit, 0x28);
            category.incomingRules = parseBasicRuleVector(base + 0x64, RuleKind::Incoming, 0x2C);
            if (pointer(base + 0x78)) {
                const size_t rules = read(0x14, 4);
                category.layerRules = parseLayerRuleVector(rules);
            }
            machine.categories.push_back(std::move(category));
        }
    }

    void parseTransitionGroupVector(size_t vectorOffset, Machine& machine) {
        if (!vectorPresent(vectorOffset)) return;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        machine.transitionGroups.reserve(total);
        for (int index = 0; index < total; ++index) {
            const size_t base = read(4, 0);
            if (u32(base) != 533) fail("ALS transition group type is unsupported");
            read(0x40 - 4, 1);
            TransitionGroup group;
            group.transitionGroups = parseIntVector(base + 4);
            group.implicitRules = parseBasicRuleVector(base + 0x14, RuleKind::Implicit, 0x24);
            group.explicitRules = parseBasicRuleVector(base + 0x28, RuleKind::Explicit, 0x28);
            if (pointer(base + 0x3C)) {
                const size_t rules = read(0x14, 4);
                group.layerRules = parseLayerRuleVector(rules);
            }
            machine.transitionGroups.push_back(std::move(group));
        }
    }

    std::vector<Rule> parseBasicRuleVector(size_t vectorOffset, RuleKind kind, size_t objectSize) {
        std::vector<Rule> rules;
        if (!vectorPresent(vectorOffset)) return rules;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        rules.reserve(total);
        for (int index = 0; index < total; ++index) {
            const size_t object = read(objectSize, 4);
            Rule rule = parseBasicRule(object, kind);
            rule.priority = index;
            if (kind == RuleKind::Explicit) rule.trigger = u32(object + 0x24);
            if (kind == RuleKind::Incoming) {
                rule.conditionA = i32(object + 0x24);
                rule.conditionB = i32(object + 0x28);
            }
            rules.push_back(std::move(rule));
        }
        return rules;
    }

    Rule parseBasicRule(size_t object, RuleKind kind) {
        Rule rule;
        rule.kind = kind;
        rule.filters = parseFilterVector(object);
        rule.action = parseAction(object + 0x14);
        rule.hasPostAction = pointer(object + 0x20);
        if (rule.hasPostAction) parsePostActionSet();
        return rule;
    }

    std::vector<Filter> parseFilterVector(size_t vectorOffset) {
        std::vector<Filter> filters;
        if (!vectorPresent(vectorOffset)) return filters;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        filters.reserve(total);
        for (int index = 0; index < total; ++index) {
            const size_t object = read(0xC, 4);
            filters.push_back({u32(object), f32(object + 4), f32(object + 8)});
        }
        return filters;
    }

    Action parseAction(size_t object) {
        Action action;
        action.type = i32(object);
        action.target = u32(object + 8);
        if (pointer(object + 4)) {
            const size_t destinations = read(0x14, 4);
            action.destinations = parseDestinationVector(destinations);
        } else if ((action.type == 0 || action.type == 1) && action.target != 0) {
            action.destinations.push_back({action.target, 1.0f});
        }
        return action;
    }

    std::vector<Destination> parseDestinationVector(size_t vectorOffset) {
        std::vector<Destination> destinations;
        if (!vectorPresent(vectorOffset)) return destinations;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        destinations.reserve(total);
        for (int index = 0; index < total; ++index) {
            const size_t object = read(8, 4);
            destinations.push_back({u32(object), f32(object + 4)});
        }
        return destinations;
    }

    std::vector<Rule> parseLayerRuleVector(size_t vectorOffset) {
        std::vector<Rule> rules;
        if (!vectorPresent(vectorOffset)) return rules;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        rules.reserve(total);
        for (int index = 0; index < total; ++index) {
            const size_t object = read(0x18, 4);
            Rule rule;
            rule.kind = RuleKind::Layer;
            rule.conditionA = i32(object);
            rule.conditionB = i32(object + 4);
            rule.action = parseAction(object + 8);
            rule.priority = index;
            rules.push_back(std::move(rule));
        }
        return rules;
    }

    void parsePostActionSet() {
        const size_t object = read(0x28, 4);
        parsePostKillVector(object);
        parsePostLayerVector(object + 0x14);
    }

    void parsePostKillVector(size_t vectorOffset) {
        if (!vectorPresent(vectorOffset)) return;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        for (int index = 0; index < total; ++index) read(8, 4);
    }

    void parsePostLayerVector(size_t vectorOffset) {
        if (!vectorPresent(vectorOffset)) return;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        for (int index = 0; index < total; ++index) {
            const size_t object = read(0x1C, 4);
            parseAlterVector(object + 4);
        }
    }

    std::vector<AlterCondition> parseAlterVector(size_t vectorOffset) {
        std::vector<AlterCondition> conditions;
        if (!vectorPresent(vectorOffset)) return conditions;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        conditions.reserve(total);
        for (int index = 0; index < total; ++index) {
            const size_t object = read(0xC, 4);
            conditions.push_back({i32(object), i32(object + 4), u32(object + 8)});
        }
        return conditions;
    }

    void parseMetaTable() {
        const size_t table = read(0x18, 4);
        const size_t vectorOffset = table;
        if (!vectorPresent(vectorOffset)) return;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        result.metaAnimations.reserve(total);
        for (int index = 0; index < total; ++index) result.metaAnimations.push_back(parseMetaAnimation());
    }

    MetaAnimation parseMetaAnimation() {
        deductiveAlign();
        require(cursor, 4);
        const uint32_t type = u32(cursor);
        size_t objectSize = 0;
        MetaKind kind = MetaKind::Unknown;
        switch (type) {
            case 149: objectSize = 0x30; kind = MetaKind::AimedShotVertical; break;
            case 150: objectSize = 0x38; kind = MetaKind::StrengthTest; break;
            case 485: objectSize = 0x60; kind = MetaKind::FourClip; break;
            case 486: objectSize = 0x48; kind = MetaKind::TwoClip; break;
            case 488: objectSize = 0x70; kind = MetaKind::Swing; break;
            case 489: objectSize = 0x40; kind = MetaKind::LinearBlend; break;
            case 573: objectSize = 0x28; kind = MetaKind::Animation; break;
            default: fail("ALS meta-animation type " + std::to_string(type) + " is unsupported");
        }
        const size_t object = read(objectSize, 1);
        MetaAnimation meta;
        meta.kind = kind;
        meta.type = type;
        meta.hash = u32(object + 8);
        require(object + 0xC, 28);
        const char* text = reinterpret_cast<const char*>(data + object + 0xC);
        size_t length = 0;
        while (length < 28 && text[length] != '\0') ++length;
        meta.name.assign(text, length);
        if (type == 485) {
            meta.animationKeys = {u32(object + 0x28), u32(object + 0x30),
                                  u32(object + 0x38), u32(object + 0x40)};
        }
        if (type == 486) {
            meta.animationKeys = {u32(object + 0x28), u32(object + 0x30)};
        }
        if (type == 488 || type == 489) meta.animationKeys = parseMetaKeyVector(object + 0x28);
        return meta;
    }

    std::vector<uint32_t> parseMetaKeyVector(size_t vectorOffset) {
        std::vector<uint32_t> keys;
        if (!vectorPresent(vectorOffset)) return keys;
        const int total = count(vectorOffset);
        readPointerTable(vectorOffset);
        keys.reserve(total);
        for (int index = 0; index < total; ++index) {
            const size_t object = read(0x10, 4);
            keys.push_back(u32(object));
        }
        return keys;
    }
};

}

File Parse(const uint8_t* data, size_t size, uint32_t resourceHash) {
    try {
        return Parser(data, size, resourceHash).parse();
    } catch (const std::exception& exception) {
        File file;
        file.resourceHash = resourceHash;
        file.error = exception.what();
        return file;
    }
}

}
