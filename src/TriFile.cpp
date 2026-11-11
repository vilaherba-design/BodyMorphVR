// BodyMorph VR. Copyright (C) 2026 Axel. GPL-3.0-or-later: see LICENSE.
#include "TriFile.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace bodymorph
{
	namespace
	{
		class Reader
		{
		public:
			explicit Reader(std::span<const std::byte> a_data) :
				_data(a_data) {}

			template <class T>
			bool Read(T& a_out)
			{
				if (_data.size() - _pos < sizeof(T)) {
					return false;
				}
				std::memcpy(&a_out, _data.data() + _pos, sizeof(T));  // little-endian file, little-endian x64
				_pos += sizeof(T);
				return true;
			}

			bool ReadName(std::string& a_out)
			{
				std::uint8_t len = 0;
				if (!Read(len) || _data.size() - _pos < len) {
					return false;
				}
				a_out.assign(reinterpret_cast<const char*>(_data.data() + _pos), len);
				_pos += len;
				return true;
			}

			bool Skip(std::size_t a_bytes)
			{
				if (_data.size() - _pos < a_bytes) {
					return false;
				}
				_pos += a_bytes;
				return true;
			}

			[[nodiscard]] bool AtEnd() const noexcept { return _pos >= _data.size(); }
			[[nodiscard]] std::size_t Pos() const noexcept { return _pos; }

		private:
			std::span<const std::byte> _data;
			std::size_t                _pos = 0;
		};

		bool Fail(std::string* a_error, std::string a_text, std::size_t a_pos)
		{
			if (a_error) {
				*a_error = a_text + " at byte " + std::to_string(a_pos);
			}
			return false;
		}
	}

	std::string TriFile::Lower(std::string_view a_text)
	{
		std::string out(a_text);
		std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	bool TriFile::Parse(std::span<const std::byte> a_data, std::string* a_error)
	{
		_shapes.clear();
		_uvMorphs = 0;
		Reader r(a_data);

		char magic[4]{};
		// BodySlide writes the 32-bit constant 'TRIP' little-endian, so the bytes on disk read "PIRT" (found with a real file);
		// the other order is accepted too.
		if (!r.Read(magic) || (std::memcmp(magic, "PIRT", 4) != 0 && std::memcmp(magic, "TRIP", 4) != 0)) {
			return Fail(a_error, "not a TRIP file", 0);
		}

		std::vector<TriShape> shapes;
		std::uint16_t         shapeCount = 0;
		if (!r.Read(shapeCount)) {
			return Fail(a_error, "truncated shape count", r.Pos());
		}
		for (std::uint16_t s = 0; s < shapeCount; ++s) {
			TriShape      shape;
			std::uint16_t morphCount = 0;
			if (!r.ReadName(shape.name) || !r.Read(morphCount)) {
				return Fail(a_error, "truncated shape header", r.Pos());
			}
			for (std::uint16_t m = 0; m < morphCount; ++m) {
				std::string   name;
				float         mult = 0.0F;
				std::uint16_t count = 0;
				if (!r.ReadName(name) || !r.Read(mult) || !r.Read(count)) {
					return Fail(a_error, "truncated morph header", r.Pos());
				}
				Morph morph;
				morph.reserve(count);
				for (std::uint16_t v = 0; v < count; ++v) {
					std::uint16_t index = 0;
					std::int16_t  x = 0, y = 0, z = 0;
					if (!r.Read(index) || !r.Read(x) || !r.Read(y) || !r.Read(z)) {
						return Fail(a_error, "truncated morph vertex", r.Pos());
					}
					if (x != 0 || y != 0 || z != 0) {
						morph.push_back({ index, x * mult, y * mult, z * mult });
					}
				}
				if (!morph.empty()) {
					shape.morphs[Lower(name)] = std::move(morph);  // a repeated name: the later one wins
				}
			}
			shapes.push_back(std::move(shape));
		}

		// Optional UV morph block: walked to validate the file, not kept (UV morphs are not applied).
		if (!r.AtEnd()) {
			std::uint16_t uvShapes = 0;
			if (!r.Read(uvShapes)) {
				return Fail(a_error, "truncated UV shape count", r.Pos());
			}
			for (std::uint16_t s = 0; s < uvShapes; ++s) {
				std::string   name;
				std::uint16_t morphCount = 0;
				if (!r.ReadName(name) || !r.Read(morphCount)) {
					return Fail(a_error, "truncated UV shape header", r.Pos());
				}
				for (std::uint16_t m = 0; m < morphCount; ++m) {
					float         mult = 0.0F;
					std::uint16_t count = 0;
					if (!r.ReadName(name) || !r.Read(mult) || !r.Read(count) || !r.Skip(std::size_t{ count } * 6)) {
						return Fail(a_error, "truncated UV morph", r.Pos());
					}
					++_uvMorphs;
				}
			}
		}

		_shapes = std::move(shapes);
		return true;
	}

	const TriShape* TriFile::FindShape(std::string_view a_shape) const
	{
		const auto key = Lower(a_shape);
		for (const auto& shape : _shapes) {
			if (Lower(shape.name) == key) {
				return &shape;
			}
		}
		return nullptr;
	}

	const Morph* TriFile::FindMorph(std::string_view a_shape, std::string_view a_morph) const
	{
		const auto* shape = FindShape(a_shape);
		if (!shape) {
			return nullptr;
		}
		const auto it = shape->morphs.find(Lower(a_morph));
		return it != shape->morphs.end() ? &it->second : nullptr;
	}
}
