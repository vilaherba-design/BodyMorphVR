// BodyMorph VR: BodySlide-style body morphs for Fallout 4 VR NPCs.
// Copyright (C) 2026 Axel. Licensed under the GNU General Public License version 3 or later: see LICENSE.
//
// Reader for BodySlide's ".tri" morph files (the "TRIP" format written by BodySlide's Build Morphs).
// Written from the format as BodySlide-and-Outfit-Studio (GPL-3.0, ousnius / Caliente) reads it:
//   magic (bytes "PIRT" on disk, i.e. the 32-bit constant 'TRIP' little-endian; "TRIP" also accepted) u16 shapeCount
//   per shape: u8 nameLen, name, u16 morphCount
//     per morph: u8 nameLen, name, f32 multiplier, u16 vertCount
//       per vertex: u16 index, s16 x, s16 y, s16 z        (offset = s16 * multiplier)
//   then, optionally, the same layout for UV morphs (u16 index, s16 u, s16 v): read past, not kept.
// Pure standard C++: no game headers, so it is unit-tested on Linux (tests/run.sh).

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bodymorph
{
	struct VertexOffset
	{
		std::uint16_t index;
		float         x;
		float         y;
		float         z;
	};

	using Morph = std::vector<VertexOffset>;  // only vertices that move

	struct TriShape
	{
		std::string                            name;    // as written in the file (the NIF shape name)
		std::unordered_map<std::string, Morph> morphs;  // key: morph (slider) name, lower case
	};

	class TriFile
	{
	public:
		// Parses a whole file. Returns false (and leaves the object empty) on a bad magic or a truncated file.
		bool Parse(std::span<const std::byte> a_data, std::string* a_error = nullptr);

		// Shape lookup by NIF shape name, case-insensitive.
		[[nodiscard]] const TriShape* FindShape(std::string_view a_shape) const;
		// Morph lookup on a shape, case-insensitive. nullptr if the shape or the morph is absent.
		[[nodiscard]] const Morph* FindMorph(std::string_view a_shape, std::string_view a_morph) const;

		[[nodiscard]] const std::vector<TriShape>& Shapes() const noexcept { return _shapes; }
		[[nodiscard]] std::size_t                   UVMorphCount() const noexcept { return _uvMorphs; }

		static std::string Lower(std::string_view a_text);

	private:
		std::vector<TriShape> _shapes;
		std::size_t           _uvMorphs = 0;
	};
}
