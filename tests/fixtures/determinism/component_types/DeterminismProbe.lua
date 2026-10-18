-- Script results that must be the same bits on every platform: float math
-- (transcendental functions and ^ go through the engine's deterministic
-- math), number formatting, string-keyed table order and sorting.
DeterminismProbe = {
	OnAgentCommand = function(self, name)
		if name ~= "digest" then return nil end
		local h = 2166136261
		local count = 0
		local function mix(s)
			for i = 1, #s do h = ((h ~ s:byte(i)) * 16777619) & 0xffffffff end
			count = count + 1
		end
		local function num(x) mix(string.format("%.17g", x)) end
		local x = 0.1
		for i = 1, 2000 do
			x = (x * 1.6180339887 + i * 0.37) % 50 - 25
			num(math.sin(x)); num(math.cos(x)); num(math.tan(x * 0.3))
			num(math.atan(x, 1.7)); num(math.atan(x)); num(math.exp(x * 0.2))
			num(math.log(math.abs(x) + 0.5)); num(math.log(math.abs(x) + 2, 2)); num(math.log(math.abs(x) + 2, 10))
			num(math.sqrt(math.abs(x))); num(math.fmod(x, 3.3)); num(math.floor(x * 7.7))
			num((math.abs(x) + 0.25) ^ 1.37); num(2 ^ (x * 0.5)); num(x ^ 2)
			local s = math.sin(x) * 0.999
			num(math.asin(s)); num(math.acos(s))
			mix(tostring(x)); mix(tostring(x * 1e-7)); mix(tostring(x * 1e15))
		end
		local t = {}
		for i = 1, 300 do t["key" .. (i * 7919 % 1009)] = i end
		for k, v in pairs(t) do mix(k); num(v) end
		local list = {}
		for i = 1, 500 do list[i] = (i * 7919) % 1013 + 0.5 end
		table.sort(list, function(a, b) return a > b end)
		for _, v in ipairs(list) do num(v) end
		return {digest = string.format("%08x", h), values = count,
			sample = string.format("%.17g %.17g %.17g", math.sin(1e22), math.exp(10.5), 3.7 ^ 2.9)}
	end,
}
