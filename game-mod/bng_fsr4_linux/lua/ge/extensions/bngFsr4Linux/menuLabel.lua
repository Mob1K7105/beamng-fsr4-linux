-- Renames the "FSR 3 (custom)" anti-aliasing entry while the BeamNG FSR 4 Linux layer replaces that upscaler.
-- Game Lua cannot read the environment, so the layer reports itself in /temp/bng_fsr4/status.json, which it
-- rewrites about every 5 s. A missing, failed or stale (> 30 s) status leaves the menu as the game ships it.
local M = {}

local customKey = 'ui.options.graphics.antialiasMode.fsrCustom'
local fsr4Key = 'ui.options.graphics.antialiasMode.bngFsr4Linux'
local liteKey = 'ui.options.graphics.antialiasMode.bngFsr4LinuxLite'
local statusFile = '/temp/bng_fsr4/status.json'
local pollInterval, staleAfter = 5, 30

local active = false
local lite = false -- the layer runs FSR 4.1.2 Lite (status variant "lite")
local sinceCheck = pollInterval -- check on the first update

-- Returns whether the layer is active, and whether it runs Lite.
local function layerActive()
  if not FS:fileExists(statusFile) then return false, false end
  local status = jsonReadFile(statusFile)
  local ok = type(status) == 'table' and status.ok == true and type(status.time) == 'number'
    and os.time() - status.time < staleAfter
  return ok, ok and status.variant == 'lite'
end

-- Wraps getModes once per option table; buildOptionHelpers can create a new table, so this is re-checked.
local function wrapOption()
  if not core_settings_graphic then return false end
  local ok, option = pcall(core_settings_graphic.getOptions, 'GraphicAntialiasType')
  if not ok or type(option) ~= 'table' or type(option.getModes) ~= 'function' or option.bngFsr4LinuxWrapped then
    return false
  end
  local getModes = option.getModes
  option.getModes = function(...)
    local modes = getModes(...)
    if active then
      for i, value in ipairs(modes.values or {}) do
        if value == customKey then modes.values[i] = lite and liteKey or fsr4Key end
      end
    end
    return modes
  end
  option.bngFsr4LinuxWrapped = true
  return true
end

local function refreshMenu()
  local settingsModule = extensions.core_settings_settings
  if settingsModule and settingsModule.refreshTSState then
    settingsModule.refreshTSState(false)
    settingsModule.notifyUI()
  end
end

local function onUpdate(dtReal)
  sinceCheck = sinceCheck + (dtReal or 0)
  if sinceCheck < pollInterval then return end
  sinceCheck = 0
  local wrapped = wrapOption()
  local now, nowLite = layerActive()
  if now ~= active or nowLite ~= lite or (wrapped and now) then
    active, lite = now, nowLite
    refreshMenu()
  end
end

local function onSettingsChanged()
  if wrapOption() and active then refreshMenu() end
end

M.onUpdate = onUpdate
M.onSettingsChanged = onSettingsChanged

return M
