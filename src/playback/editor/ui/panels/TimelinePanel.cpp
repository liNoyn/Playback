#include "TimelinePanel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/TimelineScale.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <ranges>

namespace playback::editor::ui {
using namespace playback::state;

using namespace ll::i18n_literals;

namespace {

constexpr float kMinZoomScale   = 1.0f;
constexpr float kMaxZoomScale   = 20.0f;
constexpr float kZoomStep       = 1.15f;
constexpr int   kTicksPerSecond = widgets::kTicksPerSecond;
constexpr int   kSnapTickRadius = 6;

using widgets::formatTick;

int majorTickStep(float pixelsPerTick, float minimumSpacing) {
    constexpr int steps[] = {
        1,
        2,
        5,
        10,
        kTicksPerSecond,
        kTicksPerSecond * 2,
        kTicksPerSecond * 5,
        kTicksPerSecond * 10,
        kTicksPerSecond * 20,
        kTicksPerSecond * 30,
        kTicksPerSecond * 60,
        kTicksPerSecond * 120,
        kTicksPerSecond * 300,
        kTicksPerSecond * 600,
    };
    for (int step : steps)
        if (static_cast<float>(step) * pixelsPerTick >= minimumSpacing) return step;
    return steps[std::size(steps) - 1];
}

// Whole seconds split into four, sub-second steps into single ticks, as Flashback's ruler does.
int minorTickStep(int majorStep) {
    if (majorStep >= kTicksPerSecond) return std::max(1, majorStep / 4);
    return std::max(1, majorStep / 5);
}

std::string rulerTimestamp(int tick) {
    int const seconds = std::max(0, tick) / kTicksPerSecond;
    char      value[32]{};
    if (seconds >= 3600) {
        std::snprintf(value, sizeof(value), "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
    } else {
        std::snprintf(value, sizeof(value), "%02d:%02d", seconds / 60, seconds % 60);
    }
    return value;
}

bool contains(ImVec2 const& minimum, ImVec2 const& maximum, ImVec2 const& point) {
    return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y && point.y <= maximum.y;
}

float maxScrollFor(float canvasWidth, float zoom, int totalTicks) {
    float const fit = canvasWidth / static_cast<float>(std::max(1, totalTicks));
    return std::max(0.0f, static_cast<float>(totalTicks) * fit * zoom - canvasWidth);
}

// The keyframe's shape names its interpolation: circle smooth, diamond linear, square hold, notched sides eased.
void drawKeyframeShape(
    ImDrawList*                                     drawList,
    float                                           x,
    float                                           y,
    float                                           r,
    state::editing::model::CameraInterpolationType type,
    ImU32                                           colour
) {
    using Interpolation = state::editing::model::CameraInterpolationType;
    float const ease    = r / 5.0f;
    auto const  easedLeft = [&] {
        drawList->AddTriangleFilled({x - r, y - r}, {x - ease, y - r}, {x - ease, y}, colour);
        drawList->AddTriangleFilled({x - r, y + r}, {x - ease, y}, {x - ease, y + r}, colour);
    };
    auto const easedRight = [&] {
        drawList->AddTriangleFilled({x + ease, y - r}, {x + r, y - r}, {x + ease, y}, colour);
        drawList->AddTriangleFilled({x + ease, y}, {x + r, y + r}, {x + ease, y + r}, colour);
    };
    auto const pointedLeft = [&] { drawList->AddTriangleFilled({x - r, y}, {x - ease, y - r}, {x - ease, y + r}, colour); };
    auto const pointedRight = [&] {
        drawList->AddTriangleFilled({x + ease, y - r}, {x + r, y}, {x + ease, y + r}, colour);
    };
    auto const centre = [&] { drawList->AddRectFilled({x - ease, y - r}, {x + ease, y + r}, colour); };

    switch (type) {
    case Interpolation::Smooth:
        drawList->AddCircleFilled({x, y}, r, colour);
        break;
    case Interpolation::Hold:
        drawList->AddRectFilled({x - r, y - r}, {x + r, y + r}, colour);
        break;
    case Interpolation::Hermite:
        drawList->AddTriangleFilled({x, y - r}, {x + r, y + r}, {x - r, y + r}, colour);
        break;
    case Interpolation::EaseIn:
        easedLeft();
        centre();
        pointedRight();
        break;
    case Interpolation::EaseOut:
        pointedLeft();
        centre();
        easedRight();
        break;
    case Interpolation::EaseInOut:
        easedLeft();
        centre();
        easedRight();
        break;
    case Interpolation::Linear:
    case Interpolation::CubicBezier:
    default:
        drawList->AddQuadFilled({x, y - r}, {x + r, y}, {x, y + r}, {x - r, y}, colour);
        break;
    }
}

} // namespace

void TimelinePanel::setViewPreferences(float trackListWidthRatio, float zoomScale, float horizontalScroll) {
    mTrackListWidthRatio = std::clamp(trackListWidthRatio, 0.08f, 0.55f);
    mZoomScale           = std::clamp(zoomScale, kMinZoomScale, kMaxZoomScale);
    mScrollX             = std::max(0.0f, horizontalScroll);
    mScrollY             = 0.0f;
    mPendingSeekTick     = -1;
    mRulerDragTick       = -1;
    mAnimator.clear();
    mDraggingKeyframeCameraId.clear();
    mDraggingKeyframeStartTick = -1;
    mDraggingKeyframeMoved     = false;
    mRangeDragMode             = 0;
}

void TimelinePanel::submitSeek(PanelContext const& ctx, int tick) {
    mPendingSeekTick = std::clamp(tick, 0, std::max(0, ctx.state.totalTicks));
    EditorAction action{EditorActionType::Seek};
    action.tick = mPendingSeekTick;
    ctx.submitAction(std::move(action));
}

void TimelinePanel::seekTo(PanelContext const& ctx, int tick) { submitSeek(ctx, tick); }

void TimelinePanel::seekRelative(PanelContext const& ctx, int tickDelta) {
    int const baseTick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    submitSeek(ctx, baseTick + tickDelta);
}

void TimelinePanel::seekAdjacentEditPoint(PanelContext const& ctx, bool forward) {
    auto const& state   = ctx.state;
    auto const  project = state.project;
    if (!project) return;

    int const  baseTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    int        target   = forward ? state.totalTicks : 0;
    bool       found    = false;
    auto const consider = [&](int tick) {
        if ((forward && tick <= baseTick) || (!forward && tick >= baseTick)) return;
        if (!found || (forward ? tick < target : tick > target)) {
            target = tick;
            found  = true;
        }
    };

    for (auto const& segment : project->sequence) {
        consider(segment.startTick);
        consider(segment.endTick);
    }
    for (auto const& segment : project->worldActor.segments) {
        consider(segment.startTick);
        consider(segment.endTick);
    }
    for (auto const& camera : project->cameras) {
        for (auto const& [tick, _] : camera.keysByTick) consider(tick);
    }
    for (auto const& marker : project->markers) consider(marker.tick);
    submitSeek(ctx, target);
}

bool TimelinePanel::addKeyframeAtPlayhead(PanelContext const& ctx) {
    std::string cameraId = resolveKeyframeTargetCamera(ctx);
    if (cameraId.empty()) return false;

    EditorAction action{EditorActionType::AddCameraKeyframe};
    action.id   = cameraId;
    action.tick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    ctx.submitAction(std::move(action));
    ctx.selection.select(state::editing::model::SelectedCamera{std::move(cameraId)});
    return true;
}

// Resolves a target track so the shortcut works without a prior selection.
std::string TimelinePanel::resolveKeyframeTargetCamera(PanelContext const& ctx) const {
    auto const project = ctx.state.project;
    if (!project) return {};

    auto const isUsable = [&](std::string const& id) {
        return std::ranges::any_of(project->cameras, [&](auto const& camera) {
            return camera.id == id && !camera.locked;
        });
    };

    if (auto const* selected = ctx.selection.getAs<state::editing::model::SelectedCamera>();
        selected && isUsable(selected->cameraId)) {
        return selected->cameraId;
    }
    if (auto const* selected = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
        selected && isUsable(selected->trackId)) {
        return selected->trackId;
    }
    for (auto const& camera : project->cameras) {
        if (camera.enabled && !camera.locked) return camera.id;
    }
    for (auto const& camera : project->cameras) {
        if (!camera.locked) return camera.id;
    }
    return {};
}

bool TimelinePanel::deleteSelection(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    if (!project) return false;

    EditorAction action;
    if (auto const* selectedCamera = ctx.selection.getAs<state::editing::model::SelectedCamera>();
        selectedCamera && project->cameras.size() > 1) {
        action.type = EditorActionType::DeleteCamera;
        action.id   = selectedCamera->cameraId;
    } else if (auto const* selectedKeyframe = ctx.selection.getAs<state::editing::model::SelectedKeyframe>()) {
        action.type = EditorActionType::DeleteCameraKeyframe;
        action.id   = selectedKeyframe->trackId;
        action.tick = selectedKeyframe->tick;
    } else {
        return false;
    }

    ctx.submitAction(std::move(action));
    ctx.selection.clear();
    return true;
}

void TimelinePanel::zoomIn() { mZoomScale = std::min(kMaxZoomScale, mZoomScale * kZoomStep); }

void TimelinePanel::zoomOut() { mZoomScale = std::max(kMinZoomScale, mZoomScale / kZoomStep); }

void TimelinePanel::resetZoom() {
    mZoomScale = kMinZoomScale;
    mScrollX   = 0.0f;
}

std::string TimelinePanel::windowTitle(PanelContext const& ctx) const {
    int tick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    if (mRulerDragTick >= 0) tick = mRulerDragTick;
    return "playback.refactorEditor.timeline.windowTitle"_tr(rulerTimestamp(tick), tick);
}

// Flashback's transport: five white marks spread over the header above the track list.
void TimelinePanel::drawTransport(PanelContext const& ctx, Layout const& layout, bool allowInput) {
    auto const& state    = ctx.state;
    auto*       drawList = ImGui::GetWindowDrawList();
    float const listW    = layout.listWidth;
    float const size     = std::min(metrics::font() * 1.15f, layout.headerHeight * 0.6f);
    float const top      = layout.rulerTop + (layout.headerHeight - size) * 0.5f;
    ImU32 const white    = theme::kText;

    enum Control { SkipStart, SlowDown, PlayPause, SpeedUp, SkipEnd };
    for (int control = SkipStart; control <= SkipEnd; ++control) {
        float const left = layout.fullMin.x + listW * static_cast<float>(control + 1) / 6.0f - size * 0.5f;
        ImGui::SetCursorScreenPos({left, top});
        ImGui::PushID(control);
        bool const clicked = ImGui::InvisibleButton("##transport", {size, size}) && allowInput;
        bool const hovered = ImGui::IsItemHovered();
        bool const menu    = allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Right);
        ImGui::PopID();

        float const right  = left + size;
        float const bottom = top + size;
        float const middle = top + size * 0.5f;
        switch (control) {
        case SkipStart:
            drawList->AddRectFilled({left, top}, {left + size / 3.0f, bottom}, white);
            drawList->AddTriangleFilled({left + size / 3.0f, middle}, {right, top}, {right, bottom}, white);
            if (hovered) widgets::hoverTooltip("playback.refactorEditor.timeline.skipToStart"_tr().c_str());
            if (clicked) seekTo(ctx, 0);
            break;
        case SlowDown: {
            ImU32 const colour = state.playbackSpeed < 0.999f ? theme::kSlowDown : white;
            drawList->AddTriangleFilled({left, middle}, {left + size * 0.5f, top}, {left + size * 0.5f, bottom}, colour);
            drawList->AddTriangleFilled({left + size * 0.5f, middle}, {right, top}, {right, bottom}, colour);
            if (hovered) {
                widgets::hoverTooltip(
                    "playback.refactorEditor.timeline.slowDown"_tr(static_cast<double>(state.playbackSpeed)).c_str()
                );
            }
            if (clicked) ctx.submitAction({EditorActionType::DecreaseSpeed});
            if (menu) ImGui::OpenPopup("##transport-speed-menu");
            break;
        }
        case PlayPause:
            if (state.paused) {
                drawList->AddTriangleFilled({left + size / 12.0f, top}, {right, middle}, {left + size / 12.0f, bottom}, white);
            } else {
                drawList->AddRectFilled({left, top}, {left + size / 3.0f, bottom}, white);
                drawList->AddRectFilled({left + size * 2.0f / 3.0f, top}, {right, bottom}, white);
            }
            if (hovered) {
                widgets::hoverTooltip((state.paused ? "playback.refactorEditor.timeline.play"_tr()
                                                    : "playback.refactorEditor.timeline.pause"_tr())
                                          .c_str());
            }
            if (clicked) ctx.submitAction({EditorActionType::TogglePause});
            break;
        case SpeedUp: {
            ImU32 const colour = state.playbackSpeed > 1.001f ? theme::kSpeedUp : white;
            drawList->AddTriangleFilled({left, top}, {left + size * 0.5f, middle}, {left, bottom}, colour);
            drawList->AddTriangleFilled({left + size * 0.5f, top}, {right, middle}, {left + size * 0.5f, bottom}, colour);
            if (hovered) {
                widgets::hoverTooltip(
                    "playback.refactorEditor.timeline.speedUp"_tr(static_cast<double>(state.playbackSpeed)).c_str()
                );
            }
            if (clicked) ctx.submitAction({EditorActionType::IncreaseSpeed});
            if (menu) ImGui::OpenPopup("##transport-speed-menu");
            break;
        }
        case SkipEnd:
        default:
            drawList->AddTriangleFilled({left, top}, {left + size * 2.0f / 3.0f, middle}, {left, bottom}, white);
            drawList->AddRectFilled({left + size * 2.0f / 3.0f, top}, {right, bottom}, white);
            if (hovered) widgets::hoverTooltip("playback.refactorEditor.timeline.skipToEnd"_tr().c_str());
            if (clicked) seekTo(ctx, state.totalTicks);
            break;
        }
    }

