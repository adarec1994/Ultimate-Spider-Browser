#include "Interface.h"

#include "imgui.h"
#include "imgui_node_editor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_map>

namespace ed = ax::NodeEditor;

namespace {

enum class ViewNodeKind {
    State,
    Category,
    Rule,
    Missing
};

struct ViewNode {
    int id = 0;
    int input = 0;
    int output = 0;
    ViewNodeKind kind = ViewNodeKind::State;
    uint32_t hash = 0;
    bool category = false;
    bool refocusable = false;
    std::string title;
    std::vector<std::string> lines;
    ImVec2 position = ImVec2(0, 0);
};

struct ViewLink {
    int id = 0;
    int start = 0;
    int end = 0;
    ImVec4 color = ImVec4(1, 1, 1, 1);
};

struct ViewGraph {
    uint64_t key = 0;
    std::vector<ViewNode> nodes;
    std::vector<ViewLink> links;
    std::unordered_map<uint32_t, int> stateNodes;
    std::unordered_map<uint32_t, int> categoryNodes;
    std::map<uint64_t, int> missingNodes;
    int focusNode = 0;
};

struct GraphLookups {
    std::unordered_map<uint32_t, const UsmAls::State*> states;
    std::unordered_map<uint32_t, const UsmAls::Category*> categories;
    std::unordered_map<uint32_t, const UsmAls::MetaAnimation*> metaAnimations;
};

struct CollectedRule {
    const UsmAls::Rule* rule = nullptr;
    std::string origin;
};

std::string HexHash(uint32_t hash) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << hash;
    return stream.str();
}

std::string ResolveHash(const SpiderManTool& tool, uint32_t hash) {
    if (!hash) return "None";
    auto found = tool.dictionary.find(hash);
    if (found != tool.dictionary.end()) return found->second;
    return HexHash(hash);
}

std::string FormatFloat(float value) {
    std::ostringstream stream;
    stream << std::setprecision(5) << value;
    return stream.str();
}

