// BodyMorph VR unit tests (no game): TRI reader and CPU morph application. GPL-3.0-or-later.
#include "../src/BodyGenIni.h"
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

	std::printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
	return g_fail ? 1 : 0;
}
