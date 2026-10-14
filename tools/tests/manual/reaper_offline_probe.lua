-- Run in REAPER via Actions > ReaScript > Load, or pass this file on its command line.
-- Creates its own project tab and synthetic stereo source; never starts playback.
-- Scan the intended VST3 build first. Outputs stay under build/reaper-offline/.
local source = debug.getinfo(1, 'S').source:sub(2):gsub('\\', '/')
local root = os.getenv('WEBRTC_REAPER_OFFLINE_OUTPUT_DIR') or
  (assert(source:match('^(.*)/tools/tests/')) .. '/build/reaper-offline')
root = root:gsub('\\', '/'):gsub('/+$', '') .. '/'
reaper.RecursiveCreateDirectory(root, 0)
local log = assert(io.open(root .. 'result.txt', 'w'))
log:write('RUNNING: offline render validation\n'); log:flush()

local samples = {}
for i = 0, 95999 do
  samples[#samples + 1] = string.pack('<i2i2',
    math.floor(8000 * math.sin(i * 2 * math.pi * 440 / 48000) + 0.5),
    math.floor(5000 * math.sin(i * 2 * math.pi * 997 / 48000) + 0.5))
end
local pcm = table.concat(samples)
local file = assert(io.open(root .. 'source.wav', 'wb'))
file:write('RIFF', string.pack('<I4', #pcm + 36), 'WAVEfmt ',
  string.pack('<I4I2I2I4I4I2I2', 16, 1, 2, 48000, 192000, 4, 16),
  'data', string.pack('<I4', #pcm), pcm)
file:close()

reaper.Main_OnCommand(40859, 0) -- New project tab; existing projects remain open.
local project = reaper.EnumProjects(-1)
reaper.GetSetProjectInfo(project, 'PROJECT_SRATE', 48000, true)
reaper.GetSetProjectInfo(project, 'PROJECT_SRATE_USE', 1, true)
reaper.InsertTrackAtIndex(0, true)
local track = reaper.GetTrack(project, 0)
reaper.GetSetMediaTrackInfo_String(track, 'P_NAME', 'Offline render validation', true)
reaper.SetMediaTrackInfo_Value(track, 'I_RECINPUT', -1)
reaper.SetOnlyTrackSelected(track)
reaper.SetEditCurPos(0, false, false)
reaper.InsertMedia(root .. 'source.wav', 0)
local item = assert(reaper.GetTrackMediaItem(track, 0))
reaper.SetMediaItemInfo_Value(item, 'D_FADEINLEN', 0)
reaper.SetMediaItemInfo_Value(item, 'D_FADEOUTLEN', 0)
local effects = {}
for i = 1, 2 do
  effects[i] = reaper.TrackFX_AddByName(track,
    'VST3: VDO.Ninja WebRTC Bridge (Open Source)', false, -1)
  assert(effects[i] >= 0, 'Scan the VDO.Ninja VST3 bundle first')
end
assert(reaper.TrackFX_SetParamNormalized(track, effects[1], 0, 0))
assert(reaper.TrackFX_SetParamNormalized(track, effects[2], 0, 1))

for key, value in pairs({RENDER_SETTINGS=0, RENDER_BOUNDSFLAG=0,
  RENDER_CHANNELS=2, RENDER_SRATE=48000, RENDER_STARTPOS=0, RENDER_ENDPOS=2,
  RENDER_TAILFLAG=0, RENDER_DITHER=16, RENDER_NORMALIZE=0, RENDER_ADDTOPROJ=0}) do
  reaper.GetSetProjectInfo(project, key, value, true)
end
reaper.GetSetProjectInfo_String(project, 'RENDER_FILE', root, true)
reaper.GetSetProjectInfo_String(project, 'RENDER_FORMAT', 'evaw', true)
reaper.GetSetProjectInfo_String(project, 'RENDER_FORMAT2', '', true)

local function readPcm(path)
  local f = assert(io.open(path, 'rb'), 'Render did not create ' .. path)
  local data = f:read('*a'); f:close()
  assert(data:sub(1, 4) == 'RIFF' and data:sub(9, 12) == 'WAVE', 'Invalid WAV')
  local pos, format, audio = 13
  while pos + 7 <= #data do
    local id, size = string.unpack('<c4I4', data, pos)
    local body = data:sub(pos + 8, pos + 7 + size)
    assert(#body == size, 'Truncated WAV chunk')
    if id == 'fmt ' then format = body end
    if id == 'data' then audio = body end
    pos = pos + 8 + size + size % 2
  end
  assert(format and audio, 'Missing WAV chunks')
  local _, channels, rate, _, block = string.unpack('<I2I2I4I4I2', format)
  assert(channels == 2 and rate == 48000 and #audio == 96000 * block,
    'Expected exactly two seconds of stereo audio at 48 kHz')
  return audio
end

local _, ident = reaper.TrackFX_GetNamedConfigParm(track, effects[1], 'fx_ident')
log:write('REAPER ', reaper.GetAppVersion(), '\nPlugin: ', ident, '\n'); log:flush()
local run = tostring(os.time()) -- Do not overwrite earlier render evidence.
local output = {}
for _, case in ipairs({{'bypass', false, false}, {'publish', false, true}, {'play', true, false}}) do
  reaper.TrackFX_SetEnabled(track, effects[1], case[2])
  reaper.TrackFX_SetEnabled(track, effects[2], case[3])
  local name = run .. '-' .. case[1]
  reaper.GetSetProjectInfo_String(project, 'RENDER_PATTERN', name, true)
  local started = reaper.time_precise()
  reaper.Main_OnCommand(41824, 0) -- Render using latest settings; auto-close dialog.
  output[case[1]] = readPcm(root .. name .. '.wav')
  log:write(string.format('%s: %d PCM bytes, %.3f seconds elapsed\n',
    case[1], #output[case[1]], reaper.time_precise() - started)); log:flush()
end
assert(output.bypass:find('[^\0]'), 'Bypassed reference is silent')
assert(output.publish == output.bypass, 'Publish altered the offline reference PCM')
assert(not output.play:find('[^\0]'), 'Play emitted audio during offline rendering')
log:write('PASS: Publish matches bypass byte for byte; Play is silent offline.\n')
log:close()
-- Keep the owned project for inspection, with both bridge instances bypassed.
for _, fx in ipairs(effects) do reaper.TrackFX_SetEnabled(track, fx, false) end
reaper.Main_SaveProjectEx(project, root .. 'offline-probe.rpp', 8)
