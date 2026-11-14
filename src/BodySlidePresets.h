// BodyMorph VR. Copyright (C) 2026 Axel. GPL-3.0-or-later: see LICENSE.
//
// Reads BodySlide's own files so that the bodies NPCs get are simply the presets the user has installed (no BodyGen ini to write):
//   Tools\BodySlide\SliderPresets\*.xml   <Preset name set> <Group name/> <SetSlider name size value/>        (values in percent)
//   Tools\BodySlide\SliderSets\*.osp|xml  <SliderSet name> <OutputPath/> <OutputFile/> <Slider name default invert zap uv clamp .../>
//   Tools\BodySlide\BuildSelection.xml    <OutputChoice path choice/>                                         (which set the user builds an output with)
// How BodySlide turns a preset into slider values (behaviour of BodySlide and Outfit Studio, GPL-3.0, ousnius / Caliente, as documented in Silhouette's tools/base_body.py):
//   - size="big" or "both" gives the value a Fallout 4 build uses; size="small" alone is not used; a SetSlider without size is ignored;
//   - files are read recursively and the FIRST preset with a given name wins;
//   - a slider the preset does not name is built at the slider set's default (not at 0), and invert="true" turns v into 1 - v, the default included;
//   - zap, uv and clamp sliders are not morphs (they never reach the .tri).
// The morph value of a slider is therefore that resolved value: the body on disk must be built with zeroed sliders, the .tri holds every slider at 100 %.
// Pure standard C++ (unit-tested on Linux); the game side (main.cpp) only reads the files and the .tri.

#pragma once

