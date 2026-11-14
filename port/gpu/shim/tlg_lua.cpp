#include <thread>
// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the game's Lua loadbuffer (HavokScript) is redirected here (src/probe.c,
// install_lua_hook). utils.lua gets a tick that loads "--TLGSET" every 15 frames; that chunk
// is replaced by code applying the Effects settings to the game's post-process objects, so
// menu changes apply live, plus the Lab rows (tlg_lab.h). "--TLGLOG <text>" chunks print <text>
// (data out of Lua; "fun tricos <n>" reports the Fun tab's Trico count); "--TLGVAL <row> <value>" chunks report a Lab row's game value;
// "--TLGLOADED" marks the end of a stage load.
// The settings code compiles into a function the tick keeps and runs every time; it is sent
// again only when its text changes, so a tick compiles nothing and creates few strings.
#include "tlg_fun.h"
#include "tlg_fun_points.h"
#include "tlg_lab.h"
#include "tlg_settings.h"
#include "tlg_toggles.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

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
-- Trico's drool (tlg.ini trico_drool): the game's own drool code (Ai2.Condor.Slaver,
-- condorcommonslaver.lua) stops before it shows the drool ("we were asked to remove the drool for
-- now"). While the setting is on, its step is ours: 1 = always thick, 2 = the developers' plan
-- (thickens while Trico wants a smell pot or the boy's smell bottle, thins and hides after).
-- A stage load brings the game's step back; the next run replaces it again. 0 = the game's own.
local function Drool(m)
  local S=Ai2 and Ai2.Condor and Ai2.Condor.Slaver
  if not S or type(S.ControlAlphaThreshold)~='function' then return end
  local D=_TLGDR or {} _TLGDR=D
  local was=D.m D.m=m
  if m==0 and not D.f then return end
  if S.ControlAlphaThreshold~=D.f then
    D.orig=S.ControlAlphaThreshold
    D.f=function(self,props)
      local mode=_TLGDR.m
      if mode==0 then return _TLGDR.orig(self,props) end
      local p=props.currentInfo.work.SlaverParam
      if not p.smCondorSlaver or not p.sknMeshSlaver then self:OnInitWork(props) end
      local mat,mesh=p.smCondorSlaver,p.sknMeshSlaver
      if not mat or not mesh then return end
      local a=self.fConstAlphaThresholdMin
      if mode==2 then
        a=mat.alphaThreshold+(self:IsIntrTargetSlavableObj(props) and -1 or 1)*props.currentInfo.dt*self.fConstChangeSpeed
        a=Ai2.Utils.Clamp(a,self.fConstAlphaThresholdMin,self.fConstAlphaThresholdMax)
      end
      if a~=mat.alphaThreshold then mat.alphaThreshold=a end
      local hide=a>1
      if (tostring(mesh.renderableFlags):find('Hidden',1,true)~=nil)~=hide then
        mesh.renderableFlags=hide and '|Hidden' or '~Hidden'
      end
    end
    S.ControlAlphaThreshold=D.f
  end
  -- The extra Tricos share the drool material: theirs shows whenever the story Trico's does.
  local mat=FindObject('game/materials/SM_condor_slaver_01')
  local hide=m==0 or not mat or mat.alphaThreshold>1
  if m==0 and was and was~=0 then
    if mat then mat.alphaThreshold=S.fConstAlphaThresholdMax or 1.02 end
    local o=MS.SearchObject('F:Condor|A:cdrA_slaver')
    local mesh=o and FindExtensionByType(o,'SkinMesh')
    if mesh then mesh.renderableFlags='|Hidden' end
  end
  if m==0 and (not was or was==0) then return end
  for i=0,7 do
    local name=string.format('Condor%02d',i)
    if FindObject(name) then
      local o=MS.SearchObject('F:'..name..'|A:cdrA_slaver')
      local mesh=o and FindExtensionByType(o,'SkinMesh')
      if mesh and (tostring(mesh.renderableFlags):find('Hidden',1,true)~=nil)~=hide then
        mesh.renderableFlags=hide and '|Hidden' or '~Hidden'
      end
    end
  end
end
-- Mods > Trico plays (tlg.ini mod_plays, default on): the game's own "Play" act in his boy-interest
-- table (intactprobabilitycondor_player.lua Player_Play: sniff the boy, then paw him left, right or
-- with the claws) has ValidCnd "return false -- sealed for now". On: it may run under the same check
-- as his cheek rubs (ActHierarchy_CheekActCheck: boy clear of ledges, not on a ladder, within 4 m
-- height). Re-applied every run (a stage load rebuilds
-- the table); off puts the game's own condition back.
local function Plays(on)
  local P=Ai2 and Ai2.IntActProbability and Ai2.IntActProbability.Condor
  P=P and P.Player_Play
  if not P or type(P.tbl)~='table' or type(ActHierarchy_CheekActCheck)~='function' then return end
  local D=_TLGPL or {} _TLGPL=D
  for _,e in ipairs(P.tbl) do
    if e.name=='Play' then
      if e.ValidCnd~=ActHierarchy_CheekActCheck then D.orig=e.ValidCnd end
      local want=on and ActHierarchy_CheekActCheck or D.orig
      if want and e.ValidCnd~=want then e.ValidCnd=want end
    end
  end
end
-- Mods > Trico throws up (tlg.ini mod_throw_up): AddSpitOutIntr (condorcommonactioneatable.lua),
-- called when his stomach is full of barrels, only skips the act ("sealed by order 20160704,
-- briefly back, sealed again 20160726"). 1 = run the developers' commented-out block instead (the
-- same one the Throw up trick runs); the act's own marker events then empty his stomach.
-- 0 = the game's own (skip). Any error falls back to the game's own.
local function ThrowUp(m)
  local C=Ai2 and Ai2.CommonScript and Ai2.CommonScript.Condor
  if not C or type(C.AddSpitOutIntr)~='function' then return end
  local D=_TLGTU or {} _TLGTU=D D.m=m
  if m==0 and not D.f then return end
  if C.AddSpitOutIntr~=D.f then
    D.orig=C.AddSpitOutIntr
    D.f=function(props)
      if _TLGTU.m==0 then return _TLGTU.orig(props) end
      local ok,e=pcall(function()
        local work=props.currentInfo.work
        if not work.LoadSpitOutIntr then work.LoadSpitOutIntr=Load('game/ai/condor_interest/SpitOut_intr') end
        local intr=work.LoadSpitOutIntr Connect('_world',intr)
        SetWorldPosition(intr,tostring(FindObject('game/ai/condor_interest/Idling_locater').worldPosition))
        Ai2.CommonScript.Condor.AddIntrList(props,intr._object_name)
        Ai2.CommonScript.Condor.ControlGazeLevel(props,intr._object_name,'SubSpitOutIntr','subMax')
        L('mod throw up: natural')
      end)
      if not ok then L('mod throw up error: '..tostring(e)) return _TLGTU.orig(props) end
    end
    C.AddSpitOutIntr=D.f
  end
end
-- Mods > All treasures (tlg.ini mod_treasures): the developers' Treasure.DbgFullOpen() owns every
-- treasure; the Treasures entry of the options menu only shows after a finished run (lap 2). Both
-- are written to the save (SavePerpetual), so they stay after the setting is turned off.
local function Treasures(on)
  if not on or _TLGTRS or type(Treasure)~='table' or type(Treasure.DbgFullOpen)~='function' then return end
  local sd=FindObject('_savedata') local pd=sd and sd.perpetualData
  if not pd then return end
  _TLGTRS=true
  Treasure.DbgFullOpen()
  if (pd.lap or 1)<2 then pd.lap=2 end
  if (pd.treasurePoint or 0)<999 then pd.treasurePoint=999 end
  if SavePerpetual then SavePerpetual() end
end
-- Mods > ICO horns (tlg.ini mod_horns 1): the boy wears horns on his head bone, a nod to Ico. Own
-- models (bone white, black tip) from the horn archive the launcher builds; without that archive
-- the load fails and nothing shows (tried again after the next stage load). A stage load removes
-- them and the next run puts them back.
local ICOHORN='game/assets/props/level/icohorn_%sx/icohorn_%sx'
local function Has(n) local ok,v=pcall(function() return Exists(FindObject(n)) end) return ok and v end
local function Horns(on)
  local H=_TLGHORN
  if H and (not on or not Has(H[1]) or not Has(H[2])) then
    for _,n in ipairs(H) do
      if Has(n) then local o=FindObject(n) pcall(Disconnect,o) o.entityFlags='|Hide' end
    end
    H=nil _TLGHORN=nil
  end
  if not Has('Player') then _TLGHORNX=nil return end
  if not on or H or _TLGHORNX then return end
  local ok,head=pcall(GameCharacter.FindBone,'Player','jt_Head')
  if not ok or not head then return end
  H={}
  for _,side in ipairs({'r','l'}) do
    local got,o=pcall(Load,ICOHORN:format(side,side))
    local c=got and o and (FindObject(o) or o)
    if not c then _TLGHORNX=true return end
    Connect(head,c)
    c.entityFlags='~Hide'
    c.rotation='90 90 0'
    c.scale='1 1 1 1'
    c.position='0.09 -0.03 0 1'
    H[#H+1]=c:GetName()
  end
  _TLGHORN=H
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

// Fun tab commands (tlg_fun.h): queued by the menu, run once by the next tick, at the top of the
// settings chunk like DevChunk (the chunk differs, so it is sent and run; the next one is plain).
std::mutex fun_mutex;
std::string fun_queue;
std::atomic<int> fun_tricos{-1};
std::atomic<bool> disco{false}; // Trico's disco eyes (TlgFun::Disco)
std::atomic<bool> eye_custom{false}; // Trico's own eye colour (TlgFun::EyeColour)
std::string size_pending; // the latest Trico size, under fun_mutex: a slider drag sends only the last
std::atomic<float> trico_size{1.0f};
std::string cam_pending; // the latest free camera wish, under fun_mutex
std::atomic<bool> free_cam{false};
std::atomic<bool> pilot{false}; // Tricopilot (TlgFun::Tricopilot)
// Which camera watches him (TlgFun::PilotCam). Middle is what autoTrico() picks itself, so that
// stays our default; the launcher and the Fun tab can pick another one, also while he drives.
std::atomic<TlgFun::PilotCam> pilot_cam{[] {
    const char* env = std::getenv("TLG_TRICOPILOT_CAM");
    if (env && *env) {
        for (int i = 0; i < 4; ++i)
            if (std::string_view(env) == TlgFun::PilotCamKeys[i]) return TlgFun::PilotCam(i);
    }
    return TlgFun::PilotCam::Middle;
}()};
// Tricopilot for a whole run (TLG_TRICOPILOT=1, the launcher's benchmark): switched on as soon as a
// mission scene is entered, so it starts by itself after every load.
bool PilotWanted() {
    static const bool want = [] {
        const char* env = std::getenv("TLG_TRICOPILOT");
        return env && *env && std::string_view(env) != "0";
    }();
    return want;
}
std::string eye_pending; // the latest eye colour change, under fun_mutex: a picker drag sends only the last

void FunQueue(const std::string& code) {
    std::lock_guard lock(fun_mutex);
    fun_queue += "\ndo local ok,e=pcall(function()\n" + code + "\nend) if not ok then L('fun error: '..tostring(e)) end end\n";
}

// Grip log (TLG_GRIP_LOG=1, AMD fall-off test 2026-11-09): per tick the boy's physics state, what he
// rides or holds, his and Trico's position and the game's frame time. Quiet while nothing changes;
// at every state change it prints the last 2 s of ticks and the next 2 s, so a fall shows what
// Trico's body did under the boy right before it.
bool GripWanted() {
    static const bool want = [] {
        const char* env = std::getenv("TLG_GRIP_LOG");
        return env && *env && std::string_view(env) != "0";
    }();
    return want;
}
constexpr std::string_view GripLua = R"(
if _TLG_GRIP then pcall(UnRegisterTick,_TLG_GRIP) _TLG_GRIP=nil end
local ring,ri,post,last,tick,errs,lastsum={},0,0,nil,0,0,0
local lastanim,lastctx,pbl,pbs
local pb,pc,pp
local function V(o) if not o then return nil end local ok,p=pcall(function() return o.worldPosition or o.position end)
 if not ok or p==nil then return nil end local t={} for n in tostring(p):gmatch('[-%d%.e]+') do t[#t+1]=tonumber(n) end
 if #t<3 then return nil end return t end
local function F(x) return x and string.format('%.3f',x) or '-' end
local function Q(f,...) local ok,v=pcall(f,...) if ok then return v end return nil end
_TLG_GRIP=function()
 local ok,e=pcall(function()
  tick=tick+1
  local dt=Q(MSC_GetGameDelta)
  local st=tostring(Q(MS.BoyGetDynamicsState))
  local ride=Q(MS.BoyIsRidingOn,'Condor') local body=Q(MS.BoyIsRidingOn_OnlyBody,'Condor')
  local boy=Q(FindObject,'Player') local tr=Q(FindObject,'Condor') local sen=Q(FindObject,'Player_sensor-ext')
  local part,typ='-','-'
  local pe=sen and Q(function() return sen.TrackingPoint_Entity end)
  if sen then part=tostring(Q(function() return sen.TrackingPoint_CondorPartsAttribute end)) typ=tostring(Q(function() return sen.TrackingPoint_Type end)) end
  local b,c,pv=V(boy),V(tr),V(pe)
  local rel,bm,cm='-','-','-'
  if b and c then rel=F(b[1]-c[1])..','..F(b[2]-c[2])..','..F(b[3]-c[3]) end
  if b and pb then bm=F(math.sqrt((b[1]-pb[1])^2+(b[2]-pb[2])^2+(b[3]-pb[3])^2)) end
  if c and pc then cm=F(math.sqrt((c[1]-pc[1])^2+(c[2]-pc[2])^2+(c[3]-pc[3])^2)) end
  local prel,pm='-','-'
  if b and pv then prel=F(b[1]-pv[1])..','..F(b[2]-pv[2])..','..F(b[3]-pv[3]) end
  if pv and pp then pm=F(math.sqrt((pv[1]-pp[1])^2+(pv[2]-pp[2])^2+(pv[3]-pp[3])^2)) end
  pb,pc,pp=b,c,pv
  local anim=tostring(Q(function() return Ai2.Utils.GetAIProps('Condor').currentInfo.mainPlayingAnim end))
  local w=Q(function() return Ai2.Utils.GetAIProps('Condor').currentInfo.work end)
  local function VE(n) local o=Q(FindObject,n) if not o then return '-' end local f=tostring(Q(function() return o.Flags end)) return f:find('Disabled') and 'off' or 'ON' end
  local ux=tostring(Q(MS.BoyCompareStateUserdata,'isClimbRagdoll'))..'/'..tostring(Q(MS.BoyCompareStateUserdata,'isFall'))
  local ai=Q(function() return Ai2.Utils.GetAIProps('Condor').currentInfo.ai2Ext end)
  local bl=ai and V(Q(Ai2.Utils.FindBone,ai,'jt_LegUp_Lf')) local bs=ai and V(Q(Ai2.Utils.FindBone,ai,'jt_Spine'))
  local blm='-' if bl and pbl then blm=F(math.sqrt((bl[1]-pbl[1])^2+(bl[2]-pbl[2])^2+(bl[3]-pbl[3])^2)) end
  local bsm='-' if bs and pbs then bsm=F(math.sqrt((bs[1]-pbs[1])^2+(bs[2]-pbs[2])^2+(bs[3]-pbs[3])^2)) end
  local brel='-' if b and bl then brel=F(b[1]-bl[1])..','..F(b[2]-bl[2])..','..F(b[3]-bl[3]) end
  pbl,pbs=bl,bs
  local wd,wn='-','-'
  if ai and b then local best for _,n in ipairs({'jt_Wing_Shoulder_Lf','jt_Wing_ArmUp_Lf','jt_Wing_ArmLow_Lf','jt_Wing_Hand_Lf','jt_Wing_Fing_Lf','jt_Wing_Shoulder_Rt','jt_Wing_ArmUp_Rt','jt_Wing_ArmLow_Rt','jt_Wing_Hand_Rt','jt_Wing_Fing_Rt'}) do
   local p=V(Q(Ai2.Utils.FindBone,ai,n)) if p then local d=math.sqrt((b[1]-p[1])^2+(b[2]-p[2])^2+(b[3]-p[3])^2) if not best or d<best then best=d wn=n end end end
   if best then wd=F(best) end end
  local bones='legmove='..blm..' spinemove='..bsm..' boy-leg='..brel..' wing='..wn..'@'..wd
  local ctx='sb='..tostring(w and w.TC_ShockBallType)..'/'..tostring(w and w.TC_ShockBallOn)..' wing='..tostring(w and w.TC_WingVolFlag)..' deny='..VE('TC_B_bhv_deny_climb_01')..VE('TC_B_bhv_deny_climb_03')..' ffall='..VE('TC_B_bhv_climb_forcedFall_01')..' climbrag/fall='..ux..' care1='..tostring(w and w.TC_Care1_Clear)
  if ctx~=lastctx then L('grip ctx '..ctx..' boy='..st..' anim='..anim) lastctx=ctx end
  if anim~=lastanim then L('grip anim '..tostring(lastanim)..' -> '..anim..' boy='..st) lastanim=anim end
  local key=st..' ride='..tostring(ride)..' body='..tostring(body)..' part='..part..' type='..typ
  local clk=Q(os.clock) or 0
  local line=tick..' t='..F(clk)..' dt='..F(dt and dt*1000)..'ms '..key..' boy='..(b and F(b[1])..','..F(b[2])..','..F(b[3]) or '-')..' rel='..rel..' boymove='..bm..' tricomove='..cm..' partrel='..prel..' partmove='..pm..' anim='..anim..' '..ctx..' '..bones
  if key~=last then
   L('grip change '..tostring(last)..' -> '..key)
   for i=1,120 do local r=ring[(ri+i-1)%120+1] if r then L('grip pre '..r) end end
   ring={} ri=0 post=120 last=key
  end
  local near=ride or body or (b and c and math.abs(b[1]-c[1])+math.abs(b[2]-c[2])+math.abs(b[3]-c[3])<12)
  if post>0 or near then if post>0 then post=post-1 end L('grip '..line) else ri=ri%120+1 ring[ri]=line end
  if clk-lastsum>=1 then lastsum=clk L('grip sec '..line) end
 end)
 if not ok then errs=errs+1 if errs<5 then L('grip error: '..tostring(e)) end if errs>200 then UnRegisterTick(_TLG_GRIP) _TLG_GRIP=nil L('grip off: errors') end end
end
RegisterTick(_TLG_GRIP)
L('grip log armed')
)";
// Grip auto test (TLG_GRIP_AUTO=1): every time the boy is off Trico for 3 s, put him just above
// Trico's left thigh (offset from the 5090 hold, 2026-11-09) and count what follows.
bool GripAutoWanted() {
    static const bool want = [] {
        const char* env = std::getenv("TLG_GRIP_AUTO");
        return env && *env && std::string_view(env) != "0";
    }();
    return want;
}
constexpr std::string_view GripAutoLua = R"(
if _TLG_GAUTO then pcall(UnRegisterTick,_TLG_GAUTO) _TLG_GAUTO=nil end
local t0,off,places,rags,hold,lastst,lastsum,pend=nil,nil,0,0,0,nil,0,nil
local pat,th,td,tf,tr,ntrial,nbad,gp,gat=nil,0,0,0,false,0,0,nil,nil
local jat,nmiss,jdown,jdone=nil,0,nil,nil
local function Trial() if not pat then return end ntrial=ntrial+1 local miss=_TLG_GJUMP and not tr local bad=not miss and ((td>th) or tf>0) if bad then nbad=nbad+1 end if miss then nmiss=nmiss+1 end
 L(string.format('grip auto trial %d hang=%d default=%d fell=%d result=%s (bad %d of %d, miss %d)',ntrial,th,td,tf,miss and 'MISS' or (bad and 'SLIP' or 'HOLD'),nbad,ntrial-nmiss,nmiss)) end
local function Q(f,...) local ok,v=pcall(f,...) if ok then return v end return nil end
local function V(o) if not o then return nil end local ok,p=pcall(function() return o.worldPosition or o.position end)
 if not ok or p==nil then return nil end local t={} for n in tostring(p):gmatch('[-%d%.e]+') do t[#t+1]=tonumber(n) end
 if #t<3 then return nil end return t end
_TLG_GAUTO=function()
 local ok,e=pcall(function()
  local clk=Q(os.clock) or 0
  if not t0 then t0=clk end
  if _TLG_NOCAM and clk-(_TLG_CAMT or -9)>=0.5 then _TLG_CAMT=clk Q(MS.SetDisableVolume,'GameCameraVolume_TC_CommonTricoNotice') Q(MS.SetDisableVolume,'GameCameraVolume_BoyStandAfterDemo') end
  if _TLG_GREPLAY then local st=tostring(Q(MS.BoyGetDynamicsState)) local ride=Q(MS.BoyIsRidingOn,'Condor') and 1 or 0
   if st~=_TLG_RST or ride~=_TLG_RRIDE then _TLG_RST=st _TLG_RRIDE=ride local bp=V(Q(FindObject,'Player')) L(string.format('grip replay state %s ride=%d t=%.2f pos=%s',st,ride,_TLG_RGO and clk-_TLG_RGO or -1,bp and string.format('%.2f,%.2f,%.2f',bp[1],bp[2],bp[3]) or '?')) end
   if st=='Ragdoll' then _TLG_RFELL=true end
   if not _TLG_RGO and st=='Default' and clk-t0>=1 then _TLG_RGO=clk _TLG_RFELL=nil L('grip replay go') end
   if _TLG_RGO and not _TLG_RDONE and clk-_TLG_RGO>=26 then _TLG_RDONE=true L(string.format('grip replay result %s (ride=%d fell=%s)',(ride==1 and not _TLG_RFELL) and 'HOLD' or 'SLIP',ride,tostring(_TLG_RFELL))) L('grip auto done') end
   return end
  if _TLG_GPASS then return end
  local st=tostring(Q(MS.BoyGetDynamicsState))
  local ride=Q(MS.BoyIsRidingOn,'Condor') local body=Q(MS.BoyIsRidingOn_OnlyBody,'Condor')
  if st=='Ragdoll' and lastst~='Ragdoll' then rags=rags+1 tf=tf+1 L('grip auto ragdoll '..rags) end
  local ws=_TLG_GJUMP and jdone or pat
  if ws and not pend and clk-ws<(_TLG_GJUMP and 15 or 4) and (not _TLG_GJUMP or ride or body or tr) then if ride or body then tr=true end if tr then if st=='Hang' then th=th+1 elseif st=='Default' then td=td+1 end end end
  if jat and clk-jat>=18 and not jdown then jdown=clk L('grip auto jump') elseif jdown and clk-jdown>=0.25 then L('grip auto jumpend') jdown=nil jat=nil jdone=clk off=clk end
  if _TLG_GJUMP and jdone and pat and clk-jdone>=15 then L(string.format('grip auto result %s (on=%s fell=%d hang=%d default=%d)',((ride or body) and tf==0) and 'HOLD' or (tr and 'SLIP' or 'MISS'),tostring(ride or body),tf,th,td)) pat=nil L('grip auto done') end
  lastst=st
  local dt=Q(MSC_GetGameDelta) or 0
  if ride or body then off=nil hold=hold+dt else off=off or clk end
  if pend and clk-pend>=(_TLG_GJUMP and 2 or 0.1) then pend=nil Q(MS.BoyMissionCommand,'ctrl_B_respawn','B_stand') L('grip auto release') end
  if (ride or body) and not pat then _TLG_GRS=_TLG_GRS or clk else _TLG_GRS=nil end
  local cyc=_TLG_GCYCLE
  if clk-t0>20 and st=='Default' and not ride and not body and not pend and not gat and (not pat or clk-pat>4) then local bp=V(Q(FindObject,'Player')) if bp then gp=bp end end
  if gat and (ride or body) and clk-gat>=1.5 and clk-gat<9 and not pend then gat=gat+0.0 Q(MS.BoyRagdollDown) L('grip auto drop again') gat=clk-0.01
  elseif gat and not (ride or body) and st=='Default' then gat=nil
  elseif gat and clk-gat>20 then gat=nil
  elseif cyc and not _TLG_GJUMP and not pat and not pend and not gat and (ride or body) and clk-t0>40 and _TLG_GRS and clk-_TLG_GRS>3 then _TLG_GRS=nil gat=clk Q(MS.BoyRagdollDown) L('grip auto drop (climbed on by himself)')
  elseif cyc and pat and not pend and not gat and (ride or body) and clk-pat>cyc then
   Trial() pat=nil gat=clk Q(MS.BoyRagdollDown) L('grip auto drop')
  elseif not gat and not pend and not jat and not jdown and not (_TLG_GJUMP and places>=1) and clk-t0>15 and st~='Ragdoll' and st~='Boot' and off and clk-off>3 then
   local ai=Q(function() return Ai2.Utils.GetAIProps('Condor').currentInfo.ai2Ext end)
   local bl=ai and V(Q(Ai2.Utils.FindBone,ai,'jt_LegUp_Lf'))
   local bp=_TLG_GJUMP and V(Q(FindObject,'Player'))
   if bl and _TLG_GJUMP and st=='Default' then local fy=(st=='Default' and bp and bp[2]) or -8.35 Trial() pat=clk th,td,tf,tr=0,0,0,false jdone=nil places=places+1 off=clk
    local d=_TLG_GJUMP local x,z=bl[1]-0.955*d,bl[3]-0.296*d if _TLG_GSPOT then x,fy,z=_TLG_GSPOT[1],_TLG_GSPOT[2],_TLG_GSPOT[3] end local yaw=math.deg(math.atan2(bl[1]-x,bl[3]-z))
    Q(MS.BoySetPosition,x,fy+0.05,z) Q(MS.BoySetRotation,0,yaw,0) jat=clk pend=clk
    L(string.format('grip auto jumpplace %d at %.2f,%.2f,%.2f yaw %.0f',places,x,fy,z,yaw))
   elseif bl and not _TLG_GJUMP then Trial() pat=clk th,td,tf=0,0,0 places=places+1 off=clk pend=clk
    Q(MS.BoySetPosition,bl[1]-1.13,bl[2]+0.2,bl[3]-0.35)
    L(string.format('grip auto place %d at %.2f,%.2f,%.2f',places,bl[1]-1.13,bl[2]+0.2,bl[3]-0.35)) end
  end
  if _TLG_GJUMP and clk-(_TLG_MLOG or 0)>=1 then _TLG_MLOG=clk
   local w=Q(MS.CondorAI2Work) local c2=w and w.TC_Care2_Clear
   local function vf(n) local o=Q(FindObject,n) if not o then return 'none' end local ok,f=pcall(function() return tostring(o.Flags) end) return ok and f or '?' end
   L(string.format('grip mood care2=%s deny3=%s deny1=%s ffall=%s shock=%s',tostring(c2),vf('TC_B_bhv_deny_climb_03'),vf('TC_B_bhv_deny_climb_01'),vf('TC_B_bhv_climb_forcedFall_01'),tostring(w and w.TC_ShockBallType))) end
  if clk-lastsum>=10 then lastsum=clk L(string.format('grip auto sum t=%.0f places=%d ragdolls=%d hold=%.1fs',clk-t0,places,rags,hold)) end
 end)
 if not ok then L('grip auto error: '..tostring(e)) end
end
RegisterTick(_TLG_GAUTO)
L('grip auto armed')
)";


// Launcher "Start at..." (TLG_START_POINT=<stage>:<area>): a second after the title screen shows,
// the next LoadCheckPoint is pointed at the point's scene and CONTINUE is chosen, as the game's
// aging test does (tools/scene-check WARP_LUA). The launcher puts the player's save back after.
const TlgFun::Point* StartPoint() {
    static const TlgFun::Point* point = [] () -> const TlgFun::Point* {
        const char* env = std::getenv("TLG_START_POINT");
        if (!env || !*env) return nullptr;
        const std::string_view want(env);
        for (const auto& p : TlgFun::Points) {
            if (want == std::string(p.stage) + ":" + p.area) return &p;
        }
        std::printf("Fun: start point %s is not in the list\n", env);
        return nullptr;
    }();
    return point;
}
std::atomic<bool> start_due_set{false};
std::chrono::steady_clock::time_point start_due;

std::string StartChunk() {
    static bool done = false;
    if (done || !start_due_set || std::chrono::steady_clock::now() < start_due) return {};
    done = true;
    const TlgFun::Point* p = StartPoint();
    std::printf("Fun: start at %s %s (%s)\n", p->stage, p->area, p->scene);
    return std::string("\ndo L('fun start ") + p->stage + " " + p->scene + "')\n"
           "if ControllerMenu and ControllerMenu.Tick then pcall(function() UnRegisterTick(ControllerMenu.Tick) "
           "DelayExec(function() GUI_DrawText(0,0,1,1,'0x0',' ') end) end) end\n"
           "local orig=LoadCheckPoint\n"
           "LoadCheckPoint=function(...) LoadCheckPoint=orig local r=orig(...) "
           "GameFlag.Stage,GameFlag.Scene='" + p->stage + "','" + p->scene + "' "
           "L('fun start retarget '..GameFlag.Stage..' '..GameFlag.Scene) return r end\n"
           "local ok,e=pcall(GameMenu_AutoAsing_SelectTitleContinue)\n"
           "if not ok then L('fun start error: '..tostring(e)) end\nend\n";
}

std::string FunChunk() {
    std::string code = StartChunk();
    std::lock_guard lock(fun_mutex);
    return code + std::exchange(fun_queue, {}) + std::exchange(eye_pending, {}) + std::exchange(size_pending, {}) + std::exchange(cam_pending, {});
}

// The extra Tricos are named Condor00, Condor01, ..., the enemy ones EnemyCondor00, ... (CreateParam
// addNumberToName). Both count to the one limit: they share the physics room.
constexpr std::string_view FunCount =
    "local function FunCount() local n=0 for i=0,29 do for _,f in ipairs({'Condor%02d','EnemyCondor%02d'}) do "
    "local ok,o=pcall(FindObject,string.format(f,i)) if ok and o then n=n+1 end end end return n end\n";

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

// Copies the saves to savedata_backups/<time>-<tag> (the game runs in the user folder), where the
// launcher's save manager lists them and can restore one. A failed copy only logs.
void BackupSaves(const char* tag) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_directory("savedata", error)) return;
    char name[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof(name), "%Y%m%d-%H%M%S-", std::localtime(&now));
    const std::string folder = std::string(name) + tag;
    const fs::path target = fs::path("savedata_backups") / folder;
    const fs::path temp = fs::path("savedata_backups") / (folder + ".copying");
    fs::create_directories("savedata_backups", error);
    fs::remove_all(temp, error);
    fs::copy("savedata", temp, fs::copy_options::recursive, error);
    if (!error) fs::rename(temp, target, error);
    if (error) {
        std::printf("Saves: backup (%s) failed: %s\n", tag, error.message().c_str());
        fs::remove_all(temp, error);
    } else {
        std::printf("Saves: backed up to %s\n", target.string().c_str());
    }
}

// Mods > All treasures writes to the save: before its first unlock ever (no *-treasures backup yet),
// the saves are copied, so the old state can come back through the launcher's save manager.
void BackupBeforeTreasures() {
    namespace fs = std::filesystem;
    static bool done = false;
    if (done) return;
    done = true;
    std::error_code error;
    for (fs::directory_iterator it("savedata_backups", error), end; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.size() > 10 && name.compare(name.size() - 10, 10, "-treasures") == 0) return;
    }
    BackupSaves("treasures");
}

