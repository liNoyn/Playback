#pragma once

namespace playback::editor::ui::recording {

// What the renderer draws over the game while playing (never while a replay is open):
//  - the HUD: a small recording indicator plus notifications. Draw-only; it takes no input.
//  - the panel (F6): record / pause / marker, output choice, settings and status. It takes the mouse and keys
//    like the replay browser does.

// Render thread. True when the HUD has something to show.
[[nodiscard]] bool hudWanted();

// Render thread. True while the panel is open.
[[nodiscard]] bool panelWanted();

// Inside an ImGui frame.
void drawHud();
void drawPanel();

} // namespace playback::editor::ui::recording
