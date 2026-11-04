// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the game's Lua loadbuffer (HavokScript) is redirected here (src/probe.c,
// install_lua_hook). utils.lua gets a tick that loads "--TLGSET" every 15 frames; that chunk
// is replaced by code applying the Effects settings to the game's post-process objects, so
// menu changes apply live, plus the Lab rows (tlg_lab.h). "--TLGLOG <text>" chunks print <text>
// (data out of Lua); "--TLGVAL <row> <value>" chunks report a Lab row's game value;
// "--TLGLOADED" marks the end of a stage load.
// The settings code compiles into a function the tick keeps and runs every time; it is sent
// again only when its text changes, so a tick compiles nothing and creates few strings.
#include "tlg_lab.h"
#include "tlg_settings.h"
#include "tlg_toggles.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {
using LoadFn = int(__attribute__((sysv_abi))*)(void*, void*, const char*, uint64_t,
                                                const char*);
LoadFn original;

constexpr std::string_view Bootstrap = R"(
do local n,ls,run=0,loadstring or load RegisterTick(function() n=n+1 if n>=15 then n=0
local f,e=ls(run and '--TLGSET' or '--TLGSET new') local ok=true
if f then ok,e=pcall(f) if ok and type(e)=='function' then run=e end end
if f and ok and run then ok,e=pcall(run) end
if (not f or not ok) and e~=_TLGERR then _TLGERR=e local g=ls('--TLGLOG error: '..tostring(e)) if g then g() end end
end end) end
)";