#include "TriFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bodymorph::bsp
{
	inline std::string Lower(std::string_view a_s)
	{
		std::string out(a_s);
		for (auto& c : out) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c - 'A' + 'a');
			}
		}
		return out;
	}

	inline std::string_view Trim(std::string_view a_s)
	{
		constexpr std::string_view ws = " \t\r\n";
		const auto                 first = a_s.find_first_not_of(ws);
		if (first == std::string_view::npos) {
			return {};
		}
		return a_s.substr(first, a_s.find_last_not_of(ws) - first + 1);
	}

	// ---- a very small XML reader: elements, attributes and element text. Enough for BodySlide's files (tinyxml2 output); comments, <?...?> and <!...> are skipped.
	inline std::string Unescape(std::string_view a_s)
	{
		std::string out;
		out.reserve(a_s.size());
		for (std::size_t i = 0; i < a_s.size(); ++i) {
			if (a_s[i] != '&') {
				out.push_back(a_s[i]);
				continue;
			}
			const auto end = a_s.find(';', i);
			if (end == std::string_view::npos || end - i > 10) {
				out.push_back('&');
				continue;
			}
			const auto ent = a_s.substr(i + 1, end - i - 1);
			if (ent == "amp") {
				out.push_back('&');
			} else if (ent == "lt") {
				out.push_back('<');
			} else if (ent == "gt") {
				out.push_back('>');
			} else if (ent == "quot") {
				out.push_back('"');
			} else if (ent == "apos") {
				out.push_back('\'');
			} else if (ent.size() > 1 && ent[0] == '#') {
				const bool        hex = ent[1] == 'x' || ent[1] == 'X';
				const std::string digits(ent.substr(hex ? 2 : 1));
				char*             stop = nullptr;
				const long        code = std::strtol(digits.c_str(), &stop, hex ? 16 : 10);
				if (digits.empty() || *stop || code <= 0 || code > 0x10FFFF) {
					out.append(a_s.substr(i, end - i + 1));
				} else if (code < 0x80) {
					out.push_back(static_cast<char>(code));
				} else if (code < 0x800) {
					out.push_back(static_cast<char>(0xC0 | (code >> 6)));
					out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
				} else if (code < 0x10000) {
					out.push_back(static_cast<char>(0xE0 | (code >> 12)));
					out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
				} else {
					out.push_back(static_cast<char>(0xF0 | (code >> 18)));
					out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
				}
			} else {
				out.append(a_s.substr(i, end - i + 1));
			}
			i = end;
		}
		return out;
	}

	struct XmlElement
	{
		std::string                                      name;
		std::vector<std::pair<std::string, std::string>> attrs;  // as written (names are case-sensitive in XML; BodySlide writes them in one spelling)

		[[nodiscard]] const std::string* Attr(std::string_view a_name) const
		{
			for (const auto& [k, v] : attrs) {
				if (k == a_name) {
					return &v;
				}
			}
			return nullptr;
		}
		[[nodiscard]] std::string AttrOr(std::string_view a_name, std::string_view a_default = {}) const
		{
			const auto* v = Attr(a_name);
			return v ? *v : std::string(a_default);
		}
	};

	struct XmlHandler
	{
		std::function<void(const XmlElement&, bool /*selfClosing*/)> open;
		std::function<void(std::string_view /*name*/)>               close;
		std::function<void(std::string_view /*text, unescaped by the caller if wanted*/)> text;
	};

	// Walks the document. Tolerant: a broken tail simply ends the walk. False when the text holds no element at all.
	inline bool WalkXml(std::string_view a_text, const XmlHandler& a_h)
	{
		if (a_text.size() >= 3 && static_cast<unsigned char>(a_text[0]) == 0xEF && static_cast<unsigned char>(a_text[1]) == 0xBB && static_cast<unsigned char>(a_text[2]) == 0xBF) {
			a_text.remove_prefix(3);
		}
		bool        any = false;
		std::size_t pos = 0;
		while (pos < a_text.size()) {
			const auto lt = a_text.find('<', pos);
			if (lt == std::string_view::npos) {
				break;
			}
			if (lt > pos && a_h.text) {
				a_h.text(a_text.substr(pos, lt - pos));
			}
			const auto rest = a_text.substr(lt);
			if (rest.starts_with("<!--")) {
				const auto end = a_text.find("-->", lt + 4);
				if (end == std::string_view::npos) {
					break;
				}
				pos = end + 3;
				continue;
			}
			if (rest.starts_with("<?")) {
				const auto end = a_text.find("?>", lt + 2);
				if (end == std::string_view::npos) {
					break;
				}
				pos = end + 2;
				continue;
			}
			if (rest.starts_with("<![CDATA[")) {
				const auto end = a_text.find("]]>", lt + 9);
				if (end == std::string_view::npos) {
					break;
				}
				if (a_h.text) {
					a_h.text(a_text.substr(lt + 9, end - lt - 9));
				}
				pos = end + 3;
				continue;
			}
			if (rest.starts_with("<!")) {
				const auto end = a_text.find('>', lt + 2);
				if (end == std::string_view::npos) {
					break;
				}
				pos = end + 1;
				continue;
			}
			// the end of the tag: the first '>' outside quotes
			std::size_t end = lt + 1;
			char        quote = 0;
			for (; end < a_text.size(); ++end) {
				const char c = a_text[end];
				if (quote) {
					if (c == quote) {
						quote = 0;
					}
				} else if (c == '"' || c == '\'') {
					quote = c;
				} else if (c == '>') {
					break;
				}
			}
			if (end >= a_text.size()) {
				break;
			}
			auto body = a_text.substr(lt + 1, end - lt - 1);
			pos = end + 1;
			if (!body.empty() && body.front() == '/') {
				if (a_h.close) {
					a_h.close(Trim(body.substr(1)));
				}
				continue;
			}
			bool selfClosing = false;
			if (!body.empty() && body.back() == '/') {
				selfClosing = true;
				body.remove_suffix(1);
			}
			XmlElement  el;
			std::size_t i = 0;
			const auto  isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
			while (i < body.size() && !isSpace(body[i])) {
				++i;
			}
			el.name = std::string(body.substr(0, i));
			if (el.name.empty()) {
				continue;
			}
			while (i < body.size()) {
				while (i < body.size() && isSpace(body[i])) {
					++i;
				}
				const auto keyStart = i;
				while (i < body.size() && body[i] != '=' && !isSpace(body[i])) {
					++i;
				}
				const auto key = body.substr(keyStart, i - keyStart);
				while (i < body.size() && isSpace(body[i])) {
					++i;
				}
				if (key.empty() || i >= body.size() || body[i] != '=') {
					if (key.empty()) {
						++i;  // stray character: step over it
					}
					continue;  // an attribute without a value: ignored
				}
				++i;
				while (i < body.size() && isSpace(body[i])) {
					++i;
				}
				if (i >= body.size() || (body[i] != '"' && body[i] != '\'')) {
					continue;
				}
				const char q = body[i++];
				const auto valueStart = i;
				while (i < body.size() && body[i] != q) {
					++i;
				}
				el.attrs.emplace_back(std::string(key), Unescape(body.substr(valueStart, i - valueStart)));
				if (i < body.size()) {
					++i;
				}
			}
			any = true;
			if (a_h.open) {
				a_h.open(el, selfClosing);
			}
			if (selfClosing && a_h.close) {
				a_h.close(el.name);
			}
		}
		return any;
	}

	inline bool Truthy(std::string_view a_s) { return Lower(Trim(a_s)) == "true"; }

	// A number as BodySlide writes it ("100", "37.5"). False for anything else (and for inf / nan).
	inline bool ParseNumber(std::string_view a_s, float& a_out)
	{
		const std::string text(Trim(a_s));
		if (text.empty()) {
			return false;
		}
		char*       stop = nullptr;
		const float v = std::strtof(text.c_str(), &stop);
		if (stop == text.c_str() || *stop || !std::isfinite(v)) {
			return false;
		}
		a_out = v;
		return true;
	}

	// ---- presets ---------------------------------------------------------------------------------------------------------------------------------------------
	struct Preset
	{
		std::string                                name;
		std::string                                set;     // the slider set that was open when it was saved (not what it is for)
		std::string                                file;    // for the log
		std::vector<std::string>                   groups;  // the body families it declares
		std::vector<std::pair<std::string, float>> big;     // slider -> value 0..1 (percent / 100), as written, before defaults and inversion
		int                                        smallOnly = 0;
	};

	// Appends the presets of one file. A name already in a_out (any case) is skipped: the first one wins, as in BodySlide. Returns how many were added.
	inline int ParsePresets(std::string_view a_text, const std::string& a_file, std::vector<Preset>& a_out)
	{
		std::unordered_set<std::string> have;
		for (const auto& p : a_out) {
			have.insert(Lower(p.name));
		}
		int        added = 0;
		Preset     cur;
		bool       in = false;
		bool       skip = false;
		const auto finish = [&] {
			if (in && !skip) {
				a_out.push_back(std::move(cur));
				++added;
			}
			cur = Preset{};
			in = false;
			skip = false;
		};
		XmlHandler h;
		h.open = [&](const XmlElement& a_el, bool) {
			if (a_el.name == "Preset") {
				finish();
				in = true;
				cur.name = a_el.AttrOr("name");
				cur.set = a_el.AttrOr("set");
				cur.file = a_file;
				skip = cur.name.empty() || !have.insert(Lower(cur.name)).second;
			} else if (in && a_el.name == "Group") {
				if (auto g = a_el.AttrOr("name"); !g.empty()) {
					cur.groups.push_back(std::move(g));
				}
			} else if (in && a_el.name == "SetSlider") {
				const auto name = a_el.AttrOr("name");
				const auto size = Lower(a_el.AttrOr("size"));
				float      value = 0.0f;
				if (name.empty() || !ParseNumber(a_el.AttrOr("value"), value)) {
					return;
				}
				if (size == "big" || size == "both") {
					bool replaced = false;
					for (auto& [n, v] : cur.big) {
						if (Lower(n) == Lower(name)) {
							v = value / 100.0f;
							replaced = true;
						}
					}
					if (!replaced) {
						cur.big.emplace_back(name, value / 100.0f);
					}
				} else if (size == "small") {
					++cur.smallOnly;
				}
			}
		};
		h.close = [&](std::string_view a_name) {
			if (a_name == "Preset") {
				finish();
			}
		};
		WalkXml(a_text, h);
		finish();
		return added;
	}

	// ---- slider sets -----------------------------------------------------------------------------------------------------------------------------------------
	struct SliderInfo
	{
		std::string name;  // as written
		float       def = 0.0f;  // 0..1
		bool        invert = false;
		bool        morph = true;  // false: zap / uv / clamp
	};

	struct SliderSet
	{
		std::string             name;
		std::string             file;
		std::string             output;  // "meshes\actors\character\characterassets\femalebody": lower case, backslashes, no extension
		std::vector<SliderInfo> sliders;

		[[nodiscard]] const SliderInfo* Find(std::string_view a_name) const
		{
			const auto low = Lower(a_name);
			for (const auto& s : sliders) {
				if (Lower(s.name) == low) {
					return &s;
				}
			}
			return nullptr;
		}
	};

	inline std::string NormPath(std::string_view a_p)
	{
		std::string out = Lower(Trim(a_p));
		std::replace(out.begin(), out.end(), '/', '\\');
		while (!out.empty() && out.back() == '\\') {
			out.pop_back();
		}
		while (!out.empty() && out.front() == '\\') {
			out.erase(out.begin());
		}
		return out;
	}

	inline int ParseSliderSets(std::string_view a_text, const std::string& a_file, std::vector<SliderSet>& a_out)
	{
		int         added = 0;
		SliderSet   cur;
		bool        in = false;
		int         depth = 0;  // element depth inside the current SliderSet (its own children are at 1)
		std::string path, fileName, textOf;
		const auto  finish = [&] {
			if (in && !cur.name.empty()) {
				const auto p = NormPath(path);
				cur.output = Trim(fileName).empty() ? std::string() : (p.empty() ? std::string() : p + "\\") + Lower(fileName);
				a_out.push_back(std::move(cur));
				++added;
			}
			cur = SliderSet{};
			in = false;
			depth = 0;
			path.clear();
			fileName.clear();
		};
		XmlHandler h;
		h.open = [&](const XmlElement& a_el, bool) {
			if (a_el.name == "SliderSet") {
				finish();
				in = true;
				cur.name = a_el.AttrOr("name");
				cur.file = a_file;
				return;
			}
			if (!in) {
				return;
			}
			++depth;
			textOf = depth == 1 ? a_el.name : std::string();
			if (depth == 1 && a_el.name == "Slider") {
				SliderInfo s;
				s.name = a_el.AttrOr("name");
				if (s.name.empty()) {
					return;
				}
				// Newer BodySlide writes "default"; sets that generate weights write "small" and "big" (the big one is what a Fallout 4 build uses).
				float raw = 0.0f;
				if (const auto* big = a_el.Attr("big"); big && ParseNumber(*big, raw)) {
					s.def = raw / 100.0f;
				} else if (const auto* def = a_el.Attr("default"); def && ParseNumber(*def, raw)) {
					s.def = raw / 100.0f;
				}
				s.invert = Truthy(a_el.AttrOr("invert"));
				s.morph = !(Truthy(a_el.AttrOr("zap")) || Truthy(a_el.AttrOr("uv")) || Truthy(a_el.AttrOr("clamp")));
				cur.sliders.push_back(std::move(s));
			}
		};
		h.text = [&](std::string_view a_t) {
			if (in && depth == 1) {
				if (textOf == "OutputPath") {
					path += Unescape(a_t);
				} else if (textOf == "OutputFile") {
					fileName += Unescape(a_t);  // taken exactly as written: BodySlide does not trim it
				}
			}
		};
		h.close = [&](std::string_view a_name) {
			if (!in) {
				return;
			}
			if (depth == 0 && a_name == "SliderSet") {
				finish();
				return;
			}
			if (depth > 0) {
				--depth;
			}
			textOf.clear();
		};
		WalkXml(a_text, h);
		finish();
		return added;
	}

	// BuildSelection.xml: the set chosen for an output ("" = none recorded). a_output as SliderSet::output.
	inline std::string BuildChoice(std::string_view a_text, std::string_view a_output)
	{
		std::string choice;
		XmlHandler  h;
		h.open = [&](const XmlElement& a_el, bool) {
			if (a_el.name == "OutputChoice" && choice.empty() && NormPath(a_el.AttrOr("path")) == a_output) {
				choice = a_el.AttrOr("choice");
			}
		};
		WalkXml(a_text, h);
		return choice;
	}

	// The slider set a body output is built with: the user's recorded choice, else the candidate whose morph sliders best match the body's .tri (several sets
	// can write the same output, e.g. BodyTalk4 and BodyTalk4-NeverNude: the one the mesh was built from has all its sliders in the .tri); ties: the first read.
	// a_triMorphs: the lower-case morph names of the .tri, or null to count every morph slider. Null result: no set writes that output.
	inline const SliderSet* PickSet(const std::vector<SliderSet>& a_sets, std::string_view a_output, std::string_view a_choice, const std::unordered_set<std::string>* a_triMorphs = nullptr)
	{
		const SliderSet* best = nullptr;
		long             bestScore = 0;
		for (const auto& s : a_sets) {
			if (s.output != a_output) {
				continue;
			}
			if (!a_choice.empty() && Lower(s.name) == Lower(a_choice)) {
				return &s;
			}
			long score = 0;  // sliders the .tri has, minus sliders it lacks
			for (const auto& i : s.sliders) {
				if (i.morph) {
					score += (!a_triMorphs || a_triMorphs->count(Lower(i.name))) ? 1 : -1;
				}
			}
			if (!best || score > bestScore) {
				best = &s;
				bestScore = score;
			}
		}
		return best;
	}

	// The morph values a preset gives on a body built from a_set: the preset's value, else the set's default, inverted if flagged; zeros left out.
	// Sliders the preset names that the set does not know are taken as written. With no set: the preset as written.
	inline std::vector<std::pair<std::string, float>> Resolve(const Preset& a_preset, const SliderSet* a_set)
	{
		std::vector<std::pair<std::string, float>> out;
		std::unordered_set<std::string>            done;
		const auto                                 valueOf = [&](std::string_view a_name, float& a_v) {
            const auto low = Lower(a_name);
            for (const auto& [n, v] : a_preset.big) {
                if (Lower(n) == low) {
                    a_v = v;
                    return true;
                }
            }
            return false;
		};
		if (a_set) {
			for (const auto& s : a_set->sliders) {
				if (!done.insert(Lower(s.name)).second || !s.morph) {
					continue;
				}
				float v = s.def;
				valueOf(s.name, v);
				if (s.invert) {
					v = 1.0f - v;
				}
				if (std::fabs(v) > 1e-6f) {
					out.emplace_back(s.name, v);
				}
			}
		}
		for (const auto& [n, v] : a_preset.big) {
			if (done.insert(Lower(n)).second && std::fabs(v) > 1e-6f) {
				out.emplace_back(n, v);
			}
		}
		return out;
	}

	// a - b per slider (a slider missing on a side is 0 there); zeros left out.
	inline std::vector<std::pair<std::string, float>> Subtract(std::vector<std::pair<std::string, float>> a_a, const std::vector<std::pair<std::string, float>>& a_b)
	{
		for (const auto& [name, value] : a_b) {
			const auto low = Lower(name);
			const auto it = std::find_if(a_a.begin(), a_a.end(), [&](const auto& m) { return Lower(m.first) == low; });
			if (it != a_a.end()) {
				it->second -= value;
			} else {
				a_a.emplace_back(name, -value);
			}
		}
		a_a.erase(std::remove_if(a_a.begin(), a_a.end(), [](const auto& m) { return std::fabs(m.second) <= 1e-6f; }), a_a.end());
		return a_a;
	}

	// ---- the pool of bodies of one sex -----------------------------------------------------------------------------------------------------------------------
	// Every morph name of a .tri, lower case.
	inline std::unordered_set<std::string> MorphNames(const TriFile& a_tri)
	{
		std::unordered_set<std::string> out;
		for (const auto& shape : a_tri.Shapes()) {
			for (const auto& [name, morph] : shape.morphs) {
				out.insert(name);
			}
		}
		return out;
	}

	// How far a set of morphs moves the body: the RMS displacement over the vertices any of them moves (game units). 0: nothing moves.
	inline float Displacement(const TriFile& a_tri, const std::vector<std::pair<std::string, float>>& a_morphs)
	{
		double      sum = 0.0;
		std::size_t moved = 0;
		for (const auto& shape : a_tri.Shapes()) {
			std::unordered_map<std::uint16_t, std::array<float, 3>> acc;
			for (const auto& [name, value] : a_morphs) {
				const auto it = shape.morphs.find(Lower(name));
				if (it == shape.morphs.end()) {
					continue;
				}
				for (const auto& o : it->second) {
					auto& a = acc[o.index];
					a[0] += o.x * value;
					a[1] += o.y * value;
					a[2] += o.z * value;
				}
			}
			for (const auto& [index, a] : acc) {
				sum += static_cast<double>(a[0]) * a[0] + static_cast<double>(a[1]) * a[1] + static_cast<double>(a[2]) * a[2];
			}
			moved += acc.size();
		}
		return moved ? static_cast<float>(std::sqrt(sum / static_cast<double>(moved))) : 0.0f;
	}

	struct Body
	{
		std::string                                name;    // the preset's name
		std::string                                file;
		std::vector<std::pair<std::string, float>> morphs;  // only morphs the body's .tri has
		std::vector<std::pair<std::string, float>> values;  // 0.8.2: the preset's own slider values (before the base body is subtracted), same filter: Vary() works on these
		float                                      fit = 0.0f;   // share of the preset's own sliders the .tri has
		float                                      size = 0.0f;  // Displacement(): how far from the base body
		int                                        weight = 0;
		bool                                       autoWeight = false;
	};

	struct PoolOptions
	{
		float                                   minFit = 0.5f;      // below this share a preset is for another body
		int                                     defaultWeight = 2;  // a preset with no weight of its own (when autoWeights is off)
		bool                                    autoWeights = true;  // no weight of its own: 3 / 2 / 1 for the third of the pool closest to / in the middle / farthest from the base body
		std::vector<std::string>                exclude;             // lower-case pieces of preset names that are never handed out ("outfit", "zeroed")
		std::unordered_set<std::string>         never;               // lower-case morph names no body ever sets (states other mods drive at run time, like Erection)
		std::function<int(const std::string&)>  weightOf;            // the user's weight for a preset name, or a negative number when none is written (0 = never)
	};

	// The share of a preset's own sliders (zero or not) that a body's .tri has. 0 for a preset that names none.
	inline float Fit(const Preset& a_preset, const std::unordered_set<std::string>& a_triMorphs)
	{
		if (a_preset.big.empty()) {
			return 0.0f;
		}
		std::size_t have = 0;
		for (const auto& [n, v] : a_preset.big) {
			have += a_triMorphs.count(Lower(n));
		}
		return static_cast<float>(have) / static_cast<float>(a_preset.big.size());
	}

	inline bool Excluded(const std::string& a_name, const PoolOptions& a_opt)
	{
		const auto low = Lower(a_name);
		return std::any_of(a_opt.exclude.begin(), a_opt.exclude.end(), [&](const std::string& piece) { return !piece.empty() && low.find(piece) != std::string::npos; });
	}

	// The bodies one sex can get. a_other: the other sex's .tri morph names (a preset that fits the other body better is left to it; pass an empty set when there is none).
	// a_log gets one line per preset that was left out and why.
	// a_base: the resolved values the body on disk was built with (empty = zeroed sliders, the normal case). Morphs are added on top of the mesh on disk, so
	// each body is given as the difference to that base.
	inline std::vector<Body> BuildPool(const std::vector<Preset>& a_presets, const SliderSet* a_set, const TriFile& a_tri, const std::unordered_set<std::string>& a_other, const PoolOptions& a_opt,
		const std::vector<std::pair<std::string, float>>& a_base = {}, std::vector<std::string>* a_log = nullptr)
	{
		const auto        mine = MorphNames(a_tri);
		std::vector<Body> pool;
		const auto        note = [&](const Preset& p, const std::string& why) {
            if (a_log) {
                a_log->push_back("'" + p.name + "' (" + p.file + "): " + why);
            }
		};
		for (const auto& p : a_presets) {
			const float fit = Fit(p, mine);
			if (fit < a_opt.minFit || fit < Fit(p, a_other)) {
				continue;  // another body's preset: not worth a log line
			}
			if (Excluded(p.name, a_opt)) {
				note(p, "left out by sExclude");
				continue;
			}
			const int own = a_opt.weightOf ? a_opt.weightOf(p.name) : -1;
			if (own == 0) {
				note(p, "weight 0 in [PresetWeights]");
				continue;
			}
			Body b;
			b.name = p.name;
			b.file = p.file;
			b.fit = fit;
			const auto resolved = Resolve(p, a_set);
			for (const auto& m : resolved) {
				if (mine.count(Lower(m.first)) && !a_opt.never.count(Lower(m.first))) {
					b.values.push_back(m);
				}
			}
			for (auto& m : Subtract(resolved, a_base)) {
				if (mine.count(Lower(m.first)) && !a_opt.never.count(Lower(m.first))) {
					b.morphs.push_back(std::move(m));
				}
			}
			b.size = Displacement(a_tri, b.morphs);
			// 0.8.3: the preset the body was built with (a_base) is a body too: it differs from the base in nothing, but it sets sliders of its own, so Vary() still
			// gives every NPC a varied copy of it. Only a preset that sets nothing at all (zeroed sliders on a zero-built body) is left out.
			if (b.values.empty() && (b.morphs.empty() || b.size <= 0.0f)) {
				note(p, "changes nothing on this body (all sliders at the base value)");
				continue;
			}
			b.weight = own > 0 ? std::min(own, 100) : 0;
			b.autoWeight = own < 0;
			pool.push_back(std::move(b));
		}
		// weights for the presets that have none written
		std::vector<Body*> unset;
		for (auto& b : pool) {
			if (b.autoWeight) {
				unset.push_back(&b);
			}
		}
		if (a_opt.autoWeights && !pool.empty()) {
			// rank against the whole pool, so a new preset is judged by where it stands among all the bodies
			std::vector<float> sizes;
			for (const auto& b : pool) {
				sizes.push_back(b.size);
			}
			std::sort(sizes.begin(), sizes.end());
			const float lowCut = sizes[(sizes.size() - 1) / 3];
			const float highCut = sizes[(sizes.size() - 1) * 2 / 3];
			for (auto* b : unset) {
				b->weight = sizes.size() < 3 ? 2 : (b->size <= lowCut ? 3 : (b->size <= highCut ? 2 : 1));
			}
		} else {
			for (auto* b : unset) {
				b->weight = std::clamp(a_opt.defaultWeight, 1, 100);
			}
		}
		return pool;
	}

	// One body, each with a chance proportional to its weight. Null for an empty pool.
	template <class Rng>
	[[nodiscard]] const Body* Pick(const std::vector<Body>& a_pool, Rng& a_rng)
	{
		int total = 0;
		for (const auto& b : a_pool) {
			total += std::max(b.weight, 0);
		}
		if (total <= 0) {
			return nullptr;
		}
		int at = std::uniform_int_distribution<int>(0, total - 1)(a_rng);
		for (const auto& b : a_pool) {
			at -= std::max(b.weight, 0);
			if (at < 0) {
				return &b;
			}
		}
		return &a_pool.back();
	}

	// 0.8.2: the morphs of one NPC's copy of a body, every slider the preset sets scaled by its own random factor in [1 - a_share, 1 + a_share] (a_share 0.2 = +-20 %),
	// then the base body subtracted like in BuildPool. Sliders the preset leaves at 0 stay at 0. a_share <= 0: the body's morphs unchanged.
	template <class Rng>
	[[nodiscard]] std::vector<std::pair<std::string, float>> Vary(const Body& a_body, const std::vector<std::pair<std::string, float>>& a_base, float a_share, Rng& a_rng)
	{
		if (!(a_share > 0.0f)) {
			return a_body.morphs;
		}
		a_share = std::min(a_share, 1.0f);
		std::uniform_real_distribution<float> factor(1.0f - a_share, 1.0f + a_share);
		auto                                  values = a_body.values;
		for (auto& [name, value] : values) {
			value *= factor(a_rng);
		}
		std::unordered_set<std::string> keep;  // the morphs this body may set: its own values and what BuildPool kept of the base
		for (const auto& [name, value] : a_body.values) {
			keep.insert(Lower(name));
		}
		for (const auto& [name, value] : a_body.morphs) {
			keep.insert(Lower(name));
		}
		auto out = Subtract(std::move(values), a_base);
		out.erase(std::remove_if(out.begin(), out.end(), [&](const auto& m) { return !keep.count(Lower(m.first)); }), out.end());
		return out;
	}

	// The marker stored with a preset body: no .tri has it, so it moves nothing, but it records which preset the NPC got and keeps the roll even when
	// a later change of the pool would give it another one.
	inline std::string MarkerName(std::string_view a_preset) { return "BMVR_Preset_" + std::string(a_preset); }
}