std::string SetChunk() {
    static const int subtitle_shift = SubtitleShift();
    const auto& s = TlgSettings::Get();
    if (s.mod_treasures.load()) BackupBeforeTreasures(); // before the Lua below unlocks anything
    std::string code(Prelude);
    code += TlgLab::Lua(s.wind_skip.load());
    code += DevChunk();
    code += FunChunk();
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
    code += "Drool(" + std::to_string(std::clamp(s.trico_drool.load(), 0, 2)) + ")\n";
    code += std::string("Plays(") + (s.mod_plays.load() ? "true" : "false") + ") ThrowUp(" +
            std::to_string(s.mod_throw_up.load() ? 1 : 0) + ") _TLGHINTS=" +
            (s.mod_hints.load() ? "true" : "false") + "\n";
    code += std::string("Treasures(") + (s.mod_treasures.load() ? "true" : "false") + ") Horns(" +
            (s.mod_horns.load() ? "true" : "false") +")\n";
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
// Mods > Cut narrator hints (tlg.ini mod_hints): the narrator lines cut on 2016-09-20 ("cut at
// genDESIGN's request", common/hintlist.lua) whose triggers still run in the stage scripts; only
// the play call is commented out. Each such call, and a commented SubTitleForcePlay() just above it,
// becomes "if _TLGHINTS then ... end" in every load, so the switch works at once (the settings tick
// sets _TLGHINTS); off, the scripts do what the game's own do. The hint table stays as it is (the
// hints trophy counts its entries).
bool CutHints(std::string& patched, std::string_view name) {
    if (name.find("/stages/") == std::string_view::npos || !name.ends_with("missionScript.lua")) {
        return false;
    }
    static constexpr std::string_view Ids[] = {
        "s03/S03_CA_HNT_V_0021", "s03/S03_CA_HNT_V_0110", "s03/S03_CA_HNT_V_0120",
        "s06/S06_FB_HNT_V_0040", "s06/S06_FB_HNT_V_0080", "s06/S06_FB_HNT_V_0092",
        "s07/S07_AL_HNT_V_0050", "s08/S08_LT_HNT_V_0031", "s08/S08_LT_HNT_V_0040",
        "s09/S09_FB2_HNT_V_0060", "s10/S10_PI_HNT_V_0030", "s10/S10_PI_MRY_V_0040",
        "s10/S10_PI_MRY_V_0060", "s11/S11_CC_HNT_V_0020", "s11/S11_CC_HNT_V_0051",
        "s11/S11_CC_HNT_V_0060", "s13/S13_FF_HNT_V_0001", "s13/S13_FF_HNT_V_0033",
        "s14/S14_CH_HNT_V_0120", "s14/S14_CH_HNT_V_0130", "s14/S14_CH_HNT_V_0140",
        "s15/S15_SF_HNT_V_0030", "s15/S15_SF_HNT_V_0040"};
    static constexpr std::string_view On = "if _TLGHINTS then ";
    // Wraps the commented call that starts on the line holding `at`: true if it was one.
    const auto wrap = [&](size_t at, std::string_view call) {
        const size_t line = patched.rfind('\n', at) + 1; // npos + 1 == 0
        size_t c = patched.find_first_not_of(" \t", line);
        if (c == std::string::npos || patched.compare(c, 2, "--") != 0) return false;
        const size_t code = patched.find_first_not_of(" \t", c + 2);
        if (code == std::string::npos || patched.compare(code, call.size(), call) != 0) return false;
        const size_t close = patched.find(')', code);
        const size_t eol = patched.find('\n', code);
        if (close == std::string::npos || close > eol) return false;
        patched.insert(close + 1, " end");
        patched.replace(c, 2, On);
        return true;
    };
    int done = 0;
    for (const std::string_view id : Ids) {
        size_t at = patched.find(id);
        while (at != std::string::npos && (at == 0 || (patched[at - 1] != '"' && patched[at - 1] != '\''))) {
            at = patched.find(id, at + 1);
        }
        if (at == std::string::npos) continue;
        const size_t line = patched.rfind('\n', at) + 1;
        if (!wrap(at, "MS.SubTitleStart(")) {
            std::printf("Lua: cut hint %.*s not as expected, left out\n", int(id.size()), id.data());
            continue;
        }
        ++done;
        // The commented SubTitleForcePlay() of the same block (at most 4 lines up).
        size_t up = line;
        for (int n = 0; n < 4 && up > 1; ++n) {
            up = patched.rfind('\n', up - 2) + 1;
            const size_t f = patched.find("MS.SubTitleForcePlay()", up);
            if (f != std::string::npos && f < patched.find('\n', up)) {
                wrap(f, "MS.SubTitleForcePlay(");
                break;
            }
            if (patched.find("SubTitleStart", up) < patched.find('\n', up)) break;
        }
    }
    if (done) std::printf("Lua: cut narrator hints: %d in %.*s\n", done, int(name.size()), name.data());
    return done > 0;
}

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
        // A stage started from the QA menu or the Fun tab is loaded on top of the current one (StartStage
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
    if (name.size() >= 15 && name.substr(name.size() - 15) == "GuideSystem.lua") {
        // Hint box text: each hint's text picture is loaded inside its .bod's load callback, so
        // it can arrive after the guide preload is marked done. The boxes were then created
        // without their pictures (short boxes, no text). When a picture arrives after the
        // boxes exist, rebuild them once on the next frame.
        static constexpr std::string_view Anchor = "self:inLoadTexture(data.sequenceName,data";
        static constexpr std::string_view Late =
            " if self.isCreateObj and not self.tlgRegen then self.tlgRegen = true"
            " DelayExec(function() self.tlgRegen = nil GuideSystem:RegenerationGuide() end) end";
        size_t at = patched.find(Anchor);
        if (at == std::string::npos) {
            return false;
        }
        int done = 0;
        for (std::string_view set : {std::string_view("data.sysParam.titleTex = tex or FindObject('_error_texture')"),
                                     std::string_view("data.sysParam.subTex = tex or FindObject('_error_texture')")}) {
            const size_t k = patched.find(set, at);
            if (k != std::string::npos) {
                patched.insert(k + set.size(), Late);
                ++done;
            }
        }
        return done == 2;
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
    // 30 FPS. Above 30 FPS it runs capped at 30 (start and end call only); at 30 the game's own
    // slow motion stays (it works at 30, as on PS4).
    static constexpr std::string_view SlowMotion = "MS.SetGameSpeed(0.10, 0.2)";
    // Above 30 FPS, tlg.ini slow_motion (env TLG_SLOW_MOTION wins): 0 = cap at 30 (default),
    // 1 = skip the slow motion (the v0.5.0 behaviour).
    static const char* slow_env = std::getenv("TLG_SLOW_MOTION");
    const bool skip_slow = slow_env ? std::atoi(slow_env) == 1 : s.slow_motion.load() == 1;
    if (name == "data_trico/game/scripts/stages/s10_Pit/missionScript.lua") {
        const size_t at = patched.find(SlowMotion);
        if (at == std::string::npos) {
            return false;
        }
        if (s.applied_engine_fps.load() <= 30) {
            std::printf("Lua: bridge fix off at 30 FPS (game's own slow motion)\n");
            return false;
        }
        if (skip_slow) {
            patched.insert(at, "--");
            std::printf("Lua: bridge fix (slow motion skipped)\n");
            return true;
        }
        // Only the start call: the 30 FPS cap stays past the speed reset until the cutscene
        // pi_160_p04_A finishes (tlg_lua_load). Back to the full rate right at the reset, the
        // game crashed at Trico's catch (pi_160_trico action_5, fault 0x2719aa) in a test.
        patched.replace(at, std::string_view("MS.SetGameSpeed(").size(), "_TLGSGS(");
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
        if (text == "grip replay go") {
            // tlg grip test: play the recorded run + jump (TLG_PAD_REPLAY) once the boy has control.
            if (const char* pf = std::getenv("TLG_PAD_FILE"); pf && *pf)
                if (FILE* f = std::fopen(pf, "w")) { std::fputs("replay ", f); std::fclose(f); }
        }
        if (text == "grip auto release" && std::getenv("TLG_GRIP_JUMP")) {
            // tlg grip test: run forward-right, jump (triangle) after 0.5 s, let go 0.3 s later.
            if (const char* pf = std::getenv("TLG_PAD_FILE"); pf && *pf) {
                std::thread([path = std::string(pf)] {
                    auto put = [&](const char* t) { if (FILE* f = std::fopen(path.c_str(), "w")) { std::fputs(t, f); std::fclose(f); } };
                    std::this_thread::sleep_for(std::chrono::seconds(15)); // let Trico calm down
                    put("lx=230 ly=25 ");
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    put("lx=230 ly=25 triangle ");
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    put("");
                }).detach();
            }
        }
        if (text.starts_with("grip ")) {
            static const auto t0 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("Lua: [%.1f ms] %.*s\n", ms, static_cast<int>(text.size()), text.data());
        } else
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
        // Bridge fix (s10): the cap ends with the slow-motion cutscene, not the speed reset.
        if (slowmo && text.starts_with("demo game/stages/s10_Pit/demo/pi_160_p04_A ") &&
            text.find("finish") != std::string_view::npos) {
            slowmo = false;
            TlgSettings::Get().credits_fps_lock = 0u;
            std::printf("Lua: bridge fix: 30 FPS cap ends with pi_160_p04_A\n");
        }
        if (!start_due_set && StartPoint() &&
            text.starts_with("demo game/stages/s01_OpeningA/demo/opA_010 titleScreen")) {
            start_due = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            start_due_set = true;
        }
        if (text.starts_with("fun tricos ")) {
            fun_tricos = std::atoi(std::string(text.substr(11)).c_str());
        }
        if (text.starts_with("mission ")) {
            const std::string_view scene = text.substr(8);
            const size_t space = scene.find(' ');
            const bool helper = scene.starts_with("_") || space == std::string_view::npos ||
                                scene.substr(space + 1).starts_with("sub_");
            if (!helper) {
                TlgStats::SetScene(scene);
                // Tricopilot for a whole run (the launcher's benchmark): every new scene gets its
                // own Trico and the game's own camera back, so arm the pilot again at each one.
                // The title screen counts as a scene too, so a run that warps (TLG_START_POINT)
                // first arms there and then again in the scene it lands in, which is the one that
                // matters. Arming is cheap: the pilot's tick waits for Trico and then starts him.
                if (GripWanted()) {
                    static std::string grip_for;
                    if (grip_for != scene) { grip_for = scene; FunQueue(std::string(GripLua)); if (GripAutoWanted()) { const char* cyc = std::getenv("TLG_GRIP_CYCLE"); const char* jmp = std::getenv("TLG_GRIP_JUMP"); FunQueue(std::string("_TLG_GCYCLE=") + (cyc && *cyc ? cyc : "nil") + " _TLG_GJUMP=" + (jmp && *jmp ? jmp : "nil") + (std::getenv("TLG_NO_TRICO_CAM") ? " _TLG_NOCAM=true" : "") + (std::getenv("TLG_GRIP_PASSIVE") ? " _TLG_GPASS=true" : "") + (std::getenv("TLG_GRIP_REPLAY") ? " _TLG_GREPLAY=true" : "") + " _TLG_GSPOT=" + (std::getenv("TLG_GRIP_SPOT") ? std::string("{") + std::getenv("TLG_GRIP_SPOT") + "}" : std::string("nil")) + " " + std::string(GripAutoLua)); } }
                }
                if (PilotWanted()) {
                    static std::string armed_for;
                    if (armed_for != scene) {
                        armed_for = scene;
                        TlgFun::Tricopilot(true);
                    }
                }
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
    if (name && CutHints(patched, name)) {
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


namespace TlgFun {
void Jump(const char* stage, const char* area) {
    BackupSaves("jump"); // a jump moves the story; the game saves at its next checkpoint
    trico_size = 1.0f; // the new stage's Trico has his normal size
    free_cam = false;  // and the game's own camera
    // As the QA menu does: _TLGQA_UNLOAD makes StartStage unload the current stage first
    // (GameEdits, GameSetupTbl.lua).
    std::printf("Fun: jump to %s %s\n", stage, area);
    FunQueue(std::string("L('fun jump ") + stage + " " + area + "')\n"
             "if SpellSystem and SpellSystem:IsPlay() then SpellSystem:Exit() end\n"
             "_TLGQA_UNLOAD=true GameSetupTbl:StartStage('" + stage + "','" + area + "')");
    fun_tricos = 0;
    disco = false;
    eye_custom = false;
    // The new stage's Trico is a new one and the camera is the game's again: arm Tricopilot afresh
    // when it was on, so he keeps driving after the jump instead of standing still with a dead tick.
    if (pilot) Tricopilot(true);
}

void AddTrico() {
    char code[1024];
    std::snprintf(code, sizeof(code),
                  "%.*s"
                  "local n=FunCount()\n"
                  "if n>=%d then L('fun tricos '..n) return end\n"
                  "local p=FindObject('Player').worldPosition\n"
                  "local a=n*math.pi/4\n"
                  "local pos=string.format('%%f %%f %%f',p.X+7*math.cos(a),p.Y+1,p.Z+7*math.sin(a))\n"
                  "local io=CharacterMngr.CreateParam.New() io.addNumberToName=true\n"
                  "CharacterMngr:CreateCondor(pos,nil,io)\n"
                  // CreateCondor makes every new Trico the game's main one (SetTrico): point it back.
                  "if CharacterMngr.condors.Condor then CharacterMngr:SetTrico(CharacterMngr.condors.Condor) end\n"
                  "L('fun tricos '..(n+1))",
                  static_cast<int>(FunCount.size()), FunCount.data(), MaxTricos);
    FunQueue(code);
}

// The game's own enemy (armoured) Trico, as the developers' test menu and the tower stage make it
// (CharacterMngr:CreateEnemyCondor; looks 1 and 2 = condorS_model_verA/B). It is made asleep
// (defaultSuspend), yet in play it moved about (user, 2026-11-04). Tested live on the terrace: 3 at once, removed fine.
void AddEnemyTrico() {
    char code[1024];
    std::snprintf(code, sizeof(code),
                  "%.*s"
                  "local n=FunCount()\n"
                  "if n>=%d then L('fun tricos '..n) return end\n"
                  "local p=FindObject('Player').worldPosition\n"
                  "local a=n*math.pi/4+math.pi/8\n"
                  "local pos=string.format('%%f %%f %%f',p.X+10*math.cos(a),p.Y+1,p.Z+10*math.sin(a))\n"
                  "CharacterMngr:CreateEnemyCondor(pos,nil,math.random(1,2))\n"
                  "if CharacterMngr.condors.Condor then CharacterMngr:SetTrico(CharacterMngr.condors.Condor) end\n"
                  "L('fun tricos '..(n+1))",
                  static_cast<int>(FunCount.size()), FunCount.data(), MaxTricos);
    FunQueue(code);
}

// The game's own delete (CharacterMngr:DeleteCondor queues it for its next tick); tested live
// 2026-11-04 on the terrace with 3 extra Tricos. The story's Trico (Condor) and Arico stay.
void RemoveTricos() {
    FunQueue("local M=CharacterMngr local gone={}\n"
             "for name,c in pairs(M.condors) do if string.match(tostring(name),'^Condor%d%d$') then "
             "M:DeleteCondor(c) gone[#gone+1]=name end end\n"
             "for _,name in ipairs(gone) do M.condors[name]=nil end\n"
             // Enemy Tricos sit in M.condors and/or M.characters (seen live): delete each one once.
             "local done={} for _,t in ipairs({M.condors,M.characters or {}}) do for name,c in pairs(t) do "
             "if string.match(tostring(name),'^EnemyCondor%d%d$') and not done[name] then done[name]=true "
             "pcall(function() M:DeleteCondor(c) end) end end end\n"
             "if M.condors.Condor then M:SetTrico(M.condors.Condor) end\n"
             "L('fun tricos 0')");
}

// One action at a time (_TLG_ACT, a coroutine on the game's tick; a new pick stops the last one).
// Tested live 2026-11-04: "stand" does not get him up from sitting, sit_end does (then c_stand plays);
// lying: sit first, then lie_down. Ticks run at the frame rate, so the waits count ticks (os.time
// does not move inside a tick).
constexpr std::string_view ActLua =
    "if _TLG_ACT then pcall(UnRegisterTick,_TLG_ACT) _TLG_ACT=nil end\n"
    "local function anim() local ok,a=pcall(function() return tostring(Ai2.Utils.GetAIProps('Condor').currentInfo.mainPlayingAnim) end) "
    "return ok and a:lower() or '?' end\n"
    "local function cmd(c) pcall(function() Ai2.Utils.ReqCtrlCmd(Ai2.Utils.GetAIProps('Condor').currentInfo.ai2Ext,'ControlCommand_C_'..c) end) end\n"
    "local function wait(n) for _=1,n do coroutine.yield() end end\n"
    "local function waitfor(p,n) for _=1,n do if anim():find(p,1,true) then return true end coroutine.yield() end return false end\n"
    "local function getpose(p) local a=anim()\n"
    " if p=='stand' then if a:find('sit_',1,true) or a:find('lie',1,true) then cmd('sit_end') waitfor('c_stand',540) end cmd('stand') wait(90)\n"
    " elseif p=='sit' then if not a:find('sit_loop',1,true) then cmd('sit') waitfor('sit_loop',540) end\n"
    " elseif p=='lie' then if not a:find('lie',1,true) then getpose('sit') cmd('lie_down') waitfor('lie_down',540) wait(180) end end end\n"
    "local co=coroutine.create(function()\n"
    " if not pose then cmd(act) L('fun act '..act..' sent') return end\n"
    " for try=1,3 do if pose~='' then getpose(pose) end cmd(act)\n"
    "  if waitfor(key,450) then L('fun act '..act..' ok, try '..try) return end end\n"
    " L('fun act '..act..' did not play') end)\n"
    "_TLG_ACT=function() local ok,e=coroutine.resume(co) if not ok then L('fun act error: '..tostring(e)) end\n"
    " if coroutine.status(co)=='dead' then UnRegisterTick(_TLG_ACT) _TLG_ACT=nil end end\n"
    "RegisterTick(_TLG_ACT)\n";

// The tummy moves are no control commands: they run the game's own vomit code (ai2/condor/
// condorcommonactioneatable.lua). Gag: AddVomitIntr, the retch Trico does after eating barrels; he
// picks standing or sitting himself. Throw up: the block the developers commented out of
// AddSpitOutIntr ("sealed by order ... sealed again 20160726"); it plays c_throw_up_start/loop/end.
// Both tested live 2026-11-04 on the terrace (user: "it looked perfect").
constexpr std::string_view GagLua =
    "do local ok,e=pcall(function() local props=CharacterMngr.condors.Condor.aiInfo.props\n"
    "local work=props.currentInfo.work\n"
    "if not work.LoadVomitIntr then work.LoadVomitIntr=Load('game/ai/condor_interest/Volmit_intr') end\n"
    "Ai2.CommonScript.Condor.AddVomitIntr(props) L('fun act tlg_gag ok') end)\n"
    "if not ok then L('fun error: '..tostring(e)) end end\n";
constexpr std::string_view ThrowUpLua =
    "do local ok,e=pcall(function() local props=CharacterMngr.condors.Condor.aiInfo.props\n"
    "local work=props.currentInfo.work\n"
    "if not work.LoadSpitOutIntr then work.LoadSpitOutIntr=Load('game/ai/condor_interest/SpitOut_intr') end\n"
    "local intr=work.LoadSpitOutIntr Connect('_world',intr)\n"
    "SetWorldPosition(intr,tostring(FindObject('game/ai/condor_interest/Idling_locater').worldPosition))\n"
    "Ai2.CommonScript.Condor.AddIntrList(props,intr._object_name)\n"
    "Ai2.CommonScript.Condor.ControlGazeLevel(props,intr._object_name,'SubSpitOutIntr','subMax')\n"
    "L('fun act tlg_throw_up ok') end) if not ok then L('fun error: '..tostring(e)) end end\n";

void Act(const char* command) {
    const TlgFun::Action* action = nullptr;
    for (const auto& a : TlgFun::Actions) {
        if (a.command && std::string_view(a.command) == command) action = &a;
    }
    if (!action) return;
    std::printf("Fun: Trico action %s\n", command);
    if (std::string_view(command) == "tlg_gag") return FunQueue(std::string(GagLua));
    if (std::string_view(command) == "tlg_throw_up") return FunQueue(std::string(ThrowUpLua));
    std::string key(action->key ? action->key : command);
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    FunQueue(std::string("local act,key,pose='") + command + "','" + key + "'," +
             (action->pose ? std::string("'") + action->pose + "'" : std::string("nil")) + "\n" +
             std::string(ActLua));
}

void CountTricos() {
    FunQueue(std::string(FunCount) + "L('fun tricos '..FunCount())");
}

int Tricos() {
    return fun_tricos;
}

// EyeColorRandomSet('START') saves the eye colours in work.eyeDemoRandom_Backup (which must exist)
// and 'END' puts them back; EyeColorRandom must then run every frame until the state is FINISHED.
// The game only runs it in s02/s13/s14 stage ticks, so our tick does it (tested live 2026-11-04,
// terrace s04 _090_GardenTerrace_001). The tick removes itself when Trico is gone (a stage change).
void Disco(bool on) {
    disco = on;
    if (!on) {
        FunQueue("Ai2.CommonScript.Condor.EyeColorRandomSet('END') L('fun disco off')");
        return;
    }
    FunQueue("local w=Ai2.Utils.GetAIProps('Condor').currentInfo.work\n"
             "w.eyeDemoRandom_Backup=w.eyeDemoRandom_Backup or {} w.eyeDemoRandom_cnt=0\n"
             "Ai2.CommonScript.Condor.EyeColorRandomSet('START')\n"
             "if not _TLG_DISCO then\n"
             "_TLG_DISCO=function() local ok,done=pcall(function() Ai2.CommonScript.Condor.EyeColorRandom(nil) "
             "return Ai2.Utils.GetAIProps('Condor').currentInfo.work.eyeDemoRandom=='TC_DEMO_FINISHED' end)\n"
             "if done or not ok then UnRegisterTick(_TLG_DISCO) _TLG_DISCO=nil end end\n"
             "RegisterTick(_TLG_DISCO) end\n"
             "L('fun disco on')");
}

bool DiscoOn() {
    return disco;
}

// The eye colours are Color values (R G B A intensity) per mood in ai2Ext.eyeControl, read as text
// "R G B A I" (tested live 2026-11-04). The first change saves them all as text in _TLG_EYE_ORIG,
// EyeNormal writes them back. A new stage builds a new eyeControl, so the saved colours go with a jump.
// What you see is eyeiris (the eye itself, black 0 0 0 at rest, glow 30 when excited) and tapetum (the
// shine); the light colours only tint the area around him. Iris 30 / tapetum 0.8: red + green seen live 2026-11-05.
constexpr std::string_view EyeLua =
    "local c=Ai2.Utils.GetAIProps('Condor').currentInfo.ai2Ext.eyeControl\n"
    "local sets={'defaultColor','exciteColor','lightColor','pulseColor','barrelColor','tapetumColor','chargeColor','manipulatedColor'}\n"
    "local function Each(f) for _,n in ipairs(sets) do local ok,s=pcall(function() return c[n] end) "
    "if ok and s then f(n,s) end end end\n"
    "local function Col(t) local v={} for x in tostring(t):gmatch('%S+') do v[#v+1]=tonumber(x) end "
    "return {R=v[1],G=v[2],B=v[3],A=v[4],I=v[5]} end\n"
    "if _TLG_EYE_OF~=c then _TLG_EYE_OF=c _TLG_EYE_ORIG={} Each(function(n,s) "
    "_TLG_EYE_ORIG[n]={tostring(s.pointLightColor),tostring(s.spotLightColor),tostring(s.eyeiris),tostring(s.tapetum)} end) end\n";

void EyeColour(int r, int g, int b) {
    eye_custom = true;
    char code[512];
    std::snprintf(code, sizeof(code),
                  "Each(function(n,s) for _,k in ipairs({'pointLightColor','spotLightColor'}) do local o=Col(s[k]) "
                  "s[k]={%d,%d,%d,o.A,o.I} end s.eyeiris={%d,%d,%d,255,30} s.tapetum={%d,%d,%d,255,0.8} end) "
                  "L('fun eyes %d %d %d')",
                  r, g, b, r, g, b, r, g, b, r, g, b);
    std::lock_guard lock(fun_mutex);
    eye_pending = "\ndo local ok,e=pcall(function()\n" + std::string(EyeLua) + code +
                  "\nend) if not ok then L('fun error: '..tostring(e)) end end\n";
}

// The story Trico's own scale (OBJ.Condor.scale, "x y z w", normally "1 1 1 1"): the whole of him
// grows or shrinks, tested live 2026-11-04 at 0.5 and 1.5 (s04 terrace area). A new stage or jump
// builds a new Trico at normal size.
void Size(float scale) {
    trico_size = scale;
    char code[200];
    std::snprintf(code, sizeof(code),
                  "\ndo local ok,e=pcall(function() OBJ.Condor.scale='%.3f %.3f %.3f 1' L('fun size %.2f') end) "
                  "if not ok then L('fun error: '..tostring(e)) end end\n",
                  scale, scale, scale, scale);
    std::lock_guard lock(fun_mutex);
    size_pending = code;
}

float SizeNow() {
    return trico_size;
}

// The game's own debug camera (CameraManager_ToggleDebugMove, camera/cameramanager.lua): it flips
// between the fixed camera and the normal game camera. We only flip when the state differs from the
// wish, so pressing twice does no harm. Tested live 2026-11-04 (s04 terrace area).
void FreeCamera(bool on) {
    free_cam = on;
    std::lock_guard lock(fun_mutex);
    cam_pending = on
        ? "\ndo local ok,e=pcall(function() if not FixCamera:IsActive() then CameraManager_ToggleDebugMove(0) end "
          "L('fun free camera on '..tostring(FixCamera:IsActive())) end) if not ok then L('fun error: '..tostring(e)) end end\n"
        : "\ndo local ok,e=pcall(function() if FixCamera:IsActive() then CameraManager_ToggleDebugMove(0) end "
          "L('fun free camera off '..tostring(FixCamera:IsActive())) end) if not ok then L('fun error: '..tostring(e)) end end\n";
}

bool FreeCameraOn() {
    return free_cam;
}

// Tricopilot: the developers' own self-driving Trico (game/scripts/ai2/condor/
// condordebugautotrico.lua, shipped as plain Lua in the retail data). autoTrico(true) attaches their
// helper volumes, sets work.AutoTorico and hands the camera to their chase preset. Two things are
// needed on top of that call, both found 2026-11-05:
//  - DEBUG_AUTO_TRICO: the per-tick driver (Ai2.Condor.Scripts.AutoTorico_main, condorscripttick.lua
//    line 184) only runs when that global is true, and the script forces it false outside their
//    editor ("if not Engine.editor then DEBUG_AUTO_TRICO = false end"). We set it true ourselves.
//  - Waiting for Trico: right after a load his AI props do not exist yet, so our tick keeps trying
//    until they do (up to a minute) and only then switches him on. That is also what makes the
//    launcher's benchmark work, where every scene starts with a load.
// The camera for the chosen preset. The three TricoAutoRunTest_* names are the only presets this
// tool has (condordebugautotrico.lua, condorcommonvolumeevent.lua:2239); a name the engine does not
// know is ignored silently, it cannot break anything. Nothing re-forces the camera afterwards: the
// per-tick block that would (line 1236) is gated on work.AutoTorico_AutoCam, which nothing ever
// sets. So whatever we set here stays until Trico is switched off or the stage changes.
std::string PilotCamLua(TlgFun::PilotCam cam) {
    if (cam == TlgFun::PilotCam::Game)
        return "pcall(function() CameraManager:SetActiveCamera('default') GameCamera:FixParam() end)\n";
    const char* preset = cam == TlgFun::PilotCam::Near     ? "TricoAutoRunTest_near"
                         : cam == TlgFun::PilotCam::Far    ? "TricoAutoRunTest_far"
                                                           : "TricoAutoRunTest_middle";
    return std::string("pcall(function() CameraManager:SetActiveCamera('game') GameCamera:FixParam('") +
           preset + "') end)\n";
}

constexpr std::string_view PilotOnLua =
    "DEBUG_AUTO_TRICO=true\n"
    "if _TLG_PILOT then pcall(UnRegisterTick,_TLG_PILOT) _TLG_PILOT=nil end\n"
    "local tries=0\n"
    "_TLG_PILOT=function() tries=tries+1\n"
    " local ok,ready=pcall(function() local p=Ai2.Utils.GetAIProps('Condor')\n"
    "  return p and p.currentInfo and p.currentInfo.work~=nil end)\n"
    " if ok and ready then UnRegisterTick(_TLG_PILOT) _TLG_PILOT=nil\n"
    "  local started,e=pcall(autoTrico,true)\n"
    "  if started then %CAM% end\n"
    "  L('fun tricopilot on '..tostring(started)..' after '..tries..' ticks'..(started and '' or ' '..tostring(e)))\n"
    " elseif tries>3600 then UnRegisterTick(_TLG_PILOT) _TLG_PILOT=nil\n"
    "  L('fun tricopilot: no Trico in this scene') end end\n"
    "RegisterTick(_TLG_PILOT)\n"
    "L('fun tricopilot armed')\n";
constexpr std::string_view PilotOffLua =
    "if _TLG_PILOT then pcall(UnRegisterTick,_TLG_PILOT) _TLG_PILOT=nil end\n"
    "pcall(autoTrico,false)\n"
    "DEBUG_AUTO_TRICO=false\n"
    // autoTrico(false) gives the camera back itself; do it again in case it never got as far as on.
    "pcall(function() CameraManager:SetActiveCamera('default') GameCamera:FixParam() end)\n"
    "L('fun tricopilot off')\n";

void Tricopilot(bool on) {
    pilot = on;
    std::printf("Fun: Tricopilot %s\n", on ? "on" : "off");
    if (!on) {
        FunQueue(std::string(PilotOffLua));
        return;
    }
    std::string code(PilotOnLua);
    const std::string mark = "%CAM%";
    code.replace(code.find(mark), mark.size(), PilotCamLua(pilot_cam.load()));
    FunQueue(code);
}

bool TricopilotOn() {
    return pilot;
}

void TricopilotCamera(TlgFun::PilotCam cam) {
    pilot_cam = cam;
    std::printf("Fun: Tricopilot camera %s\n", TlgFun::PilotCamKeys[int(cam)]);
    // While he drives, switch over right away; otherwise the next start picks it up.
    if (pilot) FunQueue(PilotCamLua(cam));
}

TlgFun::PilotCam TricopilotCamera() {
    return pilot_cam.load();
}

void EyeNormal() {
    eye_custom = false;
    std::lock_guard lock(fun_mutex);
    eye_pending = "\ndo local ok,e=pcall(function()\n" + std::string(EyeLua) +
                  "Each(function(n,s) local o=_TLG_EYE_ORIG[n] if o then local p,q=Col(o[1]),Col(o[2]) "
                  "s.pointLightColor={p.R,p.G,p.B,p.A,p.I} s.spotLightColor={q.R,q.G,q.B,q.A,q.I} "
                  "if o[3] then local a,b=Col(o[3]),Col(o[4]) s.eyeiris={a.R,a.G,a.B,a.A,a.I} s.tapetum={b.R,b.G,b.B,b.A,b.I} end end end) "
                  "L('fun eyes normal')\nend) if not ok then L('fun error: '..tostring(e)) end end\n";
}

bool EyeCustom() {
    return eye_custom;
}
} // namespace TlgFun