// Shared by every TLGSET chunk: Scale keeps the game's own value per object property and
// writes it times pct/100, or plus add when given (vectors: the listed components). A value differing from our last
// write was set by the game (cutscene, area change): it becomes the new base. Feature turns a
// pass off at 0% (it costs nothing then) and back on when the game had it.
constexpr std::string_view Prelude = R"(
local ls=loadstring or load
local function L(s) local f=ls('--TLGLOG '..s) if f then f() end end
_TLGB=_TLGB or {}
local B=_TLGB
local function Parts(v) local t={} for n in tostring(v):gmatch('%S+') do t[#t+1]=tonumber(n) or 0 end return t end
local function Scale(o,key,prop,pct,parts,add)
  if not o then return end
  local cur=o[prop]
  local s=tostring(cur)
  local e=B[key]
  if not e or s~=e.w then e={b=parts and Parts(cur) or cur} B[key]=e end
  if parts then
    if type(cur)~='userdata' then return end
    local t={} for i=1,4 do t[i]=e.b[i] or 0 end
    for _,i in ipairs(parts) do t[i]=add and t[i]+add or t[i]*pct/100 end
    local n=Vector(t[1],t[2],t[3],t[4])
    if tostring(n)~=s then o[prop]=n end
  else
    if type(cur)~='number' then return end
    local w=add and e.b+add or e.b*pct/100
    if w~=cur then o[prop]=w end
  end
  e.w=tostring(o[prop])
end
B.off=B.off or {}
local function Feature(o,name,pct)
  if not o or type(o.features)~='string' then return end
  local on=o.features:find(name,1,true)~=nil
  if pct==0 and on then o.features='~'..name B.off[name]=true
  elseif pct>0 and B.off[name] then B.off[name]=nil if not on then o.features='|'..name end end
end
local X,XY,XZ,ALL,XYZ={1},{1,2},{1,3},{1,2,3,4},{1,2,3}
local function Loading() return RegionController.IsLoading() end
-- Names of the objects of one class whose name contains match; the class of each name is looked
-- up once. EnumerateObjectList(match) filters in the engine (a substring test on the name): the
-- full list is ~200k names, and pushing those into Lua after every load stalled the game 0.7-0.9 s.
local function Each(key,class,match)
  local l=B[key] or {c={}}
  B[key]=l
  if not l.n or l.gen~=B.gen then
    l.n={} l.gen=B.gen
    local all=EnumerateObjectList(match) or {}
    for _,n in ipairs(all) do
      local c=l.c[n]
      if c==nil then
        local o=FindObject(n)
        if o then c=tostring(o._class_name)==class l.c[n]=c end
      end
      if c then l.n[#l.n+1]=n end
    end
  end
  return l.n
end
-- The wind settings write once per area load and when the user changes them, not every run (a
-- value the game resets in between returns at the next load).
local function Due(key,v)
  local e=B[key]
  if type(e)=='table' and e.g==B.gen and e.v==v then return false end
  B[key]={g=B.gen,v=v} return true
end
-- Trico's feathers and hair sway by the wind Influence of his SwingTypes (one per model part).
-- At the default nothing runs once restored.
local INF={'InfluencePlusMin','InfluencePlusMax','InfluenceMinusMin','InfluenceMinusMax'}
local function Feathers(pct)
  if pct==100 and not B.swd or not Due('swa',pct) then return end
  for _,n in ipairs(Each('swl','SwingType','chara/skin/condor')) do
    local o=FindObject(n)
    for _,k in ipairs(INF) do Scale(o,'sw|'..n..'|'..k,k,pct,XYZ) end
  end
  B.swd=pct~=100
end
-- Field of view: every camera mode has its own GameCameraParam (Base_FovDeg 70 as shipped).
local function Fov(deg)
  if deg==70 and not B.fovd then return end
  for _,n in ipairs(Each('fovl','GameCameraParam','gamecamera_param')) do
    Scale(FindObject(n),'fov|'..n,'Base_FovDeg',deg*100/70)
  end
  B.fovd=deg~=70
end
-- Grass: the instanced grass ships with skipFrame/slowFrame 4 (animates every 4th frame); on
-- sets 1 on every GrassType, off restores the game's values.
local GRF={'skipFrame','slowFrame'}
local function Grass(on)
  if not on and not B.grd or not Due('gra',on) then return end
  for _,n in ipairs(Each('grl','GrassType','grass')) do
    local o=FindObject(n)
    for _,k in ipairs(GRF) do
      local key='gr|'..n..'|'..k
      if o and type(o[k])=='number' then
        if B[key]==nil then B[key]=o[k] end
        local w=on and 1 or B[key]
        if o[k]~=w then o[k]=w end
      end
    end
  end
  B.grd=on
end
-- Wind: the boy's receiver (WindRecvExt2311_SNt001*, hair and clothing) caps the scene wind by
-- MaxStrength; tree and leaf motion is VertexClusterParam.windIntensity (the boy's hair param
-- SNt006 excluded). Percent of the game's value.
local function BoyWind(pct)
  if pct==100 and not B.bwd or not Due('bwa',pct) then return end
  for _,n in ipairs(Each('bwl','WindRecvExt','2311_SNt001')) do
    Scale(FindObject(n),'bw|'..n,'MaxStrength',pct)
  end
  B.bwd=pct~=100
end
local function TreeWind(pct)
  if pct==100 and not B.twd or not Due('twa',pct) then return end
  for _,n in ipairs(Each('twl','VertexClusterParam','VertexClusterParam')) do
    if not n:find('SNt006',1,true) then Scale(FindObject(n),'tw|'..n,'windIntensity',pct) end
  end
  B.twd=pct~=100
end
)";

// The start of the function the tick runs: objects are looked up again every run.
constexpr std::string_view PerRun = R"(
return function()
B.run=(B.run or 0)+1
local pp=FindObject('_default_env_pp')
local cam=FindObject('_trico_camera')
local cs=FindObject('_default_env_cs')
-- Area loads add objects, so the lists above are made again when the game finishes a load
-- (RegionController.IsLoading goes back to false). Listing every object stalls the game, which
-- re-listing every 20 ticks did every few seconds with any wind setting off its default. Without
-- the loading state the lists are made again every 240 ticks.
local okl,ld=pcall(Loading)
B.gen=B.gen or 0
if not okl then if B.run%240==0 then B.gen=B.gen+1 end
elseif ld then B.ld=true elseif B.ld then B.ld=nil B.gen=B.gen+1 end
)";

