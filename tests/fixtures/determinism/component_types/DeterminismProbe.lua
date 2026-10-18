-- Script results that must be the same bits on every platform: float math
-- (transcendental functions and ^ go through the engine's deterministic
-- math), number formatting, string-keyed table order and sorting. One FNV-1a
-- hash per category, so a mismatch names the operation.
DeterminismProbe = {
	OnAgentCommand = function(self, name)
		if name ~= "digest" then return nil end
		local hs, order, first = {}, {}, {}
		local function mixc(cat, s)
			local h = hs[cat]
			if not h then h = 2166136261; order[#order + 1] = cat; first[cat] = s end
			for i = 1, #s do h = ((h ~ s:byte(i)) * 16777619) & 0xffffffff end
			hs[cat] = h
		end
		local function num(cat, x) mixc(cat, string.format("%.17g", x)) end
		local x = 0.1
		for i = 1, 2000 do
			x = (x * 1.6180339887 + i * 0.37) % 50 - 25
			num("x", x)
			num("sin", math.sin(x)); num("cos", math.cos(x)); num("tan", math.tan(x * 0.3))
			num("atan2", math.atan(x, 1.7)); num("atan", math.atan(x)); num("exp", math.exp(x * 0.2))
			num("log", math.log(math.abs(x) + 0.5)); num("log2", math.log(math.abs(x) + 2, 2)); num("log10", math.log(math.abs(x) + 2, 10))
			num("sqrt", math.sqrt(math.abs(x))); num("fmod", math.fmod(x, 3.3)); num("floor", math.floor(x * 7.7))
			num("pow137", (math.abs(x) + 0.25) ^ 1.37); num("pow2x", 2 ^ (x * 0.5)); num("sq", x ^ 2)
			local s = math.sin(x) * 0.999
			num("asin", math.asin(s)); num("acos", math.acos(s))
			mixc("tostr", tostring(x)); mixc("tostr_e7", tostring(x * 1e-7)); mixc("tostr_e15", tostring(x * 1e15))
		end
		local t = {}
		for i = 1, 300 do t["key" .. (i * 7919 % 1009)] = i end
		for k, v in pairs(t) do mixc("pairs", k); num("pairs", v) end
		local list = {}
		for i = 1, 500 do list[i] = (i * 7919) % 1013 + 0.5 end
		table.sort(list, function(a, b) return a > b end)
		for _, v in ipairs(list) do num("sort", v) end
		local out = {}
		for _, c in ipairs(order) do out[c] = string.format("%08x", hs[c]) end
		return {cats = out, first_tostr_e7 = first["tostr_e7"], first_tostr_e15 = first["tostr_e15"], pairs_first = first["pairs"]}
	end,
}
