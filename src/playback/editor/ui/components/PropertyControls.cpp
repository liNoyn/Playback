#include "PropertyControls.h"

#include "playback/editor/ui/EditorTheme.h"

#include "imgui.h"

#include <algorithm>
#include <string>

namespace playback::editor::ui::property {

namespace {

constexpr ImU32 kSectionColor         = theme::kSection;
constexpr ImU32 kSectionHoverColor    = theme::kSectionHover;
constexpr float kFineDividerThickness = theme::kFineDividerWidth;

// Padding follows the font, so the inspector keeps its proportions at every UI scale.
float uiScale() { return std::max(1.0f, ImGui::GetFontSize() / 14.0f); }

} // namespace

// Flashback windows open with plain text, so the header is the title, the object in grey and a separator.
void beginInspector(std::string_view title, std::string_view objectName) {
    ImGui::TextUnformatted(title.data(), title.data() + title.size());
    if (!objectName.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%.*s", static_cast<int>(objectName.size()), objectName.data());
    }
    ImGui::Separator();
}

bool beginSection(char const* label, bool defaultOpen) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    ImGui::PushStyleColor(ImGuiCol_Header, kSectionColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kSectionHoverColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kSectionHoverColor);
    bool const open = ImGui::TreeNodeEx(label, flags);
    ImGui::PopStyleColor(3);
    return open;
}
void endSection() {
    ImGui::TreePop();
    ImGui::Dummy({0.0f, 4.0f * uiScale()});
}

void textRow(char const* label, char const* value) {
    float const width      = ImGui::GetContentRegionAvail().x;
    float const labelWidth = std::clamp(width * 0.38f, 72.0f, 160.0f);
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(labelWidth);
    ImGui::TextUnformatted(value);
    separator();
}

void separator() {
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    float const  width  = ImGui::GetContentRegionAvail().x;
    // Rows only need a faint rule; the bright separator is kept for section breaks.
    ImGui::GetWindowDrawList()->AddLine(origin, {origin.x + width, origin.y}, theme::kBorder, kFineDividerThickness);
    ImGui::Dummy({0.0f, 5.0f * uiScale()});
}

bool actionButton(char const* label, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    bool const clicked = ImGui::Button(label, {-1.0f, 0.0f});
    ImGui::EndDisabled();
    return clicked;
}
} // namespace playback::editor::ui::property