// Development: the file named by TLG_LUA_EXEC runs once each time it changes (live console).
std::string DevChunk() {
    static const char* path = std::getenv("TLG_LUA_EXEC");
    static std::filesystem::file_time_type seen{};
    std::error_code error;
    if (!path) return {};
    const auto time = std::filesystem::last_write_time(path, error);
    if (error || time == seen) return {};
    seen = time;
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    std::printf("Lua: running %s\n", path);
    return "\ndo\nlocal pp,cam,cs=FindObject('_default_env_pp'),FindObject('_trico_camera'),"
           "FindObject('_default_env_cs')\n" +
           text.str() + "\nend\n";
}

// Wider than 16:9 the resolution patch widens the UI canvas to 1080*W/H (tlg_patches.py); the
// subtitle layout still centers on 1920, so shift it right by half the extra width.
int SubtitleShift() {
    const char* resolution = std::getenv("TLG_RESOLUTION_PATCH");
    int width = 0, height = 0;
    if (!resolution || std::sscanf(resolution, "%dx%d", &width, &height) != 2 || height <= 0 ||
        width * 9 <= height * 16)
        return 0;
    const int ui_width = static_cast<int>(1080.0 * width / height + 0.5);
    return (ui_width - 1920) / 2;
}

std::string SetChunk() {
    static const int subtitle_shift = SubtitleShift();
    const auto& s = TlgSettings::Get();
    std::string code(Prelude);
    code += TlgLab::Lua(s.wind_skip.load());
    code += DevChunk();
    code += PerRun;
    char line[1024];
    std::snprintf(line, sizeof(line),
                  "Scale(pp,'bloom','bloomAmount',%d) Feature(pp,'Bloom',%d)\n"
                  "Scale(pp,'ao','saoIntensity',%d,XY) Feature(pp,'SSAO',%d)\n"
                  "Scale(pp,'ssr','ssrStrength',%d) Feature(pp,'SSR',%d)\n"
                  "Scale(pp,'blur','cameraBlurScale',%d) Feature(pp,'CameraBlur',%d)\n"
                  "Scale(pp,'vig','vignetteParams',100,XZ,%.4f)\n"
                  "Scale(pp,'sharp','sharpenScale',%d)\n"
                  "Scale(pp,'fogd','fogParam',%d,XY)\n"
                  "Scale(pp,'lvca','lightVolumeChromaticAberration',%d,X)\n"
                  "Feathers(%d) Fov(%d) Grass(%s) BoyWind(%d) TreeWind(%d)\n",
                  s.bloom.load(), s.bloom.load(), s.ssao.load(), s.ssao.load(), s.ssr.load(),
                  s.ssr.load(), s.motion_blur.load(), s.motion_blur.load(),
                  s.vignette.load() * 0.01, std::min(s.game_sharpen.load(), 100),
                  s.fog_distance.load(), s.haze_fringe.load(), s.trico_feathers.load(), s.fov.load(),
                  s.grass_smooth.load() ? "true" : "false", s.boy_wind.load(), s.tree_wind.load());
    code += line;
    if (subtitle_shift) {
        // Re-applied every tick: SubtitleSystem:Initialize resets it to (0, -OFFSET_Y).
        std::snprintf(line, sizeof(line),
                      "if Subtitle_SetParam then Subtitle_SetParam(%d,-(SubtitleSystem and "
                      "SubtitleSystem.OFFSET_Y or 64)) end\n",
                      subtitle_shift);
        code += line;
    }
    code += "LabApply()\nend\n";
    return code;
}

bool StartsWith(const char* buffer, uint64_t size, std::string_view prefix) {
    return size >= prefix.size() && std::memcmp(buffer, prefix.data(), prefix.size()) == 0;
}

