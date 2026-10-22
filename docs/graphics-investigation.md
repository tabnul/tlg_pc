# TLG graphics investigation (2026-10-22)

Scope: what more can be pulled out of The Last Guardian's renderer in this port. Investigation only, no code was changed.
Sources: the 790 game Lua scripts (psarc dump), camera `.bod` data, the eboot's reflection-property strings, our own shim (`gpu/shim/tlg_lua.cpp`, lab tooling) and earlier frame dumps.

## How we can change things (the levers)

| Lever | What it reaches | Cost to try |
|---|---|---|
| **A. Lua property writes** (our per-tick chunk in `tlg_lua.cpp`, `Scale()`/`Feature()`) | Any reflected property on env objects: `_rcfg`, `_default_env*`, `_default_light`, `_default_camera`, `_trico_camera`, and Lua tables like `GameCamera.game_param_tbl` | Minutes: live via `TLG_LUA_EXEC`, then a lab row (`lab.txt`) |
| **B. Emulator/render hooks** (vk_rasterizer, upscaler, sampler) | Render-target sizes (half-res passes), samplers, shader swaps, extra passes | Hours to days |
| **C. eboot patches** | Hard-coded constants, startup args, FPS cap | Hours, risky per version |
| **D. Data replacement** (psarc patch or texture-cache replacement) | Textures, models, `.bod` params | Days, plus tooling |

Key enabler not used yet: the engine exposes `GetObjPropsInfo(objName)` to Lua. One live script can dump **every property and current value** of every env object per area. Doing that once (step 0 of the plan) replaces a lot of guesswork below.

---

## 1. Fog and mist

Knobs found (all on `_default_env_pp`, PostProcessSettings, overwritten per area by `SetGI.SetEnv` in `missiongi.lua`):
- `fogParam` ("600 100 0.5 1.0": distance / start / density / curve), `fogClut`, `fogYClut`
- `heightFog*`, `waterFog*`
- Atmospheric scatter: `scatterRayleigh` 3, `scatterMie` 0.3, `scatterDist` 10, `scatterLightDir`
- Feature flags: `|Scatter|FogVolume|LightVolume`
- Local fog volumes: `SetGI.FogVolumeLoad_List` (`missiongi_fogvolumeload.lua`), per-area `FogVolume` objects
- Sky: `scaleForFog` on SkyBox
- Particles have `m_fogCanceller` (per-effect fog opt-out)

Recommendations:
- **Full-res fog/volumetrics (high value).** Frame dump: 39 fog/particle draws render to a half-res R16G16B16A16 target, then composite. Half-res fog is the main cause of the soft, slightly stair-stepped fog edges around geometry. Implemented on `feat/graphics-lab` (`half_scale`), untested in game. Cost: a few FPS.
- **Fog density slider (medium).** A `Scale()` on the density and Mie parts of `fogParam`. This is taste: the game's fog is art direction, so default 100% and allow 50 to 150%. Must reapply after each area change because `SetEnv` overwrites it (our per-tick chunk already does that).
- **Scatter (Mie) intensity (low to medium).** Stronger god-ray haze in sunlit areas. Taste slider.
- Do not touch the fog CLUTs (colour lookup per area; art).

## 2. Lighting

Knobs:
- **Volumetric light/light shafts:** `lightVolumeIntensity`, `lightVolumeParam` ("1 100 0 3"), `lightVolumeTemporalBlend`, `lightVolumeJitterWidth`, `lightVolumeNoiseMove`; per-light flag `LightShaft` and `shaft*` params.
- **GI:** `_default_env` `indirectVertexIntensity`, `indirectGridIntensity`, `specularIntensity` 0.218. `LightProbe` has a `HighResolution` flag plus `glossScale`/`glossBias`.
- **Shadows:** `_rcfg.shadowMapSize`, Cascade `splitDistances` ("50 200 1000 2000"), `numCascades`, `biasAdd/biasMul`, `globalShadow*`, PP flag `BlurShadow`, light flags `CacheShadow`/`DisableShadowCache`, `spotLightShadowScale`.
- **Exposure/tonemap:** `exposure` 2.9, `autoExposureMin/Max/OmegaUp/OmegaDn`, `tonemapParam1/2`, `vibrance`, `blackLevel`, `whiteLevel`, `bleach`.
- **AO:** `ssao`, `saoIntensity` (slider exists), `ssaoResType` (Half), `saoFalloff`, `ssaoMinRadius`.
- **Fur/skin backlight (subsurface-like):** `m_backlightRange`, `m_backlightColor`, `m_rampRange`, `m_rampIntensity`, `m_scatteringRange`, `m_backlightIndirectIntensity`.

Recommendations:
- **Light-shaft quality (high value, cheap test).** Lower `lightVolumeJitterWidth` and raise `lightVolumeTemporalBlend`. With DLAA now resolving, the shafts' own noise and temporal blending may be fighting it. Test A/B in the sunlit cave entrances. Also try full-res via the same half-res fix as fog.
- **Light probe `HighResolution` (medium, unknown).** Name suggests higher-res GI probes. Test on/off in an interior; it may cost load time and VRAM (Garlic cap).
- **AO resolution `ssaoResType` Half to Full (medium).** The same thing as the "full-res AO" item we dropped earlier as "not game changing". If it is one property write instead of a render hook, it becomes worth a lab row.
- **Shadow distance (`splitDistances`) (medium).** Was hidden as unproven because it is probably read on area load. Test by writing it before `EnvMgr_Startup()`, e.g. by wrapping `SetGI.SetEnv` in Lua so our values apply on every area load. That wrapper trick fixes *all* the "read on load" knobs in this report at once.
- **Tiled lighting looks blocky (investigate).** Lighting is computed in 32x32 tiles (cs d3bcee20). If some of the blockiness is light-list culling per tile at our higher res, a bigger tile budget may exist in a property. Low confidence; a frame capture comparison is needed.
- **Exposure adaptation speed (low).** `autoExposureOmegaUp/Dn` for gentler eye adaptation leaving caves. Taste.
- Leave tonemap/vibrance alone in the menu; HDR already fixed the main complaint.

## 3. Reflections

Knobs: `ssrStrength` (slider exists), `ssrEnvDecay`, `ssrRefScale`, `waterMaxFresnel`, LightingSettings `waterRoughnessOffset`, probe `glossScale/glossBias`, particle `refraction`.

Recommendations:
- **SSR resolution/quality (medium).** `ssrRefScale` is likely the SSR buffer scale. Test 1.0 vs default on water.
- **Water roughness (taste, cheap).** Negative `waterRoughnessOffset` gives sharper, mirror-like water. Already a lab row on `feat/graphics-lab`; untested.
- **`ssrEnvDecay`** controls the fade to probe at screen edges. Lowering it reduces visible SSR cut-off when turning the camera.
- Real RT reflections: see section 11. Not worth it.

## 4. Textures and filtering

Status: 16x AF forced and -1 G-buffer LOD bias under DLAA, both done.

