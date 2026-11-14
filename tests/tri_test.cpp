// BodyMorph VR unit tests (no game): TRI reader and CPU morph application. GPL-3.0-or-later.
#include "../src/BodyGenIni.h"
#include "../src/BodySlidePresets.h"
#include "../src/IniFile.h"
#include "../src/MorphApply.h"
#include "../src/MorphList.h"
#include "../src/MorphStore.h"
#include "../src/TriFile.h"
#include "../src/TriPath.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace bodymorph;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Writer {
	std::vector<std::byte> b;
	template <class T> void put(T v) { auto p = reinterpret_cast<const std::byte*>(&v); b.insert(b.end(), p, p + sizeof(T)); }
	void name(const std::string& s) { put<std::uint8_t>(static_cast<std::uint8_t>(s.size())); for (char c : s) b.push_back(static_cast<std::byte>(c)); }
};

static std::vector<std::byte> SampleTri(bool a_uv)
{
	Writer w;
	w.b = { std::byte{'P'}, std::byte{'I'}, std::byte{'R'}, std::byte{'T'} };  // bytes of a real BodySlide .tri
	w.put<std::uint16_t>(2);
	w.name("CBBE");                 // shape 1
	w.put<std::uint16_t>(2);
	w.name("Breasts");              // morph: 2 vertices, one zero (dropped)
	w.put<float>(0.01F);
	w.put<std::uint16_t>(2);
	w.put<std::uint16_t>(1); w.put<std::int16_t>(100); w.put<std::int16_t>(-200); w.put<std::int16_t>(300);
	w.put<std::uint16_t>(2); w.put<std::int16_t>(0); w.put<std::int16_t>(0); w.put<std::int16_t>(0);
	w.name("AllZero");              // morph with only zero offsets: not kept
	w.put<float>(1.0F);
	w.put<std::uint16_t>(1);
	w.put<std::uint16_t>(0); w.put<std::int16_t>(0); w.put<std::int16_t>(0); w.put<std::int16_t>(0);
	w.name("Hands");                // shape 2, no morphs
	w.put<std::uint16_t>(0);
	if (a_uv) {
		w.put<std::uint16_t>(1);
		w.name("CBBE");
		w.put<std::uint16_t>(1);
		w.name("UVThing");
		w.put<float>(0.5F);
		w.put<std::uint16_t>(1);
		w.put<std::uint16_t>(0); w.put<std::int16_t>(1); w.put<std::int16_t>(1);
	}
	return w.b;
}

static bool Near(float a, float b, float eps = 1e-4F) { return std::fabs(a - b) <= eps; }