// Development: every chunk the game loads is written to the folder named by TLG_LUA_DUMP.
void Dump(const char* buffer, uint64_t size, const char* name) {
    static const char* dir = std::getenv("TLG_LUA_DUMP");
    static int count = 0;
    if (!dir) return;
    std::string file = name ? name : "chunk";
    for (char& c : file)
        if (c == '/' || c == '\\' || c == ':' || c == '@' || c == '=') c = '_';
    std::filesystem::create_directories(dir);
    std::ofstream(std::string(dir) + "/" + std::to_string(count++) + "_" + file, std::ios::binary)
        .write(buffer, static_cast<std::streamsize>(size));
}

// Debug menus (setting level_select): the GoldHEN "Level Select + QA Menu" patch by illusionyy
// and DanielSvoboda, applied to the source. Startup gets the QA menu (in game: hold L1, press
// L3); the title menu's press-build level select is enabled (hold L1 as the menu closes).
// The game is paused while the QA menu is open. The menu also closes itself when a stage is
// picked, so its Hide resumes: paused, the new stage's fade-in (a game-time delay) never came.
constexpr std::string_view QaMenu = R"(
if not _TLGQA then _TLGQA=true
local function Registered(f) for _,v in ipairs(tick_funcs) do if v==f then return true end end return false end
RegisterTick(function()
  if MS.PadGetHold(PadButton.L1) and MS.PadGetDown(PadButton.L3) then
    if Registered(ControllerMenu.Tick) then
      if ControllerMenu.Hide then ControllerMenu.Hide() end
    else _TLGQA_UNLOAD=true DelayExec(function() RegisterTick(ControllerMenu.Tick) end) luaCommand("trico pause") end
  end
end)
Load("game/scripts/_editor/ControllerMenu")
local hide = ControllerMenu.Hide
ControllerMenu.Hide = function() hide() luaCommand("trico resume") end
end
)";

