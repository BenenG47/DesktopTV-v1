-- DesktopTVPos v5 (UE4SS 3.0.1 Lua) - Benja's TKA workspace
-- Tells the DesktopTV ReShade add-on where Jodi is, so the second screen's sound fades with her
-- distance to her PC / the TV and comes from the left or right.
--   Ctrl+F8  remember this spot as "Jodi's PC" (stand at her desk, facing the screen)
--   Ctrl+F9  remember this spot as "the TV"   (stand in front of the TV)
-- Spots are saved per map in DesktopTV_spots.txt; the position goes to DesktopTV_pos.txt
-- (both in Binaries\Win64, next to the add-on), about 10 times a second, only on maps with spots.
--
-- v5 (2026-10-22): no background loop any more. v4 ran a LoopAsync + ExecuteInGameThread pair every
-- 0.1 s; at a map change UE4SS crashed inside that hand-over (crash_2026_10_22_17_17_03.dmp: null read
-- in UE4SS.dll on the script's own thread) and the script stopped. Now the work is done inside Jodi's
-- own per-frame update (RegisterHook on her ReceiveTick, like TrueFPS), on the game thread, with the
-- live Jodi the game passes in - no stored objects, no lookups, nothing touched between maps.
-- Every 6th frame only; the key presses just set a flag that the next update handles.

local TAG = "[DesktopTVPos] "
local function log(s) print(TAG .. tostring(s) .. "\n") end

local POS_FILE = "DesktopTV_pos.txt"
local SPOTS_FILE = "DesktopTV_spots.txt"
local EVERY = 6               -- frames between two updates (~10 per second at 60 fps)

local spots = {}              -- spots[map] = { pc = {x,y,z}, tv = {x,y,z} }
local frame = 0
local request = nil           -- "pc" / "tv": save a spot on the next update
local hooked = {}             -- function paths already hooked

local function loadSpots()
    local f = io.open(SPOTS_FILE, "r")
    if not f then return end
    for line in f:lines() do
        local map, kind, x, y, z = line:match("^(%S+)%s+(%a+)%s+(%S+)%s+(%S+)%s+(%S+)")
        if map and tonumber(x) then
            spots[map] = spots[map] or {}
            spots[map][kind] = { tonumber(x), tonumber(y), tonumber(z) }
        end
    end
    f:close()
end

local function saveSpots()
    local f = io.open(SPOTS_FILE, "w")
    if not f then log("cannot write " .. SPOTS_FILE); return end
    for map, t in pairs(spots) do
        for kind, p in pairs(t) do
            f:write(string.format("%s %s %.1f %.1f %.1f\n", map, kind, p[1], p[2], p[3]))
        end
    end
    f:close()
end

local function valid(o)
    if o == nil then return false end
    local ok, r = pcall(function() return o:IsValid() end)
    return ok and r == true
end

-- One update, with the Jodi the game is ticking right now
local function update(pawn)
    local name = pawn:GetFullName()
    local map = name:match("/Game/[%w_/]-/([%w_]+)%.[%w_]+:") or "unknown"

    if request then
        local kind = request
        request = nil
        local p = pawn:K2_GetActorLocation()
        spots[map] = spots[map] or {}
        spots[map][kind] = { p.X, p.Y, p.Z }
        saveSpots()
        log(string.format("%s spot saved for %s: %.0f %.0f %.0f", kind == "pc" and "PC" or "TV", map, p.X, p.Y, p.Z))
    end

    local s = spots[map]
    if not s then return end                     -- not Jodi's office: nothing to do

    local ctrl = pawn:GetController()
    if not valid(ctrl) then return end
    local cam = ctrl.PlayerCameraManager
    if not valid(cam) then return end
    local p = pawn:K2_GetActorLocation()
    local cl = cam:GetCameraLocation()
    local cr = cam:GetCameraRotation()
    local f = io.open(POS_FILE, "w")
    if not f then return end
    f:write(string.format("map %s\njodi %.1f %.1f %.1f\ncam %.1f %.1f %.1f %.2f\n", map, p.X, p.Y, p.Z, cl.X, cl.Y, cl.Z, cr.Yaw))
    if s.pc then f:write(string.format("pc %.1f %.1f %.1f\n", s.pc[1], s.pc[2], s.pc[3])) end
    if s.tv then f:write(string.format("tv %.1f %.1f %.1f\n", s.tv[1], s.tv[2], s.tv[3])) end
    f:close()
end

local lastError = 0
local function onTick(self)
    frame = frame + 1
    if frame < EVERY and not request then return end
    frame = 0
    local ok, err = pcall(function()
        local pawn = self:get()
        if valid(pawn) then update(pawn) end
    end)
    if not ok then
        local now = os.time()
        if now - lastError > 10 then lastError = now; log("error: " .. tostring(err)) end
    end
end

-- Hook ReceiveTick on the Blueprint class of this Jodi (or the parent class that has it), once
local function hookJodi(jodi)
    local ok, cls = pcall(function() return jodi:GetClass() end)
    local guard = 0
    while ok and valid(cls) and guard < 20 do
        local okN, full = pcall(function() return cls:GetFullName() end)
        local path = okN and type(full) == "string" and full:match("^%S+%s+(.+)$")
        if not path or not path:find("^/Game/") then return end
        local fpath = path .. ":ReceiveTick"
        if hooked[fpath] then return end
        local okF, fn = pcall(StaticFindObject, fpath)
        if okF and valid(fn) then
            local okH, err = pcall(RegisterHook, fpath, onTick)
            if okH then hooked[fpath] = true; log("following Jodi via " .. fpath)
            else log("hook refused: " .. fpath .. " (" .. tostring(err) .. ")") end
            return
        end
        ok, cls = pcall(function() return cls:GetSuperStruct() end)
        guard = guard + 1
    end
end

-- When the player takes control of Jodi (every map load / respawn): hook her class if not done yet.
-- (NotifyOnNewObject on her Blueprint class may not fire when registered at startup - see CheatButtonOnTab
-- history - so ClientRestart, a native function hooked fine at startup, is the main trigger.)
pcall(RegisterHook, "/Script/Engine.PlayerController:ClientRestart", function(self, newPawn)
    pcall(function()
        local pawn = newPawn and newPawn:get()
        if not valid(pawn) then pawn = self:get().Pawn end
        if valid(pawn) then hookJodi(pawn) end
    end)
end)
local JODI_CLASSES = {
    "/Game/Project/Character/Jodi/Jodi_StoryMode.Jodi_StoryMode_C",
    "/Game/Project/Character/Jodi/Jodi.Jodi_C",
}
for _, c in ipairs(JODI_CLASSES) do
    pcall(NotifyOnNewObject, c, function(jodi) pcall(hookJodi, jodi) end)
end

loadSpots()
-- key callbacks run outside the game thread: they only leave a note for the next update
RegisterKeyBind(Key.F8, {ModifierKey.CONTROL}, function() request = "pc" end)
RegisterKeyBind(Key.F9, {ModifierKey.CONTROL}, function() request = "tv" end)

log("v5 loaded (Ctrl+F8 = save PC spot, Ctrl+F9 = save TV spot)")
