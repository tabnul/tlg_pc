// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the in-game menu's Fun tab (tlg_overlay.cpp): jump to a start point and extra Tricos.
// The commands are Lua, run once by the settings tick (tlg_lua.cpp, within 15 frames).
#pragma once

namespace TlgFun {
// Extra Tricos at most: the physics engine (Havok) runs out of constraint room at 10 Tricos
// in total on the terrace (s04 _090_GardenTerrace_001, 2026-11-04); 9 in total stayed stable.
inline constexpr int MaxTricos = 8;

void Jump(const char* stage, const char* area);
void AddTrico();
void AddEnemyTrico(); // the armoured one
void RemoveTricos(); // all extra Tricos, enemy ones too
// Trico actions (the Fun tab's list): the game's control commands, ControlCommand_C_<command>,
// asked of the story's Trico as the game's AI does (Ai2.Utils.ReqCtrlCmd). All tested live 2026-11-04
// (terrace s04 _090_GardenTerrace_001 and the area after it). His own AI often wins, so Act first gets him
// into the pose the move needs and tries up to 3 times, until his animation shows the move (key).
// "May not work": never seen in those tests (some small moves may play too subtly to see), or not tested.
struct Action {
    const char* command; // nullptr: a group heading
    const char* name;
    const char* pose = nullptr; // nullptr: sent once (a small move, no animation to check); "": as he is;
                                // "stand", "sit", "lie": that pose first
    const char* key = nullptr;  // part of his animation name that shows the move; nullptr: the command
};
inline constexpr Action Actions[] = {
    {nullptr, "Sounds and moods"},
    {"A_sneeze", "Sneeze"},
    {"A_yawn", "Yawn"},
    {"roar", "Roar", "stand"},
    {"howling", "Howl", "stand"},
    {"howling_calm", "Howl softly", "stand", "howling"},
    {nullptr, "Body"},
    {"flap_wings", "Flap wings", "stand"},
    {"A_wing_stretchA", "Stretch wings"},
    {"A_wing_stretchB", "Stretch wings 2"},
    {"stretch_front", "Stretch forward", "stand"},
    {"stretch_back", "Stretch back", "stand"},
    {"stretch_up", "Stretch up", "stand"},
    {"stand_shake_high", "Shake off", "stand", "shake_high"},
    {"scratch_left", "Scratch left", "stand"},
    {"scratch_right", "Scratch right", "stand"},
    {"kneading", "Knead", "stand", "knead"},
    {"P_eye_wink_left", "Wink"},
    {"P_ear_shake_left", "Shake ear"},
    {nullptr, "Play"},
    {"roll_on_ground", "Roll on the ground (needs room)", "stand"},
    {"sit_plays", "Sit and play", "sit", "sit_play"},
    {"toilet", "Toilet", "stand"},
    {nullptr, "Tummy"},
    {"tlg_gag", "Gag"},              // our Lua, not a control command (tlg_lua.cpp Act)
    {"tlg_throw_up", "Throw up"},    // the developers' cut throw-up, brought back
    {nullptr, "Rest"},
    {"sit", "Sit", "stand", "sit_loop"},
    {"sit_shake", "Sit and shake", "sit"},
    {"sit_end", "Stand up from sitting", ""},
    {"lie_down", "Lie down", "sit"},
    {"lie_down_relax", "Lie down relaxed", "stand"},
    {"rise", "Get up", "stand"},
    {"stand", "Stand", ""},
    {nullptr, "May not work"},
    {"A_groanA", "Groan"},
    {"A_excite", "Get excited"},
    {"excite_shake_head", "Excited head shake", "stand", "shake_head"},
    {"A_face_rejoice", "Happy face"},
    {"A_inclines_neck", "Tilt head"},
    {"A_hungry", "Hungry"},
    {"A_scolded", "Scolded"},
    {"A_refuse", "Refuse"},
    {"P_fur_bristle", "Fluff fur"},
    {"threat_posture", "Threaten", "stand", "threat"},
    {"threat_posture_end", "Stop threatening", "stand", "threat"},
    {"stand_shake_drainsWater", "Shake off water", "stand", "drain"},
    {"grooming_left", "Groom", "stand", "grooming"},
    {"grooming_wing_left", "Groom wing", "stand", "wing"},
    {"joy_jump", "Happy jump (needs room)", "stand"},
    {"mud_playing", "Play in the mud", "stand"},
    {"cat_toy", "Play like a cat", "stand", "cat"},
    {"roll_in_nose", "Sniff and roll", "stand", "nose"},
    {"sit_plays_pecks_nose", "Sit and peck", "sit", "peck"},
    {"puts_sand", "Dig sand", "stand", "sand"},
    {"liedown_to_sleep", "Fall asleep", "lie", "sleep"},
    {"sleep", "Sleep", "lie"},
    {"sleep_to_liedown", "Wake up", "lie", "sleep_to"},
    {"smell", "Sniff", "stand"},
    {"smellA", "Sniff around", "stand", "smell"},
    {"liedown_smell", "Sniff lying down", "lie", "smell"},
    {"touch_boy_right", "Touch the boy", "stand", "touch"},
    {"TC_reactionA_to_boy", "React to the boy", "stand", "reaction"},
    {"A_smell", "Sniff", "stand", "smell"},              // first half of the sealed Play act (Mods > Trico plays)
    {"touch_left_side_soft", "Paw", "stand", "touch"},   // second half (Common_touch_left)
};
void Act(const char* command);

// Trico's drool (setting trico_drool, tlg_lua.cpp Drool): the developers built it, then switched it off
// before release. The Fun tab and both launchers show this text.
inline constexpr const char* DroolHelp =
    "A feature the developers built and then switched off just before release: Trico drools. "
    "Always: a string of drool hangs from his beak the whole time. As the developers planned: it slowly "
    "grows while Trico smells something he wants, a smell-pot cart (s04_FirstAntenna and s10_Pit), and "
    "fades away when he loses interest (the boy's smell bottle was cut, so the carts are the only trigger). "
    "Stays on through jumps, new stages and extra Tricos, and is kept for the next start.";
void CountTricos();
int Tricos(); // the extra Tricos the game reported last; -1 before the first report
// Trico's disco eyes: the game's own random eye colours (EyeColorRandom, normally only in a few
// story spots), driven by our own tick. A jump turns them off.
void Disco(bool on);
bool DiscoOn();
// Trico's eye colour: every mood's colour (Default, Excite, Pulse, ...) gets this red/green/blue,
// each keeps its own brightness. EyeNormal puts the game's own colours back. A jump ends it.
void EyeColour(int r, int g, int b);
void EyeNormal();
// Trico's size: the story Trico's scale, 1 = normal. A jump or new stage brings him back to normal.
inline constexpr float SizeMin = 0.5f, SizeMax = 2.0f;
void Size(float scale);
float SizeNow();
void FreeCamera(bool on);
bool FreeCameraOn();
// Tricopilot: the developers' self-driving Trico, left in the retail game
// (game/scripts/ai2/condor/condordebugautotrico.lua). Trico walks the scene by himself and the
// camera follows him. The launcher's benchmark can keep it on for a whole run (TLG_TRICOPILOT=1).
void Tricopilot(bool on);
bool TricopilotOn();
// Which camera watches him. The first three are the developers' own presets for this tool
// (TricoAutoRunTest_near / _middle / _far); Game is the normal camera, which keeps following the
// boy. Middle is what autoTrico() picks itself, Near frames Trico best in open scenes. Changing it
// while he drives takes effect at once. The launcher passes it as TLG_TRICOPILOT_CAM=near|middle|
// far|game.
enum class PilotCam { Near = 0, Middle = 1, Far = 2, Game = 3 };
void TricopilotCamera(PilotCam cam);
PilotCam TricopilotCamera();
inline constexpr const char* PilotCamNames[] = {"Near", "Middle", "Far", "Game camera"};
inline constexpr const char* PilotCamKeys[] = {"near", "middle", "far", "game"};
inline constexpr const char* TricopilotCamHelp =
    "Near keeps Trico big in the picture and is the best of the four in open scenes. Middle is the "
    "one the developers' tool picks itself: a classic top-down view, but in a narrow or roofed "
    "place the camera can end up inside the rock. Far looks down on the whole area, which is lovely "
    "in a valley and useless indoors, because Trico becomes a dot. Game camera leaves your normal "
    "camera alone: it keeps following the boy while Trico wanders off on his own.";
// The Fun tab, both launchers and the docs show this text.
inline constexpr const char* TricopilotHelp =
    "Trico drives himself: he wanders the scene, looks for edges to climb or jump, and the camera "
    "follows him. It is the developers' own debug tool, still in the game. They used it to leave the "
    "game running by itself for hours: their script can even walk him on to the next scene and knows "
    "every stage's start points, their own version of a soak test. Three of its pieces say how old it "
    "is: a line to restart the game over a network socket from a PC called D:\\trico, a camera preset "
    "named TricoAutoRunTest_middle, and helper volumes that are not even on the disc any more (the "
    "game quietly skips those, which is why he sometimes picks an odd route). Turning it off gives "
    "you the camera and Trico back. For fun only: he ignores the story, he can wander into water or "
    "into a load trigger, and the story keeps running around him.";
bool EyeCustom();
} // namespace TlgFun