    // Right-clicking either speed control lists the speeds themselves.
    if (ImGui::BeginPopup("##transport-speed-menu")) {
        for (float const speed : replay::ReplaySession::playbackSpeeds()) {
            char label[16]{};
            std::snprintf(label, sizeof(label), "%gx", static_cast<double>(speed));
            if (ImGui::MenuItem(label, nullptr, std::abs(state.playbackSpeed - speed) < 0.0001f)) {
                EditorAction action{EditorActionType::SetPlaybackSpeed};
                action.speed = speed;
                ctx.submitAction(std::move(action));
            }
        }
        ImGui::EndPopup();
    }
}

// Options that had toolbar buttons before; Flashback keeps its timeline bare and puts them in a context menu.
void TimelinePanel::drawOptionsMenu(PanelContext const& ctx) {
    if (!ImGui::BeginPopup("##timeline-options")) return;
    if (ImGui::MenuItem("playback.refactorEditor.details.addFreeCamera"_tr().c_str())) {
        ctx.submitAction({EditorActionType::AddFreeCamera});
    }
    ImGui::Separator();
    if (ImGui::MenuItem("playback.refactorEditor.timeline.snapTooltip"_tr().c_str(), nullptr, mSnapEnabled)) {
        mSnapEnabled = !mSnapEnabled;
    }
    if (ImGui::MenuItem("playback.refactorEditor.timeline.zoomReset"_tr().c_str())) resetZoom();
    bool const maximized = ctx.commands.isViewportMaximized();
    if (ImGui::MenuItem(
            (maximized ? "playback.refactorEditor.timeline.restore"_tr()
                       : "playback.refactorEditor.timeline.maximize"_tr())
                .c_str()
        )) {
        ctx.commands.toggleViewportMaximized();
    }
    ImGui::EndPopup();
}

void TimelinePanel::drawTrackHeaders(PanelContext const& ctx, Layout const& layout, bool allowInput) {
    auto const  project  = ctx.state.project;
    auto*       drawList = ImGui::GetWindowDrawList();
    float const listLeft = layout.fullMin.x;
    float const listW    = layout.listWidth;

    ImGui::SetCursorScreenPos({listLeft, layout.bodyTop});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild(
        "##TimelineTrackList",
        {listW, layout.bodyBottom - layout.bodyTop},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    drawList->PushClipRect({listLeft, layout.bodyTop}, {listLeft + listW, layout.bodyBottom}, true);

    auto const* selectedCamera   = ctx.selection.getAs<state::editing::model::SelectedCamera>();
    auto const* selectedKeyframe = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
    ImVec2 const mouse           = ImGui::GetMousePos();
    float const  glyph           = metrics::iconGlyph();
    float const  spacing         = metrics::gutter() * 0.5f;
    int const    playheadTick    = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    float        rowY            = layout.bodyTop - mScrollY;
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = rowY + row.height;
        if (rowBottom < layout.bodyTop || rowY > layout.bodyBottom) {
            rowY = rowBottom;
            continue;
        }
        bool const selected = !row.cameraId.empty()
                           && ((selectedCamera && selectedCamera->cameraId == row.cameraId)
                               || (selectedKeyframe && selectedKeyframe->trackId == row.cameraId));

        ImGui::SetCursorScreenPos({listLeft, rowY});
        ImGui::InvisibleButton(("##track-row-" + row.id).c_str(), {listW, row.height});
        bool const hovered = ImGui::IsItemHovered();
        bool const clicked = allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Left);
        if (allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            ctx.selection.select(state::editing::model::SelectedCamera{row.cameraId});
            ImGui::OpenPopup(("##track-options-" + row.id).c_str());
        }
        if (selected) drawList->AddRectFilled({listLeft, rowY}, {listLeft + listW, rowBottom}, theme::kRowHighlight);
        else if (hovered) {
            drawList->AddRectFilled({listLeft, rowY}, {listLeft + listW, rowBottom}, theme::withAlpha(theme::kHover, 0x30));
        }

        float const rowCentre = (rowY + rowBottom) * 0.5f;
        ImU32 const textColor = row.enabled ? theme::kText : theme::kTextDim;
        float       labelX    = listLeft + metrics::gutter();
        float const textY     = widgets::textYForCentre(rowCentre, row.name.c_str());
        widgets::drawIconAtCentre(
            drawList,
            ICON_CAMERA,
            {labelX + glyph * 0.5f, widgets::textInkCentre(textY, row.name.c_str())},
            textColor
        );
        labelX += glyph + spacing;
        drawList->AddText({labelX, textY}, textColor, row.name.c_str());

        if (row.kind == TrackRowKind::Camera) {
            // Track options, enable toggle and add keyframe sit at the right of the row, as in Flashback.
            enum Button { Options, Enable, Add };
            std::string const enableTip = row.enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                                      : "playback.refactorEditor.timeline.enableTrack"_tr();
            std::string const addTip    = "playback.refactorEditor.timeline.addKeyframe"_tr();
            char const* tooltips[]      = {nullptr, enableTip.c_str(), addTip.c_str()};
            char const* icons[]         = {ICON_MORE, row.enabled ? ICON_EYE : ICON_HIDE, row.locked ? ICON_LOCK : ICON_ADD};

            bool overButton = false;
            for (int button = Options; button <= Add; ++button) {
                float const x = listLeft + listW - (glyph + spacing) * static_cast<float>(3 - button) - spacing;
                bool const  over = contains({x, rowY}, {x + glyph, rowBottom}, mouse);
                overButton      |= over;
                widgets::drawIconAtCentre(
                    drawList,
                    icons[button],
                    {x + glyph * 0.5f, rowCentre},
                    over ? theme::kIconHighlight : theme::kIconInactive
                );
                if (over && tooltips[button]) widgets::hoverTooltip(tooltips[button]);
                if (!over || !allowInput || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) continue;
                if (button == Options) {
                    ctx.selection.select(state::editing::model::SelectedCamera{row.cameraId});
                    ImGui::OpenPopup(("##track-options-" + row.id).c_str());
                } else if (button == Enable) {
                    EditorAction action{EditorActionType::SetCameraEnabled};
                    action.id    = row.cameraId;
                    action.value = !row.enabled;
                    ctx.submitAction(std::move(action));
                } else if (!row.locked) {
                    EditorAction action{EditorActionType::AddCameraKeyframe};
                    action.id   = row.cameraId;
                    action.tick = playheadTick;
                    ctx.submitAction(std::move(action));
                    ctx.selection.select(state::editing::model::SelectedCamera{row.cameraId});
                }
            }
            if (clicked && !overButton) {
                ctx.selection.select(state::editing::model::SelectedCamera{row.cameraId});
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = row.cameraId;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::BeginPopup(("##track-options-" + row.id).c_str())) {
                if (ImGui::MenuItem(enableTip.c_str())) {
                    EditorAction action{EditorActionType::SetCameraEnabled};
                    action.id    = row.cameraId;
                    action.value = !row.enabled;
                    ctx.submitAction(std::move(action));
                }
                if (ImGui::MenuItem(addTip.c_str(), nullptr, false, !row.locked)) {
                    EditorAction action{EditorActionType::AddCameraKeyframe};
                    action.id   = row.cameraId;
                    action.tick = playheadTick;
                    ctx.submitAction(std::move(action));
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                        "playback.refactorEditor.details.deleteCamera"_tr().c_str(),
                        nullptr,
                        false,
                        project && project->cameras.size() > 1 && !row.locked
                    )) {
                    EditorAction action{EditorActionType::DeleteCamera};
                    action.id = row.cameraId;
                    ctx.submitAction(std::move(action));
                    ctx.selection.clear();
                }
                ImGui::EndPopup();
            }
        }
        rowY = rowBottom;
    }

    // The list ends in an add row, like the button under Flashback's keyframe tracks.
    float const addHeight = metrics::cameraRow();
    if (rowY < layout.bodyBottom) {
        std::string const label = "playback.refactorEditor.details.addFreeCamera"_tr();
        ImGui::SetCursorScreenPos({listLeft, rowY});
        ImGui::InvisibleButton("##track-add", {listW, addHeight});
        bool const  hovered = ImGui::IsItemHovered();
        ImU32 const colour  = hovered ? theme::kText : theme::kTextDim;
        float const centre  = rowY + addHeight * 0.5f;
        float const textY   = widgets::textYForCentre(centre, label.c_str());
        float       x       = listLeft + metrics::gutter();
        widgets::drawIconAtCentre(drawList, ICON_ADD, {x + glyph * 0.5f, widgets::textInkCentre(textY, label.c_str())}, colour);
        x += glyph + spacing;
        drawList->AddText({x, textY}, colour, label.c_str());
        if (allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Left)) ctx.submitAction({EditorActionType::AddFreeCamera});
    }
    drawList->PopClipRect();
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void TimelinePanel::drawRuler(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale) {
    auto const& state     = ctx.state;
    auto*       drawList  = ImGui::GetWindowDrawList();
    float const tickTop   = layout.rulerTop + metrics::timestampBand();
    float const minorTop  = tickTop + metrics::minorTick();
    float const labelSpan = ImGui::CalcTextSize(rulerTimestamp(state.totalTicks).c_str()).x
                          + ImGui::CalcTextSize("/00").x + 20.0f * metrics::scale();
    int const   majorStep = majorTickStep(scale.pixelsPerTick, labelSpan);
    int const   minorStep = minorTickStep(majorStep);
    bool const  subSecond = majorStep < kTicksPerSecond;
    for (int tick = scale.firstVisibleTick(minorStep); tick <= state.totalTicks; tick += minorStep) {
        float const x = std::floor(scale.xAt(static_cast<float>(tick)));
        if (x < layout.canvasLeft - 1.0f || x > layout.fullMax.x) continue;
        if (tick % majorStep != 0) {
            drawList->AddLine({x, minorTop}, {x, layout.bodyTop}, theme::kRulerMinor);
            continue;
        }
        drawList->AddLine({x, tickTop}, {x, layout.bodyTop}, theme::kRulerMajor);
        std::string const label = rulerTimestamp(tick);
        drawList->AddText({x, layout.rulerTop}, theme::kText, label.c_str());
        if (subSecond) {
            std::string const sub = "/" + std::to_string(tick % kTicksPerSecond);
            drawList->AddText({x + ImGui::CalcTextSize(label.c_str()).x, layout.rulerTop}, theme::kRulerSubTick, sub.c_str());
        }
    }
}

