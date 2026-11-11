// BodyMorph VR. Copyright (C) 2026 Axel. GPL-3.0-or-later: see LICENSE.
// The BodyGen ini engine: parses the templates.ini / morphs.ini files that Silhouette and the other BodyGen mods ship, and rolls the morphs of an NPC from them.
// Pure C++, no game types: the game side (main.cpp) reads the files, resolves plugins / form ids / races through a Resolver and describes the NPC with NpcInfo.
// The grammar and the quirks were taken from the description of the format in Silhouette's docs/bodygen-format.md (a behaviour description, not LooksMenu's code).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bodymorph::genini
{
	inline std::string_view Trim(std::string_view a_s)
	{
		while (!a_s.empty() && (a_s.front() == ' ' || a_s.front() == '\t' || a_s.front() == '\r' || a_s.front() == '\n')) {
			a_s.remove_prefix(1);
		}
		while (!a_s.empty() && (a_s.back() == ' ' || a_s.back() == '\t' || a_s.back() == '\r' || a_s.back() == '\n')) {
			a_s.remove_suffix(1);
		}
		return a_s;
	}

	inline std::string Lower(std::string_view a_s)
	{
		std::string out(a_s);
		for (auto& c : out) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return out;
	}

	inline bool EqualsNoCase(std::string_view a_a, std::string_view a_b) { return Lower(a_a) == Lower(a_b); }

	// Splits on a separator, keeping empty pieces (callers decide what an empty piece means).
	inline std::vector<std::string_view> Split(std::string_view a_s, char a_sep)
	{
		std::vector<std::string_view> out;
		std::size_t                   start = 0;
		for (;;) {
			const auto pos = a_s.find(a_sep, start);
			if (pos == std::string_view::npos) {
				out.push_back(a_s.substr(start));
				return out;
			}
			out.push_back(a_s.substr(start, pos - start));
			start = pos + 1;
		}
	}

	// atof semantics (the format is read with atof): leading number, anything else is 0.
	inline float ParseFloat(std::string_view a_s)
	{
		const std::string text(Trim(a_s));
		return text.empty() ? 0.0f : std::strtof(text.c_str(), nullptr);
	}

	// A form id as written in morphs.ini: hex, no load-order byte. False when it is not hex.
	inline bool ParseHex(std::string_view a_s, std::uint32_t& a_out)
	{
		a_s = Trim(a_s);
		if (a_s.size() > 2 && a_s[0] == '0' && (a_s[1] == 'x' || a_s[1] == 'X')) {
			a_s.remove_prefix(2);
		}
		if (a_s.empty() || a_s.size() > 8) {
			return false;
		}
		std::uint32_t v = 0;
		for (const char c : a_s) {
			int d = 0;
			if (c >= '0' && c <= '9') {
				d = c - '0';
			} else if (c >= 'a' && c <= 'f') {
				d = c - 'a' + 10;
			} else if (c >= 'A' && c <= 'F') {
				d = c - 'A' + 10;
			} else {
				return false;
			}
			v = (v << 4) | static_cast<std::uint32_t>(d);
		}
		a_out = v;
		return true;
	}

	// ---- templates -------------------------------------------------------------------------------------------------------------------------------------------
	struct Selector  // MorphName@low:high (or @value)
	{
		std::string name;
		float       lo = 0.0f;
		float       hi = 0.0f;
	};
	struct Morph  // selector | selector | ... : one of them is chosen at random
	{
		std::vector<Selector> alts;
	};
	struct MorphSet  // morph , morph , ... : every morph is applied
	{
		std::vector<Morph> morphs;
	};
	struct Template  // set / set / ... : one set is chosen at random
	{
		std::vector<MorphSet> sets;
	};

	// Parses one templates.ini line. False for a comment / empty line (a_malformed = false) or for a line that LooksMenu would drop (a_malformed = true).
	inline bool ParseTemplateLine(std::string_view a_line, std::string& a_name, Template& a_out, bool& a_malformed)
	{
		a_malformed = false;
		a_line = Trim(a_line);
		if (a_line.empty() || a_line.front() == '#') {
			return false;
		}
		a_malformed = true;
		const auto sides = Split(a_line, '=');  // only the first two pieces are used
		if (sides.size() < 2) {
			return false;
		}
		const auto name = Trim(sides[0]);
		const auto body = Trim(sides[1]);
		if (name.empty() || body.empty()) {
			return false;
		}
		Template t;
		for (const auto setText : Split(body, '/')) {
			MorphSet set;
			for (const auto morphText : Split(Trim(setText), ',')) {
				if (Trim(morphText).empty()) {
					continue;
				}
				Morph morph;
				for (const auto selText : Split(Trim(morphText), '|')) {
					if (Trim(selText).empty()) {
						continue;
					}
					const auto parts = Split(Trim(selText), '@');
					if (parts.size() < 2) {
						return false;  // missing '@'
					}
					const auto selName = Trim(parts[0]);
					const auto value = Trim(parts[1]);
					if (selName.empty() || value.empty()) {
						return false;
					}
					const auto range = Split(value, ':');
					Selector   sel;
					sel.name = std::string(selName);
					sel.lo = ParseFloat(range[0]);
					sel.hi = range.size() > 1 ? ParseFloat(range[1]) : sel.lo;
					if (sel.hi < sel.lo) {
						std::swap(sel.lo, sel.hi);
					}
					morph.alts.push_back(std::move(sel));
				}
				if (!morph.alts.empty()) {
					set.morphs.push_back(std::move(morph));
				}
			}
			if (!set.morphs.empty()) {
				t.sets.push_back(std::move(set));
			}
		}
		if (t.sets.empty()) {
			return false;
		}
		a_name = std::string(name);
		a_out = std::move(t);
		a_malformed = false;
		return true;
	}

	// ---- rules -----------------------------------------------------------------------------------------------------------------------------------------------
	struct Group  // Template | Template | ... : one is chosen at random (list a template twice to double its chance)
	{
		std::vector<std::string> alts;
	};

	struct Rule
	{
		enum class Kind
		{
			kAll,     // All|Gender|Race
			kPlugin,  // Plugin.esp|All|Gender|Race : every NPC the plugin defines
			kForm     // Plugin.esp|FormID|Gender   : one NPC, or every NPC of a leveled list
		};
		Kind                      kind = Kind::kAll;
		std::string               plugin;
		std::uint32_t             localForm = 0;  // kForm: the id without the load-order byte
		int                       sex = -1;       // -1 both, 0 male, 1 female
		std::string               race;           // editor id; "" = none written
		std::vector<Group>        groups;
		std::string               source;  // "file:line", for the log
		std::string               key;     // the left side as written, for the log
		// filled in by Db::Finalize
		bool                      valid = false;
		int                       pluginIndex = -1;  // load-order byte
		std::vector<std::uint32_t> forms;             // kForm: the form ids it names (a leveled list expanded)
	};

	// Parses one morphs.ini line. Same return convention as ParseTemplateLine.
	inline bool ParseRuleLine(std::string_view a_line, Rule& a_out, bool& a_malformed)
	{
		a_malformed = false;
		a_line = Trim(a_line);
		if (a_line.empty() || a_line.front() == '#') {
			return false;
		}
		a_malformed = true;
		const auto sides = Split(a_line, '=');
		if (sides.size() < 2) {
			return false;
		}
		const auto key = Trim(sides[0]);
		const auto rhs = Trim(sides[1]);
		if (key.empty() || rhs.empty()) {
			return false;
		}
		std::vector<std::string_view> pieces;
		for (const auto p : Split(key, '|')) {
			pieces.push_back(Trim(p));
		}
		Rule r;
		r.key = std::string(key);
		std::size_t next = 0;  // the first piece after the plugin / "All" part
		if (EqualsNoCase(pieces[0].substr(0, 3), "all")) {
			r.kind = Rule::Kind::kAll;  // a plugin whose name starts with "all" is read as All, like LooksMenu does
			next = 1;
		} else {
			r.plugin = std::string(pieces[0]);
			if (pieces.size() < 2 || pieces[1].empty()) {
				return false;
			}
			if (EqualsNoCase(pieces[1], "all")) {
				r.kind = Rule::Kind::kPlugin;
			} else if (ParseHex(pieces[1], r.localForm)) {
				r.kind = Rule::Kind::kForm;
			} else {
				return false;
			}
			next = 2;
		}
		// the optional gender, then (not for a single form) the race; empty pieces mean "not written"
		std::vector<std::string_view> rest;
		for (std::size_t i = next; i < pieces.size(); ++i) {
			rest.push_back(pieces[i]);
		}
		std::size_t at = 0;
		if (at < rest.size() && EqualsNoCase(rest[at], "male")) {
			r.sex = 0;
			++at;
		} else if (at < rest.size() && EqualsNoCase(rest[at], "female")) {
			r.sex = 1;
			++at;
		} else if (at < rest.size() && rest[at].empty() && rest.size() > 1) {
			++at;  // "All||HumanRace": gender left out
		}
		if (r.kind != Rule::Kind::kForm && at < rest.size()) {
			r.race = std::string(rest[at]);
		}
		for (const auto groupText : Split(rhs, ',')) {
			Group g;
			for (const auto alt : Split(Trim(groupText), '|')) {
				if (!Trim(alt).empty()) {
					g.alts.emplace_back(Trim(alt));
				}
			}
			if (!g.alts.empty()) {
				r.groups.push_back(std::move(g));
			}
		}
		a_out = std::move(r);
		a_malformed = false;
		return true;
	}

	// What the game side tells the engine about the NPC (one entry per level of the template chain).
	struct NpcInfo
	{
		std::uint32_t formID = 0;
		bool          isNpc = true;         // false: a leveled list met on the template chain (only FormID lines can name it)
		bool          hasTemplate = false;  // the NPC has a template (All / plugin lines list only NPCs without one)
		std::string   race;                 // the editor id of its race ("HumanRace"); "" = the NPC has no race
	};

	// How the game side turns names into ids once all the files are read. (Races are compared by editor id when matching, so they need no lookup.)
	struct Resolver
	{
		std::function<int(const std::string&)>                                              pluginIndex;  // load-order byte of a loaded plugin, -1 = not loaded
		std::function<void(const std::string&, std::uint32_t, std::vector<std::uint32_t>&)> forms;        // the form id(s) a FormID line names (leveled list expanded)
	};

	class Db
	{
	public:
		struct Stats
		{
			std::size_t templates = 0;
			std::size_t malformedTemplates = 0;
			std::size_t rules = 0;
			std::size_t malformedRules = 0;
			std::size_t validRules = 0;
			std::size_t missingTemplates = 0;  // alternatives dropped because no such template exists
		};

		// A later definition replaces an earlier one. Problems are appended to a_problems (first ones only; the counters have the totals).
		void AddTemplates(std::string_view a_text, const std::string& a_source, std::vector<std::string>* a_problems = nullptr)
		{
			int lineNo = 0;
			for (const auto line : Split(a_text, '\n')) {
				++lineNo;
				std::string name;
				Template    t;
				bool        bad = false;
				if (ParseTemplateLine(line, name, t, bad)) {
					m_templates[Lower(name)] = std::move(t);
				} else if (bad) {
					++m_stats.malformedTemplates;
					Note(a_problems, a_source, lineNo, "malformed template dropped", line);
				}
			}
			m_stats.templates = m_templates.size();
		}

		void AddRules(std::string_view a_text, const std::string& a_source, std::vector<std::string>* a_problems = nullptr)
		{
			int lineNo = 0;
			for (const auto line : Split(a_text, '\n')) {
				++lineNo;
				Rule r;
				bool bad = false;
				if (ParseRuleLine(line, r, bad)) {
					r.source = a_source + ":" + std::to_string(lineNo);
					m_rules.push_back(std::move(r));
				} else if (bad) {
					++m_stats.malformedRules;
					Note(a_problems, a_source, lineNo, "malformed line ignored", line);
				}
			}
			m_stats.rules = m_rules.size();
		}

		// Call once, after every file has been added (templates are looked up now: all templates files are read before the morphs files, as LooksMenu does).
		void Finalize(const Resolver& a_res, std::vector<std::string>* a_problems = nullptr)
		{
			m_stats.validRules = 0;
			m_stats.missingTemplates = 0;
			for (auto& r : m_rules) {
				r.valid = false;
				r.forms.clear();
				r.pluginIndex = a_res.pluginIndex ? a_res.pluginIndex(r.plugin) : -1;
				if (r.kind != Rule::Kind::kAll && r.pluginIndex < 0) {
					Note(a_problems, r.source, 0, "plugin '" + r.plugin + "' is not loaded: line ignored", r.key);
					continue;
				}
				if (r.kind == Rule::Kind::kForm) {
					if (a_res.forms) {
						a_res.forms(r.plugin, r.localForm, r.forms);
					}
					if (r.forms.empty()) {
						Note(a_problems, r.source, 0, "form not found: line ignored", r.key);
						continue;
					}
				}
				for (auto& g : r.groups) {
					std::vector<std::string> kept;
					for (auto& alt : g.alts) {
						if (m_templates.count(Lower(alt))) {
							kept.push_back(alt);
						} else {
							++m_stats.missingTemplates;
							Note(a_problems, r.source, 0, "template '" + alt + "' not found: option dropped", r.key);
						}
					}
					g.alts = std::move(kept);
				}
				r.groups.erase(std::remove_if(r.groups.begin(), r.groups.end(), [](const Group& g) { return g.alts.empty(); }), r.groups.end());
				r.valid = true;  // a line whose templates are all missing still counts: it overwrites earlier lines with "nothing", like LooksMenu
				++m_stats.validRules;
			}
		}

		// The line that decides for this NPC (the last one that matches wins), or null.
		[[nodiscard]] const Rule* Match(const NpcInfo& a_npc, bool a_female) const
		{
			for (auto it = m_rules.rbegin(); it != m_rules.rend(); ++it) {
				const Rule& r = *it;
				if (!r.valid || (r.sex >= 0 && (r.sex == 1) != a_female)) {
					continue;
				}
				switch (r.kind) {
				case Rule::Kind::kAll:
					if (a_npc.isNpc && !a_npc.hasTemplate && RaceMatches(a_npc, r)) {
						return &r;
					}
					break;
				case Rule::Kind::kPlugin:
					if (a_npc.isNpc && !a_npc.hasTemplate && static_cast<int>(a_npc.formID >> 24) == r.pluginIndex && RaceMatches(a_npc, r)) {
						return &r;
					}
					break;
				case Rule::Kind::kForm:
					if (std::find(r.forms.begin(), r.forms.end(), a_npc.formID) != r.forms.end()) {
						return &r;
					}
					break;
				}
			}
			return nullptr;
		}

		// Follows the NPC's template chain (first entry = the actor's base) until a level has a line.
		[[nodiscard]] const Rule* MatchChain(const std::vector<NpcInfo>& a_chain, bool a_female) const
		{
			for (const auto& level : a_chain) {
				if (const auto* r = Match(level, a_female)) {
					return r;
				}
			}
			return nullptr;
		}

		// Rolls what a line gives one actor: per group one template, per template one set, per morph one selector; a value of exactly 0 is skipped
		// (as LooksMenu does: that is why a template that rolls only zeros leaves the actor with nothing). Later results for the same morph name win.
		template <class Rng>
		[[nodiscard]] std::vector<std::pair<std::string, float>> Roll(const Rule& a_rule, Rng& a_rng) const
		{
			std::vector<std::pair<std::string, float>> out;
			for (const auto& g : a_rule.groups) {
				if (g.alts.empty()) {
					continue;
				}
				const auto it = m_templates.find(Lower(g.alts[Pick(a_rng, g.alts.size())]));
				if (it == m_templates.end() || it->second.sets.empty()) {
					continue;
				}
				const auto& set = it->second.sets[Pick(a_rng, it->second.sets.size())];
				for (const auto& morph : set.morphs) {
					const auto& sel = morph.alts[Pick(a_rng, morph.alts.size())];
					float       value = sel.lo;
					if (sel.hi > sel.lo) {
						value = sel.lo + (sel.hi - sel.lo) * std::uniform_real_distribution<float>(0.0f, 1.0f)(a_rng);
					}
					if (value == 0.0f) {
						continue;
					}
					bool replaced = false;
					for (auto& have : out) {
						if (Lower(have.first) == Lower(sel.name)) {
							have.second = value;
							replaced = true;
							break;
						}
					}
					if (!replaced) {
						out.emplace_back(sel.name, value);
					}
				}
			}
			return out;
		}

		[[nodiscard]] const Stats& GetStats() const noexcept { return m_stats; }
		[[nodiscard]] bool         HasTemplate(std::string_view a_name) const { return m_templates.count(Lower(a_name)) != 0; }
		[[nodiscard]] const std::vector<Rule>& Rules() const noexcept { return m_rules; }

	private:
		// LooksMenu: npc->race == nullptr || npc->race == filter. A line with no race written therefore matches only NPCs that have none.
		static bool RaceMatches(const NpcInfo& a_npc, const Rule& a_rule) { return a_npc.race.empty() || EqualsNoCase(a_npc.race, a_rule.race); }

		template <class Rng>
		static std::size_t Pick(Rng& a_rng, std::size_t a_n)
		{
			return a_n <= 1 ? 0 : std::uniform_int_distribution<std::size_t>(0, a_n - 1)(a_rng);
		}

		static void Note(std::vector<std::string>* a_problems, const std::string& a_source, int a_line, const std::string& a_what, std::string_view a_text)
		{
			if (a_problems && a_problems->size() < 40) {
				a_problems->push_back(a_source + (a_line ? ":" + std::to_string(a_line) : std::string()) + ": " + a_what + ": " + std::string(Trim(a_text)).substr(0, 120));
			}
		}

		std::unordered_map<std::string, Template> m_templates;
		std::vector<Rule>                         m_rules;
		Stats                                     m_stats;
	};
}
