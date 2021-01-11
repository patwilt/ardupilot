local MODE_AUTO = 10

local FEET_TO_METERS = 0.3048
local METERS_TO_FEET = 1.0/FEET_TO_METERS

-- constrain a value between limits
function constrain(v, vmin, vmax)
   if v < vmin then
      v = vmin
   end
   if v > vmax then
      v = vmax
   end
   return v
end

--[[
table of fence limits from home by alt AMSL
altitudes in feet, limits in meters
--]]
local fence_limits = {
  { 9000, 9000 }, -- 9km at 9k ft
  { 6000, 6000 }, -- 6km at 6k ft
  { 3000, 3000 }, -- 3km at 3k ft
}

local fence_triggered = false

function get_dist_home()
   local loc = ahrs:get_position()
   local home = ahrs:get_home()
   if not loc or not home then
      -- no position or home yet, can't do fence
      return 0
   end
   return loc:get_distance(home)
end

function trigger_fence()
   if not fence_triggered then
      fence_triggered = true
      gcs:send_text(0, string.format("Triggering fence limit at %.0fm", get_dist_home()))
      parachute:release()
   end
end

function check_fence()
   local loc = ahrs:get_position()
   if not loc then
      -- no position yet, can't do fence
      return
   end
   local dist_home = get_dist_home()
   local alt_amsl_ft = loc:alt() * 0.01 * METERS_TO_FEET
   local num_rows = #fence_limits

   for row = 1, num_rows do
      if alt_amsl_ft >= fence_limits[row][1] then
         if dist_home > fence_limits[row][2] then
            trigger_fence()
         end
         return
      end
   end

   -- we are below lowest level
   if dist_home > fence_limits[num_rows][2] then
      trigger_fence()
   end
end

function adjust_target_speed()
   -- nothing yet
end

function update()
   if arming:is_armed() and vehicle:get_mode() == MODE_AUTO then
      check_fence()
      adjust_target_speed()
   end

   return update, 1000
end

gcs:send_text(0, string.format("Loader glider script"))

return update, 1000
