#include "EditMode.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/components/Widgets.h"

#include "imgui.h"

#include <algorithm>

namespace playback::editor::ui {

// Flashback's default layout: the game view top left, a side window on the right, the timeline along the bottom
// across the full width.
void EditMode::draw(PanelContext const& ctx) {
    auto& editor = ReplayEditor::getInstance();

    float const fontSize = ImGui::GetFontSize();
    auto const& style    = ImGui::GetStyle();
    // The menu bar is ImGui's main menu bar height, as in Flashback.
    float const kMenuHeight        = fontSize + style.FramePadding.y * 2.0f;
    float const kStatusHeight      = metrics::statusBar();
    float const kSplitterThickness = metrics::splitter();
    float const kDetailsMinWidth   = std::max(240.0f, fontSize * 14.0f);
    float const kViewportMinWidth  = std::max(320.0f, fontSize * 22.0f);
    float const kViewportMinHeight = std::max(180.0f, fontSize * 12.0f);
    float const kTimelineMinHeight =
        ImGui::GetFrameHeight() + metrics::timestampBand() + metrics::minorTick() * 2.0f + metrics::cameraRow() * 2.0f
        + metrics::rangeBar();

    ImVec2 displaySize     = ImGui::GetIO().DisplaySize;
    float  contentHeight   = std::max(1.0f, displaySize.y - kMenuHeight - kStatusHeight);
    float  maxDetailsRatio = std::min(0.50f, 1.0f - kViewportMinWidth / std::max(1.0f, displaySize.x));
    float  minDetailsRatio = std::min(kDetailsMinWidth / std::max(1.0f, displaySize.x), maxDetailsRatio);
    editor.mDetailsWidthRatio      = std::clamp(editor.mDetailsWidthRatio, minDetailsRatio, maxDetailsRatio);
    float detailsWidth             = displaySize.x * editor.mDetailsWidthRatio;
    float leftWidth                = displaySize.x - detailsWidth;
    float maxTimelineRatio         = std::min(0.70f, 1.0f - kViewportMinHeight / contentHeight);
    float minTimelineRatio         = std::min(kTimelineMinHeight / contentHeight, maxTimelineRatio);
    editor.mTimelineHeightRatio    = std::clamp(editor.mTimelineHeightRatio, minTimelineRatio, maxTimelineRatio);
    float const timelineHeight     = contentHeight * editor.mTimelineHeightRatio;
    float const upperHeight        = std::max(1.0f, contentHeight - timelineHeight - kSplitterThickness);
    float const timelineY          = kMenuHeight + upperHeight + kSplitterThickness;
    bool const  popupOpen          = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImGuiWindowFlags const inputBlock = popupOpen ? ImGuiWindowFlags_NoInputs : ImGuiWindowFlags_None;
    // Splitter bands overlap the panel edges, so a clicked panel must not rise above them.
    ImGuiWindowFlags const panelLayer = ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGuiWindowFlags const fixed      = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                 | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                                 | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;

    auto drawMenuBar = [&] {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kMenuHeight));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##EditorMenuBar", nullptr, fixed | ImGuiWindowFlags_MenuBar);
        editor.mMenuBar.draw(ctx);
        ImGui::End();
        ImGui::PopStyleVar(2);
    };

    auto drawStatusBar = [&] {
        ImGui::SetNextWindowPos(ImVec2(0, displaySize.y - kStatusHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kStatusHeight));
        ImGui::Begin("##StatusPanel", nullptr, fixed | inputBlock);
        editor.mStatusPanel.draw(ctx);
        ImGui::End();
    };

    // Panels paint their own tabs and contents edge to edge, so their windows carry no padding.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    if (editor.isViewportMaximized()) {
        ImGui::SetNextWindowPos(ImVec2(0, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, contentHeight));
        ImGui::Begin("##MaximizedViewport", nullptr, fixed | inputBlock);
        editor.mViewportPanel.draw(ctx, true);
        ImGui::End();
        drawStatusBar();
        ImGui::PopStyleVar();
        drawMenuBar();
        return;
    }

    {
        ImGui::SetNextWindowPos(ImVec2(leftWidth, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(detailsWidth, upperHeight));
        ImGui::Begin("##DetailsPanel", nullptr, fixed | panelLayer | inputBlock);
        editor.mDetailsPanel.draw(ctx);
        ImGui::End();
    }

    {
        ImGui::SetNextWindowPos(ImVec2(0, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(leftWidth, upperHeight));
        ImGui::Begin("##ViewportPanel", nullptr, fixed | panelLayer | inputBlock);
        editor.mViewportPanel.draw(ctx, false);
        ImGui::End();
    }

    {
        ImGui::SetNextWindowPos(ImVec2(0, timelineY));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, timelineHeight));
        ImGui::Begin("##TimelinePanel", nullptr, fixed | panelLayer | inputBlock);
        // A single docked tab carries the title, as Flashback's "Timeline (time / tick)" does.
        if (widgets::beginDockTabBar("##timeline-tabs")) {
            std::string const title = editor.mTimelinePanel.windowTitle(ctx) + "###timeline-tab";
            if (ImGui::BeginTabItem(title.c_str())) ImGui::EndTabItem();
            ImGui::EndTabBar();
        }
        editor.mTimelinePanel.draw(ctx, !popupOpen);
        ImGui::End();
    }

    // Splitter windows must keep their band size; the default minimum would spread them over the panels.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    {
        Rect upperArea{
            {0.0f,          kMenuHeight              },
            {displaySize.x, kMenuHeight + upperHeight}
        };
        float splitterX = leftWidth - kSplitterThickness * 0.5f;
        ImGui::SetNextWindowPos(ImVec2(splitterX, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(kSplitterThickness, upperHeight));
        ImGui::Begin("##DetailsSplitter", nullptr, fixed | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | inputBlock);
        editor.mDetailsWidthRatio =
            editor.mSplitter.drawVerticalSplit(editor.mDetailsWidthRatio, upperArea, minDetailsRatio, maxDetailsRatio);
        ImGui::End();
    }

    {
        Rect fullArea{
            {0.0f,          kMenuHeight                  },
            {displaySize.x, displaySize.y - kStatusHeight}
        };
        // The band is the gap between the upper panels and the timeline.
        float splitterY = kMenuHeight + upperHeight;
        ImGui::SetNextWindowPos(ImVec2(0, splitterY));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kSplitterThickness));
        ImGui::Begin("##TimelineSplitter", nullptr, fixed | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | inputBlock);
        editor.mTimelineHeightRatio = editor.mSplitter.drawHorizontalSplit(
            1.0f - editor.mTimelineHeightRatio,
            fullArea,
            1.0f - maxTimelineRatio,
            1.0f - minTimelineRatio
        );
        editor.mTimelineHeightRatio        = 1.0f - editor.mTimelineHeightRatio;
        static float savedDetailsRatio     = editor.mDetailsWidthRatio;
        static float savedTimelineRatio    = editor.mTimelineHeightRatio;
        static float savedTrackListRatio   = editor.mTimelinePanel.trackListWidthRatio();
        static float savedZoomScale        = editor.mTimelinePanel.zoomScale();
        static float savedHorizontalScroll = editor.mTimelinePanel.horizontalScroll();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)
            && (savedDetailsRatio != editor.mDetailsWidthRatio || savedTimelineRatio != editor.mTimelineHeightRatio
                || savedTrackListRatio != editor.mTimelinePanel.trackListWidthRatio()
                || savedZoomScale != editor.mTimelinePanel.zoomScale()
                || savedHorizontalScroll != editor.mTimelinePanel.horizontalScroll())) {
            editor.saveLayoutPreferences();
            savedDetailsRatio     = editor.mDetailsWidthRatio;
            savedTimelineRatio    = editor.mTimelineHeightRatio;
            savedTrackListRatio   = editor.mTimelinePanel.trackListWidthRatio();
            savedZoomScale        = editor.mTimelinePanel.zoomScale();
            savedHorizontalScroll = editor.mTimelinePanel.horizontalScroll();
        }
        ImGui::End();
    }
    ImGui::PopStyleVar(2);

    drawStatusBar();

    ImGui::PopStyleVar();
    drawMenuBar();
}

} // namespace playback::editor::ui
