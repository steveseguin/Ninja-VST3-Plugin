-- Run in REAPER (Actions > ReaScript > Load, or pass this file on its command line).
-- Creates a NEW test project tab. Uses generated tone only; does not record a mic.
-- The test project's master is silent. Results/projects go under build/reaper-validation.
local source = debug.getinfo(1, "S").source:sub(2):gsub("\\", "/")
local root = assert(source:match("^(.*)/tools/tests/")) .. "/build/reaper-validation/"
reaper.RecursiveCreateDirectory(root, 0)
local log = assert(io.open(root .. "result.txt", "w"))
local function report(text) log:write(text .. "\n"); log:flush() end
local function base64(data)
  local alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
  return ((data:gsub('.', function(x)
    local byte, bits = x:byte(), ''
    for i=8,1,-1 do bits = bits .. (byte % 2^i - byte % 2^(i-1) > 0 and '1' or '0') end
    return bits
  end) .. '0000'):gsub('%d%d%d?%d?%d?%d?', function(x)
    if #x < 6 then return '' end
    local value = 0
    for i=1,6 do if x:sub(i,i) == '1' then value = value + 2^(6-i) end end
    return alphabet:sub(value+1,value+1)
  end) .. ({'', '==', '='})[#data%3+1])
end
local function setState(track, fx, mode, id)
  local state = '{"mode":"' .. mode .. '","streamId":"' .. id ..
    '","roomName":"","password":"secret","salt":"reaper-custom-salt",' ..
    '"webBaseUrl":"https://studio.example.test/","handshakeUrl":"wss://wss.vdo.ninja"}'
  local chunk = string.pack('<I4I4', #state, 1) .. state .. string.pack('<I4I4', #state, 0) .. state
  assert(reaper.TrackFX_SetNamedConfigParm(track, fx, 'vst_chunk', base64(chunk)), 'Cannot set VST3 state')
end
local function value(track, fx, label)
  for i=0,reaper.TrackFX_GetNumParams(track, fx)-1 do
    local _, name = reaper.TrackFX_GetParamName(track, fx, i)
    if name == label then
      local _, text = reaper.TrackFX_GetFormattedParamValue(track, fx, i)
      return text
    end
  end
end

local function runJourney()
reaper.Main_OnCommand(40859, 0) -- new tab
local project = reaper.EnumProjects(-1)
reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(project), 'D_VOL', 0)
reaper.GetSetProjectInfo(project, 'PROJECT_SRATE', 48000, true)
reaper.GetSetProjectInfo(project, 'PROJECT_SRATE_USE', 1, true)
local id = 'reapertest' .. os.time()
local tracks, effects = {}, {}
for i=1,2 do
  reaper.InsertTrackAtIndex(i-1, true)
  local track = reaper.GetTrack(project, i-1)
  tracks[i] = track
  reaper.GetSetMediaTrackInfo_String(track, 'P_NAME', i == 1 and 'Seed tone' or 'Play return', true)
  -- Keep the real-time graph active throughout the test.
  reaper.CreateNewMIDIItemInProj(track, 0, 65, false)
  if i == 1 then
    assert(reaper.TrackFX_AddByName(track, 'JS: synthesis/tonegenerator', false, -1) >= 0, 'Missing tone generator')
  end
  local fx = reaper.TrackFX_AddByName(track, 'VST3: VDO.Ninja WebRTC Bridge (Open Source)', false, -1)
  assert(fx >= 0, 'VDO.Ninja VST3 not found; rescan plugins first')
  effects[i] = fx
  setState(track, fx, i == 1 and 'seed' or 'play', id)
  assert(value(track, fx, 'Custom salt') == 'reaper-custom-salt', 'Salt not restored')
  assert(value(track, fx, 'Web domain / URL') == 'https://studio.example.test/', 'Web URL not restored')
  report('PASS: instance ' .. i .. ' loaded with custom salt/domain')
end
reaper.Main_SaveProjectEx(project, root .. 'loopback.rpp', 8)
reaper.SetEditCurPos(0, false, false)
reaper.OnPlayButton()
local start, peak, ticks = reaper.time_precise(), 0, 0
local phase, windowStart, windowPeak, windows = 1, 0, 0, 0
local bypassed, restoredBypass, cycled = false, false, false
local function poll()
  if reaper.EnumProjects(-1) ~= project then
    if reaper.ValidatePtr(project, 'ReaProject*') then reaper.Main_OnCommandEx(1016, 0, project) end
    report('ABORT: test project is no longer active'); log:close(); return
  end
  local elapsed = reaper.time_precise() - start
  peak = math.max(peak, reaper.Track_GetPeakInfo(tracks[2], 0))
  windowPeak = math.max(windowPeak, reaper.Track_GetPeakInfo(tracks[2], 0))
  ticks = ticks + 1
  if ticks % 100 == 0 then report(string.format('elapsed=%.1f position=%.1f seed_peak=%.6f return_peak=%.6f seed_status=%s play_status=%s', elapsed, reaper.GetPlayPosition(), reaper.Track_GetPeakInfo(tracks[1],0), peak, tostring(value(tracks[1],effects[1],'Status')),tostring(value(tracks[2],effects[2],'Status')))) end
  if phase == 1 and elapsed >= 15 and not bypassed then
    bypassed = true
    for i=1,2 do reaper.TrackFX_SetEnabled(tracks[i], effects[i], false) end
  end
  if phase == 1 and elapsed >= 17 and not restoredBypass then
    restoredBypass = true
    for i=1,2 do reaper.TrackFX_SetEnabled(tracks[i], effects[i], true) end
    report('PASS: live bypass/reenable calls completed')
  end
  if elapsed >= 26 and not cycled then
    cycled = true
    for i=1,2 do for cycle=1,5 do
      reaper.TrackFX_Show(tracks[i], effects[i], 3)
      reaper.TrackFX_Show(tracks[i], effects[i], 2)
    end end
    report('PASS: editors opened/closed repeatedly during playback')
  end
  if elapsed - windowStart >= 2 then
    -- Exclude initial connection and intentional bypass/recovery windows.
    if windowStart >= 8 and (phase == 2 or windowStart >= 24 or elapsed < 15) then
      report(string.format('%s: phase=%d window=%.1f..%.1f peak=%.6f', windowPeak > 0.01 and 'PASS' or 'FAIL', phase, windowStart, elapsed, windowPeak))
      if windowPeak <= 0.01 then reaper.OnStopButton(); log:close(); error('Silent sustained DAW return window') end
      windows = windows + 1
    end
    windowStart, windowPeak = elapsed, 0
  end
  if elapsed < 35 then reaper.defer(poll); return end
  reaper.OnStopButton()
  report((peak > 0.01 and 'PASS' or 'FAIL') .. ': DAW -> WebRTC -> DAW custom-salt audio; peak=' .. peak)
  if peak <= 0.01 then log:close(); error('No WebRTC return audio'); end
  if phase == 2 then
    report('PASS: audio resumed after fresh saved-project load; sustained windows=' .. windows)
    log:close()
    -- Only the isolated command-line test instance opts into closing itself.
    if os.getenv('WEBRTC_REAPER_TEST_QUIT') == '1' then reaper.Main_OnCommand(40004, 0) end
    return
  end
  reaper.Main_SaveProjectEx(project, root .. 'loopback.rpp', 8)
  reaper.Main_openProject('noprompt:' .. root .. 'loopback.rpp')
  local restored = reaper.GetTrack(0, 1)
  assert(value(restored, 0, 'Custom salt') == 'reaper-custom-salt', 'Saved salt changed')
  assert(value(restored, 0, 'Web domain / URL') == 'https://studio.example.test/', 'Saved URL changed')
  report('PASS: saved project reopened with custom settings intact')
  project = reaper.EnumProjects(-1)
  tracks = {reaper.GetTrack(project, 0), restored}
  effects = {1, 0}
  phase, start, peak, windowStart, windowPeak = 2, reaper.time_precise(), 0, 0, 0
  reaper.SetEditCurPos(0, false, false)
  reaper.OnPlayButton()
  reaper.defer(poll)
end
reaper.defer(poll)
end
-- Command-line ReaScripts can run before a fresh host finishes restoring its
-- startup project. Do not create the owned project until startup settles.
local readyAt = reaper.time_precise() + 15
local function waitForHost()
  if reaper.time_precise() < readyAt then reaper.defer(waitForHost) else runJourney() end
end
reaper.defer(waitForHost)