void TimelinePanel::drawExportRange(
    PanelContext const&  ctx,
    Layout const&        layout,
    TimelineScale const& scale,
    bool                 allowInput
) {
    int const total = ctx.state.totalTicks;
    if (total <= 0) return;
    // Unset means "whole replay", so nothing is drawn until the user marks a point.
    int const inTick  = ctx.commands.exportStartTick();
    int const outTick = ctx.commands.exportEndTick();
    if (inTick < 0 || outTick < 0) return;

    auto*       drawList = ImGui::GetWindowDrawList();
    float const left     = scale.xAt(static_cast<float>(std::clamp(inTick, 0, total)));
    float const right    = scale.xAt(static_cast<float>(std::clamp(outTick, 0, total)));
    float const top      = layout.rulerTop + metrics::timestampBand();
    float const bottom   = layout.bodyTop;
    drawList->PushClipRect({layout.canvasLeft, layout.rulerTop}, {layout.fullMax.x, bottom}, true);

    // Flashback marks the export range as a blue band under the timestamps with a thick line at each end.
    drawList->AddRectFilled({left, top}, {right, bottom}, theme::kExportBand);
    ImVec2 const mouse  = ImGui::GetMousePos();
    bool const   inBand = allowInput && mouse.y >= top && mouse.y <= bottom;
    float const  grab   = std::max(5.0f, metrics::gutter());
    float const  width  = std::max(2.0f, 4.0f * metrics::scale());
    auto const   edge   = [&](float x, int dragMode) {
        bool const  hovered = inBand && std::abs(mouse.x - x) <= grab;
        ImU32 const color   = (mExportMarkerDrag == dragMode || hovered) ? theme::kIconHighlight : theme::kExportBracket;
        drawList->AddLine({x, top}, {x, bottom}, color, width);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // The ruler under the band took the same click; dragging an end must not also seek.
            mExportMarkerDrag = dragMode;
            mRulerDragTick    = -1;
        }
        return hovered;
    };
    bool const overIn  = edge(left, 1);
    bool const overOut = edge(right, 2);
    if (overIn || overOut || mExportMarkerDrag != 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    drawList->PopClipRect();

    if (mExportMarkerDrag == 0) return;
    if (!allowInput || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        mExportMarkerDrag = 0;
        return;
    }
    // Shift snaps to the nearest keyframe, matching the modifier used elsewhere on the timeline.
    int dragged = std::clamp(scale.tickAt(mouse.x), 0, total);
    if (ImGui::GetIO().KeyShift && ctx.project()) {
        int best     = dragged;
        int bestDist = std::numeric_limits<int>::max();
        for (auto const& camera : ctx.project()->cameras) {
            for (auto const& [tick, key] : camera.keysByTick) {
                int const distance = std::abs(tick - dragged);
                if (distance < bestDist) {
                    bestDist = distance;
                    best     = tick;
                }
            }
        }
        if (bestDist <= kSnapTickRadius) dragged = best;
    }
    if (mExportMarkerDrag == 1) ctx.commands.setExportStartTick(std::min(dragged, outTick));
    else ctx.commands.setExportEndTick(std::max(dragged, inTick));
}

