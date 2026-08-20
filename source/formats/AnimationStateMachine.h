#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace UsmAls {

enum class RuleKind {
    Implicit,
    Explicit,
    Incoming,
    Layer
};

enum class MetaKind {
    Animation,
    AimedShotVertical,
    StrengthTest,
    FourClip,
    TwoClip,
    Swing,
    LinearBlend,
    Interact,
    Unknown
};

struct Filter {
    uint32_t parameter = 0;
    float minimum = 0.0f;
    float maximum = 0.0f;
};

struct Destination {
    uint32_t target = 0;
    float weight = 1.0f;
};

struct AlterCondition {
    int mode = 0;
    int value = 0;
    uint32_t parameter = 0;
};

struct Action {
    int type = -1;
    uint32_t target = 0;
    std::vector<Destination> destinations;
};

struct Rule {
    RuleKind kind = RuleKind::Implicit;
    uint32_t trigger = 0;
    std::vector<Filter> filters;
    Action action;
    int conditionA = 0;
    int conditionB = 0;
    int priority = 0;
    bool hasPostAction = false;
};

struct State {
    uint32_t id = 0;
    uint32_t category = 0;
    uint32_t animation = 0;
    uint16_t flags = 0;
    std::vector<int> transitionGroups;
    std::vector<Rule> implicitRules;
    std::vector<Rule> explicitRules;
    std::vector<Rule> layerRules;
};

struct Category {
    uint32_t id = 0;
    uint32_t label = 0;
    int flags = 0;
    uint32_t forcedState = 0;
    std::vector<AlterCondition> forceConditions;
    std::vector<int> transitionGroups;
    std::vector<Rule> implicitRules;
    std::vector<Rule> explicitRules;
    std::vector<Rule> incomingRules;
    std::vector<Rule> layerRules;
};

struct TransitionGroup {
    std::vector<int> transitionGroups;
    std::vector<Rule> implicitRules;
    std::vector<Rule> explicitRules;
    std::vector<Rule> layerRules;
};

struct Param {
    uint32_t name = 0;
    int type = 0;
    float fvalue = 0.0f;
    int32_t ivalue = 0;
};

struct Machine {
    bool baseLayer = false;
    int layerType = -1;
    std::vector<State> states;
    std::vector<Category> categories;
    std::vector<TransitionGroup> transitionGroups;
};

struct MetaAnimation {
    MetaKind kind = MetaKind::Unknown;
    uint32_t type = 0;
    uint32_t hash = 0;
    std::string name;
    std::vector<uint32_t> animationKeys;
};

struct File {
    bool valid = false;
    uint32_t resourceHash = 0;
    std::string error;
    size_t bytesConsumed = 0;
    std::vector<Machine> machines;
    std::vector<MetaAnimation> metaAnimations;
    std::vector<Param> params;
};

File Parse(const uint8_t* data, size_t size, uint32_t resourceHash = 0);

}