Found: `_rcfg` `normalLoadBias`, `normalMaxLod`, `materialLoadBias`, `materialMaxLod`, beside the aniso settings. The editor uses MaxLod 6 / LoadBias 0; some areas set `normalMaxLod` 4 and `materialMaxLod` 1 or 4.

- **Correction to our lab:** `lab.txt` labels `normalMaxLod` "Mesh LOD limit". Its neighbours are texture settings, so it is most likely a **texture streaming mip limit**. The direction is unverified: as a sampler-style clamp, a low value (areas set `materialMaxLod 1`) would mean only the sharpest mips; as a streaming limit it could mean the opposite. The lab row's "try 0 against pop-in" note assumes mesh LOD. **Test first: possibly the cheapest real texture-sharpness win in the game.** Watch VRAM (Garlic 4064 MB cap).
- `LoadBias` negative means stream finer mips earlier (less blurry textures popping in).
- `GameConfig.dbgNoLoadLOD`: debug flag; possibly "always load full LOD". Test, but it may also break streaming.
- **AI texture upscaling (the 5090 use case):** 17,019 `.gnf` textures. A batch Real-ESRGAN/4x-UltraSharp (or a newer diffusion upscaler) pass is very doable locally. Blockers: (1) GNF is PS4-tiled BC; we need a GNF decode/encode tool; (2) the data lives in psarcs, so loose-file mods do not reach it. Best route is **emulator-side replacement in the texture cache by hash** (as other emulators do), which also bypasses the Garlic heap limit because the host allocates it. Large effort (1 to 2 weeks). The gain is real on terrain/stone, smaller on Trico (fur is shaded, not texture-driven). Do this only after the cheap knobs.

## 5. Level of detail

Knobs: `_default_env_sk.viewDistanceScale` (already the "char LOD" slider), `viewDistanceScaleRefMax`, camera Projector `lodBias`, `_trico_camera.maxz` (far clip 2000), Renderable flag `LOD1`, grass `fadeLength`/`fadeValue`, GrassSourceParam `num_high/num_middle/num_low` (density per LOD band), VertexClusterSimFade `simFadeLength`, particle `Cull`.

Recommendations:
- **Projector `lodBias` on `_default_camera`/`_trico_camera` (high, cheap).** This is the general mesh LOD knob, separate from the skin-mesh one we already use. Negative = higher LOD further out. Test on the long vistas (the tower and the valley).
- **Grass density and fade (high, foliage was called a weak spot).** Raise `fadeLength` to push the grass draw distance out; scale `num_middle`/`num_low` for denser far grass. Cost: GPU, but the 5090 has plenty headroom at 30 FPS.
- **Foliage simulation fade `simFadeLength`.** Plants further away keep animating instead of freezing. Fixes visible "dead" foliage in the distance.
- `maxz` far clip: only matters if something is clipped; low.

## 6. Fur, feathers, hair (Trico and the boy)

Knobs:
- `FurType`: `furScale`, `furParam`, `furForce`, `furAnimRanges`; CondorFurParam (`furShow`, `furBlood`, `furDirty`, `furWingScale`, `furBristle*`, `furDamaged*`); wing fur scales.
- SkinningSettings: `skinWindIntensity`, `Limit`, `Ratio`, `Speed`, `Hardness`, `TextureAnimationSpeed`, `MaxFrameSkip` (1), `NumFrames`.
- SkinMesh: `windEnabled`, `windParams`, `vertexBlurEnabled`, `vertexBlurTransparencyStrength`, `vertexBlurMotionScale`, `recalculateNormals`.
- Simulation types: `EFurSimulation`, `EGrassSimulation`.

Recommendations:
- **`skinWindMaxFrameSkip` 1 to 0 (high, cheap).** The feathers likely update wind every other frame; 0 should make it update every frame. Already implemented on `feat/graphics-lab` (`wind_skip`, untested in game). First thing to test.
- **Feather vertex blur under DLAA (medium).** `vertexBlurEnabled` is a fake motion blur on feathers via transparency. Under DLAA it can show as ghosting/smear. Offer an off toggle; test on Trico shaking.
- **Fur density/length `furScale`/`furParam` (medium, taste).** Thicker, fuller Trico. Careful: this is the character's look. Small range (100 to 130%).
- **Wind intensity** for feathers: taste slider.
- The boy's hair shimmer during camera movement is a DLSS limit (already concluded). No new knob found for it.

## 7. Weather, particles, butterflies

Knobs: `_rcfg.particleAutoDecay` (probably auto-reduces particle counts under load), ParticleRenderable `m_highres` (per-effect high-res flag), `depthFadeNearCoff`/`depthFadeFarCoff` (soft particles), `alphaThreshold`, `m_lightingOption` (`SimpleShadow`, `SimpleLightingPlusSH`, `SimpleSHPlusNormalSun`), `useProbeLighting`, `inWaterParticleIntensity`, `bloomBeforeParticle`, EmitInterval/EmitNum.

Recommendations:
- **`particleAutoDecay` 0 (high, cheap).** If it is a PS4 budget control, turning it off gives full dust/leaf/spray density all the time. Test in the waterfall and windy areas.
- **Full-res particles (high).** Same half-res target as fog (section 1). Fixes blurry, haloed dust and water spray edges.
- **Force `m_highres`** on all particle renderables (medium): probably the per-effect version of the above, set in effect data. Only reachable via an object walk in Lua; check with the property dump first.
- **Emit rate multiplier (low to medium).** Possible per effect via Lua, but the effects are tuned per scene; a global x1.5 to x2 on ambient dust/leaves only.
- **Butterflies and birds: low value.** They are gameplay actors (`CharacterMngr.CreateSubCharButterfly`, `SmallAnimalCharacter`); Trico reacts to them. Changing counts risks gameplay and AI behaviour for little visual gain. Recommend not doing it. `GameConfig.dbgDisableSmallAnimalGen` exists if someone wants them off.

## 8. Field of view and camera (biggest pain point)

How it works: the camera is data-driven. `GameCamera.game_param_tbl` (from `game/camera/gamecamera_param_tbl.bod`, 567 KB) holds one table per camera state (BackJump_Air, etc.) with categories `Base, Rod, Gaze, Tight, MoveVec, AutoAvoid, ObjAvoid, PreAvoid, NarrowAvoid, PreShorten, CamBackAway, CamSurface, NearCondor, NearSoldier, Wipe`. `SetupDefaultParam` builds `gamecamera_param_default` from it. Sub-, volume- and path-specific param sets are loaded on top (`LoadSubParam`, `LoadVolumeParam`, `LoadPathParam`).

Relevant fields: `Base_FovDeg`, `Base_SpringFov_*`, `FovByPitch`, `Rod_FovByPitchDeg`, `Rod_LengthBase`, `Rod_LengthMin`, `Rod_PitchMin/MaxDeg`, `Base_Pointing_baseFov`, `Base_Trigger_baseFov`, `Zoom_fovOfs`, `Base_CharaNearClip`, `NearCondor_*`, `AutoAvoid_*`, `Wipe_*`. Engine `GameConfig.cameraSpeedV/H`.

