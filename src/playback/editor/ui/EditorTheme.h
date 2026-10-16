#pragma once

#include "imgui.h"

#include <algorithm>

namespace playback::editor::ui::theme {

// Single source of truth for editor colours; panels reference these instead of local literals.
[[nodiscard]] constexpr ImU32 withAlpha(ImU32 colour, int alpha) {
    return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

// The palette follows Flashback's editor: blue-grey panels, white text and lines, blue buttons and headers.
constexpr ImU32 kBgPanel  = IM_COL32(0x20, 0x20, 0x28, 0xff);
constexpr ImU32 kBgHeader = IM_COL32(0x1d, 0x1d, 0x25, 0xff);
constexpr ImU32 kBorder   = IM_COL32(0x30, 0x30, 0x3c, 0xff);
constexpr ImU32 kText     = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kTextDim  = IM_COL32(0xaa, 0xaa, 0xaa, 0xff);
constexpr ImU32 kAccent   = IM_COL32(0x00, 0x68, 0xcb, 0xff);
constexpr ImU32 kSelected = IM_COL32(0xff, 0x00, 0x00, 0xff);
constexpr ImU32 kHover    = IM_COL32(0x00, 0x5f, 0xbb, 0x80);
constexpr ImU32 kSuccess  = IM_COL32(0x80, 0xff, 0x80, 0xff);
constexpr ImU32 kWarning  = IM_COL32(0xff, 0xaa, 0x00, 0xff);
constexpr ImU32 kError    = IM_COL32(0xff, 0x80, 0x80, 0xff);

// Generic controls.
constexpr ImU32 kInputBg       = IM_COL32(0x38, 0x38, 0x3f, 0xaa);
constexpr ImU32 kInputBgHover  = IM_COL32(0x43, 0x43, 0x4b, 0xaa);
constexpr ImU32 kInputBgActive = IM_COL32(0x2c, 0x2c, 0x32, 0xaa);
constexpr ImU32 kButton        = IM_COL32(0x38, 0x38, 0x48, 0xff);
constexpr ImU32 kButtonHover   = IM_COL32(0x48, 0x48, 0x5c, 0xff);
constexpr ImU32 kButtonActive  = IM_COL32(0x2c, 0x2c, 0x3a, 0xff);
constexpr ImU32 kScrollThumb   = IM_COL32(0x4f, 0x4f, 0x4f, 0xff);
// Push buttons are Flashback blue.
constexpr ImU32 kPushButton       = IM_COL32(0x00, 0x68, 0xcb, 0xff);
constexpr ImU32 kPushButtonHover  = IM_COL32(0x00, 0x7c, 0xf3, 0xff);
constexpr ImU32 kPushButtonActive = IM_COL32(0x00, 0x53, 0xa2, 0xff);
constexpr ImU32 kCheckMark        = IM_COL32(0x00, 0x88, 0xfb, 0xff);
constexpr ImU32 kPopupBg          = IM_COL32(0x28, 0x28, 0x38, 0xff);
constexpr ImU32 kTab              = IM_COL32(0x28, 0x28, 0x30, 0xff);
constexpr ImU32 kTabSelected      = IM_COL32(0x40, 0x40, 0x58, 0xff);
constexpr ImU32 kTabHovered       = IM_COL32(0x50, 0x50, 0x68, 0xff);
// Timeline zoom bar: a thin white bar that grows on hover, with round handles at its ends.
constexpr ImU32 kRangeTrack       = IM_COL32(0x40, 0x40, 0x40, 0xff);
constexpr ImU32 kRangeThumb       = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kRangeThumbHover  = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kRangeThumbActive = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kRangeHandle      = IM_COL32(0x00, 0xaa, 0xaa, 0xff);

// Shapes past ImGui's own cursors; Windows has no grab hand, so the cursor hook draws these.
constexpr ImGuiMouseCursor kCursorGrab     = ImGuiMouseCursor_COUNT;
constexpr ImGuiMouseCursor kCursorGrabbing = ImGuiMouseCursor_COUNT + 1;

// Timeline surfaces. Lines, labels, keyframes and the playhead are white; selection is red.
constexpr ImU32 kTimelineBg        = kBgPanel;
constexpr ImU32 kTimelineSidebar   = kBgPanel;
constexpr ImU32 kTimelineRuler     = kBgPanel;
constexpr ImU32 kTimelineTrackBg   = kBgPanel;
constexpr ImU32 kLine              = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kRulerMajor        = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kRulerMinor        = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kRulerSubTick      = IM_COL32(0x80, 0x80, 0x80, 0xff);
constexpr ImU32 kTrackGrid         = IM_COL32(0xff, 0xff, 0xff, 0x0c);
constexpr ImU32 kRowHighlight      = IM_COL32(0x00, 0x5f, 0xbb, 0x40);
constexpr ImU32 kRowSelectedBorder = IM_COL32(0x00, 0x5f, 0xbb, 0x80);
constexpr ImU32 kKeyframe          = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kKeyframeSelected  = IM_COL32(0xff, 0x00, 0x00, 0xff);
constexpr ImU32 kKeyframeDisabled  = IM_COL32(0xff, 0xff, 0xff, 0x80);
constexpr ImU32 kKeyframeOutline   = IM_COL32(0x00, 0x00, 0x00, 0x00);
constexpr ImU32 kPlayhead          = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kPlayheadLabel     = IM_COL32(0x11, 0x11, 0x11, 0xff);
constexpr ImU32 kMarker            = IM_COL32(0xff, 0x55, 0x55, 0xff);
constexpr ImU32 kSlowDown          = IM_COL32(0xff, 0x80, 0x80, 0xff);
constexpr ImU32 kSpeedUp           = IM_COL32(0x80, 0xff, 0x80, 0xff);
constexpr ImU32 kExportRange       = IM_COL32(0x00, 0xaa, 0xff, 0x60);
// In/out markers, drawn in the ruler band.
constexpr ImU32 kExportBracket = IM_COL32(0x00, 0xaa, 0xff, 0xff);
constexpr ImU32 kExportBand    = IM_COL32(0x00, 0xaa, 0xff, 0x60);
constexpr ImU32 kTrackDisabled = IM_COL32(0x80, 0x80, 0x80, 0xff);

// Cameras take a colour from this palette by creation order; the row fill uses it at low alpha.
constexpr ImU32 kTrackPalette[] = {
    IM_COL32(0x4f, 0x8f, 0xe0, 0xff),
    IM_COL32(0x46, 0xc8, 0xa0, 0xff),
    IM_COL32(0xe0, 0xa0, 0x40, 0xff),
    IM_COL32(0xd0, 0x6a, 0x6a, 0xff),
    IM_COL32(0xa0, 0x7f, 0xd8, 0xff),
    IM_COL32(0x58, 0xb4, 0xc8, 0xff),
    IM_COL32(0xc8, 0xc8, 0x58, 0xff),
    IM_COL32(0xd8, 0x88, 0xb8, 0xff),
};
constexpr int kTrackPaletteSize = static_cast<int>(sizeof(kTrackPalette) / sizeof(kTrackPalette[0]));

[[nodiscard]] constexpr ImU32 trackColor(int index) {
    int const wrapped = ((index % kTrackPaletteSize) + kTrackPaletteSize) % kTrackPaletteSize;
    return kTrackPalette[wrapped];
}
constexpr int kTrackFillAlpha         = 0x30;
constexpr int kTrackFillSelectedAlpha = 0x52;

// Inspector surfaces.
constexpr ImU32 kInspectorHeader = kBgPanel;
constexpr ImU32 kSection         = IM_COL32(0x00, 0x5f, 0xbb, 0x80);
constexpr ImU32 kSectionHover    = IM_COL32(0x00, 0x72, 0xe0, 0x80);
constexpr ImU32 kFineDivider     = IM_COL32(0xaa, 0xaa, 0xaa, 0xff);
constexpr ImU32 kAxisX           = IM_COL32(0xe0, 0x5a, 0x5a, 0xff);
constexpr ImU32 kAxisY           = IM_COL32(0x6a, 0xc8, 0x6e, 0xff);
constexpr ImU32 kAxisZ           = IM_COL32(0x5a, 0x96, 0xf0, 0xff);
constexpr ImU32 kAxisNeutral     = IM_COL32(0x4a, 0x4a, 0x4a, 0xff);
constexpr ImU32 kRailBg          = kBgHeader;
constexpr ImU32 kRailActive      = kAccent;

// Viewport and icon surfaces.
constexpr ImU32 kViewportBg    = IM_COL32(0x10, 0x10, 0x14, 0xff);
constexpr ImU32 kIconActive    = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kIconInactive  = IM_COL32(0xdd, 0xdd, 0xdd, 0xff);
constexpr ImU32 kIconHighlight = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr ImU32 kOverlayBg     = IM_COL32(0x20, 0x20, 0x28, 0xcc);
constexpr ImU32 kOverlayHover  = IM_COL32(0x00, 0x5f, 0xbb, 0xcc);
// Tooltips use the popup fill with a light border, as ImGui draws them in Flashback.
constexpr ImU32 kTooltipBg     = IM_COL32(0x28, 0x28, 0x38, 0xf8);
constexpr ImU32 kTooltipBorder = IM_COL32(0x48, 0x48, 0x5a, 0xff);
constexpr ImU32 kTooltipShadow = IM_COL32(0x00, 0x00, 0x00, 0x80);
// Dims the editor behind modal dialogs so the dialog reads as the only live surface.
constexpr ImU32 kModalDim           = IM_COL32(0xcc, 0xcc, 0xcc, 0x58);
constexpr ImU32 kAccentHover        = IM_COL32(0x00, 0x7c, 0xf3, 0xff);
constexpr ImU32 kCameraPath         = IM_COL32(0xff, 0xff, 0x20, 0xeb);
constexpr ImU32 kCameraPathSelected = IM_COL32(0xff, 0x30, 0x30, 0xff);
constexpr ImU32 kCameraPathPlayhead = IM_COL32(0x20, 0xff, 0x20, 0xff);

constexpr float kPanelPadding     = 8.0f;
constexpr float kItemSpacing      = 4.0f;
constexpr float kFrameRounding    = 0.0f;
constexpr float kFineDividerWidth = 1.0f;

void apply();

} // namespace playback::editor::ui::theme

// Every editor dimension derives from the font size, so raising theme::kEditorFontScale rescales the whole editor.
namespace playback::editor::ui::metrics {

[[nodiscard]] inline float font() { return ImGui::GetFontSize(); }

// Ratio of the live font size to the rasterised base, i.e. the active UI scale.
[[nodiscard]] inline float scale() { return std::max(1.0f, font() / 14.0f); }

[[nodiscard]] inline float iconButton() { return font() + 16.0f * scale(); }
// Above body-text size so icons read as controls; Lucide fills its em box, so the margin stays small.
[[nodiscard]] inline float iconGlyph() { return font() * 1.22f; }
[[nodiscard]] inline float toolbarRow() { return iconButton() + 6.0f * scale(); }
[[nodiscard]] inline float ruler() { return font() + 8.0f * scale(); }
[[nodiscard]] inline float statusBar() { return font() + 6.0f * scale(); }
[[nodiscard]] inline float cameraRow() { return font() + 10.0f * scale(); }
[[nodiscard]] inline float subRow() { return font() * 1.55f; }
[[nodiscard]] inline float propertyRow() { return font() + 8.0f * scale(); }
[[nodiscard]] inline float rail() { return font() * 2.3f; }
[[nodiscard]] inline float rangeBar() { return 12.0f * scale(); }
[[nodiscard]] inline float labelColumn() { return font() * 4.6f; }
[[nodiscard]] inline float splitter() { return 4.0f * scale(); }
[[nodiscard]] inline float trackSwatch() { return std::max(3.0f, 3.0f * scale()); }
[[nodiscard]] inline float keyframeRadius() { return std::max(5.0f, font() * 0.45f); }
[[nodiscard]] inline float gutter() { return 6.0f * scale(); }
// Flashback's timeline header: a timestamp line above major ticks twice the height of minor ones.
[[nodiscard]] inline float timestampBand() { return font() + 4.0f * scale(); }
[[nodiscard]] inline float minorTick() { return 7.0f * scale(); }
// Height of the ImGui title bar the editor panels carry.
[[nodiscard]] inline float titleBar() { return font() + ImGui::GetStyle().FramePadding.y * 2.0f; }

} // namespace playback::editor::ui::metrics
