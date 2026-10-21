-- Live Lua console (TLG_LUA_EXEC): runs each time this file is saved.
local ls = loadstring or load
local function L(s) local g = ls('--TLGLOG ' .. tostring(s)) if g then g() end end
local ok, e = pcall(function()
  L('probe stage=' .. tostring(GameFlag and GameFlag.Stage) .. ' scene=' .. tostring(GameFlag and GameFlag.Scene))
  local f = FindObject('_fade_controller')
  L('probe fade SystemValue=' .. tostring(f and f.SystemValue))
  L('probe timeBlocker=' .. tostring(GameSetupTbl and GameSetupTbl.timeBlocker))
  L('probe loading=' .. tostring(RegionController and RegionController.IsLoading()) .. ' async=' .. tostring(IsAsyncLoading and IsAsyncLoading()) .. ' rc=' .. tostring(RegionControl and RegionControl('status')))
  L('probe overlayEnd=' .. tostring(Overlay_IsEnd and Overlay_IsEnd()) .. ' player=' .. tostring(FindObject('Player_controller-ext')))
  local reg=false for _,v in ipairs(tick_funcs or {}) do if ControllerMenu and v==ControllerMenu.Tick then reg=true end end
  L('probe qa menu ticking=' .. tostring(reg) .. ' unloadflag=' .. tostring(_TLGQA_UNLOAD))
  L('probe Title=' .. tostring(Title) .. ' DelayExec=' .. tostring(DelayExec))
  luaCommand('trico resume') L('probe resumed')
end)
if not ok then L('probe error: ' .. tostring(e)) end
-- rerun 1792577595
-- rerun 1792577881
-- rerun 1792578338.2623377
-- rerun 1792578568.8570337