Recommendations (all Lua, no eboot work):
- **FOV slider (high).** After `SetupParam` runs, walk `game_param_tbl` (and the sub/volume/path tables, and `defaultParam`/`activeParam`) and multiply every `*FovDeg`/`*baseFov` by the user factor. Do it once per load, flagged so we never multiply twice. Leave `animeFov` (cutscenes) alone. Fallback if the walk misses a state: scale `_trico_camera.fov` per tick.
- **Camera distance slider (high).** Scale `Rod_LengthBase`/`Rod_LengthMin`. Many complaints about this game's camera are that it hugs the boy in tight spots; a longer rod plus wider FOV is the classic fix.
- **Less aggressive collision behaviour (medium, needs care).** `AutoAvoid_*`, `PreShorten_*`, `NearCondor_*` drive the camera pulling in near Trico and walls. Softening them reduces the "camera jumps into Trico's fur" moments, but goes against the clipping protection. Make it one "Camera assist: Game / Reduced" option, tested in the narrow ruins.
- **Near clip `Base_CharaNearClip` (low).** Reduces fur clipping close-up. Test with the above.
- **Pitch limits** `Rod_PitchMin/MaxDeg`: allow looking further up at the towers. Low.
- Camera speed is already in the game's own options (`cameraSpeedV/H`); no work needed.
- Ultrawide: with our aspect patch the game is Hor+, so 21:9 already sees more; the FOV slider stacks on top.

## 9. Depth of field

Found: `cameraposteffect.bod` holds `DOFParams` "1 500 20 1" (applied to `_trico_camera`/`_default_camera` by `CameraManager.SetupPostEff`), `cameraMaxDOFScale`, CameraManager `sysDof`, `animeDof` (cutscenes).
Earlier tests: no visible effect in gameplay. That fits: gameplay DOF looks disabled, and DOF is used in cutscenes (`animeDof`).
- Recommendation: **low priority**. Optional "Cutscene DOF: Game / Off" via `animeDof` for people who dislike it; check on one cutscene. Adding DOF to gameplay is not desired for this game.

## 10. Anti-aliasing, resolution, frame rate

- **Non-RTX users:** set `aaType` to `SMAA_T2X` with `smaaQuality` Ultra instead of the default SMAA 1x. Pure property writes; good value for AMD/Intel users at almost no work.
- Eboot strings list internal modes "1920x1080(scale)", "1680x1890(cb)", "3840x2160(native)". Worth checking we never land in a checkerboard mode at our resolutions (verify with one capture).
- **60 FPS** (`docs/60fps.md`, experimental) is the biggest single "graphics" improvement for motion clarity, and DLAA looks better at 60. Finishing it outranks most items here.
- **DLSS Frame Generation:** we already feed DLSS the game's motion vectors and depth. FG on top of 30 FPS gives 60/120 presentation. Concerns: 30 FPS base latency and UI/HUD artifacts (needs a HUD-less input, which we have via the sky pass hook point). Medium to large effort. Parked by the user (2026-10-22): only an in-game implementation counts, no driver-level FG.

## 10b. PS4 Pro mode (new, possibly the biggest lever)