// Flashback's zoom bar: a thin white bar along the bottom that thickens on hover and shows its end handles.
void TimelinePanel::drawRangeBar(PanelContext const& ctx, Layout const& layout, bool allowInput) {
    auto const& state     = ctx.state;
    auto*       drawList  = ImGui::GetWindowDrawList();
    float const barTop    = layout.bodyBottom;
    float const barLeft   = layout.canvasLeft;
    float const barWidth  = layout.canvasWidth;
    float const barHeight = layout.fullMax.y - barTop;
    if (barWidth <= 8.0f || barHeight <= 2.0f) return;

    float const windowSpan  = std::clamp(1.0f / std::max(kMinZoomScale, mZoomScale), 0.02f, 1.0f);
    float const maxScroll   = maxScrollFor(layout.canvasWidth, mZoomScale, state.totalTicks);
    float const windowStart = maxScroll > 0.0f ? (mScrollX / maxScroll) * (1.0f - windowSpan) : 0.0f;

    ImGui::SetCursorScreenPos({barLeft, barTop});
    ImGui::InvisibleButton("##timeline-range", {barWidth, barHeight});
    ImVec2 const mouse      = ImGui::GetMousePos();
    bool const   barHovered = ImGui::IsItemHovered();
    bool const   expanded   = barHovered || mRangeDragMode != 0;
    float const  height     = expanded ? barHeight : barHeight * 0.5f;
    float const  top        = layout.fullMax.y - height;
    float const  minThumb   = height * 2.0f;

    float winLeft  = barLeft + windowStart * barWidth;
    float winRight = winLeft + windowSpan * barWidth;
    if (winRight - winLeft < minThumb) {
        float const centre =
            std::clamp((winLeft + winRight) * 0.5f, barLeft + minThumb * 0.5f, barLeft + barWidth - minThumb * 0.5f);
        winLeft  = centre - minThumb * 0.5f;
        winRight = centre + minThumb * 0.5f;
    }
    float const radius    = height * 0.5f;
    bool const  overThumb = barHovered && mouse.x >= winLeft && mouse.x <= winRight;
    bool const  nearLeft  = overThumb && mouse.x <= winLeft + height;
    bool const  nearRight = overThumb && !nearLeft && mouse.x >= winRight - height;

    if (expanded) {
        drawList->AddRectFilled({barLeft, top}, {layout.fullMax.x, layout.fullMax.y}, theme::kRangeTrack, radius);
    }
    if (overThumb || mRangeDragMode != 0) {
        drawList->AddRectFilled({winLeft + radius, top}, {winRight - radius, layout.fullMax.y}, theme::kRangeThumb, radius);
        drawList->AddCircleFilled({winLeft + radius, top + radius}, radius, theme::kRangeHandle);
        drawList->AddCircleFilled({winRight - radius, top + radius}, radius, theme::kRangeHandle);
    } else {
        drawList->AddRectFilled({winLeft, top}, {winRight, layout.fullMax.y}, theme::kRangeThumb, radius);
    }

    if (!allowInput) {
        mRangeDragMode = 0;
        return;
    }
    if (barHovered || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGui::IsItemActive() ? theme::kCursorGrabbing : theme::kCursorGrab);
    }
    if (ImGui::IsItemActivated()) {
        if (nearLeft) mRangeDragMode = 1;
        else if (nearRight) mRangeDragMode = 2;
        else if (overThumb) mRangeDragMode = 3;
        else mRangeDragMode = 4;
        mRangeDragOriginX      = mouse.x;
        mRangeDragOriginScroll = mScrollX;
        mRangeDragOriginZoom   = mZoomScale;
    }
    if (ImGui::IsItemDeactivated()) {
        if (mRangeDragMode == 4) resetZoom();
        mRangeDragMode = 0;
    }
    if (!ImGui::IsItemActive() || mRangeDragMode == 0) return;

    float const spanBefore      = std::clamp(1.0f / std::max(kMinZoomScale, mRangeDragOriginZoom), 0.02f, 1.0f);
    float const maxScrollBefore = maxScrollFor(layout.canvasWidth, mRangeDragOriginZoom, state.totalTicks);
    float const startBefore =
        maxScrollBefore > 0.0f ? (mRangeDragOriginScroll / maxScrollBefore) * (1.0f - spanBefore) : 0.0f;
    float const delta = (mouse.x - mRangeDragOriginX) / barWidth;

    float newStart = startBefore;
    float newSpan  = spanBefore;
    if (mRangeDragMode == 1) {
        newStart = std::clamp(startBefore + delta, 0.0f, startBefore + spanBefore - 0.02f);
        newSpan  = startBefore + spanBefore - newStart;
    } else if (mRangeDragMode == 2) {
        newSpan = std::clamp(spanBefore + delta, 0.02f, 1.0f - startBefore);
    } else if (mRangeDragMode == 3) {
        newStart = std::clamp(startBefore + delta, 0.0f, 1.0f - spanBefore);
    } else {
        return;
    }

    mZoomScale               = std::clamp(1.0f / std::max(0.02f, newSpan), kMinZoomScale, kMaxZoomScale);
    float const clampedSpan  = std::clamp(1.0f / mZoomScale, 0.02f, 1.0f);
    float const maxScrollNow = maxScrollFor(layout.canvasWidth, mZoomScale, state.totalTicks);
    float const startSpan    = std::max(0.0001f, 1.0f - clampedSpan);
    mScrollX                 = std::clamp(newStart / startSpan * maxScrollNow, 0.0f, maxScrollNow);
}