/// Edits game chunk `name` for the debug menus and the bridge fix; true when it changed.
bool GameEdits(std::string& patched, std::string_view name) {
    const auto& s = TlgSettings::Get();
    if (s.level_select && name == "data_trico/game/scripts/startup.lua") {
        patched += QaMenu;
        return true;
    }
    if (name == "data_trico/game/scripts/common/GameSetupTbl.lua") {
        // Setting texture_cleanup (always marked, the setting is live): both stage loads, a stage
        // start and a restart (continue, checkpoint), end by disconnecting the time blocker.
        // "--TLGLOADED" tells the texture cache (TextureCache::CleanUpAfterLoad).
        static constexpr std::string_view LoadEnd = "Disconnect( GameSetupTbl.timeBlocker )";
        static constexpr std::string_view Loaded =
            " do local f=(loadstring or load)('--TLGLOADED') if f then f() end end";
        bool changed = false;
        for (size_t at = patched.find(LoadEnd); at != std::string::npos;
             at = patched.find(LoadEnd, at + LoadEnd.size() + Loaded.size())) {
            patched.insert(at + LoadEnd.size(), Loaded);
            changed = true;
        }
        if (!s.level_select) {
            return changed;
        }
        // A stage started from the QA menu is loaded on top of the current one (StartStage
        // unloads nothing), so each switch held two stages and ran the GPU heap out of memory.
        // Do what the game's own restart does: disconnect the stage's regions, preload the new
        // ones, then unload what is still disconnected. The common regions stay.
        static constexpr std::string_view Blocker = "self.timeBlocker = CreateTimeBlocker()";
        static constexpr std::string_view Preloaded =
            "DelayExec( function() self:Start_Stage_2nd_PreloadWait(area) end )";
        const size_t blocker = patched.find(Blocker);
        const size_t preloaded = patched.find(Preloaded);
        if (blocker == std::string::npos || preloaded == std::string::npos || preloaded < blocker) {
            return changed;
        }
        patched.insert(preloaded, "if _TLGQA_UNLOAD then _TLGQA_UNLOAD = nil RegionController.UnloadAll_IfDisconnect() end ");
        patched.insert(blocker + Blocker.size(),
                       " if _TLGQA_UNLOAD then for _, v in ipairs(Class('Region')) do"
                       " if OBJ[v].status ~= 'None' and not string.find(v, 'RG_common') then"
                       " RegionController.Disconnect(OBJ[v].regionName) end end"
                       " self:Disconnect_CommonRegion() end");
        return true;
    }
    if (s.level_select && name == "data_trico/game/scripts/stages/s01_Opening/missionScript.lua") {
        // Only the test guarding the L1 level select, not the press build's other changes.
        static constexpr std::string_view Test = "if gameConfig_buildId_Press==true then";
        static constexpr std::string_view Hold = "if MS.PadGetHold(PadButton.L1) then";
        for (size_t at = patched.find(Test); at != std::string::npos;
             at = patched.find(Test, at + 1)) {
            const size_t next = patched.find_first_not_of(" \t\r\n", at + Test.size());
            if (next != std::string::npos && patched.compare(next, Hold.size(), Hold) == 0) {
                patched.replace(at, Test.size(), "if true then");
                return true;
            }
        }
        return false;
    }
    // Cutscene markers for the bench: the engine sends DemoRoot.NextState on every state change
    // of every cutscene ("demo"): its file, then the state (action_N ... finish, force_finish
    // when skipped). Log it as "Lua: demo <file> <state>". A cutscene can also end without its
    // finish (the flashback fades out before it and hands back to gameplay), so log each mission
    // scene the game enters too: "Lua: mission <stage> <scene>".
    static constexpr std::string_view DemoEvent =
        "function MS_DemoRoot_NextState(ext, act, id, sender, param)";
    static constexpr std::string_view SceneCall = "function MSCore_L_CallState(func, stage, state, et)";
    if (name == "data_trico/game/scripts/common/MissionScript.lua") {
        const size_t demo = patched.find(DemoEvent);
        const size_t scene = patched.find(SceneCall);
        if (demo == std::string::npos || scene == std::string::npos) {
            return false;
        }
        static constexpr std::string_view DemoLog =
            " local f=(loadstring or load)('--TLGLOG demo '..tostring(sender)..' '"
            "..tostring(param)) if f then f() end";
        static constexpr std::string_view SceneLog =
            " if func=='onEnter' then local f=(loadstring or load)('--TLGLOG mission '"
            "..tostring(stage)..' '..tostring(state)) if f then f() end end";
        // The later one first, so the earlier offset stays valid.
        if (demo > scene) patched.insert(demo + DemoEvent.size(), DemoLog);
        patched.insert(scene + SceneCall.size(), SceneLog);
        if (demo < scene) patched.insert(demo + DemoEvent.size(), DemoLog);
        return true;
    }
    // And each fade to black ("Lua: fade out <seconds>"): such a cutscene fades out before the
    // scene change, so the bench can stop measuring where the fade starts.
    static constexpr std::string_view FadeOut =
        "function FadeOut( time, wait, force, cb, force_exec_cb, _system, _useSystemDt )";
    if (name == "data_trico/game/scripts/stages/common/ASC_basic.lua") {
        const size_t at = patched.find(FadeOut);
        if (at == std::string::npos) {
            return false;
        }
        patched.insert(at + FadeOut.size(),
                       " local f=(loadstring or load)('--TLGLOG fade out '..tostring(time))"
                       " if f then f() end");
        return true;
    }
    // Slow motion at 30 FPS (s06 tail catch, s10 collapsing bridge): _TLGSGS logs "slowmo on"
    // (speed below 1) or "slowmo off" and then sets the speed; tlg_lua_load turns that into a
    // 30 FPS cap (the credits lock switch) until the speed is back or the stage is left.
    static constexpr std::string_view SlowMoWrapper =
        "local function _TLGSGS(v,t) local f=(loadstring or load)("
        "'--TLGLOG slowmo '..((tonumber(v) or 1)<1 and 'on' or 'off')) if f then f() end"
        " return MS.SetGameSpeed(v,t) end ";
    // Bridge fix: the GoldHEN "Collapsing Bridge Freeze Fix" (s10_Pit, cutscene pi_160_p04_A
    // action_1). The slow motion while the bridge collapses can leave the game frozen above
    // 30 FPS. Above 30 FPS it runs capped at 30 (start and end call only); at 30 it is skipped
    // as before.
    static constexpr std::string_view SlowMotion = "MS.SetGameSpeed(0.10, 0.2)";
    static constexpr std::string_view SlowMotionEnd = "MS.SetGameSpeed(1.0,1.0)";
    // Above 30 FPS, tlg.ini slow_motion (env TLG_SLOW_MOTION wins): 0 = cap at 30 (default),
    // 1 = skip the slow motion (the v0.5.0 behaviour).
    static const char* slow_env = std::getenv("TLG_SLOW_MOTION");
    const bool skip_slow = slow_env ? std::atoi(slow_env) == 1 : s.slow_motion.load() == 1;
    if (name == "data_trico/game/scripts/stages/s10_Pit/missionScript.lua") {
        const size_t at = patched.find(SlowMotion);
        if (at == std::string::npos) {
            return false;
        }
        if (s.applied_engine_fps.load() <= 30 || skip_slow) {
            patched.insert(at, "--");
            if (s.applied_engine_fps.load() > 30) {
                std::printf("Lua: bridge fix (slow motion skipped)\n");
            }
            return true;
        }
        const size_t end = patched.find(SlowMotionEnd, at);
        if (end == std::string::npos) {
            return false;
        }
        patched.replace(end, 15, "_TLGSGS(");
        patched.replace(at, 15, "_TLGSGS(");
        patched.insert(0, SlowMoWrapper);
        std::printf("Lua: bridge fix (30 FPS during slow motion)\n");
        return true;
    }
    // Tail catch fix: the same freeze on the falling bridge (s06_FallingBridge, mission
    // FB_e02_Demo_003, cutscenes fb_050 action_3 + fb_110 action_1, issues #39/#40). The game
    // rounds its slowed frame time to whole 1/360 s steps and parts of the scene step per frame,
    // so its slow motion (15%) only works at 30 FPS: above that it is choppy, the catch can be
    // missed and the game waits forever. Above 30 FPS every SetGameSpeed in this script goes
    // through _TLGSGS, which caps the frame rate at 30 while the speed is below 1 (the same
    // switch as the credits lock; the next mission scene also ends it).
    if (s.applied_engine_fps.load() > 30 &&
        name == "data_trico/game/scripts/stages/s06_FallingBridge/missionScript.lua") {
        if (skip_slow) {
            static constexpr std::string_view TailJump =
                "game/stages/s06_FallingBridge/demo/fb_110', 'action_1')";
            static constexpr std::string_view TailSlowMotion = "MS.SetGameSpeed(0.15, 0.25)";
            const size_t jump = patched.find(TailJump);
            const size_t at = jump == std::string::npos ? jump : patched.find(TailSlowMotion, jump);
            if (at == std::string::npos) {
                return false;
            }
            patched.insert(at, "--");
            std::printf("Lua: tail catch fix (slow motion skipped)\n");
            return true;
        }
        static constexpr std::string_view Call = "MS.SetGameSpeed(";
        if (patched.find(Call) == std::string::npos) {
            return false;
        }
        for (size_t at = patched.find(Call); at != std::string::npos; at = patched.find(Call, at)) {
            patched.replace(at, Call.size(), "_TLGSGS(");
        }
        patched.insert(0, SlowMoWrapper);
        std::printf("Lua: tail catch fix (30 FPS during slow motion)\n");
        return true;
    }
    return false;
}
} // namespace