The port reports a base PS4: `sceKernelIsNeoMode` returns 0 in both `src/runtime_tlg.c:165` and `gpu/shim/tlggpu.cpp:169`. The eboot holds the Pro render-mode table right next to `_rcfg`: `1680x1890(cb) 1920x1080(scale) 3840x2160(native) 1920x2160(cb) 1840x2070(cb) ...` (checkerboard steps of the Pro's dynamic resolution). So the game has a Pro code path we never run.
- Why it matters: Pro builds often also raise other quality settings (LOD, shadow, effect resolution, texture mips) in the same branch. We cannot see that from strings; it needs a test.
- Test: return 1 from `IsNeoMode`, then compare a `GetObjPropsInfo` dump of `_rcfg`/pp/cascade against base mode, plus a frame dump of pass sizes.
- Risks: checkerboard rendering would conflict with DLAA and the resolution patch (we would keep the Pro quality values but force a non-cb mode); Pro memory layout differs (Garlic cap); SotC lesson: its Pro mode broke above 3840 wide.

## 11. Ray tracing viability

- **RTX Remix: not viable.** Remix hooks D3D9 fixed-function-style games (it needs the game's world/view matrices, fixed-function lights and unshaded textures). TLG here is a Vulkan deferred renderer with custom shaders and GPU-generated vertex data (skinning, fur). There is nothing for Remix to hook.
- **Our own Vulkan RT in the emulator: feasible, big.** We see every draw, vertex buffer and the camera, so per frame we could build BLAS/TLAS (static once, skinned/fur refit per frame) and add:
  - **RT sun shadows** (replaces 4 cascades; fixes shadow distance and acne for good). Only needs geometry plus sun direction.
  - **RTAO** (replaces half-res SSAO).
  - These two only need geometry, not materials, so they are the realistic scope. Estimate 3 to 6 weeks, plus risk with fur (alpha-tested shells need any-hit shaders and textures).
  - **RT reflections/GI: not realistic.** Shading a hit needs the game's material shaders, which we cannot run in RT.
- **Quick experiment:** ReShade on Vulkan with a screen-space RTGI/AO shader (e.g. Pascal Gilcher's RTGI). Hours to try. Caveats: it needs the right depth buffer (scene depths ping-pong per frame), must sit before HDR output, and screen-space only. Worth one evening to see if the user even likes the look.
- The engine's `GpuRayTarget` renderable flag is almost certainly compute raycasting (gameplay/visibility), not hardware RT. Not usable.

## 12. Local AI on the RTX 5090

Where it is worth it:
1. **Texture upscaling** (section 4): the clear match. Batch-run locally overnight; quality comparable to what we would choose by hand. Needs the GNF tool and the replacement hook first.
2. **Frame upscaling/interpolation offline** for trailers or screenshots: trivial but not a game improvement.
Where it is not worth it: tuning knobs (needs eyes on the game, not a model), code work (my own analysis is better), AI-generated new geometry (no pipeline to put it in).

## 13. Low value or not recommended

- Butterfly/bird counts: gameplay actors (section 7).
- SMAA quality under DLAA: SMAA is already skipped under DLAA.
- `dizziness`/`dizz2*`, `ExposureWave`, `blackoutParams`, `wipeParams`: gameplay-feedback effects (height vertigo, fades). Taste only; leave.
- Debug flags (`dbgEnableGI`, `dbgDispFramerate`, `ssrDebugMode`): no value for players.
- Fog CLUT and colour grading changes: art direction, not quality.
- Gameplay DOF: not wanted.
- `clampColor`/`checkOverOne`: HDR internals, already correct with native HDR10.

---

## Live test results (2026-10-22, s15 and s04 GardenTerrace, HDR off)

Method: live Lua (`exec.lua`), values written to every object of a class (`Class('GrassType')` etc.), screenshots and fast BitBlt frame-diff captures. Evidence images: `scratchpad/live/findings/`.

- **Textures `_rcfg` MaxLod/LoadBias: no gain (please confirm, 01 + 02).** Live writes (even LoadBias 4) cause 100-150 ms reload stalls but no visible change; after reloading the area with LoadBias 4 vs 0 the crops are identical. Textures already show their full detail at our resolution. Step 1 of the plan drops to low; the lab label question stays open.
- **FOV slider: works (please judge the value, 03).** Writing `Base_FovDeg` on all 8 `GameCameraParam` objects changes the live camera; it eases in (88.6 read after 4 s at 90). 70 vs 90 is clearly wider. No 90 cap: 110 reaches 107.9 with the original `Base_SpringFov_Range` "0 90" (109.5 with the range widened; 05). The value survives an area reload in the same stage (the objects persist); a stage change is still untested, so re-apply on stage load to be safe.
- **Camera distance: works, small effect in tight spots (04).** `Rod_LengthBase` x1.4 pulls the camera back, but NarrowAvoid (rod 0.5-1.5) limits it near walls. Still to test in an open area.
- **Grass animation rate (needs your eyes in motion).** Instanced grass `_ins_gr_grass_A/B/C00` has `skipFrame 4 / slowFrame 4`; other grass types 1/4 or 1/1. Frame-diff shows grass changes on every frame at any value, but skip/slow = 1 gives a smaller per-frame change (4.5-5.9 vs 6.7-8.3), i.e. smoother steps. Smoother vs calmer is a taste call; screenshots cannot settle it.
- **Area SkinningSettings:** the area env `_default_env_*_sk` uses `maxskip 4` (the others 1). Candidate for the feather/foliage wind tick; A/B on Trico pending. Garden A/B (skip 4 vs 1 on all SkinningSettings, motion normalised by a static stone region, 3 repeats): the ivy moves slightly more per frame at skip 1 (ratio ~12 vs ~10), the boy and grass do not change. Small; judge on Trico feathers by eye. `particleAutoDecay` is 1 by default; its test needs a dust/spray scene.
- **Values seen (s15 dump):** condor FurType `furForce 0.1`, `furParam "0.05"`, `furScale 0.95`; butterfly SkinMesh `windEnabled 0`; tree VertexCluster `simFadeLength 20`, `simFadeDemoLength 60`; camera `Base_FovDeg 70`, `Rod_FovByPitchDeg 70`, `Rod_LengthBase 4`.
- **Performance:** GardenTerrace runs 42-50 FPS (s15 interior 60). Log: ~167k draws/s, draw pipe stage A waited ~36%, ProcessCommands ~30%: CPU/command-processor bound, not GPU. GPU-heavy upgrades (grass density, full-res effects) are affordable; anything adding draws is not.
- **Tooling:** stage warp from Lua: `_TLGQA_UNLOAD=true GameSetupTbl:StartStage('<stage>','<area>')` (area names in `stages/<stage>/regionsetting.lua`), ~40 s load.

---

## Not covered yet (gaps)

This report came from static sources. Still missing:
1. **Live property dump** (`GetObjPropsInfo` on all env objects, per area): actual values and properties that never appear in Lua. Needs the game running; I can do it with a live-Lua script while you play or from a save.
2. **Frame capture pass audit**: every pass with its resolution, format and cost, to find more half-res or low-quality passes (we only did fog, AO, bloom, shadows). Needs one RenderDoc or our own frame dump per area type (outdoor, cave, water).
3. **Shader-level look** at the lighting (tiled 32x32), SSR and fur shaders for hard-coded sample counts or quality constants that a shader patch could raise.
4. **PS4 Pro mode test** (section 10b).
5. **Per-effect particle and fur data** in `.bod` files (only camera data decoded so far); needs a `.bod` decoder.
6. **Lua coverage**: the 790 scripts were searched by keyword, not read end to end; a sweep of the render, env and camera folders could find more.

## Suggested order (for the plan)

| # | Step | Lever | Effort | Expected gain |
|---|---|---|---|---|
| 0 | Live `GetObjPropsInfo` dump of all env objects in 3 areas; wrap `SetGI.SetEnv` so our overrides apply on area load | A | 0.5 day | Unblocks everything, fixes "read on load" knobs |
| 0b | PS4 Pro mode test (`IsNeoMode` = 1), compare settings and passes (section 10b) | B | 0.5 day | Unknown, possibly large |
| 1 | Texture `materialMaxLod`/`normalMaxLod`/`LoadBias` test, fix the lab label | A | 1 h | Possibly large (sharper textures) |
| 2 | FOV and camera distance sliders | A | 1 day | Large (top complaint) |
| 3 | `skinWindMaxFrameSkip` 0, feather vertex blur toggle | A | 1 h | Medium (Trico) |
| 4 | Grass fade/density, foliage `simFadeLength`, camera `lodBias` | A | 0.5 day | Medium to large |
| 5 | `particleAutoDecay` 0; full-res fog/particles/AO (`half_scale`, `ssaoResType`) | A+B | 1 day | Medium |
| 6 | Light-shaft temporal/jitter, probe `HighResolution`, `ssrRefScale`, water roughness | A | 0.5 day | Small to medium |
| 7 | SMAA T2X Ultra for non-RTX | A | 1 h | Medium for those users |
| 8 | Finish 60 FPS (in-game DLSS FG parked) | C | days | Large for motion |
| 9 | ReShade RTGI evening experiment | none | 2 h | Unknown, taste |
| 10 | AI texture pack (GNF tool + cache replacement + 5090 batch) | D | 1 to 2 weeks | Medium to large |
| 11 | RT shadows + RTAO in the emulator | B | 3 to 6 weeks | Medium, high risk |

Note: steps 3 and 5 are partly built on `feat/graphics-lab` (0572a21) but not tested in game. Every step 1 to 7 can be tested live with `TLG_LUA_EXEC` before any code goes in, using the still-camera base/test/base screenshot method, and then exposed as a lab row and, if proven, a menu item (default low/game value with advice text, per the "max tweakable" rule).

### Foliage and hair wind (2026-10-22, s04 GardenTerrace)

- **Grass, user-confirmed:** `skipFrame=1 slowFrame=1` on all `GrassType` objects (instanced grass ships 4/4, a few types 1/4) makes grass update every frame instead of every 4th. The user calls the gain "huge". Make it an option.
- **Boy's hair, user-confirmed:** the wild motion comes from the scene wind (WindBreeze through WindEmitter volumes), received through `WindRecvExt2311_SNt001*` on `boyA_model`. Setting its `MaxStrength` 1 → 0.3 gives slow, calm motion. `windEnabled=0` on the `boyA_hair` SkinMesh makes the hair fully still.
  - No visible effect, though some show up in pixel diffs: the hair's VertexClusterParam (`VertexClusterParam2311_SNt006`: windIntensity, texSpeed, texAmount, forceAmount, windAnimIntensity, windInterp), `windAnimationEnabled`, and the global SkinningSettings `skinWind*`.
  - **Todo (user decision):** a "Boy wind (hair + clothing)" setting in the menu and the launcher (user-confirmed: the receiver drives his hair AND his clothing) that sets the boy receiver's `MaxStrength`. Default 1 (original, may be an intended design choice); the help text advises a lower value, because the user finds the original "hurricane"-like; 0.3 is calm and 0.5 looks good (recommended). The menu marks the original value. Re-apply it per area, since receivers are probably recreated.
- **Not a frame-rate bug:** the per-frame diff at 30 FPS (7.4) is about twice that at 60 FPS (4.1), so the hair wind is time-based. Hair updates every frame, and so does cloth.
- **Trico hair and feathers:** to do (user request). Find Trico's own wind receiver and SkinMesh objects with `windEnabled` and A/B `MaxStrength` by eye. The boy's receiver probably does not affect Trico; a separate setting may be needed.
- **Trees and leaves, user-confirmed:** leaf motion comes from `VertexClusterParam.windIntensity` (451 params in s04; original 0.6 on 424, 2.5 on 24, skip the boy's `SNt006`). windIntensity 0 makes the leaves still; a 0.5 factor looks "natural" (recommended). The tree WindRecvExt `MaxStrength` cap (even 0.01) has no effect; texAmount 0 was unclear. **Todo:** a "Tree wind" factor setting (original 1 marked), re-applied per area.
- **Tree shadow shimmer, user-confirmed:** shimmer remains even with leaf wind at 0. The shadow maps render every frame (about 1270 shadow passes per 120 frames at 8192, about 10 per frame), so the shadows do not run at a lower frame rate. The cause is shadow resolution aliasing on small leaf shadows: shadow resolution 8192 "drastically reduces" the shimmer and the motion (user). Todo: mention foliage shimmer in the Shadow resolution help text as the main gain (cost about −8 FPS at 8192 earlier) and reconsider the recommended value. Still open: the patchy "strange" tree lighting.
- **User verdict (tree wind 0.5):** shadow resolution is very impactful on tree shimmer and a major improvement with DLAA, and also with SMAA. Above some level it is a matter of taste. The help text should state foliage shimmer as the main gain, with any AA.
- **SDR tree-top over-saturation:** sunlit leaves clip per channel (R ≥250 in 56%, G in 27% of bright pixels), which shifts green toward yellow-white. Bloom 0% and sharpening off have no impact (user). HDR probably fixes it (untested). The SDR fix would be a softer tonemap or exposure shoulder for highlights (find the game post-process parameter).
- **Settings walkthrough, ambient occlusion (user):** a higher AO adds atmosphere, but above 150% it looks unnatural. Decision (user): no cap, the current slider stays.
- **Settings walkthrough, motion blur (user):** fine at 100%. The slight trailing comes from the game's motion blur itself (the same with SMAA, gone at 0%), not from DLAA. Ignored for now; tested at 30 FPS.
- **Settings walkthrough, chromatic aberration:** the current slider maximum (0.2, `s.chromatic*0.002` in tlg_lua.cpp) is invisible to the user. Tested live on `_default_env_pp.chromaticAberrationAmount`: 5 is extreme, 1.0 is too strong on bright trees, and 0.5 is subtle (hard to see on a dark wall; it is strongest on high-contrast edges). Todo: slider range 0–1.0 (100% = 1.0), default 0 = game original.
- Chromatic aberration correction (user, 2026-10-22): the earlier "no difference" came from moving the wrong slider. At 100% (0.2) the effect is visible. Whether to widen the range to 0–1.0 is on hold until the user decides.
- Chromatic aberration, decided after screenshots (2026-10-22): 0.2 is invisible; 1.0 gives clear blue/cyan fringes and doubled-looking fine detail. Slider changed to 100% = 1.0 (tlg_lua.cpp). Not built yet. **Dropped** (user decision 2026-10-22): the game ships with it off; slider removed from the in-game menu, launcher and presets (key `chromatic_add`).
- Vignette and chromatic aberration (user 2026-10-22): both stay in the menu; the help text advises 0% (off, the game original).
- Game sharpening (user 2026-10-22): the original is 100% (sharpenScale 1.25). 200% and 400% look no different from 100%. The help text now recommends 100%.
- Game sharpening, decided (user 2026-10-22): measured at 500% versus 100% at a wall, edge contrast is only 3% higher and the difference is not visible. Slider range is now 0–100% (100% = original = maximum, on by default, recommended). Older ini values above 100 are capped. Not built yet.
- SDR tree-top clipping (2026-10-22): the menu color adjustments cannot fix it, because they run after tone mapping. The game tone curve (Hable) can: the textbook curve cuts clipped pixels from 10% to 4.3%. The user rejected it because it changes the whole image. The SDR fix is parked; HDR is the route.
- Character detail distance (2026-10-22): no visible impact here, because only the boy is in the scene and he stays close to the camera. Retest with Trico in view before deciding.
- Texture LOD bias, Lab tab (2026-10-22): at -3.00 versus +1.00, the grass has 31% more fine detail and half the overexposed pixels. The user sees the impact mainly on grass. The default stays -1.00; shimmer in motion at -3.00 is not yet checked.
- Anisotropic filtering, Lab tab (2026-10-22): measured over 5 screenshots per setting on a stone path, Game versus 16x shows no difference (-1.3% to +0.6%, within noise); the user sees none either. Not yet known whether the live switch takes effect at all. A restart test would settle it: start once with Game in tlg.ini and once with 16x.
- Fog, particles and AO resolution, Lab tab (2026-10-22): the switch works (the log shows half-res going from 1x to 2x). It is not visible here: hair edges +4.8%, haze and grass unchanged, and the user sees no difference. The default stays Game (half).
- Wind animation frame skip, Lab tab (2026-10-22): the user sees no effect. The row only writes _default_env, while one area environment is at 4, so it may miss the active area. The real wind fixes come from the grass skipFrame, boy MaxStrength and tree windIntensity settings; this row adds little.
- Trico wind on fur/feathers (2026-10-22, live, user eye check): no visible change from Condor_SwingExt_slaver* MaxStrength (0 / 100, orig -1), _sk.skinWindIntensity (0.2 -> 2.0) or FurType.furForce (0.1 -> 1 -> 20). None of these drive Trico's fur/feathers live (they may only be read at load). Trico wind setting: not possible with these params; candidate next tests: global Wind/WindEmitter strength, SwingType.
- Lab bug (2026-10-22): single-number Lab rows (water roughness, draw distance, height fog amount, sky fog scale, LOD limits, color table mix) threw 'operator # is not supported for number' once moved off default, so they never applied. Fixed in tlg_lab.cpp (#w only for vectors); needs build. Earlier conclusions from these rows are invalid.
- Water roughness (2026-10-22, s04 FA garden puddles, value written directly: 0.01 orig, -0.99, +1.01, 5-shot averages): no visible change; the water and the dry floor differ by the same amount (noise). Does not affect the shallow puddle water here; test again on deep water before deciding.
- Smoke check for fog/particles/AO resolution (2026-10-22, s04 smoke pot room, Half vs Full confirmed in log, 5-shot averages): no visible gain at Full; smoke edges equally soft, no extra detail in smoke regions. Confirms the fog result: default Half, row is a drop candidate (user decides).
- Fog density (2026-10-22, s04 smoke pot room, pp.fogParam written directly; game value '100 100 1 1', part 3 = 1): 0% clears some distant haze (edge detail 63 -> 126, doorway and pillars clearer); 300% adds no visible fog (likely capped at 1 = full). Most haze in this room is the smoke particles, not distance fog. Row only useful downward (less fog); 100% is the original.
- Height fog (heightFogMax 0/1/3) and fog distance (fogParam parts 1,2 at 25/100/400%) in the s04 smoke pot room (2026-10-22, written directly): no visible change. Ground fog sits below the visible floor here (top -1, bottom -100); the haze is smoke particles. CORRECTION to the fog density bullet above: edge detail varies 57-184 at unchanged settings (Trico moving behind smoke), so the '0% clears haze' result is within noise and not reliable. All fog rows: retest outdoors (open distance, no smoke) before deciding.
- Fog rows outdoors (2026-10-22, s04 cliff view, still camera, both baselines identical, direct writes; area fogParam '600 100 0.2 1', heightFogMax 1): Fog DISTANCE is the real fog control: 25% = heavy white haze over the cliff (brightness 112->142, distant detail gone); 400% = clearer, darker, crisper distance (112->101). Fog DENSITY (part 3: 0 and 300%) and HEIGHT FOG amount (0 and 300%): exactly no change (stats identical to baseline). Advice: keep Fog distance (100% original; higher = clearer distance, lower = more haze); density and height fog rows are drop candidates (no visible effect in two areas; user decides).
- Fog distance advice (user, 2026-10-22): 100% (original) looks best; default 100%, advise keeping it. The slider scales the game's per-area value (here near 100 / far 600, smoke room 100/100).
- AF A/B (2026-10-22, pillar hall, still camera, 5-shot averages, client 2792x1520, fog/particles/AO res Full in both runs): Game (aniso=0) vs 16x. Edge variance within ~3% on every region (right floor 3723 vs 3597, left floor 2627 vs 2608, front floor 1944 vs 1938, pillar 4742 vs 4700); crops of the low-angle floor look the same. No visible AF gain at this spot. Keep the setting (cheap); default 16x, original = Game. Retest on a long low-angle floor or bridge if wanted. Crops: scratchpad live/af_cmp4.png, af_cmp4L.png.
- Finding from the AF test: fog/particles/AO resolution Half (half_scale=1) vs Full (2) lowered edge variance about 25% across the whole frame, the camera-facing pillar included (pillar 4742 vs 3511, right floor 3723 vs 2625). The AO at half resolution likely softens the image everywhere. This contradicts the earlier smoke-room result (half/full no gain), which only looked at smoke. Retest half vs full at a still spot before deciding the default or dropping the setting. Backed by screenshots: live/af_cmp2.png (top Full, bottom Half).
- Half vs Full fog/particles/AO resolution (2026-10-22, pillar hall, same still camera, AF 16x in both, 5-shot averages; Half = af16 run, Full = afc run): Full is sharper everywhere. Edge variance Half vs Full: pillar 3511 vs 4700, right floor 2625 vs 3597, left floor 1943 vs 2608, grass 1593 vs 1801, boy 1461 vs 1593. Visible in crops: stone micro-detail and AO contact shading are crisper at Full; Half looks slightly washed. Corrects the smoke-room conclusion: keep the setting, change default to Full (original = Half). GPU cost not measured yet (30 FPS cap). Screenshots: scratchpad live/hs_pillar.png, hs_boy.png (left Full, right Half).
- User check of the Half vs Full crops (2026-10-22): a clear difference only on the stairs right of the pillar (af_cmp2.png, top Full, bottom Half: step edges and contact shading). Pillar and boy crops (hs_pillar, hs_boy) show no clear difference to the eye, despite the edge-variance numbers. So the gain is local (AO on steps and crevices), not a whole-frame sharpening. Default still open: Full suggested if the GPU cost is small; measure that first.
- Correction to the Half vs Full result (2026-10-22): the softness was a window artifact, not the setting. Fresh Half run, window client set to 2792x1520: edge variance close to Full (stairs 3566 vs 3597, pillar 4513 vs 4700, floor L 2448 vs 2608, grass 1749 vs 1801). The user saw the image change live the moment the window was resized from 2789 to 2792 wide. The game renders 3840x1600 and the presenter scales it into the window; an off-size window right after launch (or before a resize) gives a visibly softer image. The af16 (soft) shots were taken in that state. TODO: investigate the present scaling after launch vs after a resize (possible bug: wrong filter or size until the first resize event). GPU load at 30 FPS: Full 27.5% at 2476 MHz, Half 32.5% at 2754 MHz: within noise, no measurable cost. Half vs Full: small difference, user decides; drop candidate again. Screenshot: live/stairs3.png (top Full, middle Half fresh, bottom Half before resize).
- User note (2026-10-22): the user knows this soft/sharp change from switching to another window (focus change), not only from a resize. The port code has no focus-based quality switch (grep for focus in gpu/: only mouse capture and menus), so the cause is likely in the presenter/swapchain (recreate on resize or focus) or outside the port (driver, DWM). Note: all captures are taken while VS Code has focus. TODO test: 5-shot sets with the game focused vs unfocused, and before vs after a resize, same spot; then check whether a swapchain recreate restores the sharp image.
- User verdict (2026-10-22): the soft/sharp change on focus or resize is likely a Windows thing; low priority, the test plan above stays parked. For captures: set the client size once after launch (2792x1520) before taking shots, so A/B runs share the same window state.
- Water roughness on deep water (2026-10-22, pond with lily pads, still camera, 5-shot averages, client 2792x1520): offset 0 vs -1 vs +1 (game value 0.01, restored). Differences below shot-to-shot noise (open water 1.0/1.4 vs self-noise 1.25; reflections 1.5/1.8 vs 1.85). No visible effect at this top-down angle, nor earlier on puddles. Drop candidate (user decides); last chance would be a low angle with sky reflection. Screenshots: live/wd_cmp.png, wd_crop.png (top 0, middle -1, bottom +1).
- Menu validation (2026-10-22, all tabs captured: live/menu1.png, menu2c.png, m_imagec.png, m_perfc.png): every saved setting matches tlg.ini (display, VSync, HDR off, FSR1 off, CAS 100%, DLAA on, SMAA off, shadow 8192 = shadow_scale 4, game effects 100%, CA 0%, frame limit Unlimited, frames queued 1, async shaders, mouse, LOD bias -1.25 = lod_bias -5, AF 16x, fog/particles/AO Game (half), wind skip Game). Lab: the number-row fix works, every row shows its game value, no errors. Issues: (1) three Lab rows show "(not found)": Fog near intensity, Fog far intensity, Height fog intensity (wrong property names, or not in this area); fix or remove those rows. (2) Water roughness game value shows as 0.0099999998; format it like the other rows (%g). (3) The Performance tab shows no engine frame rate (this run uses TLG_FPS=30, the ini has engine_fps=120); the 30 FPS cap is not visible in the menu. (4) Default markers are still missing on most settings (only "Game (half)" and the "100% = as shipped" header); that todo stays open.
- Water roughness live swap while the user swims (2026-10-22, deep pond): -1/+1 alternated 6 s apart, then the extremes -5/+5; the log confirms every write landed. User: no impact. So waterRoughnessOffset has no visible effect on puddles, deep water from above, or while swimming. Drop candidate (user decides). Game value 0.01 restored.
- Water roughness: **dropped** (user decision 2026-10-22). No visible effect anywhere tested; the Lab row stays commented out in lab.txt.
- Draw distance (far plane, _trico_camera.maxz, game 2000) live test (2026-10-22, wide ruins with far cliffs, still camera): the property works. At 300 and 50 the far cliffs behind the ruins are clipped (sky shows through); at 8000 the image matches 2000, nothing new appears. So raising it gives no gain here, lowering only removes background. Keep the row for testing, default 100% (original); candidate to drop or hide. Screenshots: live/dy_grid.png (top 2000 | 300, bottom 50 | 8000).
- Mesh LOD limit (_rcfg.normalMaxLod, game 4) and material LOD limit (materialMaxLod, game 1) live swaps (2026-10-22): writes confirmed in the log (mesh 4/0/8, material 1/8). Screenshot A/B failed (the camera follows Trico; drift exceeded the change). Waiting on the user's visual verdict; retest with a still camera.
- **Shadow split distances (2026-10-22, wide ruins spot, overcast):** game value 10/30/150/2000. Cascades at 50% / 100% / 300% (far plane kept at 2000) gave a pixel diff of 6.07 / 6.80 against base, and the camera drift alone was 6.10, so the measured effect is zero. Crops (sp_row.png, sp_crop.png) show no shadow change, but this spot has almost no direct sun shadows, so the test is **inconclusive** rather than a drop. Retest in direct sun with hard shadows near the camera (cascade 1 to 2 edge). The game value was restored. The far-split (%4) row is still untested.
- **Draw distance retest (2026-10-22, Trico by the tower ruins, camera moving):** a live cycle of maxz 2000/300/150/8000 (dz3_grid.png). At 150 (7.5%) the background behind the arch is clipped to sky; 300, 2000 and 8000 look the same apart from camera movement. Pixel diffs are useless here because the camera drift is about 20. This confirms the first test: lowering clips, and raising above 2000 adds nothing at either spot. The default stays 100%, and 400% has no proven benefit, so the max could be capped lower (user decides).
- **LOD + shadow split live cycle (2026-10-22, Trico at the tower ruins, camera moving):** mesh LOD 0/4/8 and material LOD 0/1/8 (q_lod.png), then cascades ×0.5/×1/×3 and far 500/6000 (q_sp.png). All writes were confirmed in the log. At screenshot scale nothing visibly changes in either set; the scene is again overcast and in shade, so the split result stays inconclusive for hard sun shadows. Waiting on the user, who watched live. Note: the game value for splits differs per area (10/30/150 at the grass ruins, 14/42/210 here), so the Lab % mode (relative to the game value) is correct and a fixed absolute value would be wrong. The game values were restored (14/42/210/2000, LOD 4/1).
- **Mesh/material LOD verdict (2026-10-22, user live at the tower ruins/rope bridge):** the user switched by hand between MIN (0/0), game (4/1) and MAX (8/8) several times and saw "nothing really". Earlier screenshot sets (q_lod.png, r_all.png) also show no difference. **Drop candidate** (user confirmed visually). The game values were restored (4/1).
- **Shadow cascade verdict (2026-10-22, rope bridge above the tower):** the user toggled MAX (×10 = 140/420/2100) against NORMAL (14/42/210) several times and saw a definite difference. Screenshot cs_cmp.png (top MAX, bottom NORMAL): with MAX the tower and cliff get much more shadow and depth (darker self-shadowing, more relief); with NORMAL they look flatter and lighter. This is because the distant tower/cliff now falls in a finer cascade instead of the last coarse one (210–2000). The trade-off is that close-range shadows get softer, because cascade 1 covers 10x the area. **Keep**, as a relevant setting. Still to determine: a sensible max (×3–×5?), close-up softness on the boy/Trico, and GPU cost. The third split must stay below the far value (the menu must enforce this). The game value was restored.
- **Cascade MAX user note (2026-10-22):** at ×10 the user sees some flickering but finds it better overall. The flicker is probably shadow shimmer, because cascade 1 now covers 10x the area, so the shadow texels are coarse and crawl when the camera moves. Next: try ×3 and ×5 as a compromise (detail at a distance without flicker), then choose the default and max.
- **Cascade decision (2026-10-22, user):** default stays NORMAL (100%); the setting stays adjustable. The lab.txt row range went from 50–300% to 50–1000%, and the help text names the trade-off (depth at a distance vs soft/flickering up close).
- **LOD retest after reload (2026-10-22, user, ruins with pillars):** held normalMaxLod/materialMaxLod at 0/0 and 8/8 across a checkpoint restart (the log shows the game resets them to 4/1 on load, and the hold set them back within 1 s). Close-up of a stone pillar (lod_cmp.png, lod_crop.png): stone texture, relief and moss are equally sharp at 0 and 8. **Drop candidate stands** (now also tested on load). Still untested: normalLoadBias/materialLoadBias. The lab.txt labels were corrected to "Normal map mip limit" / "Material mip limit". The game value was restored (4/1).
- **Far shadow distance verdict (2026-10-22, user, ruins with pillars):** live swap of 200 (×0.1) vs 20000 (×10), 6x every 5 s; the user sees no impact. Probably in this scene the cascades (14/42/210) already cover everything shadowed, or distant shadows disappear in the haze. **Drop candidate.** Of the splits, only the cascade setting is relevant (see the cascade verdict). The game value was restored (2000).
- **Character detail distance verdict (2026-10-22, user, Trico far away):** live _default_env_sk.viewDistanceScale (game 0.139/4.5/1.02) at 25% / 400% / 100%. At 25% the boy turns "balloon" (low-detail model up close: the setting works, but only downward); Trico unchanged. 400% vs 100%: no change. **User verdict: drop** (no gain above the original; lowering only makes it worse). Game value restored. Slider removed from the in-game menu, launcher and presets (2026-10-22, key `lod`).
- Trico feather wind (2026-10-22): found it. The SwingType Influence{Plus,Minus}{Min,Max} values on all 54 condor SwingTypes (game/objects/chara/skin/condor*/..._swing, base 0.1/3) control it. At x0 the feathers are still (user confirmed live). Not the cause: FurType.furForce, TricoWindNormal (particles only), Condor_SwingExt MaxStrength, _sk.skinWindIntensity. Candidate menu row: Trico feather wind %, default 100.
- Trico feather wind, follow-up (2026-10-22): the SwingType Influence also drives Trico's hair (user confirmed: MIN = hair still). At MIN some small feathers still move. Candidate: the TricoWindNormal particles (wind_*>particles>chara>condorA>cdrA_feather_head_*), a separate particle wind system. Menu row 'Trico feather wind' added (Effects, 0-400%, default 100 = normal, built). Wind settings rule (user): all wind sliders use the same scale, 100% = normal (game standard) = default.
- Trico small feathers (neck, wing; 2026-10-22): they keep moving with the SwingType Influence at 0 and do not react to the TricoWindNormal particle airResist (those are shed loose feathers). SwingParam has no wind value (friction/hardness/angle limits only). User: probably motion-related, ignore.
- Focus/resize sharpness issue (2026-10-22): user decision DROP (was parked).
- FOV after a stage change (2026-10-22): user-tested OK, the FOV setting survives a stage change.
- FOV (2026-10-22, live): 50/70/110 all apply (live camera reached 109.9 at 110). It does not ease in while the menu is open (game paused). User: 50-110 reasonable range. Menu+launcher setting 'Field of view' added (Enhancements, default 70 = as shipped, 50-110), applied to every GameCameraParam (20-21, grows with loads; relisted every 20 ticks). Not built yet.
- FOV fights the game (user, 2026-10-22): with a changed FOV the camera keeps zooming in and out. Likely the game eases back toward its own per-area/spring FOV while our periodic re-apply (every 20 ticks) pushes the setting again. Open: find the param the game eases toward; write on change instead of overwriting periodically, or scale the target FOV instead of replacing it.
- Particle auto decay (2026-10-22): user decision IGNORE (not tested).
- **User drop calls (2026-10-22):** fog density, height fog amount, half-scale (fog/particles/AO resolution), water roughness, wind frame skip, draw distance (far plane) row = DROP. Evidence: fog density/height fog zero effect outdoors; water roughness no effect (deep pond, live swap); wind skip no visible effect; draw distance >100% adds nothing (dz3_grid.png). Done: lab.txt rows commented out ("# dropped 2026-10-22"); fixed Lab rows half_scale + wind_skip removed from the menu and no longer read/saved from tlg.ini (stay at game defaults 1 / -1; plumbing kept).
- **User drop call (2026-10-22): texture mip limits** `_rcfg.normalMaxLod` / `materialMaxLod` = DROP. Evidence: lines above (no change after reload, pillar close-up equally sharp at 0 and 8). Lab rows commented out ("# dropped 2026-10-22").
- **User drop call (2026-10-23): fog color table mix** `_default_env_pp.clutAlpha` = DROP (art direction, not quality; user does not care about it). Lab row commented out ("# dropped 2026-10-23").
- **User drop call (2026-10-23): sky fog scale** `_default_env_sb.scaleForFog` = DROP (user decision, untested in game). Lab row commented out ("# dropped 2026-10-23").

## Texture pack investigation (2026-10-22)

**Inventory** (all 8 game psarcs + patches, header-only scan, `scratchpad/gnf_inventory.py`): 5,231 unique `.gnf`, 10.7 GB uncompressed (the earlier "17,019" counted duplicates). One texture per GNF; T# header at 0x10.

| Group | Count | MB | Typical | Upscale? |
|---|---|---|---|---|
| bg3 (main ruins) | 1392 | 2738 | BC1@2048, BC5@1024 (normals) | yes, main target |
| chara (boy, Trico) | 554 | 572 | BC3/BC1@1024-2048 | yes (Trico fur is shader-driven: smaller gain) |
| props / village / bg2 | 773 | 1392 | BC1/BC5@512-2048 | yes |
| ui + subtitle | 835 | 820 | BC7@1024/1920 | yes, cheap win for 4K UI |
| stages (lightmaps, R32 heightmaps, RGBA16F) | 450 | 3918 | data, not images | no |
| probes, clut (LUTs), globalshadow, cubemaps | 790 | 965 | BC6/LUT/R32 | no |

Candidates: about 3,550 textures, about 5.5 GB. A 2x pack is about 4x that (≈22 GB host VRAM if all resident); 4x is ≈16x (not viable as a whole). Many bg3 surfaces are already 2048, so 2x is the sensible target; 4x only for selected hero surfaces and the UI.

**Route 1: file-level (psarc/loose files): rejected for upscaling.** The game parses the psarcs itself (its own Fios2 prx, overlay base -> `p` patch); our runtime only serves raw reads. Without `trico_boot.psarc` the game reads a loose `/app0/data_trico`, so extracting plus a patched tree works for **same-size** edits. But a larger GNF lands in guest memory (Garlic heap, already maxed at 4064) and the game's streaming budgets: 4x the memory per 2x texture. Also `scripts/mods.py` is a GoW3 leftover (expects `dvdroot_ps4`) and does not reach TLG data at all (separate finding).

**Route 2: host texture-cache replacement by content hash (recommended).** The hook is `TextureCache::RefreshImage` (texture_cache.cpp ~1153): on the first upload of a sampled image, XXH3 the guest data (it already hashes there) + format + size. Guest memory and the game stay untouched; the high-res copy lives only in host VRAM (5090: 32 GB).
1. **Dump mode** (`TLG_TEX_DUMP=1`), about 1 day: after `DetileImage`, write `<hash>.dds` (BC blocks as-is, so no encoder needed) for sampled textures ≥256 px. An offline name map is possible: the GNF payload in the psarc should equal the guest bytes, so `gnf_inventory.py` can hash every file and map hash -> path (to verify). That gives a full dump without playing through, and categories for free.
2. **Replace mode**, about 3-5 days: load `<pack>/<hash>.dds` into a separate host-only image (own extent and full mips); in texture binding, swap in its view for sampled-only use (never render target/storage). VRAM budget + LRU, menu toggle "Texture pack" (default Off until a pack exists). Risks: shaders that `texelFetch`/gather with guest-size coordinates (must skip those images); textures whose mips stream in later change hash (the dump shows whether TLG does this); aliasing images.
3. **Pack build**, 2-4 days plus QA: decode BC -> upscale on the 5090 (Real-ESRGAN/UltraSharp for albedo; normals BC5 with a normal-safe model or Lanczos + renormalize; BC4 masks and alpha cutouts (feathers, foliage) need a separate pass) -> encode BC7/BC5 with mips (NVTT or Compressonator CLI). Then visual QA per area.

**Total:** about 1.5-2 weeks for a 2x environment + UI pack, matching the earlier estimate. A 1-day dump-mode spike first would confirm hash stability, offline name mapping and the number of textures actually used per area before committing.
