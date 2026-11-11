// BodyMorph VR unit tests (no game): TRI reader and CPU morph application. GPL-3.0-or-later.
#include "../src/IniFile.h"
#include "../src/MorphApply.h"
#include "../src/MorphList.h"
#include "../src/MorphStore.h"
#include "../src/TriFile.h"
#include "../src/TriPath.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

	std::printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
	return g_fail ? 1 : 0;
}