bool ContainsCaseInsensitive(const std::string& value, const char* filter) {
    if (!filter || !*filter) return true;
    std::string loweredValue = value;
    std::string loweredFilter = filter;
    std::transform(loweredValue.begin(), loweredValue.end(), loweredValue.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    std::transform(loweredFilter.begin(), loweredFilter.end(), loweredFilter.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return loweredValue.find(loweredFilter) != std::string::npos;
}

std::string MetaKindName(UsmAls::MetaKind kind) {
    switch (kind) {
        case UsmAls::MetaKind::Animation: return "Animation";
        case UsmAls::MetaKind::AimedShotVertical: return "Vertical aimed shot";
        case UsmAls::MetaKind::StrengthTest: return "Strength test";
        case UsmAls::MetaKind::FourClip: return "Four-clip animation tree";
        case UsmAls::MetaKind::TwoClip: return "Two-clip animation tree";
        case UsmAls::MetaKind::Swing: return "Swing tree";
        case UsmAls::MetaKind::LinearBlend: return "Linear blend tree";
        case UsmAls::MetaKind::Interact: return "Interaction";
        default: return "Unknown meta-animation";
    }
}

std::string RuleKindName(UsmAls::RuleKind kind) {
    switch (kind) {
        case UsmAls::RuleKind::Implicit: return "Implicit";
        case UsmAls::RuleKind::Explicit: return "Explicit request";
        case UsmAls::RuleKind::Incoming: return "Incoming category";
        case UsmAls::RuleKind::Layer: return "Layer rule";
        default: return "Rule";
    }
}

std::string ActionName(int type) {
    switch (type) {
        case 0: return "Transition to state";
        case 1: return "Transition to category";
        case 2: return "Consume transition request";
        case 3: return "Enable fallback";
        case 4: return "Disable fallback";
        default: return "Unknown action " + std::to_string(type);
    }
}

ImVec4 NodeColor(ViewNodeKind kind) {
    switch (kind) {
        case ViewNodeKind::State: return ImVec4(0.10f, 0.22f, 0.38f, 0.96f);
        case ViewNodeKind::Category: return ImVec4(0.38f, 0.25f, 0.07f, 0.96f);
        case ViewNodeKind::Rule: return ImVec4(0.24f, 0.13f, 0.34f, 0.96f);
        case ViewNodeKind::Missing: return ImVec4(0.38f, 0.09f, 0.09f, 0.96f);
        default: return ImVec4(0.2f, 0.2f, 0.2f, 1.0f);
    }
}

int AddNode(ViewGraph& graph, ViewNodeKind kind, std::string title,
            std::vector<std::string> lines = {}, uint32_t hash = 0,
            bool category = false, bool refocusable = false) {
    ViewNode node;
    node.id = static_cast<int>(graph.nodes.size()) + 1;
    node.input = 1000000 + node.id * 2;
    node.output = node.input + 1;
    node.kind = kind;
    node.hash = hash;
    node.category = category;
    node.refocusable = refocusable;
    node.title = std::move(title);
    node.lines = std::move(lines);
    graph.nodes.push_back(std::move(node));
    return graph.nodes.back().id;
}

const ViewNode& GetNode(const ViewGraph& graph, int id) {
    return graph.nodes[static_cast<size_t>(id - 1)];
}

ViewNode& GetNode(ViewGraph& graph, int id) {
    return graph.nodes[static_cast<size_t>(id - 1)];
}

void AddLink(ViewGraph& graph, int sourceNode, int destinationNode, const ImVec4& color) {
    if (sourceNode <= 0 || destinationNode <= 0) return;
    ViewLink link;
    link.id = 2000000 + static_cast<int>(graph.links.size()) + 1;
    link.start = GetNode(graph, sourceNode).output;
    link.end = GetNode(graph, destinationNode).input;
    link.color = color;
    graph.links.push_back(link);
}

int AddMissingNode(ViewGraph& graph, const SpiderManTool& tool,
                   uint32_t hash, bool category) {
    const uint64_t key = (static_cast<uint64_t>(category) << 32) | hash;
    auto existing = graph.missingNodes.find(key);
    if (existing != graph.missingNodes.end()) return existing->second;
    const int id = AddNode(graph, ViewNodeKind::Missing,
                           category ? "External category" : "External state",
                           {ResolveHash(tool, hash), HexHash(hash)});
    graph.missingNodes[key] = id;
    return id;
}

int EnsureCategoryNode(ViewGraph& graph, const SpiderManTool& tool,
                       const GraphLookups& lookups, uint32_t hash) {
    auto existing = graph.categoryNodes.find(hash);
    if (existing != graph.categoryNodes.end()) return existing->second;
    auto found = lookups.categories.find(hash);
    if (found == lookups.categories.end()) return AddMissingNode(graph, tool, hash, true);
    const auto& category = *found->second;
    std::vector<std::string> lines;
    if (category.label && category.label != category.id)
        lines.push_back("Label: " + ResolveHash(tool, category.label));
    lines.push_back("Flags: " + HexHash(static_cast<uint32_t>(category.flags)));
    const int id = AddNode(graph, ViewNodeKind::Category, ResolveHash(tool, category.id),
                           std::move(lines), category.id, true, true);
    graph.categoryNodes[category.id] = id;
    return id;
}

int EnsureStateNode(ViewGraph& graph, const SpiderManTool& tool,
                    const GraphLookups& lookups, uint32_t hash) {
    auto existing = graph.stateNodes.find(hash);
    if (existing != graph.stateNodes.end()) return existing->second;
    auto found = lookups.states.find(hash);
    if (found == lookups.states.end()) return AddMissingNode(graph, tool, hash, false);
    const auto& state = *found->second;
    std::vector<std::string> lines;
    lines.push_back("Category: " + ResolveHash(tool, state.category));
    auto animation = lookups.metaAnimations.find(state.animation);
    if (animation != lookups.metaAnimations.end()) {
        const auto& value = *animation->second;
        lines.push_back("Animation: " +
                        (value.name.empty() ? ResolveHash(tool, state.animation) : value.name));
        lines.push_back(MetaKindName(value.kind));
        for (uint32_t keyAnimation : value.animationKeys)
            lines.push_back("  " + ResolveHash(tool, keyAnimation));
    } else {
        lines.push_back("Animation: " + ResolveHash(tool, state.animation));
    }
    lines.push_back("Flags: " + HexHash(state.flags));
    const int id = AddNode(graph, ViewNodeKind::State, ResolveHash(tool, state.id),
                           std::move(lines), state.id, false, true);
    graph.stateNodes[state.id] = id;
    return id;
}

int ResolveDestinationNode(ViewGraph& graph, const SpiderManTool& tool,
                           const GraphLookups& lookups, uint32_t hash, bool category) {
    return category ? EnsureCategoryNode(graph, tool, lookups, hash)
                    : EnsureStateNode(graph, tool, lookups, hash);
}

void AddRuleNode(ViewGraph& graph, const SpiderManTool& tool,
                 const GraphLookups& lookups, int sourceNode,
                 const UsmAls::Rule& rule, const std::string& origin) {
    std::vector<std::string> lines;
    lines.push_back(origin);
    if (rule.kind == UsmAls::RuleKind::Explicit) {
        lines.push_back("Request = " + ResolveHash(tool, rule.trigger));
    }
    if (rule.filters.empty() && rule.kind != UsmAls::RuleKind::Explicit &&
        rule.kind != UsmAls::RuleKind::Layer) {
        lines.push_back("Condition: always");
    }
    for (const auto& filter : rule.filters) {
        lines.push_back(ResolveHash(tool, filter.parameter) + " in [" +
                        FormatFloat(filter.minimum) + ", " +
                        FormatFloat(filter.maximum) + "]");
    }
    if (rule.kind == UsmAls::RuleKind::Layer) {
        lines.push_back("Layer values: " + std::to_string(rule.conditionA) + ", " +
                        std::to_string(rule.conditionB));
    }
    if (rule.kind == UsmAls::RuleKind::Incoming) {
        lines.push_back("Incoming values: " + std::to_string(rule.conditionA) + ", " +
                        std::to_string(rule.conditionB));
    }
    lines.push_back(ActionName(rule.action.type));
    lines.push_back("Priority #" + std::to_string(rule.priority) + " (first match)");
    if (rule.hasPostAction) lines.push_back("Runs post-transition actions");
    if (rule.action.destinations.size() > 1) {
        for (const auto& destination : rule.action.destinations) {
            lines.push_back(ResolveHash(tool, destination.target) + " weight " +
                            FormatFloat(destination.weight));
        }
    }
    const int ruleNode = AddNode(graph, ViewNodeKind::Rule, RuleKindName(rule.kind), std::move(lines));
    AddLink(graph, sourceNode, ruleNode, ImVec4(0.72f, 0.45f, 0.95f, 1.0f));
    if (rule.action.type != 0 && rule.action.type != 1) return;
    const bool category = rule.action.type == 1;
    for (const auto& destination : rule.action.destinations) {
        const int targetNode = ResolveDestinationNode(graph, tool, lookups,
                                                      destination.target, category);
        AddLink(graph, ruleNode, targetNode,
                category ? ImVec4(0.95f, 0.68f, 0.22f, 1.0f)
                         : ImVec4(0.28f, 0.72f, 1.0f, 1.0f));
    }
}

void AddForcedCategoryRule(ViewGraph& graph, const SpiderManTool& tool,
                           const GraphLookups& lookups, int sourceNode,
                           const UsmAls::Category& category) {
    if (!category.forcedState) return;
    std::vector<std::string> lines;
    lines.push_back("Category force transition");
    if (category.forceConditions.empty()) lines.push_back("Condition: always");
    for (const auto& condition : category.forceConditions) {
        lines.push_back(ResolveHash(tool, condition.parameter) + " mode " +
                        std::to_string(condition.mode) + " value " +
                        std::to_string(condition.value));
    }
    lines.push_back("Transition to state");
    const int ruleNode = AddNode(graph, ViewNodeKind::Rule, "Forced transition", std::move(lines));
    AddLink(graph, sourceNode, ruleNode, ImVec4(0.95f, 0.55f, 0.20f, 1.0f));
    AddLink(graph, ruleNode,
            ResolveDestinationNode(graph, tool, lookups, category.forcedState, false),
            ImVec4(0.28f, 0.72f, 1.0f, 1.0f));
}

void CollectTransitionGroupRules(const UsmAls::Machine& machine, int groupIndex,
                                 std::set<int>& visited,
                                 std::vector<CollectedRule>& rules) {
    if (groupIndex < 0 || groupIndex >= static_cast<int>(machine.transitionGroups.size())) return;
    if (!visited.insert(groupIndex).second) return;
    const auto& group = machine.transitionGroups[groupIndex];
    for (int nested : group.transitionGroups)
        CollectTransitionGroupRules(machine, nested, visited, rules);
    const std::string origin = "Shared group #" + std::to_string(groupIndex);
    for (const auto& rule : group.implicitRules) rules.push_back({&rule, origin});
    for (const auto& rule : group.explicitRules) rules.push_back({&rule, origin});
    for (const auto& rule : group.layerRules) rules.push_back({&rule, origin});
}

void AppendRules(const std::vector<UsmAls::Rule>& source, const std::string& origin,
                 std::vector<CollectedRule>& destination) {
    for (const auto& rule : source) destination.push_back({&rule, origin});
}

std::vector<CollectedRule> CollectRules(const UsmAls::Machine& machine,
                                        const UsmAls::State& state) {
    std::vector<CollectedRule> rules;
    std::set<int> visited;
    for (int group : state.transitionGroups)
        CollectTransitionGroupRules(machine, group, visited, rules);
    AppendRules(state.implicitRules, "State-local rules", rules);
    AppendRules(state.explicitRules, "State-local rules", rules);
    AppendRules(state.layerRules, "State-local rules", rules);
    return rules;
}

std::vector<CollectedRule> CollectRules(const UsmAls::Machine& machine,
                                        const UsmAls::Category& category) {
    std::vector<CollectedRule> rules;
    std::set<int> visited;
    for (int group : category.transitionGroups)
        CollectTransitionGroupRules(machine, group, visited, rules);
    AppendRules(category.implicitRules, "Category-local rules", rules);
    AppendRules(category.explicitRules, "Category-local rules", rules);
    AppendRules(category.incomingRules, "Category-local rules", rules);
    AppendRules(category.layerRules, "Category-local rules", rules);
    return rules;
}

bool RuleTargets(const UsmAls::Rule& rule, uint32_t hash, bool category) {
    if (rule.action.type != (category ? 1 : 0)) return false;
    return std::any_of(rule.action.destinations.begin(), rule.action.destinations.end(),
                       [hash](const UsmAls::Destination& destination) {
                           return destination.target == hash;
                       });
}

void LayoutGraph(ViewGraph& graph) {
    std::unordered_map<int, std::vector<int>> outgoing;
    std::unordered_map<int, std::vector<int>> incoming;
    std::unordered_map<int, int> outputNodes;
    std::unordered_map<int, int> inputNodes;
    for (const auto& node : graph.nodes) {
        outputNodes[node.output] = node.id;
        inputNodes[node.input] = node.id;
    }
    for (const auto& link : graph.links) {
        const int source = outputNodes[link.start];
        const int target = inputNodes[link.end];
        if (source && target) {
            outgoing[source].push_back(target);
            incoming[target].push_back(source);
        }
    }

    std::unordered_map<int, int> levels;
    levels[graph.focusNode] = 0;
    std::queue<int> queue;
    queue.push(graph.focusNode);
    while (!queue.empty()) {
        const int node = queue.front();
        queue.pop();
        for (int next : outgoing[node]) {
            if (levels.find(next) != levels.end()) continue;
            levels[next] = levels[node] + 1;
            queue.push(next);
        }
    }
    queue.push(graph.focusNode);
    while (!queue.empty()) {
        const int node = queue.front();
        queue.pop();
        for (int previous : incoming[node]) {
            if (levels.find(previous) != levels.end()) continue;
            levels[previous] = levels[node] - 1;
            queue.push(previous);
        }
    }

    std::map<int, std::vector<int>> columns;
    for (const auto& node : graph.nodes) {
        auto level = levels.find(node.id);
        columns[level == levels.end() ? 3 : level->second].push_back(node.id);
    }
    for (auto& [level, nodes] : columns) {
        const float center = static_cast<float>(nodes.size() - 1) * 0.5f;
        for (size_t index = 0; index < nodes.size(); ++index) {
            GetNode(graph, nodes[index]).position =
                ImVec2(static_cast<float>(level) * 480.0f,
                       (static_cast<float>(index) - center) * 210.0f);
        }
    }
}

ViewGraph BuildGraph(const SpiderManTool& tool, const UsmAls::File& file,
                     const UsmAls::Machine& machine, uint32_t focusHash,
                     bool focusCategory, bool includeIncoming, uint64_t key) {
    ViewGraph graph;
    graph.key = key;
    GraphLookups lookups;
    for (const auto& state : machine.states) lookups.states[state.id] = &state;
    for (const auto& category : machine.categories) lookups.categories[category.id] = &category;
    for (const auto& animation : file.metaAnimations)
        lookups.metaAnimations[animation.hash] = &animation;

    if (focusCategory) {
        auto focused = lookups.categories.find(focusHash);
        if (focused == lookups.categories.end()) return graph;
        graph.focusNode = EnsureCategoryNode(graph, tool, lookups, focusHash);
        for (const auto& collected : CollectRules(machine, *focused->second))
            AddRuleNode(graph, tool, lookups, graph.focusNode,
                        *collected.rule, collected.origin);
        AddForcedCategoryRule(graph, tool, lookups, graph.focusNode, *focused->second);
    } else {
        auto focused = lookups.states.find(focusHash);
        if (focused == lookups.states.end()) return graph;
        graph.focusNode = EnsureStateNode(graph, tool, lookups, focusHash);
        for (const auto& collected : CollectRules(machine, *focused->second))
            AddRuleNode(graph, tool, lookups, graph.focusNode,
                        *collected.rule, collected.origin);
    }

    if (includeIncoming) {
        for (const auto& state : machine.states) {
            if (!focusCategory && state.id == focusHash) continue;
            const auto rules = CollectRules(machine, state);
            const bool hasMatch = std::any_of(rules.begin(), rules.end(),
                [focusHash, focusCategory](const CollectedRule& collected) {
                    return RuleTargets(*collected.rule, focusHash, focusCategory);
                });
            if (!hasMatch) continue;
            const int source = EnsureStateNode(graph, tool, lookups, state.id);
            for (const auto& collected : rules) {
                if (RuleTargets(*collected.rule, focusHash, focusCategory))
                    AddRuleNode(graph, tool, lookups, source,
                                *collected.rule, collected.origin);
            }
        }
        for (const auto& category : machine.categories) {
            if (focusCategory && category.id == focusHash) continue;
            const auto rules = CollectRules(machine, category);
            const bool hasMatch = std::any_of(rules.begin(), rules.end(),
                [focusHash, focusCategory](const CollectedRule& collected) {
                    return RuleTargets(*collected.rule, focusHash, focusCategory);
                });
            const bool forcedMatch = !focusCategory && category.forcedState == focusHash;
            if (!hasMatch && !forcedMatch) continue;
            const int source = EnsureCategoryNode(graph, tool, lookups, category.id);
            for (const auto& collected : rules) {
                if (RuleTargets(*collected.rule, focusHash, focusCategory))
                    AddRuleNode(graph, tool, lookups, source,
                                *collected.rule, collected.origin);
            }
            if (forcedMatch) AddForcedCategoryRule(graph, tool, lookups, source, category);
        }
    }

    LayoutGraph(graph);
    return graph;
}

void DrawNode(const ViewNode& node) {
    ed::PushStyleColor(ed::StyleColor_NodeBg, NodeColor(node.kind));
    ed::BeginNode(ed::NodeId(node.id));
    ed::BeginPin(ed::PinId(node.input), ed::PinKind::Input);
    ImGui::TextUnformatted(" ");
    ed::EndPin();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.46f, 1.0f), "%s", node.title.c_str());
    for (const auto& line : node.lines) ImGui::TextUnformatted(line.c_str());
    ImGui::EndGroup();
    ImGui::SameLine();
    ed::BeginPin(ed::PinId(node.output), ed::PinKind::Output);
    ImGui::TextUnformatted(" ");
    ed::EndPin();
    ed::EndNode();
    ed::PopStyleColor();
}

}

