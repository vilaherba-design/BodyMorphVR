// BodyMorph VR. Copyright (C) 2026 Axel. GPL-3.0-or-later: see LICENSE.
//
// Small pure-C++ text helpers (unit-tested on Linux): the "name:weight,name:weight" list used by the INI, and the
// instance suffix the engine adds to shape names.

#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace bodymorph
{
	struct MorphWeight
	{
		std::string name;
		float       weight;
	};

	namespace detail
	{
		[[nodiscard]] constexpr std::string_view Trim(std::string_view a_text) noexcept
		{
			constexpr std::string_view ws = " \t\r\n";
			const auto                 first = a_text.find_first_not_of(ws);
			if (first == std::string_view::npos) {
				return {};
			}
			return a_text.substr(first, a_text.find_last_not_of(ws) - first + 1);
		}
	}

	// "Breasts:0.5, Butt:1,  Waist:-0.25"  ->  { Breasts 0.5 }, { Butt 1 }, { Waist -0.25 }.
	// Separators: ',' or ';'. A weight that is missing or not a number means 1 (and then the whole token is the name).
	// Names may contain spaces ("7B Upper"). Empty names are dropped.
	[[nodiscard]] inline std::vector<MorphWeight> ParseMorphList(std::string_view a_text)
	{
		std::vector<MorphWeight> out;
		std::size_t              pos = 0;
		while (pos <= a_text.size()) {
			auto end = a_text.find_first_of(",;", pos);
			if (end == std::string_view::npos) {
				end = a_text.size();
			}
			const auto token = detail::Trim(a_text.substr(pos, end - pos));
			pos = end + 1;
			if (token.empty()) {
				continue;
			}
			std::string_view name = token;
			float            weight = 1.0F;
			if (const auto colon = token.rfind(':'); colon != std::string_view::npos) {
				const auto number = detail::Trim(token.substr(colon + 1));
				float      value = 0.0F;
				const auto res = std::from_chars(number.data(), number.data() + number.size(), value);
				if (!number.empty() && res.ec == std::errc{} && res.ptr == number.data() + number.size()) {
					name = detail::Trim(token.substr(0, colon));
					weight = value;
				}
			}
			if (!name.empty()) {
				out.push_back({ std::string(name), weight });
			}
		}
		return out;
	}

	struct ShapeAlias
	{
		std::string prefix;  // start of the in-game shape name
		std::string shape;   // shape name inside the .tri
	};

	// "NakedTorso=CBBE, Other prefix=Shape": the game renames the shapes of the skin ("NakedTorso (00000D67)[1]/SkinNaked (00000D64)") so the name
	// stored in the .tri ("CBBE") is not found by name. Separators ',' or ';', pairs split at the last '='. Pairs with an empty side are dropped.
	[[nodiscard]] inline std::vector<ShapeAlias> ParseShapeAliases(std::string_view a_text)
	{
		std::vector<ShapeAlias> out;
		std::size_t             pos = 0;
		while (pos <= a_text.size()) {
			auto end = a_text.find_first_of(",;", pos);
			if (end == std::string_view::npos) {
				end = a_text.size();
			}
			const auto token = detail::Trim(a_text.substr(pos, end - pos));
			pos = end + 1;
			const auto eq = token.rfind('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			const auto prefix = detail::Trim(token.substr(0, eq));
			const auto shape = detail::Trim(token.substr(eq + 1));
			if (!prefix.empty() && !shape.empty()) {
				out.push_back({ std::string(prefix), std::string(shape) });
			}
		}
		return out;
	}

	[[nodiscard]] inline bool StartsWithNoCase(std::string_view a_text, std::string_view a_prefix) noexcept
	{
		if (a_prefix.size() > a_text.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a_prefix.size(); ++i) {
			auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
			if (lower(a_text[i]) != lower(a_prefix[i])) {
				return false;
			}
		}
		return true;
	}

	// The engine names the shapes of an actor's instance "CBBE:0", "CBBE:1": the .tri knows "CBBE".
	[[nodiscard]] constexpr std::string_view StripInstanceSuffix(std::string_view a_name) noexcept
	{
		const auto colon = a_name.rfind(':');
		if (colon == std::string_view::npos || colon + 1 == a_name.size()) {
			return a_name;
		}
		for (std::size_t i = colon + 1; i < a_name.size(); ++i) {
			if (a_name[i] < '0' || a_name[i] > '9') {
				return a_name;
			}
		}
		return a_name.substr(0, colon);
	}
}