extern "C" void tlg_lua_status(uintptr_t loadbuffer) {
    original = reinterpret_cast<LoadFn>(loadbuffer);
}

extern "C" __attribute__((sysv_abi)) int tlg_lua_load(void* L, void* options, const char* buffer,
                                                      uint64_t size, const char* name) {
    static constexpr std::string_view Empty = "--";
    if (StartsWith(buffer, size, "--TLGLOG ")) {
        const std::string_view text(buffer + 9, size - 9);
        std::printf("Lua: %.*s\n", static_cast<int>(text.size()), text.data());
        std::fflush(stdout);
        // tlg: "mission <stage> <scene>" starts a new scene, so diagnostics that report per
        // scene (TLG_WRITE_ATTRIB) can close their window here instead of on a wall clock. Only
        // the stage's own scenes: the game also enters helper scenes all the time ("_Worker ...",
        // "_tmp ...", "<stage> sub_...") and each of those would cut the report into fragments of
        // a tenth of a second.
        // Tail catch fix: 30 FPS while the falling bridge's slow motion runs (see PatchChunk).
        static bool slowmo = false;
        if (text == "slowmo on" || text == "slowmo off") {
            slowmo = text == "slowmo on";
            TlgSettings::Get().credits_fps_lock = slowmo ? 30u : 0u;
        }
        if (text.starts_with("mission ")) {
            const std::string_view scene = text.substr(8);
            const size_t space = scene.find(' ');
            const bool helper = scene.starts_with("_") || space == std::string_view::npos ||
                                scene.substr(space + 1).starts_with("sub_");
            if (!helper) {
                TlgStats::SetScene(scene);
                // The credits (s18_Ending *_Ending_StaffRoll*) hitch at every cut above 30 FPS,
                // the rate the PS4 game runs them at. The next real scene ends the lock.
                // TLG_CREDITS_FPS=30|60|unlocked (benchmarks) wins over tlg.ini credits_fps.
                static const char* credits_env = std::getenv("TLG_CREDITS_FPS");
                const std::uint32_t credits_fps =
                    !credits_env ? std::uint32_t(TlgSettings::Get().credits_fps.load())
                    : std::strcmp(credits_env, "unlocked") == 0 ? 0u
                    : std::strcmp(credits_env, "60") == 0      ? 60u
                                                                : 30u;
                const bool credits = scene.starts_with("s18_Ending ") &&
                                     scene.find("_Ending_StaffRoll") != std::string_view::npos;
                // The slow motion spans mission scenes; only leaving the stage ends it.
                if (!scene.starts_with("s06_FallingBridge ") && !scene.starts_with("s10_Pit ")) {
                    slowmo = false;
                }
                TlgSettings::Get().credits_fps_lock = credits ? credits_fps : slowmo ? 30u : 0u;
            }
        }
        return original(L, options, Empty.data(), Empty.size(), name);
    }
    if (StartsWith(buffer, size, "--TLGVAL ")) {
        TlgLab::Report(std::string_view(buffer + 9, size - 9));
        return original(L, options, Empty.data(), Empty.size(), name);
    }
    if (StartsWith(buffer, size, "--TLGLOADED")) {
        TlgSettings::Get().stage_loaded = true;
        return original(L, options, Empty.data(), Empty.size(), name);
    }
    if (StartsWith(buffer, size, "--TLGSET")) {
        // "--TLGSET new": the tick has no function yet (first tick, or a new Lua state).
        static std::string sent; // game thread only
        std::string code = SetChunk();
        if (code == sent && size == 8) return original(L, options, Empty.data(), Empty.size(), name);
        sent = std::move(code);
        return original(L, options, sent.data(), sent.size(), name);
    }
    Dump(buffer, size, name);
    static constexpr std::string_view Marker = "function RegisterTick(";
    std::string patched(buffer, size);
    bool changed = false;
    if (patched.find(Marker) != std::string::npos) {
        patched += Bootstrap;
        std::printf("Lua: settings tick added to %s\n", name ? name : "?");
        changed = true;
    }
    if (name && GameEdits(patched, name)) {
        std::printf("Lua: edited %s\n", name);
        changed = true;
    }
    std::fflush(stdout);
    return changed ? original(L, options, patched.data(), patched.size(), name)
                   : original(L, options, buffer, size, name);
}
