// BodyMorph VR. Copyright (C) 2026 Axel. GPL-3.0-or-later: see LICENSE.
//
// Minimal INI reader (pure C++, unit-tested on Linux). Why not GetPrivateProfileString: with a relative path it looks in the Windows
// directory, not in the game's (virtual, under Mod Organizer 2) Data folder, so it silently returned the defaults.
// Format: "[section]", "key=value", comment lines start with ';' or '#', CRLF or LF, section and key names are case-insensitive,
// the value is the rest of the line trimmed (a ';' inside a value is kept: it is the list separator of sMorphs).

#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bodymorph
{
	class IniFile
	{
	public:
		void Parse(std::string_view a_text)
		{
			_values.clear();
			std::string section;
			std::size_t pos = 0;
			while (pos <= a_text.size()) {
				auto end = a_text.find('\n', pos);
				if (end == std::string_view::npos) {
					end = a_text.size();
				}
				auto line = Trim(a_text.substr(pos, end - pos));
				pos = end + 1;
				if (line.empty() || line.front() == ';' || line.front() == '#') {
					continue;
				}
				if (line.front() == '[') {
					const auto close = line.find(']');
					if (close != std::string_view::npos) {
						section = Lower(Trim(line.substr(1, close - 1)));
					}
					continue;
				}
				const auto eq = line.find('=');
				if (eq == std::string_view::npos) {
					continue;
				}
				const auto key = Lower(Trim(line.substr(0, eq)));
				if (!key.empty()) {
					_values[section + '\n' + key] = std::string(Trim(line.substr(eq + 1)));
				}
			}
		}

		[[nodiscard]] std::string GetString(std::string_view a_section, std::string_view a_key, std::string_view a_default) const
		{
			const auto it = _values.find(Lower(a_section) + '\n' + Lower(a_key));
			return it != _values.end() ? it->second : std::string(a_default);
		}

		// A value that is not a whole integer gives the default.
		[[nodiscard]] int GetInt(std::string_view a_section, std::string_view a_key, int a_default) const
		{
			const auto it = _values.find(Lower(a_section) + '\n' + Lower(a_key));
			if (it == _values.end()) {
				return a_default;
			}
			int         value = 0;
			const auto& s = it->second;
			const auto  res = std::from_chars(s.data(), s.data() + s.size(), value);
			return (res.ec == std::errc{} && res.ptr == s.data() + s.size()) ? value : a_default;
		}

		[[nodiscard]] std::size_t Size() const noexcept { return _values.size(); }

	private:
		static std::string_view Trim(std::string_view a_text) noexcept
		{
			constexpr std::string_view ws = " \t\r\n";
			const auto                 first = a_text.find_first_not_of(ws);
			if (first == std::string_view::npos) {
				return {};
			}
			return a_text.substr(first, a_text.find_last_not_of(ws) - first + 1);
		}

		static std::string Lower(std::string_view a_text)
		{
			std::string out(a_text);
			for (auto& c : out) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return out;
		}

		std::unordered_map<std::string, std::string> _values;
	};
}
