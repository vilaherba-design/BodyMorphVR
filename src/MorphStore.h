// BodyMorph VR. Copyright (C) 2026 Axel. GPL-3.0-or-later: see LICENSE.
//
// The per-actor morph table behind the Papyrus "BodyGen" functions (SetMorph / GetMorph / RemoveAllMorphs ...), written from the behaviour that Silhouette's
// documentation (docs/bodygen-format.md, GPL-3.0) describes, not from any LooksMenu code:
//  - every actor has one table per sex; a table maps morph name -> { keyword form ID (0 = "None") -> value };
//  - morph names are case-insensitive and may be any string (Silhouette stores marker morphs that no .tri contains: they persist all the same);
//  - setting a value of 0 removes that key; a morph without keys disappears; an actor whose table is empty counts as "has no stored morphs";
//  - the value the body gets for a morph is the MAXIMUM over its keys (not the sum).
// Plus the (de)serialisation of one table, used for the F4SE co-save. Pure standard C++ (unit-tested on Linux), thread-safe.

#pragma once

#include "MorphList.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bodymorph
{
	namespace detail
	{
		[[nodiscard]] inline std::string LowerAscii(std::string_view a_text)
		{
			std::string out(a_text);
			for (auto& c : out) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return out;
		}
	}

	class MorphStore
	{
	public:
		// One serialised table (what the co-save stores as one record).
		struct Record
		{
			std::uint32_t          actor = 0;
			bool                   female = false;
			std::vector<std::byte> bytes;
		};

		void Set(std::uint32_t a_actor, bool a_female, std::string_view a_morph, std::uint32_t a_keyword, float a_value)
		{
			if (a_morph.empty()) {
				return;
			}
			std::lock_guard lock(_mutex);
			const auto      key = detail::LowerAscii(a_morph);
			if (a_value == 0.0F) {  // "setting a value to 0 removes that key"
				const auto actorIt = _tables.find(MakeKey(a_actor, a_female));
				if (actorIt == _tables.end()) {
					return;
				}
				auto morphIt = actorIt->second.find(key);
				if (morphIt != actorIt->second.end()) {
					morphIt->second.keys.erase(a_keyword);
					if (morphIt->second.keys.empty()) {
						actorIt->second.erase(morphIt);
					}
				}
				if (actorIt->second.empty()) {
					_tables.erase(actorIt);
				}
				return;
			}
			auto& morph = _tables[MakeKey(a_actor, a_female)][key];
			if (morph.name.empty()) {
				morph.name = std::string(a_morph);
			}
			morph.keys[a_keyword] = a_value;
		}

		// The value stored under exactly this keyword (0 = the "None" key); 0 when there is none.
		[[nodiscard]] float Get(std::uint32_t a_actor, bool a_female, std::string_view a_morph, std::uint32_t a_keyword) const
		{
			std::lock_guard lock(_mutex);
			const auto*     table = Find(a_actor, a_female);
			if (!table) {
				return 0.0F;
			}
			const auto it = table->find(detail::LowerAscii(a_morph));
			if (it == table->end()) {
				return 0.0F;
			}
			const auto key = it->second.keys.find(a_keyword);
			return key == it->second.keys.end() ? 0.0F : key->second;
		}

		// True when the actor holds at least one morph for this sex.
		[[nodiscard]] bool Has(std::uint32_t a_actor, bool a_female) const
		{
			std::lock_guard lock(_mutex);
			return Find(a_actor, a_female) != nullptr;
		}

		[[nodiscard]] std::vector<std::string> Names(std::uint32_t a_actor, bool a_female) const
		{
			std::lock_guard          lock(_mutex);
			std::vector<std::string> out;
			if (const auto* table = Find(a_actor, a_female)) {
				for (const auto& [lower, morph] : *table) {
					out.push_back(morph.name);
				}
			}
			return out;
		}

		// The keywords (form IDs, never 0) a morph has a value under.
		[[nodiscard]] std::vector<std::uint32_t> Keywords(std::uint32_t a_actor, bool a_female, std::string_view a_morph) const
		{
			std::lock_guard            lock(_mutex);
			std::vector<std::uint32_t> out;
			if (const auto* table = Find(a_actor, a_female)) {
				if (const auto it = table->find(detail::LowerAscii(a_morph)); it != table->end()) {
					for (const auto& [keyword, value] : it->second.keys) {
						if (keyword != 0) {
							out.push_back(keyword);
						}
					}
				}
			}
			return out;
		}

		void RemoveByName(std::uint32_t a_actor, bool a_female, std::string_view a_morph)
		{
			std::lock_guard lock(_mutex);
			const auto      it = _tables.find(MakeKey(a_actor, a_female));
			if (it == _tables.end()) {
				return;
			}
			it->second.erase(detail::LowerAscii(a_morph));
			if (it->second.empty()) {
				_tables.erase(it);
			}
		}

		void RemoveByKeyword(std::uint32_t a_actor, bool a_female, std::uint32_t a_keyword)
		{
			std::lock_guard lock(_mutex);
			const auto      it = _tables.find(MakeKey(a_actor, a_female));
			if (it == _tables.end()) {
				return;
			}
			for (auto m = it->second.begin(); m != it->second.end();) {
				m->second.keys.erase(a_keyword);
				m = m->second.keys.empty() ? it->second.erase(m) : std::next(m);
			}
			if (it->second.empty()) {
				_tables.erase(it);
			}
		}

		void RemoveAll(std::uint32_t a_actor, bool a_female)
		{
			std::lock_guard lock(_mutex);
			_tables.erase(MakeKey(a_actor, a_female));
		}

		// Both sexes (RegenerateMorphs: "clears the actor's morphs").
		void RemoveActor(std::uint32_t a_actor)
		{
			std::lock_guard lock(_mutex);
			_tables.erase(MakeKey(a_actor, false));
			_tables.erase(MakeKey(a_actor, true));
		}

		void Clear()
		{
			std::lock_guard lock(_mutex);
			_tables.clear();
		}

		[[nodiscard]] std::size_t TableCount() const
		{
			std::lock_guard lock(_mutex);
			return _tables.size();
		}

		// What the body gets: one entry per morph, the maximum over its keys. Morph names keep the case they were stored with.
		[[nodiscard]] std::vector<MorphWeight> Effective(std::uint32_t a_actor, bool a_female) const
		{
			std::lock_guard          lock(_mutex);
			std::vector<MorphWeight> out;
			if (const auto* table = Find(a_actor, a_female)) {
				for (const auto& [lower, morph] : *table) {
					float best = 0.0F;
					bool  any = false;
					for (const auto& [keyword, value] : morph.keys) {
						if (!any || value > best) {
							best = value;
							any = true;
						}
					}
					if (any && best != 0.0F) {
						out.push_back({ morph.name, best });
					}
				}
			}
			return out;
		}

		// ---- co-save: one record per table. Layout (host byte order, like the rest of the F4SE co-save): u32 actor, u8 female, u8 0, u32 morphCount,
		// then per morph: u16 nameLength, name bytes, u16 keyCount, per key u32 keyword + f32 value.
		[[nodiscard]] std::vector<Record> Serialize() const
		{
			std::lock_guard     lock(_mutex);
			std::vector<Record> out;
			out.reserve(_tables.size());
			for (const auto& [key, table] : _tables) {
				Record r;
				r.actor = static_cast<std::uint32_t>(key >> 1);
				r.female = (key & 1) != 0;
				Put(r.bytes, r.actor);
				Put(r.bytes, static_cast<std::uint8_t>(r.female ? 1 : 0));
				Put(r.bytes, static_cast<std::uint8_t>(0));
				Put(r.bytes, static_cast<std::uint32_t>(table.size()));
				for (const auto& [lower, morph] : table) {
					const auto nameLen = static_cast<std::uint16_t>(std::min<std::size_t>(morph.name.size(), 0xFFFF));
					Put(r.bytes, nameLen);
					const auto* chars = reinterpret_cast<const std::byte*>(morph.name.data());
					r.bytes.insert(r.bytes.end(), chars, chars + nameLen);
					Put(r.bytes, static_cast<std::uint16_t>(std::min<std::size_t>(morph.keys.size(), 0xFFFF)));
					std::size_t written = 0;
					for (const auto& [keyword, value] : morph.keys) {
						if (written++ >= 0xFFFF) {
							break;
						}
						Put(r.bytes, keyword);
						Put(r.bytes, value);
					}
				}
				out.push_back(std::move(r));
			}
			return out;
		}

		// Reads one record and merges it into the store. a_resolve(uint32_t&) maps an old form ID to the current load order (false = drop it): an actor that
		// no longer resolves drops the whole table, a keyword that no longer resolves drops that value. Returns false for a damaged record (nothing merged).
		template <class Resolve>
		bool Deserialize(const std::byte* a_data, std::size_t a_size, Resolve&& a_resolve)
		{
			Reader        in{ a_data, a_size };
			std::uint32_t actor = 0;
			std::uint8_t  female = 0, pad = 0;
			std::uint32_t morphCount = 0;
			if (!in.Get(actor) || !in.Get(female) || !in.Get(pad) || !in.Get(morphCount) || morphCount > 100000) {
				return false;
			}
			struct Pending
			{
				std::string                                       name;
				std::vector<std::pair<std::uint32_t, float>> keys;
			};
			std::vector<Pending> pending;
			for (std::uint32_t m = 0; m < morphCount; ++m) {
				std::uint16_t nameLen = 0, keyCount = 0;
				if (!in.Get(nameLen) || in.Left() < nameLen) {
					return false;
				}
				Pending p;
				p.name.assign(reinterpret_cast<const char*>(in.Take(nameLen)), nameLen);
				if (!in.Get(keyCount) || in.Left() < std::size_t{ keyCount } * 8) {
					return false;
				}
				for (std::uint16_t k = 0; k < keyCount; ++k) {
					std::uint32_t keyword = 0;
					float         value = 0.0F;
					if (!in.Get(keyword) || !in.Get(value)) {
						return false;
					}
					p.keys.emplace_back(keyword, value);
				}
				pending.push_back(std::move(p));
			}
			std::uint32_t resolvedActor = actor;
			if (!a_resolve(resolvedActor)) {
				return true;  // intact, but the actor's plugin is gone: nothing to merge
			}
			for (const auto& p : pending) {
				for (auto [keyword, value] : p.keys) {
					if (keyword != 0 && !a_resolve(keyword)) {
						continue;
					}
					Set(resolvedActor, female != 0, p.name, keyword, value);  // zero values are dropped by Set
				}
			}
			return true;
		}

	private:
		struct StoredMorph
		{
			std::string                      name;  // as first written
			std::map<std::uint32_t, float>   keys;
		};
		using Table = std::map<std::string, StoredMorph>;  // lower-case name -> morph

		struct Reader
		{
			const std::byte* data;
			std::size_t      size;
			std::size_t      pos = 0;

			[[nodiscard]] std::size_t Left() const noexcept { return size - pos; }
			template <class T>
			bool Get(T& a_out) noexcept
			{
				if (Left() < sizeof(T)) {
					return false;
				}
				std::memcpy(&a_out, data + pos, sizeof(T));
				pos += sizeof(T);
				return true;
			}
			const std::byte* Take(std::size_t a_n) noexcept
			{
				const auto* p = data + pos;
				pos += a_n;
				return p;
			}
		};

		template <class T>
		static void Put(std::vector<std::byte>& a_out, T a_value)
		{
			std::byte raw[sizeof(T)];
			std::memcpy(raw, &a_value, sizeof(T));
			a_out.insert(a_out.end(), raw, raw + sizeof(T));
		}

		[[nodiscard]] static std::uint64_t MakeKey(std::uint32_t a_actor, bool a_female) noexcept { return (std::uint64_t{ a_actor } << 1) | (a_female ? 1U : 0U); }

		[[nodiscard]] const Table* Find(std::uint32_t a_actor, bool a_female) const
		{
			const auto it = _tables.find(MakeKey(a_actor, a_female));
			return it == _tables.end() ? nullptr : &it->second;
		}

		mutable std::mutex                   _mutex;
		std::map<std::uint64_t, Table>       _tables;
	};
}