void RenderAnimationStateMachineViewer(SpiderManTool& tool) {
    if (!tool.showAnimationStateMachine) return;
    if (tool.animationStateMachines.empty()) {
        tool.showAnimationStateMachine = false;
        return;
    }

    static ed::EditorContext* context = [] {
        ed::Config config;
        config.SettingsFile = nullptr;
        return ed::CreateEditor(&config);
    }();
    static ViewGraph graph;
    static bool positionGraph = false;
    static uint64_t focusScope = 0;
    static uint32_t focusHash = 0;
    static bool focusCategory = false;
    static bool includeIncoming = false;
    static std::array<char, 128> focusFilter{};

    tool.selectedAnimationStateMachine = std::clamp(tool.selectedAnimationStateMachine, 0,
        static_cast<int>(tool.animationStateMachines.size()) - 1);
    if (tool.animationStateMachines[tool.selectedAnimationStateMachine].machines.empty()) {
        tool.showAnimationStateMachine = false;
        return;
    }
    tool.selectedAnimationMachineLayer = std::clamp(tool.selectedAnimationMachineLayer, 0,
        static_cast<int>(tool.animationStateMachines[tool.selectedAnimationStateMachine].machines.size()) - 1);

    ImGui::SetNextWindowSize(ImVec2(1400, 850), ImGuiCond_FirstUseEver);
    const std::string title = "Animation State Machine - " + tool.animationStateMachineModelName +
                              "###AnimationStateMachine";
    if (!ImGui::Begin(title.c_str(), &tool.showAnimationStateMachine,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("ALS")) {
            for (int index = 0; index < static_cast<int>(tool.animationStateMachines.size()); ++index) {
                const auto& candidate = tool.animationStateMachines[index];
                const std::string label = ResolveHash(tool, candidate.resourceHash);
                if (ImGui::MenuItem(label.c_str(), nullptr, index == tool.selectedAnimationStateMachine)) {
                    tool.selectedAnimationStateMachine = index;
                    tool.selectedAnimationMachineLayer = 0;
                    for (int machineIndex = 0;
                         machineIndex < static_cast<int>(candidate.machines.size());
                         ++machineIndex) {
                        if (candidate.machines[machineIndex].baseLayer) {
                            tool.selectedAnimationMachineLayer = machineIndex;
                            break;
                        }
                    }
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    auto& selectedFile = tool.animationStateMachines[tool.selectedAnimationStateMachine];
    tool.selectedAnimationMachineLayer = std::clamp(tool.selectedAnimationMachineLayer, 0,
        static_cast<int>(selectedFile.machines.size()) - 1);

    const auto& currentMachine = selectedFile.machines[tool.selectedAnimationMachineLayer];
    const std::string currentLayer = currentMachine.baseLayer
        ? "Base state machine"
        : "Layer " + std::to_string(currentMachine.layerType);
    if (ImGui::BeginCombo("State machine", currentLayer.c_str())) {
        for (int index = 0; index < static_cast<int>(selectedFile.machines.size()); ++index) {
            const auto& candidate = selectedFile.machines[index];
            const std::string label = candidate.baseLayer
                ? "Base state machine"
                : "Layer " + std::to_string(candidate.layerType);
            if (ImGui::Selectable(label.c_str(), index == tool.selectedAnimationMachineLayer)) {
                tool.selectedAnimationMachineLayer = index;
            }
        }
        ImGui::EndCombo();
    }
    tool.selectedAnimationMachineLayer = std::clamp(tool.selectedAnimationMachineLayer, 0,
        static_cast<int>(selectedFile.machines.size()) - 1);
    const auto& machine = selectedFile.machines[tool.selectedAnimationMachineLayer];

    const uint64_t scope = (static_cast<uint64_t>(selectedFile.resourceHash) << 32) ^
                           (static_cast<uint64_t>(tool.selectedAnimationMachineLayer) << 16) ^
                           static_cast<uint64_t>(tool.animationStateMachineModelHash);
    const uint32_t currentFocusHash = focusHash;
    const bool validStateFocus = std::any_of(machine.states.begin(), machine.states.end(),
        [currentFocusHash](const UsmAls::State& state) { return state.id == currentFocusHash; });
    const bool validCategoryFocus = std::any_of(machine.categories.begin(), machine.categories.end(),
        [currentFocusHash](const UsmAls::Category& category) {
            return category.id == currentFocusHash;
        });
    if (focusScope != scope || (focusCategory ? !validCategoryFocus : !validStateFocus)) {
        focusScope = scope;
        focusFilter.fill(0);
        includeIncoming = false;
        if (!machine.states.empty()) {
            focusHash = machine.states.front().id;
            focusCategory = false;
        } else if (!machine.categories.empty()) {
            focusHash = machine.categories.front().id;
            focusCategory = true;
        }
    }

    struct FocusOption {
        std::string label;
        uint32_t hash = 0;
        bool category = false;
    };
    std::vector<FocusOption> focusOptions;
    focusOptions.reserve(machine.states.size() + machine.categories.size());
    std::unordered_map<uint32_t, const UsmAls::MetaAnimation*> focusMetaAnimations;
    for (const auto& animation : selectedFile.metaAnimations)
        focusMetaAnimations[animation.hash] = &animation;
    for (const auto& state : machine.states) {
        std::string label = "State: " + ResolveHash(tool, state.id);
        auto animation = focusMetaAnimations.find(state.animation);
        if (animation != focusMetaAnimations.end()) {
            label += " -> " + (animation->second->name.empty()
                ? ResolveHash(tool, state.animation)
                : animation->second->name);
        } else if (state.animation) {
            label += " -> " + ResolveHash(tool, state.animation);
        }
        focusOptions.push_back({std::move(label), state.id, false});
    }
    for (const auto& category : machine.categories)
        focusOptions.push_back({"Category: " + ResolveHash(tool, category.id), category.id, true});
    std::sort(focusOptions.begin(), focusOptions.end(),
              [](const FocusOption& left, const FocusOption& right) {
                  return left.label < right.label;
              });
    const std::string focusLabel = (focusCategory ? "Category: " : "State: ") +
                                   ResolveHash(tool, focusHash);
    ImGui::SetNextItemWidth(600.0f);
    if (ImGui::BeginCombo("Focus", focusLabel.c_str())) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##FocusFilter", "Search states and categories",
                                 focusFilter.data(), focusFilter.size());
        for (const auto& option : focusOptions) {
            if (!ContainsCaseInsensitive(option.label, focusFilter.data())) continue;
            const bool selected = option.hash == focusHash && option.category == focusCategory;
            const std::string itemLabel = option.label + "###Focus" +
                                          (option.category ? "C" : "S") +
                                          HexHash(option.hash);
            if (ImGui::Selectable(itemLabel.c_str(), selected)) {
                focusHash = option.hash;
                focusCategory = option.category;
                focusFilter.fill(0);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Incoming transitions", &includeIncoming);

    const uint64_t key = scope ^
                         (static_cast<uint64_t>(focusHash) * 0x9E3779B185EBCA87ull) ^
                         (focusCategory ? 0xD1B54A32D192ED03ull : 0) ^
                         (includeIncoming ? 0x94D049BB133111EBull : 0);
    if (graph.key != key) {
        graph = BuildGraph(tool, selectedFile, machine, focusHash,
                           focusCategory, includeIncoming, key);
        positionGraph = true;
    }

    ed::SetCurrentEditor(context);
    ed::Begin("AnimationStateMachineEditor", ImGui::GetContentRegionAvail());
    const bool positionNodes = positionGraph;
    for (const auto& node : graph.nodes) {
        if (positionNodes) ed::SetNodePosition(ed::NodeId(node.id), node.position);
        DrawNode(node);
    }
    for (const auto& link : graph.links) {
        ed::Link(ed::LinkId(link.id), ed::PinId(link.start), ed::PinId(link.end), link.color, 2.0f);
    }
    ed::End();
    const ed::NodeId doubleClickedNode = ed::GetDoubleClickedNode();
    if (positionGraph && graph.focusNode) {
        ed::SelectNode(ed::NodeId(graph.focusNode));
        ed::NavigateToContent(0.0f);
        positionGraph = false;
    }
    if (doubleClickedNode) {
        const int nodeId = static_cast<int>(doubleClickedNode.Get());
        if (nodeId > 0 && nodeId <= static_cast<int>(graph.nodes.size())) {
            const auto& node = GetNode(graph, nodeId);
            if (node.refocusable) {
                focusHash = node.hash;
                focusCategory = node.category;
            }
        }
    }
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
}
