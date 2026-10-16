#include "EditorTheme.h"

#include "imgui.h"

namespace playback::editor::ui::theme {

namespace {

ImVec4 colour(ImU32 value) { return ImGui::ColorConvertU32ToFloat4(value); }

} // namespace

void apply() {
    auto& style = ImGui::GetStyle();

    // Spacing follows the font so raising the UI scale grows padding along with glyphs; borders stay 1px.
    float const scale = metrics::scale();

    // ImGui's default metrics with square corners, as Flashback leaves them.
    style.WindowPadding     = {kPanelPadding * scale, kPanelPadding * scale};
    style.FramePadding      = {4.0f * scale, 3.0f * scale};
    style.ItemSpacing       = {8.0f * scale, kItemSpacing * scale};
    style.ItemInnerSpacing  = {kItemSpacing * scale, kItemSpacing * scale};
    style.FrameRounding     = 0.0f;
    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 0.0f;
    style.GrabRounding      = 0.0f;
    style.PopupRounding     = 0.0f;
    style.TabRounding       = 0.0f;
    style.ScrollbarSize     = 14.0f * scale;
    style.ScrollbarRounding = 9.0f * scale;
    style.WindowBorderSize  = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.TabBorderSize     = 0.0f;
    style.WindowTitleAlign  = {0.0f, 0.5f};
    style.WindowMenuButtonPosition = ImGuiDir_None;

    auto* colors                          = style.Colors;
    colors[ImGuiCol_WindowBg]             = colour(kBgPanel);
    colors[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_PopupBg]              = colour(kPopupBg);
    colors[ImGuiCol_MenuBarBg]            = colour(kBgPanel);
    colors[ImGuiCol_Border]               = colour(kBorder);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_Text]                 = colour(kText);
    colors[ImGuiCol_TextDisabled]         = colour(kTextDim);
    colors[ImGuiCol_Button]               = colour(kPushButton);
    colors[ImGuiCol_ButtonHovered]        = colour(kPushButtonHover);
    colors[ImGuiCol_ButtonActive]         = colour(kPushButtonActive);
    colors[ImGuiCol_Header]               = colour(kHover);
    colors[ImGuiCol_HeaderHovered]        = colour(IM_COL32(0x00, 0x72, 0xe0, 0x80));
    colors[ImGuiCol_HeaderActive]         = colour(IM_COL32(0x00, 0x4c, 0x95, 0x80));
    colors[ImGuiCol_FrameBg]              = colour(kInputBg);
    colors[ImGuiCol_FrameBgHovered]       = colour(kInputBgHover);
    colors[ImGuiCol_FrameBgActive]        = colour(kInputBgActive);
    colors[ImGuiCol_TitleBg]              = colour(IM_COL32(0x15, 0x15, 0x1b, 0xff));
    colors[ImGuiCol_TitleBgActive]        = colour(kBgHeader);
    colors[ImGuiCol_TitleBgCollapsed]     = colour(IM_COL32(0x15, 0x15, 0x1b, 0xff));
    colors[ImGuiCol_CheckMark]            = colour(kCheckMark);
    colors[ImGuiCol_SliderGrab]           = colour(IM_COL32(0x3d, 0x85, 0xe0, 0xff));
    colors[ImGuiCol_SliderGrabActive]     = colour(IM_COL32(0x42, 0x96, 0xfa, 0xff));
    colors[ImGuiCol_ScrollbarBg]          = colour(IM_COL32(0x05, 0x05, 0x05, 0x87));
    colors[ImGuiCol_ScrollbarGrab]        = colour(kScrollThumb);
    colors[ImGuiCol_ScrollbarGrabHovered] = colour(IM_COL32(0x69, 0x69, 0x69, 0xff));
    colors[ImGuiCol_ScrollbarGrabActive]  = colour(IM_COL32(0x82, 0x82, 0x82, 0xff));
    colors[ImGuiCol_Separator]            = colour(kFineDivider);
    colors[ImGuiCol_SeparatorHovered]     = colour(IM_COL32(0x1a, 0x66, 0xbf, 0xc7));
    colors[ImGuiCol_SeparatorActive]      = colour(IM_COL32(0x1a, 0x66, 0xbf, 0xff));
    colors[ImGuiCol_TableBorderLight]     = colour(kBorder);
    colors[ImGuiCol_TableBorderStrong]    = colour(kBorder);
    colors[ImGuiCol_Tab]                  = colour(kTab);
    colors[ImGuiCol_TabHovered]           = colour(kTabHovered);
    colors[ImGuiCol_TabSelected]          = colour(kTabSelected);
    colors[ImGuiCol_TabSelectedOverline]  = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TabDimmed]            = colour(withAlpha(kTab, 0xaa));
    colors[ImGuiCol_TabDimmedSelected]    = colour(withAlpha(kTabSelected, 0xaa));
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ResizeGripHovered]    = colour(kHover);
    colors[ImGuiCol_ResizeGripActive]     = colour(kAccent);
    colors[ImGuiCol_ModalWindowDimBg]     = colour(kModalDim);
}

} // namespace playback::editor::ui::theme
