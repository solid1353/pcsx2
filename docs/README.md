# Fork documentation

These documents describe behavior that this fork adds to or changes in
upstream PCSX2, written from the current fork source. Upstream behavior is
documented by the PCSX2 project.

Start File Directory (`StartFile` in `[Folders]`) sets the Start File folder; when empty or missing, it uses the last folder used.

`InputProfileName` in `PCSX2.ini`'s `[EmuCore]` names a default input profile
that replaces the shared input settings: it is active from startup, between
games, and in every game whose settings name no other profile. PCSX2 rereads
the active profile whenever it reloads settings, including at every game start
and stop, so saved edits apply without a restart. Controller
Settings then lists only input profiles and opens on the active one, and Game
Properties calls the no-profile choice "Default". When the key is empty or
names a missing profile, the shared settings apply as upstream. The
implementation is in `pcsx2/VMManager.cpp` and
`pcsx2-qt/Settings/ControllerSettingsWindow.cpp`.

While a hotkey chord such as `SDL-0/Guide & SDL-0/DPadUp` is fully held,
controller and keyboard pad bindings made only of its keys stay released, so
the game does not see the chord's inputs (`pcsx2/Input/InputManager.cpp`).

Starting a game with a non-default PAL frame rate shows no warning.

While the save state selector is shown, Esc closes it, and neither that press
nor its release reaches any hotkey or pad binding
(`pcsx2/ImGui/ImGuiManager.cpp`).

- [Launch options](launch_options.md): command-line options for windows,
  speed, settings, memory cards, PINE, and agent replay.
- [Input recording capture](input_recording_capture.md): recording paths,
  read-only and concurrent playback, and L3+R3 marker capture.
- [Content folders and aliases](content_folders.md): additional content
  folders, game settings lookup, content alias bundles, and canonical local
  identity.
- [PNACH files](pnach.md): PNACH discovery, section markers, CRC lists, and
  command-line PNACH files and lines.
- [PINE agent control](pine_agent_control.md): the fork's PINE opcodes for VM
  control, agent input, and replay analysis.
