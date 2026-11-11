// BodyMorph VR. Copyright (C) 2026 Axel. GPL-3.0-or-later: see LICENSE.
//
// BodySlide's "Build Morphs" writes the path of a garment's .tri into a NiStringExtraData called BODYTRI on the nif's root node, relative to the
// game's Meshes folder, e.g. "Actors\Character\CharacterAssets\FemaleBody.tri" or "Armor\ArmoredCoat\OutfitF.tri".
// Pure standard C++ (unit-tested on Linux).

#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace bodymorph
{
	namespace detail
	{
		[[nodiscard]] inline bool StartsWithI(std::string_view a_text, std::string_view a_prefix)
		{
			if (a_text.size() < a_prefix.size()) {
				return false;
			}
			for (std::size_t i = 0; i < a_prefix.size(); ++i) {
				if (std::tolower(static_cast<unsigned char>(a_text[i])) != std::tolower(static_cast<unsigned char>(a_prefix[i]))) {
					return false;
				}
			}
			return true;
		}
	}

	// A BODYTRI value as the plugin opens it, relative to the game folder ("Data\Meshes\<value>"), or "" when the value is not a plausible
	// .tri path (empty, not ending in ".tri", control characters, a drive letter, or ".." that would leave the Meshes folder).
	[[nodiscard]] inline std::string BodyTriToDataPath(std::string_view a_value)
	{
		std::string s;
		s.reserve(a_value.size());
		for (const char c : a_value) {
			if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) {
				return {};
			}
			s.push_back(c == '/' ? '\\' : c);
		}
		while (!s.empty() && (s.front() == ' ' || s.front() == '\\' || s.front() == '"')) {
			s.erase(s.begin());
		}
		while (!s.empty() && (s.back() == ' ' || s.back() == '"')) {
			s.pop_back();
		}
		if (s.size() < 5 || s.find(':') != std::string::npos || s.find("..") != std::string::npos) {
			return {};
		}
		if (!detail::StartsWithI(std::string_view(s).substr(s.size() - 4), ".tri")) {
			return {};
		}
		if (detail::StartsWithI(s, "data\\")) {
			s.erase(0, 5);
		}
		if (!detail::StartsWithI(s, "meshes\\")) {
			s.insert(0, "Meshes\\");
		}
		return "Data\\" + s;
	}
}
