#pragma once

#include "playback/exporting/ExportTypes.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/state/ReplayBrowserState.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include <memory>
#include <string>
#include <vector>

namespace playback::state {

struct EditorCapabilities {
    bool cameraEditing{};
    bool videoEditing{};
    bool videoExport{};
    bool ffmpegVideoExport{};
};

struct ProjectPersistenceState {
    bool        dirty{};
    std::string projectFile;
    std::string error;
};

// A player in the replay, listed in the editor's Players tab.
struct ReplayPlayerEntry {
    std::string id;
    std::string name;
    bool        recorder{};
};

struct EditorState {
    bool                                                         replayVisible{};
    bool                                                         editorVisible{};
    bool                                                         hudVisible{};
    bool                                                         paused{};
    float                                                        playbackSpeed{1.0f};
    int                                                          currentTick{};
    int                                                          totalTicks{};
    bool                                                         canUndo{};
    bool                                                         canRedo{};
    std::shared_ptr<state::editing::model::EditorStateExt const> project;
    keyframe::CameraTimelineHandle                               cameraTimeline;
    EditorCapabilities                                           capabilities;
    ProjectPersistenceState                                      persistence;
    exporting::ExportStatus                                      exportStatus;
    ReplayBrowserState                                           browser;
    std::vector<ReplayPlayerEntry>                               players;
    // Id of the player the camera is spectating, empty when it is free.
    std::string spectatingId;
};

} // namespace playback::state