void TimelinePanel::draw(PanelContext const& ctx, bool allowInput) {
    auto const& state   = ctx.state;
    auto const  project = state.project;
    if (!project) {
        widgets::noActiveProjectPlaceholder();
        return;
    }

    if (!allowInput) {
        mRulerDragTick    = -1;
        mDraggingPlayhead = false;
        mDraggingKeyframeCameraId.clear();
        mDraggingKeyframeStartTick = -1;
        mDraggingKeyframeMoved     = false;
        mRangeDragMode             = 0;
    }

    mAnimator.beginFrame();
    mTrackTree.setSearch({});
    mTrackTree.rebuild(*project);
    int displayTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    if (mRulerDragTick >= 0) displayTick = mRulerDragTick;
    if (mPendingSeekTick >= 0 && state.currentTick == mPendingSeekTick) mPendingSeekTick = -1;

    ImVec2 const fullMin   = ImGui::GetCursorScreenPos();
    ImVec2 const available = ImGui::GetContentRegionAvail();
    if (available.x < 220.0f || available.y < 80.0f) return;

    // Flashback's layout: the window title carries the time, the header holds the transport beside the ruler.
    Layout layout;
    layout.fullMin      = fullMin;
    layout.fullMax      = {fullMin.x + available.x, fullMin.y + available.y};
    layout.titleBottom  = fullMin.y;
    layout.rulerTop     = fullMin.y;
    layout.headerHeight = metrics::timestampBand() + metrics::minorTick() * 2.0f;
    layout.bodyTop      = layout.rulerTop + layout.headerHeight;
    layout.bodyBottom   = layout.fullMax.y - metrics::rangeBar();

    float const fontSize         = ImGui::GetFontSize();
    float const minimumListWidth = std::max(fontSize * 11.0f, 160.0f);
    layout.listWidth             = std::clamp(
        available.x * mTrackListWidthRatio,
        minimumListWidth,
        std::max(minimumListWidth, available.x - std::max(fontSize * 12.0f, 180.0f))
    );
    layout.canvasLeft  = fullMin.x + layout.listWidth + 1.0f;
    layout.canvasWidth = std::max(1.0f, layout.fullMax.x - layout.canvasLeft);

    auto* drawList = ImGui::GetWindowDrawList();

    float const fitPixelsPerTick = layout.canvasWidth / static_cast<float>(std::max(1, state.totalTicks));
    float const pixelsPerTick    = fitPixelsPerTick * mZoomScale;
    float const maxScroll        = std::max(0.0f, state.totalTicks * pixelsPerTick - layout.canvasWidth);
    mScrollX                     = std::clamp(mScrollX, 0.0f, maxScroll);

    TimelineScale const scale{layout.canvasLeft, layout.canvasWidth, pixelsPerTick, mScrollX, state.totalTicks};

    float trackContentHeight = metrics::cameraRow();
    for (auto const& row : mTrackTree.rows()) trackContentHeight += row.height;
    float const visibleTrackHeight = std::max(0.0f, layout.bodyBottom - layout.bodyTop);
    float const maxScrollY         = std::max(0.0f, trackContentHeight - visibleTrackHeight);
    mScrollY                       = std::clamp(mScrollY, 0.0f, maxScrollY);

    drawTransport(ctx, layout, allowInput);

    // The white divider beside the track list doubles as the handle that resizes it.
    float const  grab = metrics::splitter();
    ImVec2 const listSplitterMin{layout.canvasLeft - grab, layout.bodyTop};
    ImVec2 const listSplitterMax{layout.canvasLeft + grab, layout.bodyBottom};
    ImGui::SetCursorScreenPos(listSplitterMin);
    ImGui::InvisibleButton(
        "##timeline-list-splitter",
        {listSplitterMax.x - listSplitterMin.x, listSplitterMax.y - listSplitterMin.y}
    );
    bool const listSplitterHovered = ImGui::IsItemHovered();
    bool const listSplitterActive  = ImGui::IsItemActive();
    if (listSplitterHovered || listSplitterActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (allowInput && listSplitterActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        mTrackListWidthRatio = std::clamp((ImGui::GetMousePos().x - fullMin.x) / available.x, 0.08f, 0.55f);
    }

    drawTrackHeaders(ctx, layout, allowInput);

    drawList->PushClipRect({layout.canvasLeft, layout.rulerTop}, {layout.fullMax.x, layout.fullMax.y}, true);
    drawRuler(ctx, layout, scale);

    ImGui::SetCursorScreenPos({layout.canvasLeft, layout.rulerTop});
    ImGui::InvisibleButton("##timeline-ruler", {layout.canvasWidth, layout.bodyTop - layout.rulerTop});
    if (allowInput && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && mExportMarkerDrag == 0) {
        mRulerDragTick = std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks);
        displayTick    = mRulerDragTick;
    }
    if (allowInput && ImGui::IsItemDeactivated()) {
        if (mRulerDragTick >= 0) submitSeek(ctx, mRulerDragTick);
        mRulerDragTick = -1;
    }

    auto tickFromMouse = [&] { return std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks); };
    auto snapTick      = [&](int tick) {
        tick = std::clamp(tick, 0, state.totalTicks);
        if (!mSnapEnabled) return tick;
        constexpr float GridTicks = static_cast<float>(kTicksPerSecond);
        return std::clamp(
            static_cast<int>(std::round(static_cast<float>(tick) / GridTicks) * GridTicks),
            0,
            state.totalTicks
        );
    };
    auto boundedKeyframeTick = [&](int tick) {
        auto const camera =
            std::ranges::find(project->cameras, mDraggingKeyframeCameraId, &state::editing::model::CameraEntity::id);
        if (camera == project->cameras.end()) return tick;
        auto const key = camera->keysByTick.find(mDraggingKeyframeStartTick);
        if (key == camera->keysByTick.end()) return tick;

        int minimum = 0;
        int maximum = state.totalTicks;
        if (key != camera->keysByTick.begin()) minimum = std::min(state.totalTicks, std::prev(key)->first + 1);
        auto const next = std::next(key);
        if (next != camera->keysByTick.end()) maximum = std::max(0, next->first - 1);
        if (minimum > maximum) return mDraggingKeyframeStartTick;
        return std::clamp(tick, minimum, maximum);
    };

    auto const* selectedKeyframe = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
    float const rowHeight        = metrics::cameraRow();
    float const keyRadius        = std::min(metrics::keyframeRadius(), rowHeight * 0.4f);
    float       y                = layout.bodyTop - mScrollY;
    bool        clickConsumed    = false;
    ImVec2 const mouse           = ImGui::GetMousePos();
    drawList->PushClipRect({layout.canvasLeft, layout.bodyTop}, {layout.fullMax.x, layout.bodyBottom}, true);
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = y + row.height;
        if (rowBottom < layout.bodyTop || y > layout.bodyBottom) {
            y = rowBottom;
            continue;
        }
        float const centerY = (y + rowBottom) * 0.5f;

        if (row.cameraIndex >= 0 && row.cameraIndex < static_cast<int>(project->cameras.size())) {
            auto const& camera = project->cameras[row.cameraIndex];
            bool const  cameraSelected =
                (ctx.selection.getAs<state::editing::model::SelectedCamera>()
                 && ctx.selection.getAs<state::editing::model::SelectedCamera>()->cameraId == camera.id)
                || (selectedKeyframe && selectedKeyframe->trackId == camera.id);
            if (cameraSelected) {
                drawList->AddRectFilled({layout.canvasLeft, y}, {layout.fullMax.x, rowBottom}, theme::withAlpha(theme::kHover, 0x20));
            }

            for (auto const& [keyTick, key] : camera.keysByTick) {
                bool const  dragging  = mDraggingKeyframeCameraId == camera.id && mDraggingKeyframeStartTick == keyTick;
                int const   drawnTick = dragging ? mDraggingKeyframeTick : keyTick;
                float const x         = scale.xAt(static_cast<float>(drawnTick));
                if (x < layout.canvasLeft - keyRadius || x > layout.fullMax.x + keyRadius) continue;
                bool const selected =
                    selectedKeyframe && selectedKeyframe->trackId == camera.id && selectedKeyframe->tick == keyTick;

                ImU32 colour = selected || dragging ? theme::kKeyframeSelected : theme::kKeyframe;
                if (!camera.enabled) colour = theme::withAlpha(colour, 0x80);
                drawKeyframeShape(drawList, x, centerY, keyRadius, key.interpolationType, colour);

                if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                    && std::abs(mouse.x - x) <= keyRadius + 3.0f && mouse.y >= y && mouse.y <= rowBottom) {
                    clickConsumed = true;
                    ctx.selection.select(state::editing::model::SelectedKeyframe{camera.id, keyTick});
                    EditorAction previewAction{EditorActionType::SetPreviewCamera};
                    previewAction.id = camera.id;
                    ctx.submitAction(std::move(previewAction));
                    if (camera.locked || !camera.enabled) {
                        submitSeek(ctx, keyTick);
                    } else {
                        mDraggingKeyframeCameraId    = camera.id;
                        mDraggingKeyframeStartMouseX = mouse.x;
                        mDraggingKeyframeStartTick   = keyTick;
                        mDraggingKeyframeTick        = keyTick;
                        mDraggingKeyframeMoved       = false;
                    }
                }
            }

            // Runs after the keyframe pass so a keyframe hit always wins over the row.
            if (row.kind == TrackRowKind::Camera && allowInput && !clickConsumed
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && contains({layout.canvasLeft, y}, {layout.fullMax.x, rowBottom}, mouse)) {
                clickConsumed = true;
                ctx.selection.select(state::editing::model::SelectedCamera{camera.id});
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = camera.id;
                ctx.submitAction(std::move(action));
            }
        }
        y = rowBottom;
    }
    drawList->PopClipRect();

    drawExportRange(ctx, layout, scale, allowInput);

    if (allowInput && !mDraggingKeyframeCameraId.empty()) {
        // Any horizontal movement latches into a drag, so a plain click only seeks.
        if (mouse.x != mDraggingKeyframeStartMouseX) mDraggingKeyframeMoved = true;
        int const candidateTick =
            mDraggingKeyframeMoved ? boundedKeyframeTick(snapTick(tickFromMouse())) : mDraggingKeyframeStartTick;
        mDraggingKeyframeTick = candidateTick;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (mDraggingKeyframeMoved) {
                if (candidateTick != mDraggingKeyframeStartTick) {
                    EditorAction action{EditorActionType::MoveCameraKeyframe};
                    action.id            = mDraggingKeyframeCameraId;
                    action.tick          = mDraggingKeyframeStartTick;
                    action.secondaryTick = candidateTick;
                    ctx.submitAction(std::move(action));
                }
            } else {
                submitSeek(ctx, mDraggingKeyframeStartTick);
            }
            mDraggingKeyframeCameraId.clear();
            mDraggingKeyframeStartTick = -1;
            mDraggingKeyframeMoved     = false;
        }
    }

    // Markers sit on the line under the ruler as coloured dots; clicking one jumps to it.
    float const markerRadius = 5.0f * metrics::scale();
    for (auto const& marker : project->markers) {
        float const x = scale.xAt(static_cast<float>(marker.tick));
        if (x < layout.canvasLeft - markerRadius || x > layout.fullMax.x + markerRadius) continue;
        drawList->AddCircleFilled({x, layout.bodyTop}, markerRadius, theme::kMarker);
        bool const hovered = std::abs(mouse.x - x) <= markerRadius && std::abs(mouse.y - layout.bodyTop) <= markerRadius;
        if (!hovered) continue;
        if (!marker.label.empty() && !ImGui::IsAnyMouseDown()) widgets::hoverTooltip(marker.label.c_str());
        if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            clickConsumed = true;
            submitSeek(ctx, marker.tick);
        }
    }

    float const playheadSize = 5.0f * metrics::scale();
    float const playheadX    = scale.xAt(static_cast<float>(displayTick));
    if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && std::abs(mouse.x - playheadX) <= playheadSize * 2.0f && mouse.y >= layout.rulerTop
        && mouse.y < layout.bodyBottom) {
        mDraggingPlayhead = true;
        clickConsumed     = true;
    }
    if (allowInput && mDraggingPlayhead) {
        displayTick = tickFromMouse();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            submitSeek(ctx, displayTick);
            mDraggingPlayhead = false;
        }
    }
    if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && contains({layout.canvasLeft, layout.bodyTop}, {layout.fullMax.x, layout.bodyBottom}, mouse)) {
        submitSeek(ctx, tickFromMouse());
    }
    if (allowInput && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        && contains({layout.canvasLeft, layout.rulerTop}, {layout.fullMax.x, layout.bodyBottom}, mouse)) {
        ImGui::OpenPopup("##timeline-options");
    }

    // The end of the replay gets a closing line once it is in view.
    float const endX = scale.xAt(static_cast<float>(state.totalTicks));
    if (endX <= layout.fullMax.x) {
        float const lineX = std::min(endX, layout.fullMax.x - 1.0f);
        drawList->AddLine({lineX, layout.rulerTop + metrics::timestampBand()}, {lineX, layout.bodyBottom}, theme::kLine);
    }

    // Playhead: a white line under a downward triangle, after Flashback's cursor.
    float const cursorX = std::floor(scale.xAt(static_cast<float>(displayTick)));
    if (cursorX >= layout.canvasLeft - playheadSize * 2.0f && cursorX <= layout.fullMax.x + playheadSize * 2.0f) {
        ImU32 const colour = displayTick < state.currentTick ? theme::withAlpha(theme::kPlayhead, 0x80) : theme::kPlayhead;
        drawList->AddTriangleFilled(
            {cursorX, layout.bodyTop},
            {cursorX - playheadSize * 2.0f, layout.rulerTop + metrics::timestampBand() + playheadSize},
            {cursorX + playheadSize * 2.0f, layout.rulerTop + metrics::timestampBand() + playheadSize},
            colour
        );
        drawList->AddRectFilled({cursorX - 1.0f, layout.bodyTop - 2.0f}, {cursorX + 1.0f, layout.bodyBottom}, colour);
    }
    drawList->PopClipRect();

    // Dividers: one under the header across the whole panel, one between the track list and the tracks.
    drawList->AddLine({layout.fullMin.x, layout.bodyTop}, {layout.fullMax.x, layout.bodyTop}, theme::kLine);
    drawList->AddLine(
        {layout.canvasLeft - 1.0f, layout.rulerTop + metrics::timestampBand()},
        {layout.canvasLeft - 1.0f, layout.fullMax.y},
        listSplitterActive ? theme::kAccentHover : theme::kLine
    );

    if (maxScrollY > 0.0f) {
        float const trackHeight = std::max(1.0f, visibleTrackHeight);
        float const thumbHeight = std::max(20.0f, trackHeight * trackHeight / std::max(1.0f, trackContentHeight));
        float const thumbTop    = layout.bodyTop + (trackHeight - thumbHeight) * (mScrollY / maxScrollY);
        drawList->AddRectFilled(
            {layout.fullMax.x - 5.0f, thumbTop},
            {layout.fullMax.x - 2.0f, thumbTop + thumbHeight},
            theme::withAlpha(theme::kScrollThumb, 200),
            2.0f
        );
    }

    bool const canvasHovered = contains({layout.canvasLeft, layout.rulerTop}, {layout.fullMax.x, layout.bodyBottom}, mouse);
    bool const trackListHovered = mouse.x >= fullMin.x && mouse.x < layout.canvasLeft && mouse.y >= layout.bodyTop
                               && mouse.y < layout.bodyBottom;
    if (allowInput && (canvasHovered || trackListHovered) && ImGui::GetIO().MouseWheel != 0.0f) {
        float const wheel = ImGui::GetIO().MouseWheel;
        if (ImGui::GetIO().KeyAlt || ImGui::GetIO().KeyCtrl) {
            float const anchorX    = std::clamp(mouse.x - layout.canvasLeft, 0.0f, layout.canvasWidth);
            float const anchorTick = scale.tickAtExact(layout.canvasLeft + anchorX);
            mZoomScale =
                std::clamp(mZoomScale * (wheel > 0.0f ? kZoomStep : 1.0f / kZoomStep), kMinZoomScale, kMaxZoomScale);
            float const nextPixelsPerTick = fitPixelsPerTick * mZoomScale;
            float const nextMaxScroll     = std::max(0.0f, state.totalTicks * nextPixelsPerTick - layout.canvasWidth);
            mScrollX                      = std::clamp(anchorTick * nextPixelsPerTick - anchorX, 0.0f, nextMaxScroll);
        } else if (canvasHovered && maxScroll > 0.0f) {
            mScrollX = std::clamp(mScrollX - wheel * 60.0f, 0.0f, maxScroll);
        } else {
            mScrollY = std::clamp(mScrollY - wheel * 45.0f, 0.0f, maxScrollY);
        }
    }

    drawRangeBar(ctx, layout, allowInput);
    drawOptionsMenu(ctx);
}

} // namespace playback::editor::ui