int main()
{
	// --- TRI parsing
	for (bool uv : { false, true }) {
		auto data = SampleTri(uv);
		TriFile tri;
		std::string err;
		CHECK(tri.Parse(data, &err));
		CHECK(tri.Shapes().size() == 2);
		CHECK(tri.UVMorphCount() == (uv ? 1u : 0u));
		const Morph* m = tri.FindMorph("cbbe", "BREASTS");
		CHECK(m && m->size() == 1);
		if (m && !m->empty()) {
			CHECK((*m)[0].index == 1);
			CHECK(Near((*m)[0].x, 1.0F) && Near((*m)[0].y, -2.0F) && Near((*m)[0].z, 3.0F));
		}
		CHECK(tri.FindMorph("CBBE", "AllZero") == nullptr);
		CHECK(tri.FindShape("hands") != nullptr);
		CHECK(tri.FindMorph("Nope", "Breasts") == nullptr);
	}
	// truncated at every length: never crashes, fails cleanly (except the full length, and the pre-UV cut which is valid)
	{
		auto full = SampleTri(true);
		auto noUV = SampleTri(false);
		for (std::size_t n = 0; n < full.size(); ++n) {
			TriFile tri;
			std::string err;
			bool ok = tri.Parse(std::span(full.data(), n), &err);
			CHECK(ok == (n == noUV.size()));
			if (!ok) CHECK(tri.Shapes().empty() && !err.empty());
		}
	}
	{
		std::vector<std::byte> bad = { std::byte{'T'}, std::byte{'R'}, std::byte{'I'}, std::byte{'X'}, std::byte{0}, std::byte{0} };
		TriFile tri;
		CHECK(!tri.Parse(bad));
	}

	// --- INI morph list and instance suffix
	{
		auto l = ParseMorphList(" Breasts:0.5, Butt:1 ;7B Upper:-0.25,, Waist , Odd:x, :3 ,Neg:-2");
		CHECK(l.size() == 6);
		if (l.size() == 6) {
			CHECK(l[0].name == "Breasts" && Near(l[0].weight, 0.5F));
			CHECK(l[1].name == "Butt" && Near(l[1].weight, 1.0F));
			CHECK(l[2].name == "7B Upper" && Near(l[2].weight, -0.25F));
			CHECK(l[3].name == "Waist" && Near(l[3].weight, 1.0F));
			CHECK(l[4].name == "Odd:x" && Near(l[4].weight, 1.0F));  // not a number: part of the name
			CHECK(l[5].name == "Neg" && Near(l[5].weight, -2.0F));
		}
		CHECK(ParseMorphList("").empty());
		CHECK(ParseMorphList(" , ; ").empty());
		CHECK(StripInstanceSuffix("CBBE:0") == "CBBE");
		CHECK(StripInstanceSuffix("CBBE:12") == "CBBE");
		CHECK(StripInstanceSuffix("CBBE") == "CBBE");
		CHECK(StripInstanceSuffix("A:b") == "A:b");
		CHECK(StripInstanceSuffix("A:") == "A:");
		CHECK(StripInstanceSuffix("NakedHands (00000D6C)[1]/SkinNaked (00000D64)") == "NakedHands (00000D6C)[1]/SkinNaked (00000D64)");
		CHECK(StripInstanceSuffix("") == "");
	}

	// --- shape aliases
	{
		auto a = ParseShapeAliases(" NakedTorso=CBBE , Skin (x)[1]/Y = Body Shape;bad;=x;y=; Z=Q=R");
		CHECK(a.size() == 3);
		if (a.size() == 3) {
			CHECK(a[0].prefix == "NakedTorso" && a[0].shape == "CBBE");
			CHECK(a[1].prefix == "Skin (x)[1]/Y" && a[1].shape == "Body Shape");
			CHECK(a[2].prefix == "Z=Q" && a[2].shape == "R");
		}
		CHECK(ParseShapeAliases("").empty());
		CHECK(StartsWithNoCase("NakedTorso (00000D67)[1]/SkinNaked (00000D64)", "nakedtorso"));
		CHECK(!StartsWithNoCase("NakedHands (00000D6C)[1]/SkinNaked (00000D64)", "NakedTorso"));
		CHECK(!StartsWithNoCase("Nak", "NakedTorso"));
		CHECK(StartsWithNoCase("abc", ""));
	}

	// --- INI reader
	{
		IniFile ini;
		ini.Parse("; comment\r\n[Keys]\r\niApplyKey=118\r\n  iBad = 12x \r\n# other\n[Morph]\nsMorphs = A:1; B:0.5 , C \nsTriFile=Data\\Meshes\\x.tri\n[keys]\nIRESTOREKEY=117\nnoequals\n[Empty]\nk=\n");
		CHECK(ini.GetInt("keys", "iApplyKey", 0) == 118);
		CHECK(ini.GetInt("Keys", "iRestoreKey", 0) == 117);   // section and key case-insensitive, sections merge
		CHECK(ini.GetInt("Keys", "iBad", 7) == 7);             // not a whole number: default
		CHECK(ini.GetInt("Keys", "iMissing", 5) == 5);
		CHECK(ini.GetString("Morph", "sMorphs", "x") == "A:1; B:0.5 , C");  // ';' inside a value is kept
		CHECK(ini.GetString("Morph", "sTriFile", "") == "Data\\Meshes\\x.tri");
		CHECK(ini.GetString("Empty", "k", "d").empty());
		CHECK(ini.GetString("Nope", "k", "d") == "d");
		ini.Parse("");
		CHECK(ini.Size() == 0);
	}

	// --- half floats
	for (float f : { 0.0F, 1.0F, -2.5F, 65504.0F, 0.000061035156F, 0.0000000596F, 12.34F, -0.001F }) {
		float back = HalfToFloat(FloatToHalf(f));
		CHECK(Near(back, f, std::fabs(f) * 1e-3F + 1e-7F));
	}
	CHECK(FloatToHalf(1.0F) == 0x3C00);
	CHECK(FloatToHalf(-2.0F) == 0xC000);
	CHECK(std::isinf(HalfToFloat(FloatToHalf(1e6F))));

	// --- applying morphs, half and full precision blocks
	auto tri = TriFile{};
	auto data = SampleTri(false);
	CHECK(tri.Parse(data));
	const Morph* breasts = tri.FindMorph("CBBE", "Breasts");
	for (bool full : { false, true }) {
		const std::uint32_t stride = full ? 32 : 16;  // bytes; desc low nibble = stride / 4
		std::uint64_t desc = (stride / 4) | (static_cast<std::uint64_t>(kFlagVertex | (full ? kFlagFullPrecision : 0)) << 44);
		const std::uint32_t count = 3;
		std::vector<std::byte> block(stride * count, std::byte{0xAB});  // 0xAB = other attributes, must survive
		for (std::uint32_t i = 0; i < count; ++i) {
			float p[3] = { float(i), float(i) * 2, float(i) * 3 };
			if (full) std::memcpy(block.data() + stride * i, p, 12);
			else { std::uint16_t h[3] = { FloatToHalf(p[0]), FloatToHalf(p[1]), FloatToHalf(p[2]) }; std::memcpy(block.data() + stride * i, h, 6); }
		}
		std::vector<Position> base;
		CHECK(ReadPositions(block, desc, count, base));
		CHECK(base.size() == 3 && Near(base[2].z, 6.0F));

		auto copy = block;
		Morph outside = { { 7, 1, 1, 1 } };  // index past the mesh
		std::pair<const Morph*, float> ms[] = { { breasts, 0.5F }, { &outside, 1.0F }, { nullptr, 1.0F } };
		CHECK(WritePositions(copy, desc, base, ms) == 1);
		std::vector<Position> after;
		CHECK(ReadPositions(copy, desc, count, after));
		CHECK(Near(after[0].x, 0.0F) && Near(after[2].y, 4.0F));                  // untouched vertices
		CHECK(Near(after[1].x, 1.5F, 1e-2F) && Near(after[1].y, 1.0F, 1e-2F) && Near(after[1].z, 4.5F, 1e-2F));
		for (std::uint32_t i = 0; i < count; ++i)                                  // other attributes intact
			for (std::uint32_t k = full ? 12 : 6; k < stride; ++k) CHECK(copy[stride * i + k] == std::byte{0xAB});

		// weight 0 restores the base exactly (morphs never accumulate)
		std::pair<const Morph*, float> none[] = { { breasts, 0.0F } };
		WritePositions(copy, desc, base, none);
		CHECK(std::memcmp(copy.data(), block.data(), copy.size()) == 0);

		// too small a block / no position flag
		CHECK(!ReadPositions(std::span(block.data(), block.size() - 1), desc, count, after));
		CHECK(!ReadPositions(block, desc & ~(std::uint64_t{ kFlagVertex } << 44), count, after));
	}

	// --- BODYTRI value -> file path
	{
		CHECK(BodyTriToDataPath("Actors\\Character\\CharacterAssets\\FemaleBody.tri") == "Data\\Meshes\\Actors\\Character\\CharacterAssets\\FemaleBody.tri");
		CHECK(BodyTriToDataPath("Armor/ArmoredCoat/OutfitF.TRI") == "Data\\Meshes\\Armor\\ArmoredCoat\\OutfitF.TRI");   // slashes, case
		CHECK(BodyTriToDataPath("meshes\\Armor\\x.tri") == "Data\\meshes\\Armor\\x.tri");                            // already has Meshes
		CHECK(BodyTriToDataPath("Data\\Meshes\\Armor\\x.tri") == "Data\\Meshes\\Armor\\x.tri");                    // already has Data
		CHECK(BodyTriToDataPath(" \\Armor\\x.tri ") == "Data\\Meshes\\Armor\\x.tri");                                // stray space / slash
		CHECK(BodyTriToDataPath("").empty());
		CHECK(BodyTriToDataPath("Armor\\x.nif").empty());                                                                 // not a .tri
		CHECK(BodyTriToDataPath(".tri").empty());
		CHECK(BodyTriToDataPath("C:\\x\\y.tri").empty());                                                                // drive letter
		CHECK(BodyTriToDataPath("..\\..\\y.tri").empty());                                                               // leaves Meshes
		CHECK(BodyTriToDataPath(std::string("Armor\\x\x01.tri")).empty());                                               // control character
	}

	// --- per-actor morph store (the Papyrus BodyGen table)
	{
		MorphStore st;
		const std::uint32_t kActor = 0x0001A2B3, kKw1 = 0x0A00F001, kKw2 = 0x0A00F002;
		CHECK(!st.Has(kActor, true));
		st.Set(kActor, true, "Breasts", 0, 0.5F);
		st.Set(kActor, true, "breasts", kKw1, 0.8F);   // same morph (case-insensitive), another keyword
		st.Set(kActor, true, "Breasts", kKw2, -0.2F);
		st.Set(kActor, true, "Silhouette_Marker", kKw1, 1.0F);   // a marker morph no .tri contains
		CHECK(st.Has(kActor, true) && !st.Has(kActor, false));    // one table per sex
		CHECK(Near(st.Get(kActor, true, "BREASTS", 0), 0.5F));
		CHECK(Near(st.Get(kActor, true, "Breasts", kKw1), 0.8F));
		CHECK(Near(st.Get(kActor, true, "Breasts", 0x1234), 0.0F));
		CHECK(Near(st.Get(kActor, false, "Breasts", 0), 0.0F));
		auto names = st.Names(kActor, true);
		CHECK(names.size() == 2 && names[0] == "Breasts" && names[1] == "Silhouette_Marker");   // first spelling kept
		auto kws = st.Keywords(kActor, true, "Breasts");
		CHECK(kws.size() == 2 && kws[0] == kKw1 && kws[1] == kKw2);                              // never the "None" key
		auto eff = st.Effective(kActor, true);
		CHECK(eff.size() == 2);
		CHECK(eff[0].name == "Breasts" && Near(eff[0].weight, 0.8F));                            // the MAXIMUM over the keys, not the sum
		CHECK(eff[1].name == "Silhouette_Marker" && Near(eff[1].weight, 1.0F));

		st.Set(kActor, true, "Breasts", kKw1, 0.0F);    // 0 removes that key
		CHECK(Near(st.Effective(kActor, true)[0].weight, 0.5F));
		st.RemoveByKeyword(kActor, true, kKw2);
		st.RemoveByKeyword(kActor, true, kKw1);          // the marker loses its only key: it disappears
		CHECK(st.Names(kActor, true).size() == 1);
		st.RemoveByName(kActor, true, "BREASTS");
		CHECK(!st.Has(kActor, true));                    // an empty table is "no stored morphs"
		st.Set(kActor, true, "Neg", 0, -0.3F);
		CHECK(Near(st.Effective(kActor, true)[0].weight, -0.3F));
		st.Set(kActor, true, "", 0, 1.0F);               // empty names are ignored
		CHECK(st.Names(kActor, true).size() == 1);

		// serialise / merge back, with a load order that moved the actor and dropped one keyword
		st.Set(kActor, true, "Hips", kKw1, 0.25F);
		st.Set(kActor, true, "Hips", kKw2, 0.75F);
		st.Set(0x00000014, false, "Arms", 0, 1.5F);
		const auto records = st.Serialize();
		CHECK(records.size() == 2);
		MorphStore copy;
		for (const auto& r : records) {
			CHECK(copy.Deserialize(r.bytes.data(), r.bytes.size(), [&](std::uint32_t& id) {
				if (id == kKw2) return false;            // plugin gone
				if (id == kActor) id = 0x0200A2B3;       // load order shifted
				return true;
			}));
		}
		CHECK(copy.Has(0x0200A2B3, true) && !copy.Has(kActor, true));
		CHECK(Near(copy.Get(0x0200A2B3, true, "Neg", 0), -0.3F));
		CHECK(Near(copy.Get(0x0200A2B3, true, "Hips", kKw1), 0.25F));
		CHECK(Near(copy.Get(0x0200A2B3, true, "Hips", kKw2), 0.0F));   // dropped keyword
		CHECK(Near(copy.Get(0x14, false, "Arms", 0), 1.5F));
		MorphStore gone;   // the actor's plugin is missing: intact record, nothing merged
		CHECK(gone.Deserialize(records[0].bytes.data(), records[0].bytes.size(), [](std::uint32_t&) { return false; }) && gone.TableCount() == 0);
		// damaged records are refused without crashing, whatever the cut
		for (const auto& r : records) {
			for (std::size_t cut = 0; cut < r.bytes.size(); ++cut) {
				MorphStore bad;
				CHECK(!bad.Deserialize(r.bytes.data(), cut, [](std::uint32_t&) { return true; }));
				CHECK(bad.TableCount() == 0);
			}
		}
		st.RemoveActor(0x14);
		CHECK(!st.Has(0x14, false));
		st.Clear();
		CHECK(st.TableCount() == 0);
	}

	// --- BodyGen ini engine (templates.ini / morphs.ini)
	{
		namespace gi = bodymorph::genini;
		gi::Db                   db;
		std::vector<std::string> problems;
		db.AddTemplates(
			"# a comment\r\n"
			"Slim = Breasts@-0.5:0.2, Waist@-1\r\n"
			"Pick = A@1 / B@2 , C@3|D@4\r\n"
			"Zero = Nothing@0\r\n"
			"NoAt = Breasts 0.5\r\n"
			"NoValue = Breasts@\r\n"
			"= Breasts@1\r\n"
			"Slim = Hips@1.5\r\n"  // a later definition replaces the earlier one
			"Big = Breasts@1:2 ,  Hips @ 3\r\n"
			"Twice = X@1=ignored\r\n"
			"Neg = M@-0.5:-0.1\r\n"
			"Swap = M@2:1\r\n",
			"templates.ini", &problems);
		CHECK(db.GetStats().templates == 7);  // Slim, Pick, Zero, Big, Twice, Neg, Swap
		CHECK(db.GetStats().malformedTemplates == 3);
		CHECK(db.HasTemplate("slim") && db.HasTemplate("BIG") && !db.HasTemplate("NoAt"));
		CHECK(problems.size() == 3);

		db.AddRules(
			"All|Female|HumanRace = Slim|Big\r\n"
			"All|Male|HumanRace = Big\r\n"
			"All||GhoulRace = Pick\r\n"
			"Mod.esp|All|Female|HumanRace = Big , Zero\r\n"
			"Mod.esp|1A2B|Female = Slim\r\n"
			"Mod.esp|0xFF = Missing\r\n"
			"Broken line\r\n"
			"Mod.esp|nothex|Female = Slim\r\n"
			"Gone.esp|All|HumanRace = Slim\r\n"
			"Allied.esp|Male|HumanRace = Big\r\n"  // a plugin name that starts with "all" is read as All
			"All|Female|GhoulRace = slim|Nope\r\n",
			"morphs.ini", &problems);
		CHECK(db.GetStats().rules == 9 && db.GetStats().malformedRules == 2);
		const auto& rules = db.Rules();
		CHECK(rules[0].kind == gi::Rule::Kind::kAll && rules[0].sex == 1 && rules[0].race == "HumanRace" && rules[0].groups.size() == 1 && rules[0].groups[0].alts.size() == 2);
		CHECK(rules[2].kind == gi::Rule::Kind::kAll && rules[2].sex == -1 && rules[2].race == "GhoulRace");
		CHECK(rules[3].kind == gi::Rule::Kind::kPlugin && rules[3].plugin == "Mod.esp" && rules[3].groups.size() == 2);
		CHECK(rules[4].kind == gi::Rule::Kind::kForm && rules[4].localForm == 0x1A2B && rules[4].sex == 1);
		CHECK(rules[5].kind == gi::Rule::Kind::kForm && rules[5].localForm == 0xFF && rules[5].sex == -1);
		CHECK(rules[7].kind == gi::Rule::Kind::kAll && rules[7].sex == 0 && rules[7].race == "HumanRace");

		gi::Resolver res;
		res.pluginIndex = [](const std::string& p) { return p == "Mod.esp" ? 0x05 : p == "Gone.esp" ? -1 : 0x00; };
		res.forms = [](const std::string&, std::uint32_t local, std::vector<std::uint32_t>& out) {
			if (local == 0x1A2B) {  // a leveled list: itself and what it produces
				out.push_back(0x05001A2B);
				out.push_back(0x05001A2C);
			}
		};
		problems.clear();
		db.Finalize(res, &problems);
		CHECK(db.GetStats().validRules == 7);  // not valid: the Gone.esp line and the 0xFF form
		CHECK(db.GetStats().missingTemplates == 1);  // "Nope"
		CHECK(!problems.empty());

		// which line decides for an NPC: the LAST matching one; All / plugin lines list only NPCs without a template; forms and leveled lists by id
		const std::string human = "HumanRace";
		const std::string ghoul = "ghoulrace";  // editor ids are compared without case
		auto              npc = [](std::uint32_t id, const std::string& race, bool templ = false, bool isNpc = true) {
            gi::NpcInfo n;
            n.formID = id;
            n.race = race;
            n.hasTemplate = templ;
            n.isNpc = isNpc;
            return n;
		};
		CHECK(db.Match(npc(0x00001234, human), true) == &rules[0]);
		CHECK(db.Match(npc(0x05000777, human), true) == &rules[3]);                // defined by Mod.esp (load-order byte 05)
		CHECK(db.Match(npc(0x05001A2C, human), true) == &rules[4]);                // named through the leveled list, beats the plugin line
		CHECK(db.Match(npc(0x05001A2C, human), false) == &rules[7]);               // the male table: the later male All line wins
		CHECK(db.Match(npc(0x00001234, ghoul), true) == &rules[8]);
		CHECK(db.Match(npc(0x00001234, ghoul), false) == &rules[2]);               // no gender written: both tables
		CHECK(db.Match(npc(0x00001234, ""), true) == &rules[8]);                   // an NPC with no race matches any race filter
		CHECK(db.Match(npc(0x00001234, "SuperMutantRace"), true) == nullptr);      // some other race: no line
		CHECK(db.Match(npc(0x00001234, human, true), true) == nullptr);            // has a template: All lines do not list it ...
		CHECK(db.MatchChain({ npc(0x00001234, human, true), npc(0x00005678, human) }, true) == &rules[0]);  // ... but its template chain finds the root
		CHECK(db.MatchChain({ npc(0x05009999, human, true), npc(0x05001A2B, "", false, false) }, true) == &rules[4]);  // a leveled list on the chain: only FormID lines name it
		CHECK(db.MatchChain({ npc(0x05009999, human, true), npc(0x05001A2B, "", false, false) }, false) == nullptr);

		// rolling
		std::mt19937 rng(12345);
		int          slim = 0, big = 0;
		float        minB = 99, maxB = -99;
		for (int i = 0; i < 2000; ++i) {
			const auto out = db.Roll(rules[0], rng);  // Slim | Big
			if (out.size() == 1 && out[0].first == "Hips" && Near(out[0].second, 1.5F)) {
				++slim;
			} else if (out.size() == 2 && out[0].first == "Breasts" && out[1].first == "Hips" && Near(out[1].second, 3.0F)) {
				++big;
				minB = std::min(minB, out[0].second);
				maxB = std::max(maxB, out[0].second);
			} else {
				CHECK(false);
			}
		}
		CHECK(slim > 800 && big > 800 && slim + big == 2000);  // about half each
		CHECK(minB >= 1.0F && maxB <= 2.0F && maxB - minB > 0.9F);  // a uniform range
		{
			const auto out = db.Roll(rules[3], rng);  // Big , Zero: the zero template adds nothing
			CHECK(out.size() == 2);
		}
		std::map<std::string, int> seen;  // A / B,C|D
		for (int i = 0; i < 1000; ++i) {
			std::string key;
			for (const auto& m : db.Roll(rules[2], rng)) {
				key += m.first;
			}
			++seen[key];
		}
		CHECK(seen.size() == 3 && seen["A"] > 300 && seen["BC"] > 100 && seen["BD"] > 100);
		{  // negative and reversed ranges, same seed = same roll
			gi::Db d2;
			d2.AddTemplates("Neg = M@-0.5:-0.1\r\nSwap = M@2:1\r\nZero = Z@0\r\nDup = X@1, x@2\r\n", "t");
			d2.AddRules("All|Female|HumanRace = Neg\r\nAll|Male|HumanRace = Swap\r\nAll|Female|GhoulRace = Zero\r\nAll||HumanRace = Dup\r\n", "m");
			gi::Resolver r2;
			r2.pluginIndex = [](const std::string&) { return 0; };
			d2.Finalize(r2);
			CHECK(d2.GetStats().validRules == 4);
			std::mt19937 a(7), b(7);
			for (int i = 0; i < 200; ++i) {
				const auto n = d2.Roll(d2.Rules()[0], a);
				CHECK(n.size() == 1 && n[0].second >= -0.5F && n[0].second <= -0.1F);
				const auto sw = d2.Roll(d2.Rules()[1], a);
				CHECK(sw.size() == 1 && sw[0].second >= 1.0F && sw[0].second <= 2.0F);
				CHECK(d2.Roll(d2.Rules()[2], a).empty());  // a template that only rolls 0 leaves nothing
			}
			a.seed(7);
			CHECK(d2.Roll(d2.Rules()[0], a) == d2.Roll(d2.Rules()[0], b));
			const auto dup = d2.Roll(d2.Rules()[3], a);  // the same morph name twice (case-insensitive): the later value wins
			CHECK(dup.size() == 1 && Near(dup[0].second, 2.0F));
		}
		// a line whose templates are all missing still overwrites an earlier line, with nothing
		{
			gi::Db d3;
			d3.AddTemplates("T = A@1\r\n", "t");
			d3.AddRules("All|Female|HumanRace = T\r\nAll|Female|HumanRace = Nope\r\n", "m");
			gi::Resolver r3;
			r3.pluginIndex = [](const std::string&) { return 0; };
			d3.Finalize(r3);
			const auto* m = d3.Match(npc(0x10, "HumanRace"), true);
			CHECK(m == &d3.Rules()[1] && m->groups.empty());
			std::mt19937 g(1);
			CHECK(d3.Roll(*m, g).empty());
		}
		// garbage never crashes
		{
			gi::Db d4;
			std::string            junk;
			const std::string_view alphabet = "=|,/@:#\r\n aZ09-.";
			for (int i = 0; i < 4000; ++i) {
				junk.push_back(alphabet[static_cast<std::size_t>(i * 7 + i / 5) % alphabet.size()]);
			}
			d4.AddTemplates(junk, "j");
			d4.AddRules(junk, "j");
			gi::Resolver r4;
			r4.pluginIndex = [](const std::string&) { return 0; };
			r4.forms = [](const std::string&, std::uint32_t, std::vector<std::uint32_t>& o) { o.push_back(1); };
			d4.Finalize(r4);
			std::mt19937 g(2);
			for (const auto& r : d4.Rules()) {
				(void)d4.Roll(r, g);
			}
		}
	}

	// ---- BodySlide presets (BodySlidePresets.h) ----
	{
		namespace bs = bodymorph::bsp;
		// the tiny XML reader
		{
			std::vector<std::string> seen;
			bs::XmlHandler           h;
			h.open = [&](const bs::XmlElement& e, bool self) { seen.push_back((self ? "s:" : "o:") + e.name + "[" + e.AttrOr("a") + "|" + e.AttrOr("b", "-") + "]"); };
			h.close = [&](std::string_view n) { seen.push_back("c:" + std::string(n)); };
			h.text = [&](std::string_view t) { if (!bs::Trim(t).empty()) seen.push_back("t:" + std::string(bs::Trim(t))); };
			CHECK(bs::WalkXml("\xEF\xBB\xBF<?xml version=\"1.0\"?><!-- c > --><R a=\"x &amp; &quot;y&quot; &#65;&#xe9;\"><K a='1>2' b = \"z\" />text<![CDATA[<raw>]]></R>", h));
			CHECK(seen.size() == 6);
			CHECK(seen[0] == "o:R[x & \"y\" A\xC3\xA9|-]");
			CHECK(seen[1] == "s:K[1>2|z]" && seen[2] == "c:K");
			CHECK(seen[3] == "t:text" && seen[4] == "t:<raw>" && seen[5] == "c:R");
			CHECK(!bs::WalkXml("no xml here", h));
			CHECK(!bs::WalkXml("", h));
			bs::WalkXml("<a b=\"unterminated", h);  // must not crash
			bs::WalkXml("<a><!-- never closed", h);
			bs::WalkXml("< ><a =\"x\" b c=d e='f", h);
		}
		// presets: the real "CBBE Curvy" head, sizes, duplicates, bad numbers
		const std::string presetXml =
			"<SliderPresets>\n"
			"<Preset name=\"CBBE Curvy\" set=\"CBBE Body\"><Group name=\"CBBE\" /><SetSlider name=\"Ankles\" size=\"big\" value=\"25\" /><SetSlider name=\"Breasts\" size=\"big\" value=\"30\" />"
			"<SetSlider name=\"Waist\" size=\"big\" value=\"75\" /><SetSlider name=\"Waist\" size=\"small\" value=\"10\" /><SetSlider name=\"Butt\" value=\"60\" /><SetSlider name=\"Hips\" size=\"big\" value=\"abc\" /></Preset>\n"
			"<Preset name=\"CBBE Curvy (Outfit)\" set=\"CBBE Body\"><SetSlider name=\"Breasts\" size=\"both\" value=\"20\"/></Preset>\n"
			"<Preset name=\"CBBE Zeroed Sliders\" set=\"CBBE Body\"><SetSlider name=\"Breasts\" size=\"big\" value=\"0\"/></Preset>\n"
			"<Preset name=\"cbbe curvy\" set=\"x\"><SetSlider name=\"Breasts\" size=\"big\" value=\"99\"/></Preset>\n"
			"<Preset set=\"x\"><SetSlider name=\"Breasts\" size=\"big\" value=\"99\"/></Preset>\n"
			"<Preset name=\"Big\" set=\"CBBE Body\"><SetSlider name=\"Breasts\" size=\"big\" value=\"100\"/><SetSlider name=\"Butt\" size=\"big\" value=\"100\"/></Preset>\n"
			"<Preset name=\"Mid\" set=\"CBBE Body\"><SetSlider name=\"Breasts\" size=\"big\" value=\"50\"/></Preset>\n"
			"<Preset name=\"Other body\" set=\"FG\"><SetSlider name=\"FGThing\" size=\"big\" value=\"50\"/><SetSlider name=\"FGMore\" size=\"big\" value=\"50\"/><SetSlider name=\"Breasts\" size=\"big\" value=\"50\"/></Preset>\n"
			"</SliderPresets>";
		std::vector<bs::Preset> presets;
		CHECK(bs::ParsePresets(presetXml, "CBBE.xml", presets) == 6);
		CHECK(presets.size() == 6 && presets[0].name == "CBBE Curvy" && presets[0].set == "CBBE Body" && presets[0].groups.size() == 1);
		CHECK(presets[0].big.size() == 3 && presets[0].smallOnly == 1);  // Butt has no size, Hips no number
		CHECK(presets[0].big[2].first == "Waist" && Near(presets[0].big[2].second, 0.75F));
		CHECK(bs::ParsePresets("<Preset name=\"MID\"><SetSlider name=\"Breasts\" size=\"big\" value=\"1\"/></Preset>", "later.xml", presets) == 0);  // first of a name wins, across files
		// the real "BT - Chubby" lines that matter: explicit zeros, a value over 100, sliders left at the set's default
		const std::string btXml =
			"\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SliderPresets>\n    <Preset name=\"BT - Chubby\" set=\"BodyTalk4\">\n        <Group name=\"BodyTalk - Bodies\"/>\n"
			"        <SetSlider name=\"BTAbDefinition\" size=\"big\" value=\"0\"/>\n        <SetSlider name=\"BTBiceps\" size=\"big\" value=\"41\"/>\n        <SetSlider name=\"BTStomachFat\" size=\"big\" value=\"130\"/>\n"
			"        <SetSlider name=\"Erection\" size=\"big\" value=\"0\"/>\n        <SetSlider name=\"Hips\" size=\"big\" value=\"100\"/>\n    </Preset>\n"
			"    <Preset name=\"BT - Zero\" set=\"BodyTalk4\"><SetSlider name=\"BTAbDefinition\" size=\"big\" value=\"0\"/><SetSlider name=\"BTBiceps\" size=\"big\" value=\"0\"/><SetSlider name=\"BTStomachFat\" size=\"big\" value=\"0\"/>"
			"<SetSlider name=\"BTWaist-In\" size=\"big\" value=\"0\"/><SetSlider name=\"Erection\" size=\"big\" value=\"0\"/></Preset>\n</SliderPresets>\n";
		CHECK(bs::ParsePresets(btXml, "BT-Chubby.xml", presets) == 2);
		const auto find = [&](const char* n) -> const bs::Preset& { for (const auto& p : presets) if (p.name == n) return p; return presets[0]; };

		// slider sets: the real attribute spelling, OutputFile with an attribute, a nested Data element, zap / uv, a lower-case path
		const std::string osp =
			"\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SliderSetInfo version=\"1\">\n"
			"<SliderSet name=\"CBBE Body\"><DataFolder>CBBE</DataFolder><SourceFile>CBBE.nif</SourceFile><OutputPath>Meshes\\Actors\\Character\\CharacterAssets</OutputPath><OutputFile GenWeights=\"false\">FemaleBody</OutputFile>"
			"<Slider name=\"Breasts\" invert=\"false\" default=\"0\"><Data name=\"CBBEBreasts\" target=\"CBBE\" local=\"true\">CBBEBody.osd\\CBBEBreasts</Data></Slider>"
			"<Slider name=\"Waist\" invert=\"false\" default=\"0\"/><Slider name=\"Ankles\" invert=\"false\" default=\"0\"/><Slider name=\"Butt\" invert=\"false\" default=\"0\"/></SliderSet>\n"
			"<SliderSet name=\"CBBE Body Physics\"><OutputPath>meshes/actors/character/characterassets/</OutputPath><OutputFile>FemaleBody</OutputFile>"
			"<Slider name=\"Breasts\" invert=\"false\" default=\"0\"/><Slider name=\"Waist\" default=\"0\"/><Slider name=\"Ankles\" default=\"0\"/><Slider name=\"Butt\" default=\"0\"/><Slider name=\"Old\" invert=\"true\" default=\"100\"/>"
			"<Slider name=\"BodyZap\" default=\"100\" zap=\"true\"/></SliderSet>\n"
			"<SliderSet name=\"BodyTalk4\"><OutputPath>Meshes\\Actors\\Character\\CharacterAssets</OutputPath><OutputFile GenWeights=\"false\">MaleBody</OutputFile>"
			"<Slider name=\"Erection\" invert=\"false\" default=\"100\"><Data name=\"x\" target=\"BaseMaleBody:0\" local=\"true\">BodyTalk4-Nude.osd\\x</Data></Slider>"
			"<Slider name=\"BTBiceps\" invert=\"false\" default=\"100\"/><Slider name=\"BTWaist-In\" invert=\"false\" default=\"100\"/><Slider name=\"BTAbDefinition\" invert=\"false\" default=\"100\"/>"
			"<Slider name=\"BTStomachFat\" invert=\"false\" default=\"100\"/><Slider name=\"Hips\" invert=\"false\" default=\"0\"/><Slider name=\"BTAreolaSize\" default=\"0\" uv=\"true\"/><Slider name=\"Legacy\" small=\"0\" big=\"40\"/></SliderSet>\n"
			"<SliderSet name=\"Some Outfit\"><OutputPath>Meshes\\Clothes\\X</OutputPath><OutputFile>F</OutputFile><Slider name=\"Breasts\" default=\"0\"/></SliderSet>\n"
			"<SliderSet name=\"NoOutput\"><Slider name=\"Breasts\" default=\"0\"/></SliderSet>\n</SliderSetInfo>";
		std::vector<bs::SliderSet> sets;
		CHECK(bs::ParseSliderSets(osp, "x.osp", sets) == 5);
		const std::string kF = "meshes\\actors\\character\\characterassets\\femalebody", kM = "meshes\\actors\\character\\characterassets\\malebody";
		CHECK(sets[0].output == kF && sets[1].output == kF && sets[2].output == kM && sets[4].output.empty());
		CHECK(sets[0].sliders.size() == 4 && sets[2].sliders.size() == 8);
		CHECK(sets[2].Find("erection") && Near(sets[2].Find("erection")->def, 1.0F) && !sets[2].Find("BTAreolaSize")->morph && Near(sets[2].Find("Legacy")->def, 0.4F));
		CHECK(sets[1].Find("Old")->invert && !sets[1].Find("BodyZap")->morph);
		// which set builds a body: the recorded choice, else the one with the most morph sliders
		const std::string sel = "\xEF\xBB\xBF<BuildSelection><OutputChoice path=\"Meshes\\Actors\\Character\\CharacterAssets\\FemaleBody\" choice=\"CBBE Body\"/></BuildSelection>";
		CHECK(bs::BuildChoice(sel, kF) == "CBBE Body" && bs::BuildChoice(sel, kM).empty());
		CHECK(bs::PickSet(sets, kF, "CBBE Body") == &sets[0]);
		CHECK(bs::PickSet(sets, kF, "") == &sets[1] && bs::PickSet(sets, kF, "gone") == &sets[1]);
		CHECK(bs::PickSet(sets, kM, "") == &sets[2] && bs::PickSet(sets, "meshes\\nothing", "") == nullptr);
		{
			// two sets write the same body: the one whose sliders are all in the .tri wins over a bigger one with sliders the .tri lacks
			const std::unordered_set<std::string> triNames{ "breasts", "waist", "ankles", "butt" };
			CHECK(bs::PickSet(sets, kF, "", &triNames) == &sets[0]);  // "CBBE Body Physics" has "Old", which this .tri lacks
			CHECK(bs::PickSet(sets, kF, "CBBE Body Physics", &triNames) == &sets[1]);
		}

		// resolving: defaults for what the preset leaves out, explicit zeros, inversion, values over 100, non-morph sliders dropped
		const auto value = [](const std::vector<std::pair<std::string, float>>& m, const char* n) { for (const auto& [k, v] : m) if (k == n) return v; return -99.0F; };
		{
			const auto chubby = bs::Resolve(find("BT - Chubby"), &sets[2]);
			CHECK(Near(value(chubby, "BTBiceps"), 0.41F) && Near(value(chubby, "BTStomachFat"), 1.3F) && Near(value(chubby, "Hips"), 1.0F));
			CHECK(Near(value(chubby, "BTWaist-In"), 1.0F));                                               // not named: the set's default, not 0
			CHECK(value(chubby, "BTAbDefinition") == -99.0F && value(chubby, "Erection") == -99.0F);     // explicit 0
			CHECK(value(chubby, "BTAreolaSize") == -99.0F && Near(value(chubby, "Legacy"), 0.4F));
			const auto zero = bs::Resolve(find("BT - Zero"), &sets[2]);
			CHECK(zero.size() == 1 && zero[0].first == "Legacy");
			const auto defaults = bs::Resolve(bs::Preset{}, &sets[2]);
			CHECK(defaults.size() == 6 && Near(value(defaults, "Erection"), 1.0F));
			// a body built at the defaults: the preset is given as the difference (negative where it lowers a default)
			const auto delta = bs::Subtract(chubby, defaults);
			CHECK(Near(value(delta, "BTBiceps"), -0.59F) && Near(value(delta, "BTAbDefinition"), -1.0F) && Near(value(delta, "BTStomachFat"), 0.3F) && Near(value(delta, "Hips"), 1.0F));
			CHECK(value(delta, "BTWaist-In") == -99.0F && value(delta, "Legacy") == -99.0F);
			const auto raw = bs::Resolve(find("BT - Chubby"), nullptr);
			CHECK(raw.size() == 3);
			const auto inv = bs::Resolve(find("Mid"), &sets[1]);  // "Old" is inverted with default 100: 1 - 1 = 0
			CHECK(inv.size() == 1 && Near(value(inv, "Breasts"), 0.5F));
			bs::Preset withOld = find("Mid");
			withOld.big.emplace_back("old", 0.25F);
			CHECK(Near(value(bs::Resolve(withOld, &sets[1]), "Old"), 0.75F));
		}

		// the pools: a female .tri (CBBE: Breasts, Waist, Ankles, Butt) and a male one
		const auto makeTri = [](const char* shape, std::initializer_list<std::pair<const char*, int>> morphs) {
			Writer w;
			w.b = { std::byte{ 'P' }, std::byte{ 'I' }, std::byte{ 'R' }, std::byte{ 'T' } };
			w.put<std::uint16_t>(1);
			w.name(shape);
			w.put<std::uint16_t>(static_cast<std::uint16_t>(morphs.size()));
			std::uint16_t vertex = 0;
			for (const auto& [n, amount] : morphs) {
				w.name(n);
				w.put<float>(0.01F);
				w.put<std::uint16_t>(1);
				w.put<std::uint16_t>(vertex++);
				w.put<std::int16_t>(static_cast<std::int16_t>(amount)); w.put<std::int16_t>(0); w.put<std::int16_t>(0);
			}
			TriFile t;
			CHECK(t.Parse(w.b));
			return t;
		};
		const TriFile femaleTri = makeTri("CBBE", { { "Breasts", 100 }, { "Waist", 100 }, { "Ankles", 100 }, { "Butt", 100 } });
		const TriFile maleTri = makeTri("BaseMaleBody:0", { { "Erection", 100 }, { "BTBiceps", 100 }, { "BTWaist-In", 100 }, { "BTAbDefinition", 100 }, { "BTStomachFat", 100 }, { "Hips", 100 } });
		CHECK(bs::MorphNames(femaleTri).size() == 4);
		CHECK(Near(bs::Displacement(femaleTri, { { "Breasts", 1.0F } }), 1.0F) && Near(bs::Displacement(femaleTri, { { "breasts", 0.5F }, { "Nope", 3.0F } }), 0.5F));
		CHECK(bs::Displacement(femaleTri, {}) == 0.0F);
		bs::PoolOptions opt;
		opt.exclude = { "(outfit)", "zeroed" };
		opt.never = { "erection" };
		std::map<std::string, int> weights{ { "CBBE Curvy", 5 } };
		opt.weightOf = [&](const std::string& n) { const auto it = weights.find(n); return it == weights.end() ? -1 : it->second; };
		std::vector<std::string> notes;
		auto women = bs::BuildPool(presets, &sets[0], femaleTri, bs::MorphNames(maleTri), opt, {}, &notes);
		CHECK(women.size() == 3);  // Curvy, Big, Mid; not the outfit copy, the zeroed one, the other body's or the men's
		const auto body = [](const std::vector<bs::Body>& pool, const char* n) -> const bs::Body* { for (const auto& b : pool) if (b.name == n) return &b; return nullptr; };
		CHECK(body(women, "CBBE Curvy") && body(women, "CBBE Curvy")->weight == 5 && !body(women, "CBBE Curvy")->autoWeight && body(women, "CBBE Curvy")->morphs.size() == 3);
		CHECK(body(women, "Mid") && body(women, "Mid")->weight == 2 && body(women, "Mid")->autoWeight);  // in the middle of the pool (Curvy is the closest to the base body)
		CHECK(body(women, "Big") && body(women, "Big")->weight == 1);                                    // the farthest: least often
		CHECK(!body(women, "Other body") && !body(women, "BT - Chubby"));
		CHECK(notes.size() == 2 && notes[0].find("Outfit") != std::string::npos && notes[1].find("Zeroed") != std::string::npos);
		weights["Big"] = 0;  // 0 = never
		opt.autoWeights = false;
		opt.defaultWeight = 4;
		women = bs::BuildPool(presets, &sets[0], femaleTri, bs::MorphNames(maleTri), opt);
		CHECK(women.size() == 2 && body(women, "Mid")->weight == 4);
		opt.exclude.clear();
		women = bs::BuildPool(presets, &sets[0], femaleTri, {}, opt);
		CHECK(women.size() == 3 && body(women, "CBBE Curvy (Outfit)") && !body(women, "CBBE Zeroed Sliders"));  // a body that changes nothing is never handed out
		// men: defaults resolved, the state morph never set, "BT - Zero" changes nothing on a zero-built body
		auto men = bs::BuildPool(presets, &sets[2], maleTri, bs::MorphNames(femaleTri), opt);
		CHECK(men.size() == 1 && men[0].name == "BT - Chubby" && men[0].morphs.size() == 4);
		CHECK(Near(value(men[0].morphs, "BTWaist-In"), 1.0F) && value(men[0].morphs, "Erection") == -99.0F && value(men[0].morphs, "Legacy") == -99.0F);
		// ... and on a body built at the set's defaults both are bodies, as differences
		men = bs::BuildPool(presets, &sets[2], maleTri, bs::MorphNames(femaleTri), opt, bs::Resolve(bs::Preset{}, &sets[2]));
		CHECK(men.size() == 2 && body(men, "BT - Zero") && Near(value(body(men, "BT - Zero")->morphs, "BTBiceps"), -1.0F) && value(body(men, "BT - Zero")->morphs, "Erection") == -99.0F);

		// 0.8.2 variation: every slider the preset sets is scaled by its own factor in [0.8, 1.2], the base subtracted afterwards, zero sliders stay zero
		{
			std::vector<std::pair<std::string, float>> base{ { "Breasts", 0.4F }, { "Waist", 0.2F } };
			const auto pool = bs::BuildPool(presets, &sets[0], femaleTri, bs::MorphNames(maleTri), opt, base);
			const auto* curvy = body(pool, "CBBE Curvy");
			CHECK(curvy && !curvy->values.empty());
			std::mt19937 rng(11);
			CHECK(bs::Vary(*curvy, base, 0.0F, rng) == curvy->morphs);  // off: the body as it is
			bool differs = false;
			for (int i = 0; i < 200; ++i) {
				const auto v = bs::Vary(*curvy, base, 0.2F, rng);
				for (const auto& [name, own] : curvy->values) {
					float b = 0.0F;
					for (const auto& [bn, bv] : base) {
						if (bs::Lower(bn) == bs::Lower(name)) {
							b = bv;
						}
					}
					const float got = value(v, name.c_str()) == -99.0F ? 0.0F : value(v, name.c_str());
					const float lo = std::min(own * 0.8F, own * 1.2F) - b - 1e-4F, hi = std::max(own * 0.8F, own * 1.2F) - b + 1e-4F;
					CHECK(got >= lo && got <= hi);
					differs = differs || std::fabs(got - (own - b)) > 1e-3F;
				}
				for (const auto& [name, m] : v) {  // nothing outside what the body may set
					bool known = false;
					for (const auto& [n2, x] : curvy->values) known = known || bs::Lower(n2) == bs::Lower(name);
					for (const auto& [n2, x] : curvy->morphs) known = known || bs::Lower(n2) == bs::Lower(name);
					CHECK(known);
				}
			}
			CHECK(differs);
		}

		// weighted pick: the shares follow the weights; an empty pool gives nothing
		{
			std::vector<bs::Body> pool(3);
			pool[0].name = "a"; pool[0].weight = 3;
			pool[1].name = "b"; pool[1].weight = 1;
			pool[2].name = "c"; pool[2].weight = 0;
			std::mt19937               rng(7);
			std::map<std::string, int> hits;
			for (int i = 0; i < 4000; ++i) {
				++hits[bs::Pick(pool, rng)->name];
			}
			CHECK(hits["c"] == 0 && hits["a"] > 2850 && hits["a"] < 3150 && hits["a"] + hits["b"] == 4000);
			CHECK(bs::Pick(std::vector<bs::Body>{}, rng) == nullptr);
			pool.resize(0);
			CHECK(bs::MarkerName("Karma") == "BMVR_Preset_Karma");
		}
		// garbage never crashes
		{
			std::string            junk;
			const std::string_view alphabet = "<>/=\"' &;#!-?[]aZ09\r\n";
			for (int i = 0; i < 6000; ++i) {
				junk.push_back(alphabet[static_cast<std::size_t>(i * 11 + i / 7) % alphabet.size()]);
			}
			std::vector<bs::Preset>    p;
			std::vector<bs::SliderSet> ss;
			bs::ParsePresets(junk, "j", p);
			bs::ParseSliderSets(junk, "j", ss);
			(void)bs::BuildChoice(junk, "x");
			bs::ParsePresets("<Preset name=\"a\"><SetSlider name=\"b\" size=\"big\" value=\"nan\"/><SetSlider name=\"c\" size=\"big\" value=\"1e99\"/>", "j", p);
			CHECK(p.back().name == "a" && p.back().big.empty());
		}
	}

	std::printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
	return g_fail ? 1 : 0;
}
