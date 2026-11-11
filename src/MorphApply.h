// BodyMorph VR. Copyright (C) 2026 Axel. GPL-3.0-or-later: see LICENSE.
//
// CPU side of a morph: writes base position + sum(weight * offset) into a copy of a shape's vertex block.
// Pure standard C++ (unit-tested on Linux). The game side (cloning the geometry, the D3D11 buffer) is elsewhere.
//
// Vertex layout (Bethesda's packed vertex description, a 64-bit "vertexDesc"):
//   stride = (desc & 0xF) * 4 bytes; flags = desc >> 44; position is always the first attribute (offset 0):
//   VF_FULLPREC (0x400) set -> 3 x float32 position (+ float bitangent.x), else 3 x float16 (+ half bitangent.x).

#pragma once

#include "TriFile.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace bodymorph
{
	constexpr std::uint16_t kFlagVertex = 1 << 0;
	constexpr std::uint16_t kFlagFullPrecision = 0x400;

	[[nodiscard]] constexpr std::uint32_t VertexStride(std::uint64_t a_desc) noexcept { return static_cast<std::uint32_t>(a_desc & 0xF) * 4; }
	[[nodiscard]] constexpr std::uint16_t VertexFlags(std::uint64_t a_desc) noexcept { return static_cast<std::uint16_t>(a_desc >> 44); }

	// IEEE 754 binary16 <-> binary32, round to nearest even; denormals, infinities and NaN handled.
	[[nodiscard]] inline float HalfToFloat(std::uint16_t a_h) noexcept
	{
		const std::uint32_t sign = (a_h & 0x8000U) << 16;
		std::uint32_t       exp = (a_h >> 10) & 0x1FU;
		std::uint32_t       man = a_h & 0x3FFU;
		std::uint32_t       bits;
		if (exp == 0) {
			if (man == 0) {
				bits = sign;
			} else {  // denormal: normalise
				exp = 127 - 15 + 1;
				while ((man & 0x400U) == 0) {
					man <<= 1;
					--exp;
				}
				man &= 0x3FFU;
				bits = sign | (exp << 23) | (man << 13);
			}
		} else if (exp == 0x1F) {
			bits = sign | 0x7F800000U | (man << 13);
		} else {
			bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
		}
		return std::bit_cast<float>(bits);
	}

	[[nodiscard]] inline std::uint16_t FloatToHalf(float a_f) noexcept
	{
		const std::uint32_t bits = std::bit_cast<std::uint32_t>(a_f);
		const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000U);
		const std::uint32_t exp = (bits >> 23) & 0xFFU;
		std::uint32_t       man = bits & 0x7FFFFFU;
		if (exp == 0xFF) {  // inf / NaN
			return static_cast<std::uint16_t>(sign | 0x7C00U | (man ? 0x200U : 0U));
		}
		const int e = static_cast<int>(exp) - 127 + 15;
		if (e >= 0x1F) {  // overflow -> inf
			return static_cast<std::uint16_t>(sign | 0x7C00U);
		}
		if (e <= 0) {  // denormal or zero
			if (e < -10) {
				return sign;
			}
			man |= 0x800000U;
			const int      shift = 14 - e;
			std::uint32_t  half = man >> shift;
			const std::uint32_t rest = man & ((1U << shift) - 1);
			const std::uint32_t mid = 1U << (shift - 1);
			if (rest > mid || (rest == mid && (half & 1U))) {
				++half;
			}
			return static_cast<std::uint16_t>(sign | half);
		}
		std::uint32_t half = (static_cast<std::uint32_t>(e) << 10) | (man >> 13);
		const std::uint32_t rest = man & 0x1FFFU;
		if (rest > 0x1000U || (rest == 0x1000U && (half & 1U))) {
			++half;  // may carry into the exponent: still correct (up to inf)
		}
		return static_cast<std::uint16_t>(sign | half);
	}

	struct Position
	{
		float x, y, z;
	};

	// Reads every vertex position of a block (the unmorphed base, kept per shape so morphs never accumulate).
	[[nodiscard]] inline bool ReadPositions(std::span<const std::byte> a_block, std::uint64_t a_desc, std::uint32_t a_count, std::vector<Position>& a_out)
	{
		const auto stride = VertexStride(a_desc);
		const bool full = (VertexFlags(a_desc) & kFlagFullPrecision) != 0;
		if (stride < (full ? 12U : 6U) || (VertexFlags(a_desc) & kFlagVertex) == 0 || a_block.size() < std::size_t{ stride } * a_count) {
			return false;
		}
		a_out.resize(a_count);
		for (std::uint32_t i = 0; i < a_count; ++i) {
			const auto* v = a_block.data() + std::size_t{ stride } * i;
			if (full) {
				std::memcpy(&a_out[i], v, 12);
			} else {
				std::uint16_t h[3];
				std::memcpy(h, v, 6);
				a_out[i] = { HalfToFloat(h[0]), HalfToFloat(h[1]), HalfToFloat(h[2]) };
			}
		}
		return true;
	}

	// Writes base + sum(weight * offset) into a_block (a copy of the shape's block, same desc). Offsets whose
	// index is past the vertex count are ignored (a .tri built for another mesh) and counted in the result.
	inline std::size_t WritePositions(std::span<std::byte> a_block, std::uint64_t a_desc, std::span<const Position> a_base,
		std::span<const std::pair<const Morph*, float>> a_morphs)
	{
		const auto stride = VertexStride(a_desc);
		const bool full = (VertexFlags(a_desc) & kFlagFullPrecision) != 0;
		std::vector<Position> pos(a_base.begin(), a_base.end());
		std::size_t           outOfRange = 0;
		for (const auto& [morph, weight] : a_morphs) {
			if (!morph || weight == 0.0F) {
				continue;
			}
			for (const auto& o : *morph) {
				if (o.index >= pos.size()) {
					++outOfRange;
					continue;
				}
				pos[o.index].x += o.x * weight;
				pos[o.index].y += o.y * weight;
				pos[o.index].z += o.z * weight;
			}
		}
		for (std::size_t i = 0; i < pos.size(); ++i) {
			auto* v = a_block.data() + std::size_t{ stride } * i;
			if (full) {
				std::memcpy(v, &pos[i], 12);
			} else {
				const std::uint16_t h[3] = { FloatToHalf(pos[i].x), FloatToHalf(pos[i].y), FloatToHalf(pos[i].z) };
				std::memcpy(v, h, 6);
			}
		}
		return outOfRange;
	}
}
