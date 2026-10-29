-- Copy this file to ~/.config/kilo/plugins/ to enable it.
kilo.bind("ctrl+shift+w", function()
	local text = kilo.get_text()
	local _, words = text:gsub("%S+", "")
	local line, column = kilo.cursor()
	kilo.status(string.format("%d words | line %d, column %d", words, line, column))
end)
