// BodyMorph VR: BodySlide body morphs for Fallout 4 VR NPCs, written from scratch (no LooksMenu code).
// Copyright (C) 2026 Axel. Licensed under the GNU General Public License version 3 or later: see LICENSE.
//
// 0.6.0: the BodyGen ini engine (templates.ini / morphs.ini roll the morphs of an NPC that has none). 0.5.0: BodyGen (a per-NPC morph store behind the Papyrus BodyGen natives, saved in the F4SE co-save) on top of 0.4.0 (auto-apply at attach; each garment is morphed with the .tri its BODYTRI names). Keys (INI): F8 = read-only probe (logs the geometry of the nearest NPCs),
//   F7  = clone the body mesh of the nearest NPC and apply the sliders from sMorphs (read from the BodySlide .tri),
//   F6  = put the original meshes back.  (F5/F9 are the game's quick save/load: do not use them.)
// How a morph reaches the screen (VR layout and engine functions verified in the 1.2.72 exe, see tools/ and the findings):
//   BSGeometry: rendererData (BSGraphics::TriShape*) 0x188, vertexDesc 0x190, BSTriShape: numTriangles 0x1A0, numVertices 0x1A4
//   BSGraphics::TriShape { vertexDesc, VertexBuffer*, IndexBuffer*, refcount }, Buffer { d3d buffer, CPU copy 0x08, ..., refCount 0x38, pendingCopy 0x4E }
// Meshes are shared by refcount between all NPCs, so a morph never edits a buffer: it builds a NEW vertex buffer with the engine's own
// CreateBuffer (base positions read from the CPU copy + sum(weight * offset)), wraps it in a new TriShape that shares the index buffer,
// waits until the render side has finished uploading it, and only then points the geometry at it (so a draw never sees a half-made buffer).
// Restore points the geometry back at the untouched original and releases the clone after a delay.
// Threads: [poll thread] reads the keys, [main] (F4SE task) does everything else; every game pointer read goes through SEH.

#include "BodyGenIni.h"
#include "BodySlidePresets.h"
#include "HolderScan.h"
#include "IniFile.h"
#include "MorphApply.h"
#include "MorphList.h"
#include "MorphStore.h"
#include "TriFile.h"
#include "TriPath.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#include <intrin.h>
#undef ERROR

namespace
{
	constexpr auto kName = "BodyMorphVR";
	constexpr REL::Version kVersion{ 0, 8, 0, 0 };

	std::atomic<int> g_probeKey{ 0x77 };    // F8
	std::atomic<int> g_applyKey{ 0x76 };    // F7 (F5/F9 are the game's quick save / quick load)
	std::atomic<int> g_restoreKey{ 0x75 };  // F6
	int              g_maxActors = 2;       // probe only
	float            g_maxDistance = 600.0F;
	std::string      g_triPath = "Data\\Meshes\\Actors\\Character\\CharacterAssets\\FemaleBody.tri";
	std::string      g_morphs = "PregnancyBelly:2.0";
	bool             g_femaleOnly = true;
	std::string      g_aliases = "NakedTorso=CBBE";
	bool             g_scan = false;           // [Lab] iScan: after every swap, scan the process memory for other holders of the meshes
	int              g_aliasVerts = 22708;    // [Morph] iAliasVertexCount: a shape matched by alias must have exactly this many vertices (0 = no check)
	bool             g_autoEnabled = true;    // [Auto] bEnabled: morph NPC bodies as the game builds them (no key needed)
	bool             g_autoPlayer = false;    // [Auto] bIncludePlayer
	int              g_autoWaitMs = 0;        // [Auto] iWaitUploadMs: how long the attach hook may wait for the GPU upload before it swaps (0 = swap at once)
	bool             g_autoUseBodyTri = true;  // [Auto] bUseBodyTri: morph garments with the .tri named by their BODYTRI extra data
	int              g_autoLogAttaches = 120;  // [Auto] iLogAttaches: one diagnostic line for each of the first N attaches
	bool             g_useGlobalSliders = true;  // [Auto] bUseGlobalSliders: an actor with no stored morphs gets the sliders of [Morph] sMorphs
	std::atomic<int> g_storeSetKey{ 0x72 };    // [Test] iSetKey, F3: put sStoreMorphs into the store of the nearest NPC and update it (the Papyrus path, no script needed)
	std::atomic<int> g_storeClearKey{ 0x73 };  // [Test] iClearKey, F4: empty the store of the nearest NPC and update it
	std::string      g_storeMorphs = "PregnancyBelly:2.0";  // [Test] sStoreMorphs
	int              g_autoScanFirst = 0;     // [Auto] iScanFirst: scan memory after the first N auto-morphed meshes to check that no stale draw record exists (diagnostic)
	bool             g_patchHolders = false;   // [Lab] iPatchHolders: repoint the engine's draw records (copies of the vertex buffer pointer) at the clone / back

	void InitLog()
	{
		auto path = F4SE::log::log_directory();
		if (!path) {
			return;
		}
		*path /= "BodyMorphVR.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::debug);
		log->flush_on(spdlog::level::debug);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%Y-%m-%d %T.%e] [%L] %v");
	}

	// Read with an ifstream, relative to the game folder like the .tri (works under Mod Organizer 2's virtual file system).
	// GetPrivateProfileString does not: a relative path there means the Windows directory, and it silently gave the defaults.
	constexpr auto kIni = "Data\\F4SE\\Plugins\\BodyMorphVR.ini";

	bodymorph::IniFile ReadIni()
	{
		bodymorph::IniFile ini;
		std::ifstream      file(kIni, std::ios::binary);
		if (!file) {
			spdlog::warn("cannot open '{}': using the built-in defaults", kIni);
			return ini;
		}
		const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		ini.Parse(text);
		return ini;
	}

	// [main] Everything except the keys is re-read on every apply key press, so the INI can be edited without restarting the game.
	void LoadSettings()
	{
		const auto ini = ReadIni();
		g_maxActors = ini.GetInt("Probe", "iMaxActors", 2);
		g_maxDistance = static_cast<float>(ini.GetInt("Probe", "iMaxDistance", 600));
		g_triPath = ini.GetString("Morph", "sTriFile", "Data\\Meshes\\Actors\\Character\\CharacterAssets\\FemaleBody.tri");
		g_morphs = ini.GetString("Morph", "sMorphs", "PregnancyBelly:2.0");
		g_femaleOnly = ini.GetInt("Morph", "bFemaleOnly", 1) != 0;
		g_aliases = ini.GetString("Morph", "sShapeAliases", "NakedTorso=CBBE");
		g_aliasVerts = std::max(0, ini.GetInt("Morph", "iAliasVertexCount", 22708));
		g_autoEnabled = ini.GetInt("Auto", "bEnabled", 1) != 0;
		g_autoPlayer = ini.GetInt("Auto", "bIncludePlayer", 0) != 0;
		g_autoWaitMs = std::clamp(ini.GetInt("Auto", "iWaitUploadMs", 0), 0, 500);
		g_autoScanFirst = std::clamp(ini.GetInt("Auto", "iScanFirst", 0), 0, 10);
		g_autoUseBodyTri = ini.GetInt("Auto", "bUseBodyTri", 1) != 0;
		g_autoLogAttaches = std::clamp(ini.GetInt("Auto", "iLogAttaches", 120), 0, 2000);
		g_useGlobalSliders = ini.GetInt("Auto", "bUseGlobalSliders", 1) != 0;
		g_storeMorphs = ini.GetString("Test", "sStoreMorphs", "PregnancyBelly:2.0");
		g_scan = ini.GetInt("Lab", "iScan", 0) != 0;
		g_patchHolders = ini.GetInt("Lab", "iPatchHolders", 0) != 0;
	}

	void LoadKeys()
	{
		const auto ini = ReadIni();
		g_probeKey = ini.GetInt("Keys", "iProbeKey", 0x77);
		g_applyKey = ini.GetInt("Keys", "iApplyKey", 0x76);
		g_restoreKey = ini.GetInt("Keys", "iRestoreKey", 0x75);
		g_storeSetKey = ini.GetInt("Test", "iSetKey", 0x72);
		g_storeClearKey = ini.GetInt("Test", "iClearKey", 0x73);
		spdlog::info("settings file {} entries read from '{}'", ini.Size(), kIni);
	}

	// ---- SEH-safe reads: a bad pointer gives false, never a crash. (No C++ objects with destructors in these functions.)
	bool SafeCopy(void* a_dst, const void* a_src, std::size_t a_size)
	{
		__try {
			std::memcpy(a_dst, a_src, a_size);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	template <class T>
	bool Peek(const void* a_base, std::size_t a_off, T& a_out)
	{
		return a_base && SafeCopy(&a_out, static_cast<const std::byte*>(a_base) + a_off, sizeof(T));
	}

	std::string Hex(const std::byte* a_data, std::size_t a_n)
	{
		static constexpr char d[] = "0123456789abcdef";
		std::string           s;
		for (std::size_t i = 0; i < a_n; ++i) {
			if (i && i % 4 == 0) {
				s += ' ';
			}
			s += d[std::to_integer<unsigned>(a_data[i]) >> 4];
			s += d[std::to_integer<unsigned>(a_data[i]) & 15];
		}
		return s;
	}

	// VR NiRTTI of BSGeometry (HBCM lesson: CommonLibF4VR's Ni_RTTI ID for it is wrong in VR). The name is checked.
	constexpr std::uintptr_t kBSGeometryRttiRva = 0x5C62A30;
	constexpr std::uintptr_t kVtBSTriShape = 0x2E5A0B8;         // verified in the unpacked 1.2.72 exe
	constexpr std::uintptr_t kVtBSDynamicTriShape = 0x2E5B248;
	constexpr std::uintptr_t kVtBSSubIndexTriShape = 0x2E757C8;

	const RE::NiRTTI* BSGeometryRtti()
	{
		static const RE::NiRTTI* rtti = [] -> const RE::NiRTTI* {
			const auto* r = reinterpret_cast<const RE::NiRTTI*>(REL::Module::get().base() + kBSGeometryRttiRva);
			if (!r->name || std::strcmp(r->name, "BSGeometry") != 0) {
				spdlog::error("BSGeometry NiRTTI not found at RVA {:X}", kBSGeometryRttiRva);
				return nullptr;
			}
			return r;
		}();
		return rtti;
	}

	const char* KindOf(const void* a_obj)
	{
		std::uintptr_t vt = 0;
		if (!Peek(a_obj, 0, vt)) {
			return "?";
		}
		const auto rva = vt - REL::Module::get().base();
		switch (rva) {
		case kVtBSTriShape: return "BSTriShape";
		case kVtBSDynamicTriShape: return "BSDynamicTriShape";
		case kVtBSSubIndexTriShape: return "BSSubIndexTriShape";
		default: return "other";
		}
	}

	// Logs a BSGraphics::Buffer (vertex buffer or its 'next' stream).
	void LogBuffer(const char* a_label, const void* a_buf, std::uint64_t a_desc, std::uint32_t a_numVerts, bool a_decode)
	{
		if (!a_buf) {
			spdlog::info("      {}: null", a_label);
			return;
		}
		std::uintptr_t d3d = 0, data = 0, next = 0;
		std::uint32_t  maxSize = 0, size = 0, ref = 0;
		std::uint8_t   invalid = 0, heap = 0, pending = 0;
		const bool     ok = Peek(a_buf, 0x00, d3d) && Peek(a_buf, 0x08, data) && Peek(a_buf, 0x10, next) && Peek(a_buf, 0x30, maxSize) &&
		                Peek(a_buf, 0x34, size) && Peek(a_buf, 0x38, ref) && Peek(a_buf, 0x4C, invalid) && Peek(a_buf, 0x4D, heap) &&
		                Peek(a_buf, 0x4E, pending);
		if (!ok) {
			spdlog::warn("      {} {:p}: not readable", a_label, a_buf);
			return;
		}
		spdlog::info("      {} {:p}: d3dBuffer={:#x} data={:#x} next={:#x} maxDataSize={} dataSize={} refCount={} invalidCpuData={} heapAllocated={} pendingCopy={}", a_label,
			a_buf, d3d, data, next, maxSize, size, ref, invalid, heap, pending);
		const auto stride = bodymorph::VertexStride(a_desc);
		if (data && stride && size) {
			std::byte raw[96]{};
			const auto n = std::min<std::size_t>(sizeof(raw), std::size_t{ stride } * 2);
			if (SafeCopy(raw, reinterpret_cast<const void*>(data), n)) {
				spdlog::info("        data[0..{}] = {}", n, Hex(raw, n));
			} else {
				spdlog::warn("        data {:#x} not readable", data);
			}
			if (a_decode && stride * std::size_t{ a_numVerts } <= size + 64U) {
				std::byte first[96]{};
				if (stride * 3U <= sizeof(first) && SafeCopy(first, reinterpret_cast<const void*>(data), std::size_t{ stride } * 3)) {
					std::vector<bodymorph::Position> pos;
					if (bodymorph::ReadPositions(std::span<const std::byte>(first, std::size_t{ stride } * 3), a_desc, 3, pos)) {
						for (std::size_t i = 0; i < pos.size(); ++i) {
							spdlog::info("        v{} position (as {}) = {:.4f} {:.4f} {:.4f}", i, (bodymorph::VertexFlags(a_desc) & bodymorph::kFlagFullPrecision) ? "float32" : "float16", pos[i].x, pos[i].y,
								pos[i].z);
						}
					}
				}
			}
		}
	}

	void LogGeometry(RE::BSGeometry* a_geo)
	{
		const void* g = a_geo;
		std::uint64_t  desc = 0;
		std::uintptr_t rendererData = 0;
		std::uint32_t  numTris = 0;
		std::uint16_t  numVerts = 0;
		std::uint8_t   type = 0;
		Peek(g, 0x188, rendererData);
		Peek(g, 0x190, desc);
		Peek(g, 0x198, type);
		Peek(g, 0x1A0, numTris);
		Peek(g, 0x1A4, numVerts);
		std::string bodyTri = "-";
		if (a_geo->extra) {
			for (auto* e : *a_geo->extra) {
				if (!e || !e->name.c_str() || std::strcmp(e->name.c_str(), "BODYTRI") != 0) {
					continue;
				}
				const char* v = static_cast<RE::NiStringExtraData*>(e)->data.c_str();
				bodyTri = v ? v : "(null)";
			}
		}
		spdlog::info("    shape '{}' kind={} type={} numTriangles={} numVertices={} vertexDesc={:#018x} (stride {} flags {:#x}) BODYTRI='{}'", std::string(a_geo->name.c_str() ? a_geo->name.c_str() : ""),
			KindOf(g), type, numTris, numVerts, desc, bodymorph::VertexStride(desc), bodymorph::VertexFlags(desc), bodyTri);
		if (!rendererData) {
			spdlog::info("      rendererData: null");
			return;
		}
		std::uint64_t  triDesc = 0;
		std::uintptr_t vb = 0, ib = 0;
		std::uint32_t  ref = 0;
		if (!Peek(reinterpret_cast<const void*>(rendererData), 0x00, triDesc) || !Peek(reinterpret_cast<const void*>(rendererData), 0x08, vb) ||
			!Peek(reinterpret_cast<const void*>(rendererData), 0x10, ib) || !Peek(reinterpret_cast<const void*>(rendererData), 0x18, ref)) {
			spdlog::warn("      rendererData {:#x}: not readable", rendererData);
			return;
		}
		spdlog::info("      TriShape {:#x}: vertexDesc={:#018x}{} vertexBuffer={:#x} indexBuffer={:#x} refCount={}", rendererData, triDesc, triDesc == desc ? " (same as shape)" : " (DIFFERENT)", vb, ib, ref);
		LogBuffer("vertexBuffer", reinterpret_cast<const void*>(vb), triDesc, numVerts, true);
		std::uintptr_t next = 0;
		if (Peek(reinterpret_cast<const void*>(vb), 0x10, next) && next) {
			LogBuffer("vertexBuffer.next", reinterpret_cast<const void*>(next), triDesc, numVerts, false);
		}
		LogBuffer("indexBuffer", reinterpret_cast<const void*>(ib), 0, 0, false);
	}

	void Walk(RE::NiAVObject* a_obj, int a_depth, int& a_logged)
	{
		if (!a_obj || a_depth > 32 || a_logged >= 40) {
			return;
		}
		if (const auto* geom = BSGeometryRtti(); geom && a_obj->GetRTTI() && a_obj->GetRTTI()->IsKindOf(geom)) {
			++a_logged;
			LogGeometry(static_cast<RE::BSGeometry*>(a_obj));
		}
		if (auto* node = netimmerse_cast<RE::NiNode*>(a_obj)) {
			for (auto& child : node->GetRuntimeData().children) {
				Walk(child.get(), a_depth + 1, a_logged);
			}
		}
	}

	// [main] Logs the geometry of the nearest NPCs. Read-only.
	void Probe()
	{
		spdlog::info("=== probe ===");
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!player || !lists) {
			spdlog::warn("no player or process lists yet");
			return;
		}
		const auto                                  ppos = player->GetPosition();
		std::vector<std::pair<float, RE::Actor*>> found;
		for (auto& h : lists->highActorHandles) {
			auto actor = h.get();
			if (!actor || actor.get() == player || !actor->Get3D()) {
				continue;
			}
			const auto  p = actor->GetPosition();
			const float dx = p.x - ppos.x, dy = p.y - ppos.y, dz = p.z - ppos.z;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (dist <= g_maxDistance) {
				found.emplace_back(dist, actor.get());
			}
		}
		std::ranges::sort(found, {}, &decltype(found)::value_type::first);
		spdlog::info("{} NPC(s) within {:.0f} units; logging the {} nearest", found.size(), g_maxDistance, g_maxActors);
		int n = 0;
		for (auto& [dist, actor] : found) {
			if (n++ >= g_maxActors) {
				break;
			}
			const auto* npc = actor->GetNPC();
			spdlog::info("  actor {:08X} dist={:.0f} sex={}", actor->GetFormID(), dist, npc ? (npc->IsFemale() ? "female" : "male") : "?");
			int logged = 0;
			Walk(actor->Get3D(), 0, logged);
		}
		spdlog::info("=== probe done ===");
	}

	// =====================================================================================================================
	// Morph: engine access
	// =====================================================================================================================
	// The game reaches these functions through a tiny interface object: global 0x5C61848 -> object at RVA 0x392D990 whose only member is the vtable 0x30C1F38.
	// Its slots (+0x30 AddRef, +0x38 Release) are thunks: "lea rcx, [RVA 0x6239340]; jmp <implementation>", i.e. the real 'this' is the static object at
	// RVA 0x6239340 and its member +0x18 is the renderer. (The first 0.2.0 build used the interface object as 'this' and found renderer 0: wrong.)
	constexpr std::uintptr_t kRvaManagerGlobal = 0x5C61848;
	constexpr std::uintptr_t kRvaManagerVtable = 0x30C1F38;
	constexpr std::uintptr_t kRvaReleaseThunk = 0x2938C30;    // manager vtable slot 0x38
	constexpr std::uintptr_t kRvaImpl = 0x6239340;            // the real 'this' of CreateBuffer / BuildTriShape / Release; [+0x18] = renderer
	constexpr std::uintptr_t kRvaCreateBuffer = 0x1DD2C50;    // Buffer* (renderer, BufDesc*, data): copies data, queues the GPU upload; refCount starts at 2
	constexpr std::uintptr_t kRvaBuildTriShape = 0x1D8E6F0;   // TriShape* (manager, VertexBuffer*, vertexDesc, IndexBuffer*): AddRefs both buffers, refcount 1
	constexpr std::uintptr_t kRvaReleaseTriShape = 0x1D8EEB0; // implementation behind the thunk (prologue checked)
	constexpr std::uintptr_t kRvaTlsIndex = 0x689CACC;        // TLS slot of the engine's per-thread block (byte +0x1524 goes into BufDesc.flag)

	// The first bytes of the three functions in the 1.2.72 exe: a different build fails the check and the morph code stays off.
	struct Prologue
	{
		const char*   name;
		std::uintptr_t rva;
		std::uint8_t  bytes[16];
	};
	constexpr Prologue kPrologues[] = {
		{ "CreateBuffer", kRvaCreateBuffer, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 } },
		{ "BuildTriShape", kRvaBuildTriShape, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48 } },
		{ "ReleaseTriShape", kRvaReleaseTriShape, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0xF0 } },
	};

	struct BufDesc  // what CreateBuffer reads: +8 type (0 vertex, 1 index), +0xC size in bytes, +0x10 flag
	{
		std::uint64_t pad = 0;
		std::uint32_t type = 0;
		std::uint32_t size = 0;
		std::uint8_t  flag = 0;
		std::uint8_t  pad2[7]{};
	};
	static_assert(sizeof(BufDesc) == 0x18);

	using CreateBufferFn = void* (*)(void*, const BufDesc*, const void*);
	using BuildTriShapeFn = void* (*)(void*, void*, std::uint64_t, void*);
	using ReleaseFn = void (*)(void*, void*);

	struct Engine
	{
		void* manager = nullptr;   // interface object (vtable only): Release goes through its vtable like the game does
		void* impl = nullptr;      // real 'this' of the implementation
		void* renderer = nullptr;  // impl+0x18
	};

	// No C++ objects with destructors in the SEH functions below.
	void* CallCreateBuffer(void* a_renderer, const BufDesc* a_desc, const void* a_data)
	{
		__try {
			return reinterpret_cast<CreateBufferFn>(REL::Module::get().base() + kRvaCreateBuffer)(a_renderer, a_desc, a_data);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	void* CallBuildTriShape(void* a_manager, void* a_vb, std::uint64_t a_desc, void* a_ib)
	{
		__try {
			return reinterpret_cast<BuildTriShapeFn>(REL::Module::get().base() + kRvaBuildTriShape)(a_manager, a_vb, a_desc, a_ib);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	bool CallRelease(void* a_manager, void* a_triShape)
	{
		__try {
			const auto vtable = *reinterpret_cast<const std::uintptr_t*>(a_manager);
			reinterpret_cast<ReleaseFn>(*reinterpret_cast<const std::uintptr_t*>(vtable + 0x38))(a_manager, a_triShape);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool ReadTlsFlag(std::uint8_t& a_out)
	{
		__try {
			const auto index = *reinterpret_cast<const std::uint32_t*>(REL::Module::get().base() + kRvaTlsIndex);
			const auto* slots = reinterpret_cast<const std::uintptr_t*>(__readgsqword(0x58));
			const auto  block = slots[index];
			a_out = *reinterpret_cast<const std::uint8_t*>(block + 0x1524);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool WritePointer(void* a_base, std::size_t a_off, std::uintptr_t a_value)
	{
		__try {
			*reinterpret_cast<volatile std::uintptr_t*>(static_cast<std::byte*>(a_base) + a_off) = a_value;  // aligned 8-byte store: atomic on x64
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool AtomicDecrement(void* a_base, std::size_t a_off)
	{
		__try {
			::InterlockedDecrement(reinterpret_cast<volatile LONG*>(static_cast<std::byte*>(a_base) + a_off));
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// [main] Checks the engine objects and the function prologues. False (and a log line) = the morph code stays off.
	bool GetEngine(Engine& a_out)
	{
		// The auto-apply hook calls this from loader threads too: the prologue check runs once, thread-safely.
		static const bool prologuesOk = [] {
			const auto base = REL::Module::get().base();
			bool       ok = true;
			for (const auto& p : kPrologues) {
				std::uint8_t now[16]{};
				if (!SafeCopy(now, reinterpret_cast<const void*>(base + p.rva), sizeof(now)) || std::memcmp(now, p.bytes, sizeof(now)) != 0) {
					spdlog::error("engine function {} at RVA {:X} does not match the 1.2.72 exe: morph code disabled", p.name, p.rva);
					ok = false;
				}
			}
			return ok;
		}();
		static std::atomic<bool> logged{ false };
		const auto               base = REL::Module::get().base();
		if (!prologuesOk) {
			return false;
		}
		std::uintptr_t manager = 0, vtable = 0, slot = 0, renderer = 0;
		const auto     impl = base + kRvaImpl;
		if (!Peek(reinterpret_cast<const void*>(base + kRvaManagerGlobal), 0, manager) || !manager || !Peek(reinterpret_cast<const void*>(manager), 0, vtable) ||
			vtable != base + kRvaManagerVtable || !Peek(reinterpret_cast<const void*>(vtable), 0x38, slot) || slot != base + kRvaReleaseThunk) {
			spdlog::warn("shader resource manager not ready or not the expected object (global {:#x}, vtable {:#x}, release slot {:#x}, expected {:#x})", manager, vtable, slot,
				base + kRvaReleaseThunk);
			return false;
		}
		if (!Peek(reinterpret_cast<const void*>(impl), 0x18, renderer) || !renderer) {
			spdlog::warn("renderer not ready yet (implementation object {:#x}, renderer {:#x})", impl, renderer);
			return false;
		}
		if (!logged.exchange(true)) {
			spdlog::info("engine objects ok: manager {:#x}, implementation {:#x}, renderer {:#x}", manager, impl, renderer);
		}
		a_out.manager = reinterpret_cast<void*>(manager);
		a_out.impl = reinterpret_cast<void*>(impl);
		a_out.renderer = reinterpret_cast<void*>(renderer);
		return true;
	}

	// =====================================================================================================================
	// Morph: jobs
	// =====================================================================================================================
	enum class JobState
	{
		Pending,   // clone built, waiting for the render side to finish uploading it; the geometry still draws the original
		Applied,   // the geometry draws the clone
		Retiring,  // the geometry is back on the original; the clone is released after a delay (the render thread may still be drawing it)
	};

	struct Job
	{
		RE::NiPointer<RE::BSGeometry>         geom;  // keeps the geometry alive while we point it at our clone
		std::string                           name;
		std::uintptr_t                        oldTri = 0;  // the geometry's own reference to it is held by us while Applied
		std::uintptr_t                        oldVB = 0;
		std::uintptr_t                        ib = 0;  // the index buffer, shared by the original and the clone
		std::uintptr_t                        newTri = 0;
		std::uintptr_t                        newVB = 0;
		std::uint64_t                         desc = 0;
		JobState                              state = JobState::Pending;
		std::chrono::steady_clock::time_point since;
		int                                   logs = 0;
		bool                                  keep = true;   // false (Papyrus updates): forgotten once the clone is on the geometry, which then owns it
		std::uintptr_t                        replaced = 0;  // a clone of ours that this one takes the place of (retired: kept in memory, see Tick)
		std::uint64_t                         weightsKey = 0;  // WeightsKey of the sliders the clone was built with
	};

	// ---- what the attach hook learned about each geometry: its ORIGINAL mesh and which .tri / shape morphs it, so that a later update (Papyrus UpdateMorphs)
	// can re-morph from the original and put the original back. Keyed by the geometry's address: an entry whose mesh no longer matches is stale and ignored.
	struct GeoInfo
	{
		std::uintptr_t orig = 0;   // the original TriShape (the geometry's own reference to it is kept by us once a clone replaces it)
		std::uintptr_t clone = 0;  // our clone on the geometry now (0 = none)
		std::string    tri;        // game-folder path of the garment .tri ("" = the global body .tri)
		std::string    shape;      // shape name inside that .tri
		std::uint64_t  weightsKey = 0;  // WeightsKey of the sliders behind the clone on the geometry (an update with the same sliders changes nothing)
	};

	// A key for a set of sliders (name, value), independent of their order: an update that would build the same clone again is skipped.
	std::uint64_t WeightsKey(std::vector<bodymorph::MorphWeight> a_weights)
	{
		std::ranges::sort(a_weights, [](const auto& a, const auto& b) { return a.name < b.name; });
		std::uint64_t key = 1469598103934665603ULL;  // FNV-1a
		auto          mix = [&key](std::uint8_t b) { key = (key ^ b) * 1099511628211ULL; };
		for (const auto& w : a_weights) {
			for (const char c : w.name) {
				mix(static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(c))));
			}
			mix(0);
			std::uint32_t bits = 0;
			std::memcpy(&bits, &w.weight, sizeof(bits));
			for (int i = 0; i < 4; ++i) {
				mix(static_cast<std::uint8_t>(bits >> (8 * i)));
			}
		}
		return key | 1;  // never 0 (0 = unknown)
	}

	std::mutex                                  g_geoMutex;
	std::unordered_map<std::uintptr_t, GeoInfo> g_geo;

	void RememberGeo(const RE::BSGeometry* a_geo, GeoInfo a_info)
	{
		std::lock_guard lock(g_geoMutex);
		if (g_geo.size() > 50000) {
			g_geo.clear();
		}
		g_geo[reinterpret_cast<std::uintptr_t>(a_geo)] = std::move(a_info);
	}

	// True (and the entry) when the geometry is known AND still shows either its original mesh or our clone.
	bool LookupGeo(const RE::BSGeometry* a_geo, std::uintptr_t a_currentTri, GeoInfo& a_out)
	{
		std::lock_guard lock(g_geoMutex);
		const auto      it = g_geo.find(reinterpret_cast<std::uintptr_t>(a_geo));
		if (it == g_geo.end() || (it->second.orig != a_currentTri && (it->second.clone == 0 || it->second.clone != a_currentTri))) {
			return false;
		}
		a_out = it->second;
		return true;
	}

	void SetGeoClone(const RE::BSGeometry* a_geo, std::uintptr_t a_clone, std::uint64_t a_weightsKey = 0)
	{
		std::lock_guard lock(g_geoMutex);
		if (const auto it = g_geo.find(reinterpret_cast<std::uintptr_t>(a_geo)); it != g_geo.end()) {
			it->second.clone = a_clone;
			it->second.weightsKey = a_clone ? a_weightsKey : 0;
		}
	}

	std::vector<Job>  g_jobs;  // [main] only
	std::atomic<int>  g_active{ 0 };  // jobs that need Tick (Pending or Retiring); read by the poll thread

	constexpr auto kUploadTimeout = std::chrono::seconds(5);
	constexpr auto kRetireDelay = std::chrono::seconds(30);  // the clone may still be on screen for a while after the swap back: keep its memory

	void RefreshActive()
	{
		int n = 0;
		for (const auto& j : g_jobs) {
			n += j.state != JobState::Applied ? 1 : 0;
		}
		g_active = n;
	}

	// ---- [main] reads of a Buffer: has the render side finished with it?
	struct BufferState
	{
		std::uintptr_t d3d = 0;
		std::uint32_t  refCount = 0;
		std::uint8_t   pendingCopy = 1;
		bool           ok = false;
	};

	BufferState ReadBufferState(std::uintptr_t a_buf)
	{
		BufferState s;
		s.ok = Peek(reinterpret_cast<const void*>(a_buf), 0x00, s.d3d) && Peek(reinterpret_cast<const void*>(a_buf), 0x38, s.refCount) && Peek(reinterpret_cast<const void*>(a_buf), 0x4E, s.pendingCopy);
		return s;
	}

	void ReleaseClone(const Engine& a_engine, Job& a_job)
	{
		if (a_job.newTri && !CallRelease(a_engine.manager, reinterpret_cast<void*>(a_job.newTri))) {
			spdlog::error("  {}: releasing the clone threw (leaked)", a_job.name);
		}
		a_job.newTri = 0;
	}

	// =====================================================================================================================
	// Lab: who else holds the mesh?  (INI [Lab] iScan / iPatchHolders)
	// =====================================================================================================================
	// 0.2.4: the swap of geometry+0x188 is done within 60 ms, yet the screen follows seconds (4 s .. minutes) later. The 0.2.5 scan found the reason:
	// besides the geometry's TriShape, the engine keeps small DRAW RECORDS in the heap, laid out as  [shader?][VertexBuffer*][IndexBuffer*][1][draw parameters...],
	// that hold a COPY of the mesh's vertex buffer pointer; the renderer draws from those, and they are only rebuilt (from geometry+0x188) when the engine
	// decides to re-register the geometry. So after a swap this scans the process memory for copies of the vertex buffer pointers, logs the memory around them
	// (internal copies of our own bookkeeping are left out) and, with iPatchHolders=1, repoints every such draw record (VB immediately followed by the old index
	// buffer pointer) from the old vertex buffer to the new one: after an apply old -> clone, after a restore clone -> old.
	std::atomic<bool> g_scanning{ false };
	std::atomic<int>  g_generation{ 0 };  // bumped by Restore: a scan that started before it must not patch anything
	std::mutex        g_scanMutex;

	std::uintptr_t ImageSize()
	{
		static const std::uintptr_t size = [] {
			const auto    base = REL::Module::get().base();
			std::uint32_t lfanew = 0, sizeOfImage = 0;
			if (Peek(reinterpret_cast<const void*>(base), 0x3C, lfanew) && Peek(reinterpret_cast<const void*>(base + lfanew), 0x50, sizeOfImage)) {
				return std::uintptr_t{ sizeOfImage };
			}
			return std::uintptr_t{ 0 };
		}();
		return size;
	}

	constexpr const char* kScanNames[] = { "OLD_TRI", "OLD_VB", "NEW_TRI", "NEW_VB" };
	enum ScanIndex : std::size_t { kOldTri = 0, kOldVB = 1, kNewTri = 2, kNewVB = 3 };

	std::string DescribeValue(std::uint64_t a_v, const std::uintptr_t (&a_vals)[4])
	{
		for (std::size_t i = 0; i < 4; ++i) {
			if (a_vals[i] && a_v == a_vals[i]) {
				return kScanNames[i];
			}
		}
		const auto base = REL::Module::get().base();
		if (a_v >= base && a_v < base + ImageSize()) {
			return std::format("exe+{:X}", a_v - base);
		}
		if (a_v < 0x10000) {
			return std::to_string(a_v);
		}
		return std::format("{:#x}", a_v);
	}

	constexpr int kContext = 10;  // 8-byte slots logged before and after a holder

	bool ReadContext(std::uintptr_t a_addr, std::uint64_t (&a_out)[2 * kContext + 1])
	{
		return SafeCopy(a_out, reinterpret_cast<const void*>(a_addr - kContext * 8), sizeof(a_out));
	}

	void LogHolder(std::uintptr_t a_addr, const std::uint64_t (&a_w)[2 * kContext + 1], const std::uintptr_t (&a_vals)[4])
	{
		spdlog::info("    holder at {:#x}:", a_addr);
		for (int row = 0; row < 2 * kContext + 1; row += 4) {
			std::string line;
			for (int j = row; j < std::min(row + 4, 2 * kContext + 1); ++j) {
				line += std::format("{}{:+}={}  ", j == kContext ? "*" : "", (j - kContext) * 8, DescribeValue(a_w[j], a_vals));
			}
			spdlog::info("      {}", line);
		}
	}

	struct ScanRequest
	{
		std::string  label;
		std::string  jobName;
		std::uintptr_t vals[4]{};  // old TriShape, old vertex buffer, new TriShape, new vertex buffer
		std::uintptr_t indexBuffer = 0;
		bool         toClone = true;  // patch direction: true = old VB -> new VB (after an apply), false = new VB -> old VB (after a restore)
		bool         patch = false;
		int          generation = 0;
		int          delayMs = 0;  // wait before scanning (the auto path scans a few seconds after the swap, when the renderer has had time to register the mesh)
		bodymorph::AddressRange jobsRange{};
	};

	// [scan thread]
	void RunScanOnce(const ScanRequest& a_rq)
	{
		if (a_rq.delayMs > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(a_rq.delayMs));
		}
		const auto t0 = std::chrono::steady_clock::now();
		std::vector<std::uintptr_t> values;
		std::vector<std::size_t>    valueIndex;
		for (std::size_t i = 0; i < 4; ++i) {
			if (a_rq.vals[i]) {
				values.push_back(a_rq.vals[i]);
				valueIndex.push_back(i);
			}
		}
		std::vector<bodymorph::AddressRange> skip;
		skip.push_back(a_rq.jobsRange);
		skip.emplace_back(reinterpret_cast<std::uintptr_t>(values.data()), reinterpret_cast<std::uintptr_t>(values.data()) + values.size() * sizeof(values[0]));
		skip.emplace_back(__readgsqword(0x10), __readgsqword(0x08));  // this thread's stack (a_rq and the locals live there)
		skip.emplace_back(a_rq.vals[kOldTri] + 8, a_rq.vals[kOldTri] + 0x18);  // the TriShapes' own vertex / index buffer fields
		skip.emplace_back(a_rq.vals[kNewTri] + 8, a_rq.vals[kNewTri] + 0x18);
		const auto res = bodymorph::ScanForPointers(values, 64, skip);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		spdlog::info("  [scan '{}' for {}] {} MB in {} regions, {} ms; index buffer {:#x}", a_rq.label, a_rq.jobName, res.bytes >> 20, res.regions, ms, a_rq.indexBuffer);

		const std::size_t fromIdx = a_rq.toClone ? kOldVB : kNewVB;
		const std::size_t toIdx = a_rq.toClone ? kNewVB : kOldVB;
		std::size_t       internal = 0, geometrySlots = 0, records = 0, patched = 0;
		for (const auto& hit : res.hits) {
			const auto idx = valueIndex[hit.which];
			std::uint64_t w[2 * kContext + 1]{};
			if (!ReadContext(hit.addr, w)) {
				continue;
			}
			// internal copies: our own Job / ScanRequest / thread arguments hold several of the four pointers side by side
			int others = 0;
			for (int j = 0; j < 2 * kContext + 1; ++j) {
				for (std::size_t i = 0; i < 4; ++i) {
					others += (j != kContext && i != idx && a_rq.vals[i] && w[j] == a_rq.vals[i]) ? 1 : 0;
				}
			}
			if (others >= 2) {
				++internal;
				continue;
			}
			if ((idx == kOldTri || idx == kNewTri)) {
				std::uintptr_t vt = 0;
				if (Peek(reinterpret_cast<const void*>(hit.addr - 0x188), 0, vt)) {
					const auto rva = vt - REL::Module::get().base();
					if (rva == kVtBSTriShape || rva == kVtBSDynamicTriShape || rva == kVtBSSubIndexTriShape) {
						++geometrySlots;
						continue;
					}
				}
			}
			const bool isRecord = (idx == kOldVB || idx == kNewVB) && a_rq.indexBuffer && w[kContext + 1] == a_rq.indexBuffer;
			spdlog::info("    {} ({:#x}) is held here{}:", kScanNames[idx], a_rq.vals[idx], isRecord ? "  [DRAW RECORD: followed by the index buffer]" : "");
			LogHolder(hit.addr, w, a_rq.vals);
			records += isRecord ? 1 : 0;
			if (a_rq.patch && isRecord && idx == fromIdx && a_rq.vals[toIdx]) {
				std::lock_guard lock(g_scanMutex);
				if (g_generation.load() == a_rq.generation && WritePointer(reinterpret_cast<void*>(hit.addr), 0, a_rq.vals[toIdx])) {
					++patched;
					spdlog::info("    -> repointed to {}", kScanNames[toIdx]);
				}
			}
		}
		for (std::size_t k = 0; k < values.size(); ++k) {
			spdlog::info("    {} {:#x}: {} copy(ies) in memory{}", kScanNames[valueIndex[k]], values[k], res.counts[k], res.counts[k] > 64 ? " (only the first 64 were examined)" : "");
		}
		spdlog::info("  {} internal copies of our own bookkeeping, {} geometry mesh slot(s), {} draw record(s), {} repointed", internal, geometrySlots, records, patched);
	}

	std::optional<ScanRequest> g_pending;  // guarded by g_scanMutex

	void RunScan(ScanRequest a_rq)
	{
		::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		for (;;) {
			try {
				RunScanOnce(a_rq);
			} catch (...) {
				spdlog::error("scan threw");
			}
			// a request that arrived while this scan was running (e.g. the restore key right after the apply key) is run next
			std::lock_guard lock(g_scanMutex);
			if (!g_pending) {
				g_scanning = false;
				return;
			}
			a_rq = std::move(*g_pending);
			g_pending.reset();
		}
	}

	// [main] a_toClone: after an apply (old VB -> clone VB); false: after a restore (clone VB -> old VB).
	void ScheduleScan(const Job& a_job, const char* a_label, bool a_toClone, bool a_allowPatch = true, int a_delayMs = 0)
	{
		if (!g_scan) {
			return;
		}
		ScanRequest rq;
		rq.label = a_label;
		rq.jobName = a_job.name;
		rq.vals[kOldTri] = a_job.oldTri;
		rq.vals[kOldVB] = a_job.oldVB;
		rq.vals[kNewTri] = a_job.newTri;
		rq.vals[kNewVB] = a_job.newVB;
		rq.indexBuffer = a_job.ib;
		rq.toClone = a_toClone;
		rq.patch = a_allowPatch && g_patchHolders;
		rq.delayMs = a_delayMs;
		rq.generation = g_generation.load();
		if (a_delayMs == 0) {  // called from the main thread: g_jobs may be read
			rq.jobsRange = { reinterpret_cast<std::uintptr_t>(g_jobs.data()), reinterpret_cast<std::uintptr_t>(g_jobs.data()) + g_jobs.capacity() * sizeof(Job) };
		}
		std::lock_guard lock(g_scanMutex);
		if (g_scanning) {
			g_pending = std::move(rq);
			return;
		}
		g_scanning = true;
		std::thread(RunScan, std::move(rq)).detach();
	}

	// [main] Called every 100 ms while any job is Pending or Retiring.
	void Tick()
	{
		Engine engine;
		if (!GetEngine(engine)) {
			return;
		}
		const auto       now = std::chrono::steady_clock::now();
		std::vector<Job> retired;  // clones replaced during this pass: queued after it
		for (auto it = g_jobs.begin(); it != g_jobs.end();) {
			bool erase = false;
			if (it->state == JobState::Pending) {
				const auto st = ReadBufferState(it->newVB);
				const auto waited = now - it->since;
				if (it->logs < 3) {
					++it->logs;
					spdlog::info("  {}: upload check: d3dBuffer={:#x} pendingCopy={} refCount={} readable={} after {} ms", it->name, st.d3d, st.pendingCopy, st.refCount, st.ok,
						std::chrono::duration_cast<std::chrono::milliseconds>(waited).count());
				}
				const bool ready = st.ok && st.d3d != 0 && st.pendingCopy == 0 && (st.refCount <= 1 || waited >= std::chrono::milliseconds(1500));
				if (ready) {
					std::uintptr_t current = 0;
					if (Peek(it->geom.get(), 0x188, current) && current == it->oldTri && WritePointer(it->geom.get(), 0x188, it->newTri)) {
						it->state = JobState::Applied;
						spdlog::info("  {}: upload finished (refCount {}), geometry now draws the morphed clone", it->name, st.refCount);
						ScheduleScan(*it, "after apply", true);
						SetGeoClone(it->geom.get(), it->newTri, it->weightsKey);
						if (it->replaced) {  // the clone this one replaced is retired: kept in memory for good (see below)
							Job old;
							old.geom = it->geom;
							old.name = it->name;
							old.newTri = it->replaced;
							old.state = JobState::Retiring;
							old.since = now;
							old.keep = false;
							retired.push_back(std::move(old));
							it->replaced = 0;
						}
						erase = !it->keep;
					} else {
						spdlog::warn("  {}: the geometry's mesh changed while the clone was uploading ({:#x} instead of {:#x}): dropped", it->name, current, it->oldTri);
						ReleaseClone(engine, *it);
						erase = true;
					}
				} else if (waited >= kUploadTimeout) {
					spdlog::error("  {}: the upload did not finish in {} s (d3dBuffer={:#x} pendingCopy={} refCount={}): dropped, the geometry was never touched", it->name,
						std::chrono::duration_cast<std::chrono::seconds>(kUploadTimeout).count(), st.d3d, st.pendingCopy, st.refCount);
					ReleaseClone(engine, *it);
					erase = true;
				}
			} else if (it->state == JobState::Retiring && now - it->since >= kRetireDelay) {
				if (it->keep) {  // the F7 lab path: its draw records were repointed by the scan, so the clone can go
					ReleaseClone(engine, *it);
					spdlog::info("  {}: clone released", it->name);
				} else {
					// A live update (Papyrus / F3): a clone that was ever on screen is NEVER released. The engine's draw records keep their own copy of the vertex
					// buffer pointer and are only rebuilt when the engine re-registers the geometry, so a record made while this clone was on the geometry still
					// points at it; freeing it crashed the game in the graphics driver (0.6.0 test: six F3 presses, the first replaced clone released 30 s later).
					// The price is the clone's memory (about 0.1 to 0.7 MB each); an update that would build the same clone again is skipped (WeightsKey).
					static std::atomic<int> s_kept{ 0 };
					const int               n = s_kept.fetch_add(1) + 1;
					if (n <= 20 || n % 100 == 0) {
						spdlog::info("  {}: retired clone kept in memory (not released: a draw record may still point at it); {} kept so far", it->name, n);
					}
					it->newTri = 0;
				}
				erase = true;
			}
			it = erase ? g_jobs.erase(it) : it + 1;
		}
		for (auto& r : retired) {
			g_jobs.push_back(std::move(r));
		}
		RefreshActive();
	}

	// [main] restore key: every geometry goes back to its own mesh.
	void Restore()
	{
		spdlog::info("=== restore: {} job(s) ===", g_jobs.size());
		++g_generation;  // a scan that is still running from the apply must not patch any more
		Engine engine;
		const bool haveEngine = GetEngine(engine);
		for (auto& job : g_jobs) {
			if (job.state == JobState::Applied) {
				std::uintptr_t current = 0;
				if (Peek(job.geom.get(), 0x188, current) && current == job.newTri) {
					WritePointer(job.geom.get(), 0x188, job.oldTri);
					spdlog::info("  {}: original mesh back", job.name);
					ScheduleScan(job, "after restore", false);
				} else {
					spdlog::warn("  {}: the geometry no longer points at our clone ({:#x}); left alone", job.name, current);
				}
				job.state = JobState::Retiring;
				job.since = std::chrono::steady_clock::now();
			} else if (job.state == JobState::Pending) {
				if (haveEngine) {
					ReleaseClone(engine, job);  // never published: safe to release at once
				}
				job.state = JobState::Retiring;
				job.since = std::chrono::steady_clock::now() - kRetireDelay;  // already released: Tick just erases it
			}
		}
		RefreshActive();
		Tick();
	}

	// =====================================================================================================================
	// Morph: apply
	// =====================================================================================================================
	bool LoadTri(const std::string& a_path, bodymorph::TriFile& a_tri)
	{
		std::ifstream file(a_path, std::ios::binary);
		if (!file) {
			spdlog::warn("cannot open '{}' (build the body in BodySlide with Build Morphs)", a_path);
			return false;
		}
		std::vector<std::byte> data;
		file.seekg(0, std::ios::end);
		data.resize(static_cast<std::size_t>(file.tellg()));
		file.seekg(0);
		file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
		std::string error;
		if (!a_tri.Parse(data, &error)) {
			spdlog::error("'{}' ({} bytes) is not readable: {}", a_path, data.size(), error);
			return false;
		}
		return true;
	}

	void CollectGeometries(RE::NiAVObject* a_obj, int a_depth, std::vector<RE::BSGeometry*>& a_out)
	{
		if (!a_obj || a_depth > 32 || a_out.size() >= 64) {
			return;
		}
		if (const auto* geom = BSGeometryRtti(); geom && a_obj->GetRTTI() && a_obj->GetRTTI()->IsKindOf(geom)) {
			a_out.push_back(static_cast<RE::BSGeometry*>(a_obj));
		}
		if (auto* node = netimmerse_cast<RE::NiNode*>(a_obj)) {
			for (auto& child : node->GetRuntimeData().children) {
				CollectGeometries(child.get(), a_depth + 1, a_out);
			}
		}
	}

	RE::Actor* NearestActor(float a_maxDistance, bool a_femaleOnly, float& a_dist)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!player || !lists) {
			return nullptr;
		}
		const auto  ppos = player->GetPosition();
		RE::Actor*  best = nullptr;
		float       bestDist = a_maxDistance;
		for (auto& h : lists->highActorHandles) {
			auto actor = h.get();
			if (!actor || actor.get() == player || !actor->Get3D()) {
				continue;
			}
			const auto* npc = actor->GetNPC();
			if (a_femaleOnly && (!npc || !npc->IsFemale())) {
				continue;
			}
			const auto  p = actor->GetPosition();
			const float dx = p.x - ppos.x, dy = p.y - ppos.y, dz = p.z - ppos.z;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (dist <= bestDist) {
				bestDist = dist;
				best = actor.get();
			}
		}
		a_dist = bestDist;
		return best;
	}

	struct CloneResult
	{
		std::uintptr_t oldTri = 0;
		std::uintptr_t oldVB = 0;
		std::uintptr_t ib = 0;
		std::uintptr_t newTri = 0;
		std::uintptr_t newVB = 0;
		std::uint64_t  desc = 0;
	};

	// [any thread] Builds a morphed clone of one geometry's vertex buffer (CreateBuffer + BuildTriShape); the geometry itself is not touched.
	// The clone TriShape has refCount 1: whoever points a geometry at it owns that reference.
	// a_baseTri: the TriShape the base positions come from when it is not the geometry's current one (a live update re-morphs from the ORIGINAL mesh, never
	// from an earlier clone: morphs would add up). 0 = the geometry's current TriShape.
	bool BuildClone(const Engine& a_engine, RE::BSGeometry* a_geo, const std::string& a_shapeName, const std::string& a_triShape, const bodymorph::TriFile& a_tri,
		const std::vector<bodymorph::MorphWeight>& a_weights, CloneResult& a_out, bool a_quiet = false, std::string* a_note = nullptr, std::uintptr_t a_baseTri = 0)
	{
		const void*    g = a_geo;
		std::uint64_t  desc = 0;
		std::uintptr_t current = 0;
		std::uint16_t  numVerts = 0;
		if (!Peek(g, 0x188, current) || !Peek(g, 0x190, desc) || !Peek(g, 0x1A4, numVerts) || !current || !numVerts) {
			spdlog::warn("  {}: no readable mesh (rendererData {:#x}, {} vertices)", a_shapeName, current, numVerts);
			if (a_note) {
				*a_note = "no readable mesh";
			}
			return false;
		}
		const std::uintptr_t ts = a_baseTri ? a_baseTri : current;  // the mesh whose vertices are the starting point
		std::uint64_t        triDesc = 0;
		std::uintptr_t       vb = 0, ib = 0, currentVB = 0;
		if (!Peek(reinterpret_cast<const void*>(ts), 0x00, triDesc) || !Peek(reinterpret_cast<const void*>(ts), 0x08, vb) || !Peek(reinterpret_cast<const void*>(ts), 0x10, ib) || !vb || !ib ||
			triDesc != desc || !Peek(reinterpret_cast<const void*>(current), 0x08, currentVB)) {
			spdlog::warn("  {}: TriShape {:#x} not usable (vertexBuffer {:#x}, indexBuffer {:#x}, desc {:#x} vs {:#x})", a_shapeName, ts, vb, ib, triDesc, desc);
			if (a_note) {
				*a_note = "TriShape not usable";
			}
			return false;
		}
		const auto stride = bodymorph::VertexStride(desc);
		const auto flags = bodymorph::VertexFlags(desc);
		std::uintptr_t cpuData = 0;
		std::uint32_t  dataSize = 0;
		std::uint8_t   invalid = 1;
		if (!Peek(reinterpret_cast<const void*>(vb), 0x08, cpuData) || !Peek(reinterpret_cast<const void*>(vb), 0x34, dataSize) || !Peek(reinterpret_cast<const void*>(vb), 0x4C, invalid) ||
			!cpuData || invalid != 0 || (flags & bodymorph::kFlagVertex) == 0 || stride < 8 || std::size_t{ stride } * numVerts > dataSize) {
			spdlog::warn("  {}: vertex buffer not usable (cpu copy {:#x}, invalidCpuData {}, dataSize {}, stride {}, flags {:#x}, {} vertices)", a_shapeName, cpuData, invalid, dataSize, stride, flags, numVerts);
			if (a_note) {
				*a_note = std::format("vertex buffer not usable (cpu copy {:#x}, invalid {}, size {}, stride {})", cpuData, invalid, dataSize, stride);
			}
			return false;
		}

		// morphs of this shape, by slider name
		std::vector<std::pair<const bodymorph::Morph*, float>> morphs;
		for (const auto& w : a_weights) {
			if (const auto* m = a_tri.FindMorph(a_triShape, w.name)) {
				morphs.emplace_back(m, w.weight);
			} else if (!a_quiet) {
				spdlog::warn("  {}: no morph named '{}' in shape '{}' of the .tri", a_shapeName, w.name, a_triShape);
			}
		}
		if (morphs.empty()) {
			if (a_note) {
				*a_note = "none of the sliders exists in this shape";
			}
			return false;
		}

		// base positions straight from the CPU copy, morphs added, written back into a private copy of the whole block
		const std::size_t      bytes = std::size_t{ stride } * numVerts;
		std::vector<std::byte> block(bytes);
		if (!SafeCopy(block.data(), reinterpret_cast<const void*>(cpuData), bytes)) {
			spdlog::warn("  {}: CPU copy {:#x} not readable", a_shapeName, cpuData);
			if (a_note) {
				*a_note = "CPU copy not readable";
			}
			return false;
		}
		std::vector<bodymorph::Position> base;
		if (!bodymorph::ReadPositions(block, desc, numVerts, base)) {
			spdlog::warn("  {}: cannot decode positions (desc {:#x})", a_shapeName, desc);
			if (a_note) {
				*a_note = "cannot decode positions";
			}
			return false;
		}
		const auto outOfRange = bodymorph::WritePositions(block, desc, base, morphs);
		if (outOfRange != 0) {
			if (!a_quiet) {
				spdlog::warn("  {}: {} morph offset(s) point past the mesh's {} vertices: this is not the mesh the .tri was built for, left alone", a_shapeName, outOfRange, numVerts);
			}
			if (a_note) {
				*a_note = std::format("the .tri has offsets past the mesh's {} vertices (built for another mesh)", numVerts);
			}
			return false;
		}

		BufDesc bd;
		bd.type = 0;
		bd.size = static_cast<std::uint32_t>(bytes);
		if (!ReadTlsFlag(bd.flag)) {
			spdlog::error("  {}: cannot read the engine's TLS flag", a_shapeName);
			return false;
		}
		auto* newVB = CallCreateBuffer(a_engine.renderer, &bd, block.data());
		if (!newVB) {
			spdlog::error("  {}: CreateBuffer failed", a_shapeName);
			return false;
		}
		auto* newTri = CallBuildTriShape(a_engine.impl, newVB, desc, reinterpret_cast<void*>(ib));
		if (!newTri) {
			spdlog::error("  {}: BuildTriShape failed (new vertex buffer {:p} leaked)", a_shapeName, newVB);
			return false;
		}
		// CreateBuffer hands out refCount 2 (owner + the queued upload), BuildTriShape added one for the TriShape: the extra one is ours to drop,
		// exactly as the engine's own loader ends up with 1 (TriShape) once the upload is done.
		AtomicDecrement(newVB, 0x38);

		a_out.oldTri = current;
		a_out.oldVB = currentVB;
		a_out.ib = ib;
		a_out.newTri = reinterpret_cast<std::uintptr_t>(newTri);
		a_out.newVB = reinterpret_cast<std::uintptr_t>(newVB);
		a_out.desc = desc;
		static std::atomic<int> s_quietLogged{ 0 };  // the auto path creates hundreds of clones: only the first ones are logged in full
		if (!a_quiet || s_quietLogged.fetch_add(1) < 150) {
			spdlog::info("  {}: {} vertices (stride {}, {}), {} morph(s) applied to the copy, {} offset(s) past the vertex count; clone vertex buffer {:p}, TriShape {:p}", a_shapeName, numVerts, stride,
				(flags & bodymorph::kFlagFullPrecision) ? "float32" : "float16", morphs.size(), outOfRange, newVB, newTri);
		}
		return true;
	}

	// [main] Manual path (F7): builds the clone and queues a job that swaps it in once the upload has finished. Returns true when a job was queued.
	bool CloneGeometry(const Engine& a_engine, RE::BSGeometry* a_geo, const std::string& a_shapeName, const std::string& a_triShape, const bodymorph::TriFile& a_tri,
		const std::vector<bodymorph::MorphWeight>& a_weights)
	{
		CloneResult cr;
		if (!BuildClone(a_engine, a_geo, a_shapeName, a_triShape, a_tri, a_weights, cr)) {
			return false;
		}
		Job job;
		job.geom = RE::NiPointer<RE::BSGeometry>(a_geo);
		job.name = a_shapeName;
		job.oldTri = cr.oldTri;
		job.oldVB = cr.oldVB;
		job.ib = cr.ib;
		job.newTri = cr.newTri;
		job.newVB = cr.newVB;
		job.desc = cr.desc;
		job.since = std::chrono::steady_clock::now();
		g_jobs.push_back(std::move(job));
		return true;
	}

	void RefreshAutoSettings();  // defined with the auto-apply code below

	// [main] apply key
	void Apply()
	{
		spdlog::info("=== apply ===");
		RefreshAutoSettings();  // re-reads the INI and the .tri; NPCs built from now on use the new sliders (this also loads the settings)
		Engine engine;
		if (!GetEngine(engine)) {
			return;
		}
		bodymorph::TriFile tri;
		if (!LoadTri(g_triPath, tri)) {
			return;
		}
		const auto weights = bodymorph::ParseMorphList(g_morphs);
		const auto aliases = bodymorph::ParseShapeAliases(g_aliases);
		if (weights.empty()) {
			spdlog::warn("sMorphs is empty: nothing to apply (example: sMorphs=BreastsSmall:1.0,Butt:0.5)");
			return;
		}
		float      dist = 0.0F;
		auto*      actor = NearestActor(g_maxDistance, g_femaleOnly, dist);
		if (!actor) {
			spdlog::warn("no {}NPC within {:.0f} units", g_femaleOnly ? "female " : "", g_maxDistance);
			return;
		}
		spdlog::info("actor {:08X} at {:.0f} units, .tri '{}' ({} shape(s)), sMorphs '{}'", actor->GetFormID(), dist, g_triPath, tri.Shapes().size(), g_morphs);
		std::vector<RE::BSGeometry*> geoms;
		CollectGeometries(actor->Get3D(), 0, geoms);
		int queued = 0, skipped = 0;
		for (auto* geo : geoms) {
			const std::string full = geo->name.c_str() ? geo->name.c_str() : "";
			std::string       shape(bodymorph::StripInstanceSuffix(full));
			if (!tri.FindShape(shape)) {
				for (const auto& alias : aliases) {
					if (bodymorph::StartsWithNoCase(full, alias.prefix) && tri.FindShape(alias.shape)) {
						spdlog::info("  shape '{}': matched to .tri shape '{}' by alias '{}'", full, alias.shape, alias.prefix);
						shape = alias.shape;
						break;
					}
				}
			}
			if (!tri.FindShape(shape)) {
				spdlog::info("  shape '{}': not in the .tri, left alone", full);
				continue;
			}
			if (std::ranges::any_of(g_jobs, [&](const Job& j) { return j.geom.get() == geo && j.state != JobState::Retiring; })) {
				spdlog::info("  shape '{}': already morphed (press the restore key first)", full);
				++skipped;
				continue;
			}
			queued += CloneGeometry(engine, geo, full, shape, tri, weights) ? 1 : 0;
		}
		spdlog::info("=== apply done: {} clone(s) queued, {} already morphed, {} geometries seen ===", queued, skipped, geoms.size());
		RefreshActive();
	}

	// =====================================================================================================================
	// Auto-apply: morph the body while the game builds the NPC (no key, no scan)
	// =====================================================================================================================
	// Why here: the renderer copies a mesh's vertex buffer pointer into its own draw records the first time it sees the geometry, and only rebuilds them
	// much later (that was the "the change shows up seconds or minutes late" problem). Head Body Color Match VR hit the same wall and hooks
	// BipedAnim::AttachSkinnedObject (RVA 0x1CBC00 in 1.2.72): it runs while the NPC's 3D is being built, before any frame has drawn it.
	// So after the original returns, the new subtree's body geometry gets its morphed clone (own TriShape, shared index buffer) at once, and every
	// draw record is later born from the clone. The clone TriShape (refCount 1) is owned by the geometry: the engine releases it when the geometry dies.
	// The original TriShape keeps one reference that the geometry used to hold (a harmless leak on a shared mesh the game keeps loaded anyway).
	// The hook can run on loader threads: only the immutable settings snapshot, thread-safe calls and the SEH guarded engine helpers are used.
	struct AutoSettings
	{
		bool                                enabled = false;
		bool                                femaleOnly = true;
		bool                                includePlayer = false;
		bool                                triOk = false;  // the global body .tri (sTriFile) loaded: used for meshes that carry no BODYTRI
		bool                                useBodyTri = true;
		bool                                useGlobalSliders = true;  // an actor with no stored morphs gets the INI sliders (weights)
		int                                 waitMs = 0;
		int                                 aliasVerts = 0;
		int                                 scanFirst = 0;
		int                                 logAttaches = 0;
		std::vector<bodymorph::MorphWeight> weights;
		std::vector<bodymorph::ShapeAlias>  aliases;
		bodymorph::TriFile                  tri;
	};

	bodymorph::MorphStore               g_store;  // per-actor morphs: the Papyrus BodyGen functions write it, the attach hook and UpdateActorNow read it
	std::mutex                          g_autoMutex;
	std::shared_ptr<const AutoSettings> g_auto;
	std::atomic<int>                    g_autoActors{ 0 }, g_autoShapes{ 0 }, g_autoScans{ 0 }, g_attachLogged{ 0 }, g_morphLogged{ 0 };

	std::shared_ptr<const AutoSettings> GetAutoSettings()
	{
		std::lock_guard lock(g_autoMutex);
		return g_auto;
	}

	// ---- garment .tri files, shared by every NPC that wears the garment (read once, then kept)
	std::mutex                                                                 g_triCacheMutex;
	std::unordered_map<std::string, std::shared_ptr<const bodymorph::TriFile>> g_triCache;  // key: lower-case game-folder path; null = could not be read (logged once)

	// [any thread] The .tri a BODYTRI value points at ("Data\Meshes\<value>"), or null.
	std::shared_ptr<const bodymorph::TriFile> GetGarmentTri(const std::string& a_dataPath)
	{
		const auto      key = bodymorph::TriFile::Lower(a_dataPath);
		std::lock_guard lock(g_triCacheMutex);
		if (const auto it = g_triCache.find(key); it != g_triCache.end()) {
			return it->second;
		}
		std::shared_ptr<const bodymorph::TriFile> result;
		auto                                      tri = std::make_shared<bodymorph::TriFile>();
		if (LoadTri(a_dataPath, *tri)) {
			spdlog::info("[tri] loaded '{}': {} shape(s)", a_dataPath, tri->Shapes().size());
			result = std::move(tri);
		}
		g_triCache.emplace(key, result);
		return result;
	}

	void ClearGarmentTris()
	{
		std::lock_guard lock(g_triCacheMutex);
		g_triCache.clear();
	}

	// [main] Re-reads the INI and the .tri files. Applies to NPCs built from now on.
	void RefreshAutoSettings()
	{
		LoadSettings();
		auto s = std::make_shared<AutoSettings>();
		s->enabled = g_autoEnabled;
		s->femaleOnly = g_femaleOnly;
		s->includePlayer = g_autoPlayer;
		s->useBodyTri = g_autoUseBodyTri;
		s->useGlobalSliders = g_useGlobalSliders;
		s->waitMs = g_autoWaitMs;
		s->aliasVerts = g_aliasVerts;
		s->scanFirst = g_autoScanFirst;
		s->logAttaches = g_autoLogAttaches;
		s->weights = bodymorph::ParseMorphList(g_morphs);
		s->aliases = bodymorph::ParseShapeAliases(g_aliases);
		s->triOk = LoadTri(g_triPath, s->tri);
		ClearGarmentTris();
		g_attachLogged = 0;
		g_morphLogged = 0;
		spdlog::info("[auto] settings: enabled {}, female only {}, player {}, garment .tri via BODYTRI {}, wait {} ms, INI sliders '{}' (used by actors with no stored morphs: {}), global .tri {}",
			s->enabled, s->femaleOnly, s->includePlayer, s->useBodyTri, s->waitMs, g_morphs, s->useGlobalSliders, s->triOk ? "ok" : "NOT usable");
		std::lock_guard lock(g_autoMutex);
		g_auto = std::move(s);
	}

	// ---- BODYTRI on the nif's root node. Reads are SEH guarded (no C++ objects with destructors in these two functions): a layout surprise
	// gives "not found", never a crash. The extra data array is NiObjectNET::extra (a BSTArray of NiExtraData*), as in CommonLibF4.
	bool ReadStringExtra(RE::NiObjectNET* a_obj, const char* a_name, char* a_out, std::size_t a_cap)
	{
		if (!a_obj || !a_out || a_cap < 8) {
			return false;
		}
		a_out[0] = 0;
		__try {
			const auto* container = a_obj->extra;
			if (!container) {
				return false;
			}
			const std::uint32_t count = container->size();
			if (count == 0 || count > 64) {
				return false;
			}
			for (std::uint32_t i = 0; i < count; ++i) {
				auto* extra = (*container)[i];
				if (!extra) {
					continue;
				}
				const char* name = extra->name.c_str();
				if (!name || _stricmp(name, a_name) != 0) {
					continue;
				}
				const char* text = static_cast<RE::NiStringExtraData*>(extra)->data.c_str();
				if (!text) {
					return false;
				}
				std::size_t len = 0;
				while (len + 1 < a_cap && text[len] != 0) {
					a_out[len] = text[len];
					++len;
				}
				a_out[len] = 0;
				return len > 0 && text[len] == 0;
			}
			return false;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_out[0] = 0;
			return false;
		}
	}

	// Diagnostic: the names of the extra data on a node, comma separated.
	void ListExtraNames(RE::NiObjectNET* a_obj, char* a_out, std::size_t a_cap)
	{
		if (!a_out || a_cap < 8) {
			return;
		}
		a_out[0] = 0;
		if (!a_obj) {
			return;
		}
		std::size_t len = 0;
		__try {
			const auto* container = a_obj->extra;
			if (!container) {
				return;
			}
			const std::uint32_t count = container->size();
			for (std::uint32_t i = 0; i < count && i < 16; ++i) {
				auto* extra = (*container)[i];
				const char* name = extra ? extra->name.c_str() : nullptr;
				if (!name) {
					continue;
				}
				if (len && len + 1 < a_cap) {
					a_out[len++] = ',';
				}
				for (std::size_t k = 0; name[k] != 0 && len + 1 < a_cap; ++k) {
					a_out[len++] = name[k];
				}
				a_out[len] = 0;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_out[len] = 0;
		}
	}

	void CopyCString(const char* a_src, char* a_dst, std::size_t a_cap)
	{
		std::size_t n = 0;
		if (a_src) {
			while (a_src[n] != 0 && n + 1 < a_cap) {
				a_dst[n] = a_src[n];
				++n;
			}
		}
		a_dst[n] = 0;
	}

	// ---- what we already morphed (see GeoInfo): a second AttachSkinnedObject on a geometry that already shows our clone must not morph the morphed copy again.
	bool AlreadyMorphed(const RE::BSGeometry* a_geo, std::uintptr_t a_currentTri)
	{
		std::lock_guard lock(g_geoMutex);
		const auto      it = g_geo.find(reinterpret_cast<std::uintptr_t>(a_geo));
		return it != g_geo.end() && it->second.clone != 0 && it->second.clone == a_currentTri;
	}

	// The .tri shape that goes with a geometry name: the exact name, the name without the ":N" suffix, or (global body .tri only) the first matching alias. "" = none.
	std::string ResolveShape(const AutoSettings& a_cfg, const bodymorph::TriFile& a_tri, bool a_allowAlias, const std::string& a_fullName)
	{
		if (const auto* shape = a_tri.FindShape(a_fullName)) {
			return shape->name;
		}
		std::string stripped(bodymorph::StripInstanceSuffix(a_fullName));
		if (a_tri.FindShape(stripped)) {
			return stripped;
		}
		if (a_allowAlias) {
			for (const auto& alias : a_cfg.aliases) {
				if (bodymorph::StartsWithNoCase(a_fullName, alias.prefix) && a_tri.FindShape(alias.shape)) {
					return alias.shape;
				}
			}
		}
		return {};
	}

	// ---- the VR address library (a CSV: header row "id,offset", then one "id,hexoffset" row per id). CommonLibF4VR ends the game when it needs an id that is not in
	// it, so every feature that reaches the game through ids this plugin has not used before checks them first and switches itself off, with a logged reason.
	const std::unordered_set<std::uint64_t>* AddressIds()
	{
		static std::unordered_set<std::uint64_t> ids;
		static bool                              ok = false;
		static std::once_flag                    once;
		std::call_once(once, [] {
			const auto    path = std::format("Data/F4SE/Plugins/version-{}.csv", REL::Module::get().version().string("-"));
			std::ifstream file(path);
			if (!file) {
				spdlog::error("[ids] cannot read the VR address library '{}'", path);
				return;
			}
			std::string line;
			std::getline(file, line);  // header row
			while (std::getline(file, line)) {
				std::uint64_t id = 0;
				if (std::from_chars(line.data(), line.data() + line.size(), id).ec == std::errc{}) {
					ids.insert(id);
				}
			}
			ok = true;
			spdlog::info("[ids] VR address library: {} ids read", ids.size());
		});
		return ok ? &ids : nullptr;
	}

	bool HaveIds(std::initializer_list<std::uint64_t> a_ids, const char* a_what)
	{
		const auto* ids = AddressIds();
		if (!ids) {
			spdlog::error("[ids] {} switched off: the address library could not be read", a_what);
			return false;
		}
		std::string missing;
		for (const auto id : a_ids) {
			if (!ids->contains(id)) {
				missing += std::format(" {}", id);
			}
		}
		if (!missing.empty()) {
			spdlog::error("[ids] {} switched off: the VR address library lacks the id(s){}", a_what, missing);
			return false;
		}
		return true;
	}

	// [any thread] A form by its id, or null. (Needs the two global form-map ids; without them it logs once and returns null instead of ending the game.)
	bool FormLookupAvailable()
	{
		static const bool ok = HaveIds({ 422985, 691815 }, "lookup of forms by id");
		return ok;
	}

	RE::TESForm* FormById(std::uint32_t a_id) { return FormLookupAvailable() ? RE::TESForm::GetFormByID(a_id) : nullptr; }

	template <class T>
	T* FormById(std::uint32_t a_id)
	{
		auto* form = FormById(a_id);
		return form ? form->As<T>() : nullptr;
	}

	// =====================================================================================================================
	// BodyGen ini engine, game side. Reads F4SE\Plugins\F4EE\BodyGen\<Plugin.esp>\{templates,morphs}.ini and Loose\*_{templates,morphs}.ini (the place and the file
	// grammar LooksMenu's BodyGen uses, so Silhouette's files work unchanged), and rolls the morphs of an actor that has none: the actor's template chain, race and
	// sex pick the line, the line's templates give random values, they are written to the store (what the Papyrus functions write too).
	// The parsing, matching and rolling are in BodyGenIni.h (unit-tested without the game); this part only reads game data, through SEH-guarded reads.
	// =====================================================================================================================
	namespace autogen
	{
		namespace gi = bodymorph::genini;

		struct RawForm
		{
			std::uint32_t formID = 0;
			std::uint8_t  type = 0;
		};
		struct RawNpc
		{
			std::uint32_t  formID = 0;
			std::uintptr_t tmpl = 0;  // TESActorBaseData::baseTemplateForm
			std::uintptr_t race = 0;  // TESRaceForm::formRace
		};

		constexpr auto kTypeNpc = static_cast<std::uint8_t>(RE::ENUM_FORM_ID::kNPC_);
		constexpr auto kTypeLeveled = static_cast<std::uint8_t>(RE::ENUM_FORM_ID::kLVLN);
		constexpr auto kTypeRace = static_cast<std::uint8_t>(RE::ENUM_FORM_ID::kRACE);

		// ---- SEH-guarded reads (no C++ objects with destructors in these)
		bool ReadForm(const RE::TESForm* a_form, RawForm& a_out)
		{
			__try {
				a_out.formID = a_form->GetFormID();
				a_out.type = static_cast<std::uint8_t>(a_form->GetFormType());
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool ReadNpc(const RE::TESNPC* a_npc, RawNpc& a_out)
		{
			__try {
				a_out.formID = a_npc->GetFormID();
				a_out.tmpl = reinterpret_cast<std::uintptr_t>(a_npc->baseTemplateForm);
				a_out.race = reinterpret_cast<std::uintptr_t>(a_npc->GetFormRace());
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool ReadEditorId(const RE::TESForm* a_form, char* a_buf, int a_size)
		{
			__try {
				const char* id = a_form->GetFormEditorID();
				int         i = 0;
				for (; id && i < a_size - 1 && id[i]; ++i) {
					a_buf[i] = id[i];
				}
				a_buf[i] = 0;
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		int ReadLeveled(const RE::TESLevCharacter* a_list, const RE::TESForm** a_out, int a_max)
		{
			__try {
				const int   count = a_list->baseListCount;
				const auto* entries = a_list->leveledLists;
				int         n = 0;
				for (int i = 0; entries && i < count && n < a_max; ++i) {
					if (entries[i].form) {
						a_out[n++] = entries[i].form;
					}
				}
				return n;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		int PluginIndex(const char* a_name)
		{
			__try {
				auto* handler = RE::TESDataHandler::GetSingleton();
				if (!handler) {
					return -1;
				}
				const auto* file = handler->LookupLoadedModByName(a_name);
				return file ? static_cast<int>(file->compileIndex) : -1;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}

		const RE::TESForm* LookupIn(const char* a_plugin, std::uint32_t a_localId)
		{
			__try {
				auto* handler = RE::TESDataHandler::GetSingleton();
				return handler ? handler->LookupForm(a_localId, a_plugin) : nullptr;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		// The form itself and, for a leveled list, everything it can produce.
		void ExpandForm(const RE::TESForm* a_form, std::vector<std::uint32_t>& a_out, int a_depth)
		{
			RawForm raw;
			if (!a_form || a_depth > 6 || !ReadForm(a_form, raw) || std::find(a_out.begin(), a_out.end(), raw.formID) != a_out.end()) {
				return;
			}
			a_out.push_back(raw.formID);
			if (raw.type == kTypeLeveled) {
				const RE::TESForm* entries[96];
				const int          n = ReadLeveled(static_cast<const RE::TESLevCharacter*>(a_form), entries, 96);
				for (int i = 0; i < n; ++i) {
					ExpandForm(entries[i], a_out, a_depth + 1);
				}
			}
		}

		// The editor id of a race. HumanRace is known by its form id (Fallout4.esm 013746); any other race is asked for its editor id.
		std::string RaceName(std::uintptr_t a_race)
		{
			if (!a_race) {
				return {};
			}
			RawForm raw;
			if (!ReadForm(reinterpret_cast<const RE::TESForm*>(a_race), raw) || raw.type != kTypeRace) {
				return "?";
			}
			if (raw.formID == 0x00013746) {
				return "HumanRace";
			}
			char buf[128]{};
			if (ReadEditorId(reinterpret_cast<const RE::TESForm*>(a_race), buf, sizeof(buf)) && buf[0]) {
				return buf;
			}
			return std::format("?{:08X}", raw.formID);
		}

		// The actor's base NPC and the NPCs it is templated from (a leveled list ends the chain).
		std::vector<gi::NpcInfo> BuildChain(const RE::TESNPC* a_npc)
		{
			std::vector<gi::NpcInfo> chain;
			const RE::TESForm*       cur = a_npc;
			for (int depth = 0; depth < 8 && cur; ++depth) {
				RawForm form;
				if (!ReadForm(cur, form)) {
					break;
				}
				if (form.type == kTypeLeveled) {
					gi::NpcInfo level;
					level.formID = form.formID;
					level.isNpc = false;
					chain.push_back(std::move(level));
					break;
				}
				if (form.type != kTypeNpc) {
					break;
				}
				RawNpc raw;
				if (!ReadNpc(static_cast<const RE::TESNPC*>(cur), raw)) {
					break;
				}
				gi::NpcInfo info;
				info.formID = raw.formID;
				info.hasTemplate = raw.tmpl != 0;
				info.race = RaceName(raw.race);
				chain.push_back(std::move(info));
				cur = reinterpret_cast<const RE::TESForm*>(raw.tmpl);
			}
			return chain;
		}

		// ---- state
		struct State
		{
			std::mutex                        mutex;  // the rng, the tried set and the log counter
			gi::Db                            db;     // read-only after Load
			std::mt19937                      rng;
			std::unordered_set<std::uint32_t> tried;  // actors rolled this session (a roll that gave nothing is not repeated for every garment of the same actor)
			std::atomic<bool>                 ready{ false };
			int                               logLimit = 60;
			int                               logged = 0;
		};
		State g_gen;

		bool ReadWhole(const std::filesystem::path& a_path, std::string& a_out)
		{
			std::ifstream file(a_path, std::ios::binary);
			if (!file) {
				return false;
			}
			a_out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			return true;
		}

		// ---- BodySlide presets as bodies (0.8.0). The plugin reads the user's own BodySlide files (Data\Tools\BodySlide: presets, slider sets, build selection) and the
		// body .tri of each sex, and hands the presets out by weight to every NPC of the listed races that no FormID / plugin line of a BodyGen ini names.
		// Reading and resolving are in BodySlidePresets.h (unit-tested without the game).
		namespace bs = bodymorph::bsp;

		struct PresetSex
		{
			std::vector<bs::Body> pool;
			bool                  triFound = false;
			std::string           setName;
		};
		struct PresetState
		{
			PresetSex                sex[2];  // 0 male, 1 female
			std::vector<std::string> races;   // lower case
			bool                     enabled = false;
			bool                     overGeneral = true;  // presets win over "All|..." lines of BodyGen ini files
			bool                     notify = true;
			int                      presetsRead = 0;
			std::string              problem;  // what the player is told once ("" = nothing to tell)
		};
		PresetState g_presets;  // read-only after LoadPresets

		std::vector<std::string> SplitList(const std::string& a_text)
		{
			std::vector<std::string> out;
			for (const auto piece : gi::Split(a_text, ',')) {
				if (const auto t = gi::Trim(piece); !t.empty()) {
					out.push_back(bs::Lower(t));
				}
			}
			return out;
		}

		void CollectFiles(const std::filesystem::path& a_folder, std::initializer_list<const char*> a_exts, std::vector<std::filesystem::path>& a_out)
		{
			namespace fs = std::filesystem;
			std::error_code ec;
			if (!fs::is_directory(a_folder, ec)) {
				return;
			}
			for (fs::recursive_directory_iterator it(a_folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
				std::error_code ec2;
				if (!it->is_regular_file(ec2)) {
					continue;
				}
				const auto ext = bs::Lower(it->path().extension().string());
				for (const char* want : a_exts) {
					if (ext == want) {
						a_out.push_back(it->path());
						break;
					}
				}
			}
			std::sort(a_out.begin(), a_out.end());  // a fixed order: "the first preset of a name wins" must not depend on the file system
		}

		void LoadPresets(const bodymorph::IniFile& a_ini)
		{
			namespace fs = std::filesystem;
			auto& st = g_presets;
			if (a_ini.GetInt("Presets", "bEnable", 1) == 0) {
				spdlog::info("[presets] switched off by the INI ([Presets] bEnable=0)");
				return;
			}
			const fs::path folder = a_ini.GetString("Presets", "sFolder", "Data\\Tools\\BodySlide");
			st.races = SplitList(a_ini.GetString("Presets", "sRaces", "HumanRace"));
			st.overGeneral = a_ini.GetInt("Presets", "bOverGeneralRules", 1) != 0;
			st.notify = a_ini.GetInt("Presets", "bNotify", 1) != 0;

			std::vector<fs::path> presetFiles, setFiles;
			CollectFiles(folder / "SliderPresets", { ".xml" }, presetFiles);
			CollectFiles(folder / "SliderSets", { ".osp", ".xml" }, setFiles);
			std::vector<bs::Preset>    presets;
			std::vector<bs::SliderSet> sets;
			std::string                text;
			for (const auto& f : presetFiles) {
				if (ReadWhole(f, text)) {
					bs::ParsePresets(text, f.filename().string(), presets);
				}
			}
			for (const auto& f : setFiles) {
				if (ReadWhole(f, text)) {
					bs::ParseSliderSets(text, f.filename().string(), sets);
				}
			}
			std::string selection;
			ReadWhole(folder / "BuildSelection.xml", selection);
			st.presetsRead = static_cast<int>(presets.size());
			spdlog::info("[presets] '{}': {} preset(s) in {} file(s), {} slider set(s) in {} file(s)", folder.string(), presets.size(), presetFiles.size(), sets.size(), setFiles.size());

			bs::PoolOptions opt;
			opt.minFit = static_cast<float>(std::clamp(a_ini.GetInt("Presets", "iMinFitPercent", 50), 1, 100)) / 100.0f;
			opt.defaultWeight = std::clamp(a_ini.GetInt("Presets", "iDefaultWeight", 2), 1, 100);
			opt.autoWeights = a_ini.GetInt("Presets", "bAutoWeights", 1) != 0;
			opt.exclude = SplitList(a_ini.GetString("Presets", "sExclude", "(outfit),zeroed,cbbe vanilla"));
			for (const auto& n : SplitList(a_ini.GetString("Presets", "sNeverMorphs", "Erection,Erection Up,Erection Down,CErection,VaginaPenetrate,AnusPenetrate,VaginaSpread,ButtcheeksSpread"))) {
				opt.never.insert(n);
			}
			opt.weightOf = [&a_ini](const std::string& a_name) { return a_ini.GetInt("PresetWeights", a_name, -1); };

			struct SexCfg
			{
				const char* label;
				const char* triKey;
				const char* triDefault;
				const char* builtKey;
				const char* output;
			};
			const SexCfg cfg[2] = {
				{ "male", "sMaleTri", "Data\\Meshes\\Actors\\Character\\CharacterAssets\\MaleBody.tri", "sMaleBuiltWith", "meshes\\actors\\character\\characterassets\\malebody" },
				{ "female", "sFemaleTri", "Data\\Meshes\\Actors\\Character\\CharacterAssets\\FemaleBody.tri", "sFemaleBuiltWith", "meshes\\actors\\character\\characterassets\\femalebody" },
			};
			bodymorph::TriFile                 tri[2];
			std::unordered_set<std::string>    names[2];
			for (int i = 0; i < 2; ++i) {
				st.sex[i].triFound = LoadTri(a_ini.GetString("Presets", cfg[i].triKey, cfg[i].triDefault), tri[i]);
				if (st.sex[i].triFound) {
					names[i] = bs::MorphNames(tri[i]);
				}
			}
			for (int i = 0; i < 2; ++i) {
				if (!st.sex[i].triFound) {
					spdlog::warn("[presets] {}: no body .tri, so {} NPCs get no preset body (build the body in BodySlide with \"Build Morphs\" ticked)", cfg[i].label, cfg[i].label);
					continue;
				}
				const auto* set = bs::PickSet(sets, cfg[i].output, bs::BuildChoice(selection, cfg[i].output), &names[i]);
				st.sex[i].setName = set ? set->name : std::string();
				// what the body on disk was built with: "" = zeroed sliders (every slider at 0), "(defaults)" = the slider set's defaults, else a preset's name
				const std::string                          built = a_ini.GetString("Presets", cfg[i].builtKey, "");
				std::vector<std::pair<std::string, float>> base;
				if (gi::EqualsNoCase(built, "(defaults)")) {
					base = bs::Resolve(bs::Preset{}, set);
				} else if (!built.empty()) {
					const auto it = std::find_if(presets.begin(), presets.end(), [&](const bs::Preset& p) { return gi::EqualsNoCase(p.name, built); });
					if (it != presets.end()) {
						base = bs::Resolve(*it, set);
					} else {
						spdlog::warn("[presets] {}: {}='{}' names no installed preset: taken as zeroed sliders", cfg[i].label, cfg[i].builtKey, built);
					}
				}
				std::vector<std::string> notes;
				st.sex[i].pool = bs::BuildPool(presets, set, tri[i], names[1 - i], opt, base, &notes);
				int total = 0;
				for (const auto& b : st.sex[i].pool) {
					total += b.weight;
				}
				spdlog::info("[presets] {}: slider set '{}'{}, body built with {}, {} morph name(s) in the .tri, {} bod{} in the pool ({} slot(s))", cfg[i].label, st.sex[i].setName,
					set ? "" : " (none found: presets are taken as written)", built.empty() ? "zeroed sliders" : "'" + built + "'", names[i].size(), st.sex[i].pool.size(), st.sex[i].pool.size() == 1 ? "y" : "ies", total);
				for (const auto& b : st.sex[i].pool) {
					spdlog::info("[presets]   {} '{}' ({}): weight {}{} = {:.1f} %, {} morph(s), fit {:.0f} %, size {:.2f}", cfg[i].label, b.name, b.file, b.weight, b.autoWeight ? " (auto)" : "",
						total ? 100.0 * b.weight / total : 0.0, b.morphs.size(), b.fit * 100.0f, b.size);
				}
				for (const auto& n : notes) {
					spdlog::info("[presets]   {} not used: {}", cfg[i].label, n);
				}
			}
			st.enabled = !st.sex[0].pool.empty() || !st.sex[1].pool.empty();

			// what the player has to be told (once per session, after a save is loaded)
			if (!st.sex[0].triFound && !st.sex[1].triFound) {
				st.problem = "BodyMorph VR: no body .tri files found. Build your bodies and outfits in BodySlide with \"Build Morphs\" ticked.";
			} else if (!st.sex[1].triFound) {
				st.problem = "BodyMorph VR: FemaleBody.tri not found. Build the female body in BodySlide with \"Build Morphs\" ticked.";
			} else if (presets.empty()) {
				st.problem = "BodyMorph VR: no BodySlide presets found in Data\\Tools\\BodySlide\\SliderPresets.";
			} else if (st.sex[1].pool.empty()) {
				st.problem = "BodyMorph VR: none of your BodySlide presets fits the female body. See BodyMorphVR.log.";
			}
			if (!st.problem.empty()) {
				spdlog::warn("[presets] {}", st.problem);
			}
		}

		// [any thread] The preset pool for an NPC, or null: presets on, a pool for its sex, and its race in the list.
		const std::vector<bs::Body>* PresetPool(const std::vector<gi::NpcInfo>& a_chain, bool a_female)
		{
			const auto& sex = g_presets.sex[a_female ? 1 : 0];
			if (!g_presets.enabled || sex.pool.empty() || a_chain.empty()) {
				return nullptr;
			}
			// the race of the NPC itself, else of the first level of its template chain that has one
			for (const auto& level : a_chain) {
				if (level.isNpc && !level.race.empty()) {
					const auto race = bs::Lower(level.race);
					return std::find(g_presets.races.begin(), g_presets.races.end(), race) != g_presets.races.end() ? &sex.pool : nullptr;
				}
			}
			return nullptr;
		}

		// [main thread task] Tells the player once per session what is missing (a HUD line, like the game's own notices).
		void NotifyOnce()
		{
			static std::atomic<bool> told{ false };
			if (!g_presets.notify || g_presets.problem.empty() || told.exchange(true)) {
				return;
			}
			if (!HaveIds({ 1163005 }, "the in-game notice")) {
				return;
			}
			if (auto* task = F4SE::GetTaskInterface()) {
				task->AddTask([] {
					using func_t = void(const char*, const char*, bool, bool);
					static REL::Relocation<func_t*> show{ REL::ID(1163005) };  // SendHUDMessage::ShowHUDMessage(message, sound, throttle, warning)
					show(g_presets.problem.c_str(), nullptr, false, true);
				});
			}
		}

		void Load()
		{
			namespace fs = std::filesystem;
			const auto ini = ReadIni();
			g_gen.logLimit = std::clamp(ini.GetInt("BodyGen", "iLogActors", 60), 0, 5000);
			const int seed = ini.GetInt("BodyGen", "iSeed", 0);
			g_gen.rng.seed(seed ? static_cast<std::uint32_t>(seed) : std::random_device{}());
			try {
				LoadPresets(ini);
			} catch (const std::exception& e) {
				spdlog::error("[presets] reading BodySlide's files failed: {}", e.what());
				g_presets.enabled = false;
			}
			if (ini.GetInt("BodyGen", "bEnable", 1) == 0) {
				spdlog::info("[bodygen] switched off by the INI (bEnable=0)");
				return;
			}
			const std::string folder = ini.GetString("BodyGen", "sFolder", "Data\\F4SE\\Plugins\\F4EE\\BodyGen");
			if (!HaveIds({ 711558 }, "BodyGen (plugin list)") || !FormLookupAvailable()) {
				return;  // (both log the ids that are missing)
			}

			std::error_code ec;
			if (!fs::is_directory(folder, ec)) {
				spdlog::info("[bodygen] no folder '{}': nothing to generate from (Silhouette or any BodyGen mod puts its templates.ini / morphs.ini there)", folder);
				return;
			}
			struct PluginDir
			{
				int         index;
				std::string name;
				fs::path    path;
			};
			std::vector<PluginDir>   dirs;
			std::vector<std::string> looseTemplates, looseMorphs;
			fs::path                 loose;
			int                      notLoaded = 0;
			for (const auto& entry : fs::directory_iterator(folder, ec)) {
				std::error_code ec2;
				if (!entry.is_directory(ec2)) {
					continue;
				}
				const auto name = entry.path().filename().string();
				if (gi::EqualsNoCase(name, "Loose")) {
					loose = entry.path();
					for (const auto& f : fs::directory_iterator(entry.path(), ec2)) {
						const auto file = f.path().filename().string();
						const auto low = gi::Lower(file);
						if (low.ends_with("_templates.ini")) {
							looseTemplates.push_back(file);
						} else if (low.ends_with("_morphs.ini")) {
							looseMorphs.push_back(file);
						}
					}
					continue;
				}
				const int index = PluginIndex(name.c_str());
				if (index < 0) {
					++notLoaded;
					continue;
				}
				dirs.push_back({ index, name, entry.path() });
			}
			std::sort(dirs.begin(), dirs.end(), [](const PluginDir& a, const PluginDir& b) { return a.index < b.index; });  // load order
			std::sort(looseTemplates.begin(), looseTemplates.end());                                                     // plain byte order, like LooksMenu
			std::sort(looseMorphs.begin(), looseMorphs.end());

			std::vector<std::string> problems;
			int                      files = 0;
			auto                     add = [&](bool a_templates, const fs::path& a_path, const std::string& a_label) {
                std::string text;
                if (!fs::exists(a_path, ec) || !ReadWhole(a_path, text)) {
                    return;
                }
                ++files;
                if (a_templates) {
                    g_gen.db.AddTemplates(text, a_label, &problems);
                } else {
                    g_gen.db.AddRules(text, a_label, &problems);
                }
			};
			for (const auto& d : dirs) {
				add(true, d.path / "templates.ini", d.name + "/templates.ini");
			}
			for (const auto& f : looseTemplates) {
				add(true, loose / f, "Loose/" + f);
			}
			for (const auto& d : dirs) {
				add(false, d.path / "morphs.ini", d.name + "/morphs.ini");
			}
			for (const auto& f : looseMorphs) {
				add(false, loose / f, "Loose/" + f);
			}

			gi::Resolver res;
			res.pluginIndex = [](const std::string& a_name) { return PluginIndex(a_name.c_str()); };
			res.forms = [](const std::string& a_plugin, std::uint32_t a_local, std::vector<std::uint32_t>& a_out) { ExpandForm(LookupIn(a_plugin.c_str(), a_local), a_out, 0); };
			g_gen.db.Finalize(res, &problems);

			const auto& st = g_gen.db.GetStats();
			spdlog::info("[bodygen] '{}': {} plugin folder(s) ({} skipped: plugin not loaded), {} loose templates file(s), {} loose morphs file(s), {} file(s) read; {} template(s) ({} malformed), {} line(s) "
						 "({} malformed, {} usable), {} option(s) named a template that does not exist",
				folder, dirs.size(), notLoaded, looseTemplates.size(), looseMorphs.size(), files, st.templates, st.malformedTemplates, st.rules, st.malformedRules, st.validRules, st.missingTemplates);
			for (const auto& p : problems) {
				spdlog::warn("[bodygen]   {}", p);
			}
			g_gen.ready = st.validRules > 0;
		}

		// [any thread] Reads the files once (at the first use, or when the game data is ready). False: neither the BodyGen ini files nor the presets have anything to give.
		bool EnsureLoaded()
		{
			static std::once_flag once;
			std::call_once(once, [] {
				try {
					Load();
				} catch (const std::exception& e) {
					spdlog::error("[bodygen] loading the files failed: {}", e.what());
					g_gen.ready = false;
				} catch (...) {
					spdlog::error("[bodygen] loading the files failed");
					g_gen.ready = false;
				}
			});
			return g_gen.ready || g_presets.enabled;
		}

		// [any thread] BodyGen for one actor that has no stored morphs: once per session unless a_force (Papyrus RegenerateMorphs). The result goes to the store,
		// under the keyword None, like LooksMenu's. True: the actor got at least one morph.
		bool Generate(RE::Actor* a_actor, bool a_female, bool a_force)
		{
			if (!a_actor || !EnsureLoaded()) {
				return false;
			}
			const auto id = a_actor->GetFormID();
			{
				std::lock_guard lock(g_gen.mutex);
				if (!g_gen.tried.insert(id).second && !a_force) {
					return false;
				}
			}
			const auto                                 chain = BuildChain(a_actor->GetNPC());
			const auto* rule = (chain.empty() || !g_gen.ready) ? nullptr : g_gen.db.MatchChain(chain, a_female);
			// A line that names this NPC (FormID) or its plugin always decides. Otherwise a BodySlide preset does; an "All|..." line only when presets are off or lose to it.
			const auto* pool = PresetPool(chain, a_female);
			if (pool && rule && rule->kind == gi::Rule::Kind::kAll && !g_presets.overGeneral) {
				pool = nullptr;
			}
			if (pool && rule && rule->kind != gi::Rule::Kind::kAll) {
				pool = nullptr;
			}
			const bs::Body*                            body = nullptr;
			std::vector<std::pair<std::string, float>> morphs;
			bool                                       log = false;
			{
				std::lock_guard lock(g_gen.mutex);
				if (pool) {
					body = bs::Pick(*pool, g_gen.rng);
				}
				if (body) {
					morphs = body->morphs;
					morphs.emplace_back(bs::MarkerName(body->name), 1.0f);
				} else if (rule) {
					morphs = g_gen.db.Roll(*rule, g_gen.rng);
				}
				log = g_gen.logged < g_gen.logLimit;
				g_gen.logged += log ? 1 : 0;
			}
			for (const auto& [name, value] : morphs) {
				g_store.Set(id, a_female, name, 0, value);
			}
			if (log) {
				std::string what;
				for (const auto& [name, value] : morphs) {
					what += std::format("{}{} {:.2f}", what.empty() ? "" : ", ", name, value);
				}
				std::string levels;
				for (const auto& c : chain) {
					levels += std::format("{}{:08X}{}{}", levels.empty() ? "" : " > ", c.formID, c.isNpc ? "" : " (leveled list)", c.race.empty() ? std::string() : " " + c.race);
				}
				spdlog::info("[bodygen] actor {:08X} ({}) chain [{}]: {}{}", id, a_female ? "female" : "male", levels,
					body ? std::format("BodySlide preset '{}' (weight {}): {}", body->name, body->weight, what)
						 : rule ? std::format("line {} '{}' rolled {} morph(s){}{}", rule->source, rule->key, morphs.size(), morphs.empty() ? "" : ": ", what) : std::string("no line or preset matches"),
					a_force ? " (forced)" : "");
			}
			return !morphs.empty();
		}

		// [any thread] A new session and ClearAll let every actor be rolled again.
		void ForgetAll()
		{
			std::lock_guard lock(g_gen.mutex);
			g_gen.tried.clear();
		}
	}

	// ---- loading a save: the game builds the actors of the loaded cell BEFORE F4SE calls our co-save load callback (0.6.1 test: attach 3.4 s before it), so at that
	// moment the store is empty and an actor with a saved body would get a BodyGen roll instead. Between PreLoadGame / revert and PostLoadGame the attach hook therefore
	// only RECORDS the meshes and remembers the actor; FinishLoad then gives each of them its stored morphs (or BodyGen's) with a live update.
	std::atomic<std::int64_t>         g_loadPendingUntil{ 0 };  // steady-clock ms; expires by itself in case PostLoadGame never comes
	std::mutex                        g_deferMutex;
	std::unordered_set<std::uint32_t> g_deferred;

	std::int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
	bool         LoadPending() { return NowMs() < g_loadPendingUntil.load(); }
	void         SetLoadPending() { g_loadPendingUntil = NowMs() + 30000; }
	void         Defer(std::uint32_t a_formID)
	{
		std::lock_guard lock(g_deferMutex);
		g_deferred.insert(a_formID);
	}

	// ---- which sliders an actor gets: its stored morphs (Papyrus BodyGen / the store) when it has any, else the INI's global sliders (the old test behaviour), else none.
	struct WeightPick
	{
		std::vector<bodymorph::MorphWeight> weights;
		const char*                         source = "none";
	};

	// [any thread]
	// a_generate: the attach path, where an actor with no stored morphs gets BodyGen's (templates.ini / morphs.ini). A live update never rolls.
	WeightPick PickWeights(const AutoSettings& a_cfg, RE::Actor* a_actor, bool a_female, bool a_generate)
	{
		WeightPick pick;
		const auto id = a_actor->GetFormID();
		bool       generated = false;
		if (a_generate && !g_store.Has(id, a_female)) {
			generated = autogen::Generate(a_actor, a_female, false);
		}
		if (g_store.Has(id, a_female)) {
			pick.weights = g_store.Effective(id, a_female);
			pick.source = generated ? "BodyGen" : "stored morphs";
		} else if (a_cfg.useGlobalSliders && !a_cfg.weights.empty() && (!a_cfg.femaleOnly || a_female)) {
			pick.weights = a_cfg.weights;
			pick.source = "INI sliders";
		}
		return pick;
	}

	// [hook thread] Records one geometry and, when the actor has sliders, morphs it. Returns "morphed ..." or the reason it was left alone (for the diagnostic line).
	std::string MorphOne(const Engine& a_engine, const AutoSettings& a_cfg, const bodymorph::TriFile& a_tri, bool a_allowAlias, const std::string& a_triPath,
		const std::vector<bodymorph::MorphWeight>& a_weights, RE::BSGeometry* a_geo)
	{
		const std::string full = a_geo->name.c_str() ? a_geo->name.c_str() : "";
		const auto        shape = ResolveShape(a_cfg, a_tri, a_allowAlias, full);
		if (shape.empty()) {
			return "not in the .tri";
		}
		std::uint16_t  numVerts = 0;
		std::uintptr_t current = 0;
		if (!Peek(a_geo, 0x1A4, numVerts) || !Peek(a_geo, 0x188, current)) {
			return "mesh unreadable";
		}
		const bool viaAlias = !a_tri.FindShape(full) && !a_tri.FindShape(bodymorph::StripInstanceSuffix(full));
		if (viaAlias && a_cfg.aliasVerts > 0 && numVerts != a_cfg.aliasVerts) {
			return std::format("alias match but {} vertices, not the body's {}", numVerts, a_cfg.aliasVerts);
		}
		if (AlreadyMorphed(a_geo, current)) {
			return "already morphed";
		}
		RememberGeo(a_geo, { current, 0, a_triPath, shape });  // the original mesh, for later updates
		if (a_weights.empty()) {
			return "recorded (no sliders for this actor)";
		}
		CloneResult cr;
		std::string note;
		if (!BuildClone(a_engine, a_geo, full, shape, a_tri, a_weights, cr, true, &note)) {
			return note.empty() ? "clone failed (see the log)" : note;
		}
		const auto t0 = std::chrono::steady_clock::now();
		if (a_cfg.waitMs > 0) {
			while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(a_cfg.waitMs)) {
				const auto st = ReadBufferState(cr.newVB);
				if (st.ok && st.d3d != 0 && st.pendingCopy == 0 && st.refCount <= 1) {
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		std::uintptr_t now = 0;
		if (!Peek(a_geo, 0x188, now) || now != cr.oldTri || !WritePointer(a_geo, 0x188, cr.newTri)) {
			return std::format("the mesh changed under us ({:#x} instead of {:#x}), clone unused", now, cr.oldTri);
		}
		SetGeoClone(a_geo, cr.newTri, WeightsKey(a_weights));
		const int total = ++g_autoShapes;
		if (total % 25 == 0) {
			spdlog::info("[auto] {} meshes morphed so far", total);
		}
		if (a_cfg.scanFirst > 0 && g_autoScans.fetch_add(1) < a_cfg.scanFirst) {
			Job j;  // bookkeeping only, for the scan request
			j.name = full;
			j.oldTri = cr.oldTri;
			j.oldVB = cr.oldVB;
			j.ib = cr.ib;
			j.newTri = cr.newTri;
			j.newVB = cr.newVB;
			ScheduleScan(j, "auto: a few seconds after the attach", true, false, 4000);
		}
		return std::format("morphed ({} vertices, waited {} ms)", numVerts, waited);
	}

	// [hook thread] a_root: the subtree the game has just attached for this actor. a_bodyTri: the BODYTRI string read from the nif's root node
	// before the attach ("" = none). a_extras, a_rootName, a_slot: for the diagnostic line only.
	void AutoMorph(RE::Actor* a_actor, RE::NiAVObject* a_root, const char* a_bodyTri, const char* a_extras, const char* a_rootName, std::uint32_t a_slot)
	{
		const auto cfg = GetAutoSettings();
		if (!cfg || !cfg->enabled || !a_actor || !a_root) {
			return;
		}
		if (a_actor->GetFormID() == 0x14 && !cfg->includePlayer) {
			return;
		}
		const auto* npc = a_actor->GetNPC();
		const bool  female = npc && npc->IsFemale();
		const bool  deferring = LoadPending();
		WeightPick  waiting;
		if (deferring) {
			waiting.source = "waiting for the co-save";
			Defer(a_actor->GetFormID());
		}
		const auto pick = deferring ? waiting : PickWeights(*cfg, a_actor, female, true);
		Engine      engine;
		if (!GetEngine(engine)) {
			return;
		}

		// which .tri: the garment's own one (BODYTRI) or, for a mesh that carries none, the global (female) body .tri by shape name
		std::shared_ptr<const bodymorph::TriFile> garment;
		const bodymorph::TriFile*                 tri = nullptr;
		std::string                               triPath;  // "" = the global body .tri
		std::string                               source;
		const bool                                hasBodyTri = cfg->useBodyTri && a_bodyTri && a_bodyTri[0];
		if (hasBodyTri) {
			const auto path = bodymorph::BodyTriToDataPath(a_bodyTri);
			if (path.empty()) {
				source = std::format("BODYTRI '{}' is not a usable path", a_bodyTri);
			} else if ((garment = GetGarmentTri(path))) {
				tri = garment.get();
				triPath = path;
				source = std::format("BODYTRI '{}'", a_bodyTri);
			} else {
				source = std::format("BODYTRI '{}' but that .tri cannot be read", a_bodyTri);
			}
		} else if (cfg->triOk && female) {
			tri = &cfg->tri;
			source = "no BODYTRI, global .tri by shape name";
		} else {
			source = female ? "no BODYTRI and no global .tri" : "no BODYTRI (the global .tri is the female body's)";
		}

		const bool  logThis = g_attachLogged.fetch_add(1) < cfg->logAttaches;
		std::string line;
		if (logThis) {
			line = std::format("[attach] actor {:08X} ({}, {}) slot {} root '{}' extras [{}] {}:", a_actor->GetFormID(), female ? "female" : "male", pick.source, a_slot,
				a_rootName ? a_rootName : "", a_extras ? a_extras : "", source);
		}
		std::vector<RE::BSGeometry*> geoms;
		CollectGeometries(a_root, 0, geoms);
		int morphed = 0;
		for (auto* geo : geoms) {
			std::string outcome = tri ? MorphOne(engine, *cfg, *tri, !hasBodyTri, triPath, pick.weights, geo) : "no .tri to use";
			if (outcome.starts_with("morphed")) {
				++morphed;
			}
			if (logThis) {
				std::uint16_t numVerts = 0;
				Peek(geo, 0x1A4, numVerts);
				line += std::format(" '{}' ({} v): {};", geo->name.c_str() ? geo->name.c_str() : "", numVerts, outcome);
			}
		}
		if (logThis) {
			spdlog::info("{}", line);
		}
		if (morphed) {
			++g_autoActors;
			if (!logThis && g_morphLogged.fetch_add(1) < 300) {
				spdlog::info("[auto] actor {:08X}: {} mesh(es) morphed ({}, {})", a_actor->GetFormID(), morphed, pick.source, source);
			}
		}
	}

	// =====================================================================================================================
	// Live update of an actor that is already on screen (Papyrus UpdateMorphs / RegenerateMorphs). The attach hook is the exact path (a mesh is born
	// morphed); this one swaps a geometry to a new clone built from its ORIGINAL mesh, with the same upload wait and draw-record patching the F7 lab uses.
	// =====================================================================================================================
	// [main] A clone that was queued for this geometry but is still uploading is superseded: release it, never published.
	void CancelPending(const Engine& a_engine, const RE::BSGeometry* a_geo)
	{
		for (auto it = g_jobs.begin(); it != g_jobs.end();) {
			if (it->geom.get() == a_geo && it->state == JobState::Pending) {
				ReleaseClone(a_engine, *it);
				it = g_jobs.erase(it);
			} else {
				++it;
			}
		}
	}

	// [main] Builds the clone for one geometry from its original mesh and queues the swap. False: nothing to morph (reason in the log).
	bool QueueLiveClone(const Engine& a_engine, RE::BSGeometry* a_geo, const GeoInfo& a_info, const std::string& a_name, const bodymorph::TriFile& a_tri,
		const std::vector<bodymorph::MorphWeight>& a_weights)
	{
		CancelPending(a_engine, a_geo);
		CloneResult cr;
		std::string note;
		if (!BuildClone(a_engine, a_geo, a_name, a_info.shape, a_tri, a_weights, cr, true, &note, a_info.orig)) {
			static std::atomic<int> s_notes{ 0 };  // shapes the sliders do not touch (hands, boots ...) are the common case: only the first ones are logged
			if (s_notes.fetch_add(1) < 100) {
				spdlog::info("[update]   '{}': {}", a_name, note.empty() ? "clone failed (see the log)" : note);
			}
			return false;
		}
		Job job;
		job.geom = RE::NiPointer<RE::BSGeometry>(a_geo);
		job.name = a_name;
		job.oldTri = cr.oldTri;
		job.oldVB = cr.oldVB;
		job.ib = cr.ib;
		job.newTri = cr.newTri;
		job.newVB = cr.newVB;
		job.desc = cr.desc;
		job.since = std::chrono::steady_clock::now();
		job.keep = false;
		job.weightsKey = WeightsKey(a_weights);
		job.replaced = (a_info.clone != 0 && cr.oldTri == a_info.clone) ? a_info.clone : 0;
		g_jobs.push_back(std::move(job));
		return true;
	}

	// [main] The geometry shows our clone and no sliders apply any more: point it at the original again; the clone is released after the usual delay.
	void RestoreToOriginal(const Engine& a_engine, RE::BSGeometry* a_geo, const GeoInfo& a_info, const std::string& a_name)
	{
		CancelPending(a_engine, a_geo);
		std::uintptr_t current = 0;
		if (!Peek(a_geo, 0x188, current) || current != a_info.clone || !a_info.clone || !a_info.orig) {
			return;
		}
		Job job;
		job.geom = RE::NiPointer<RE::BSGeometry>(a_geo);
		job.name = a_name;
		job.oldTri = a_info.orig;
		job.newTri = a_info.clone;
		Peek(reinterpret_cast<const void*>(a_info.orig), 0x08, job.oldVB);
		Peek(reinterpret_cast<const void*>(a_info.orig), 0x10, job.ib);
		Peek(reinterpret_cast<const void*>(a_info.clone), 0x08, job.newVB);
		if (!WritePointer(a_geo, 0x188, a_info.orig)) {
			return;
		}
		SetGeoClone(a_geo, 0);
		ScheduleScan(job, "after restore (update)", false);
		job.state = JobState::Retiring;
		job.since = std::chrono::steady_clock::now();
		job.keep = false;
		g_jobs.push_back(std::move(job));
	}

	// A script can ask for hundreds of updates: the first ones are logged, then only the ones that changed something (and one in a hundred of the rest).
	bool UpdateLogOk()
	{
		static std::atomic<int> n{ 0 };
		const int               i = n.fetch_add(1);
		return i < 150 || i % 100 == 0;
	}

	// [main] Puts the actor's stored morphs (or, with none, the INI sliders / nothing) on the body it has on screen now.
	void UpdateActorNow(std::uint32_t a_formID)
	{
		const bool logIt = UpdateLogOk();
		const auto cfg = GetAutoSettings();
		if (!cfg || !cfg->enabled) {
			if (logIt) {
				spdlog::info("[update] {:08X}: bEnabled=0 in the INI, nothing done", a_formID);
			}
			return;
		}
		auto* actor = FormById<RE::Actor>(a_formID);
		if (!actor) {
			if (logIt) {
				spdlog::info("[update] {:08X}: not an actor that is loaded", a_formID);
			}
			return;
		}
		if (a_formID == 0x14 && !cfg->includePlayer) {
			return;
		}
		auto* root = actor->Get3D();
		if (!root) {
			if (logIt) {
				spdlog::info("[update] {:08X}: no 3D loaded; its stored morphs apply when the body is built", a_formID);
			}
			return;
		}
		Engine engine;
		if (!GetEngine(engine)) {
			return;
		}
		const auto* npc = actor->GetNPC();
		const bool  female = npc && npc->IsFemale();
		const auto  pick = PickWeights(*cfg, actor, female, false);
		std::vector<RE::BSGeometry*> geoms;
		CollectGeometries(root, 0, geoms);
		const auto weightsKey = pick.weights.empty() ? std::uint64_t{ 0 } : WeightsKey(pick.weights);
		int        queued = 0, restored = 0, unknown = 0, noEffect = 0, unchanged = 0;
		for (auto* geo : geoms) {
			std::uintptr_t current = 0;
			GeoInfo        info;
			if (!Peek(geo, 0x188, current) || !LookupGeo(geo, current, info)) {
				++unknown;  // not a mesh the attach hook recorded (hands, gear without a .tri shape ...)
				continue;
			}
			const std::string name = geo->name.c_str() ? geo->name.c_str() : "";
			const bool        morphedNow = info.clone != 0 && current == info.clone;
			if (pick.weights.empty()) {
				if (morphedNow) {
					RestoreToOriginal(engine, geo, info, name);
					++restored;
				}
				continue;
			}
			if (morphedNow && info.weightsKey == weightsKey) {  // the clone on screen already has exactly these sliders
				++unchanged;
				continue;
			}
			std::shared_ptr<const bodymorph::TriFile> garment;
			const bodymorph::TriFile*                 tri = nullptr;
			if (!info.tri.empty()) {
				garment = GetGarmentTri(info.tri);
				tri = garment.get();
			} else if (cfg->triOk) {
				tri = &cfg->tri;
			}
			if (!tri) {
				++noEffect;
				continue;
			}
			if (QueueLiveClone(engine, geo, info, name, *tri, pick.weights)) {
				++queued;
			} else {
				++noEffect;
				if (morphedNow) {  // the new sliders do not touch this shape any more
					RestoreToOriginal(engine, geo, info, name);
					++restored;
				}
			}
		}
		if (logIt || queued || restored) {
			spdlog::info("[update] actor {:08X} ({}): {} sliders ({}); {} clone(s) queued, {} put back to the original, {} already up to date, {} unaffected, {} geometries not recorded", a_formID,
				female ? "female" : "male", pick.weights.size(), pick.source, queued, restored, unchanged, noEffect, unknown);
		}
		RefreshActive();
	}

	// ---- the hook (same site and prologue as Head Body Color Match VR)
	constexpr std::uintptr_t kRvaAttachSkinnedObject = 0x1CBC00;
	constexpr std::uint8_t   kAttachPrologue[5] = { 0x44, 0x89, 0x4C, 0x24, 0x20 };  // mov [rsp+20h], r9d

	using AttachSkinnedObject_t = RE::NiAVObject*(RE::BipedAnim*, RE::NiNode*, RE::NiNode*, std::uint32_t, bool);
	AttachSkinnedObject_t* g_attachOriginal = nullptr;

	RE::NiAVObject* AttachSkinnedObject_Hook(RE::BipedAnim* a_biped, RE::NiNode* a_objectRoot, RE::NiNode* a_parent, std::uint32_t a_index, bool a_firstPerson)
	{
		// Read what we need from the nif's root BEFORE the original runs: the engine may release that node while it attaches.
		char bodyTri[260]{};
		char extras[160]{};
		char rootName[64]{};
		bool wanted = false;
		if (!a_firstPerson && a_objectRoot) {
			try {
				const auto cfg = GetAutoSettings();
				wanted = cfg && cfg->enabled;
				if (wanted && cfg->useBodyTri) {
					ReadStringExtra(a_objectRoot, "BODYTRI", bodyTri, sizeof(bodyTri));
				}
				if (wanted && cfg->logAttaches > 0 && g_attachLogged.load() < cfg->logAttaches) {
					ListExtraNames(a_objectRoot, extras, sizeof(extras));
					CopyCString(a_objectRoot->name.c_str(), rootName, sizeof(rootName));
				}
			} catch (...) {
				wanted = false;
			}
		}
		auto* result = g_attachOriginal(a_biped, a_objectRoot, a_parent, a_index, a_firstPerson);
		try {
			if (wanted && a_biped && result) {
				auto  ref = a_biped->actorRef.get();
				auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (actor) {
					AutoMorph(actor, result, bodyTri, extras, rootName, a_index);
				}
			}
		} catch (...) {
			spdlog::error("[auto] attach hook threw");
		}
		return result;
	}

	// Copies the first a_count bytes (whole instructions, no rip-relative operand) of the function into a stub that jumps back to the rest.
	AttachSkinnedObject_t* MakeStub(std::uintptr_t a_addr, const std::uint8_t* a_bytes, std::size_t a_count)
	{
		auto*        stub = static_cast<std::uint8_t*>(F4SE::GetTrampoline().allocate(32));
		const std::uint8_t jmp[6]{ 0xFF, 0x25, 0, 0, 0, 0 };
		std::memcpy(stub, a_bytes, a_count);
		std::memcpy(stub + a_count, jmp, sizeof(jmp));
		const std::uint64_t back = a_addr + a_count;
		std::memcpy(stub + a_count + sizeof(jmp), &back, sizeof(back));
		return reinterpret_cast<AttachSkinnedObject_t*>(stub);
	}

	void InstallAttachHook()
	{
		const auto addr = REL::Module::get().base() + kRvaAttachSkinnedObject;
		std::uint8_t now[sizeof(kAttachPrologue)]{};
		if (!SafeCopy(now, reinterpret_cast<const void*>(addr), sizeof(now)) || std::memcmp(now, kAttachPrologue, sizeof(now)) != 0) {
			spdlog::error("AttachSkinnedObject prologue does not match at RVA {:X}: auto-apply NOT installed (F7 / F6 still work)", kRvaAttachSkinnedObject);
			return;
		}
		g_attachOriginal = MakeStub(addr, kAttachPrologue, sizeof(kAttachPrologue));
		F4SE::GetTrampoline().write_branch<5>(addr, reinterpret_cast<std::uintptr_t>(&AttachSkinnedObject_Hook));
		spdlog::info("AttachSkinnedObject hook installed at RVA {:X}: NPC bodies are morphed while the game builds them", kRvaAttachSkinnedObject);
	}

	// =====================================================================================================================
	// Keys
	// =====================================================================================================================
	// a_what != nullptr: log how long the task waited in F4SE's queue (the apply/restore log times are when the task RUNS, not when the key went down).
	void PostTask(void (*a_fn)(), const char* a_what = nullptr)
	{
		if (auto* task = F4SE::GetTaskInterface()) {
			const auto queued = std::chrono::steady_clock::now();
			task->AddTask([a_fn, a_what, queued] {
				try {
					if (a_what) {
						spdlog::info("[{}] key seen by the poll thread, task started {} ms later", a_what,
							std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - queued).count());
					}
					a_fn();
				} catch (...) {
					spdlog::error("task threw");
				}
			});
		}
	}

	// =====================================================================================================================
	// BodyGen: Papyrus natives, test keys, co-save
	// =====================================================================================================================
	// [any thread] Queues UpdateActorNow on the main thread.
	void PostUpdate(std::uint32_t a_formID)
	{
		if (auto* task = F4SE::GetTaskInterface()) {
			task->AddTask([a_formID] {
				try {
					UpdateActorNow(a_formID);
				} catch (...) {
					spdlog::error("[update] {:08X}: task threw", a_formID);
				}
			});
		}
	}

	// [any thread] The save is loaded (PostLoadGame / NewGame): the attach hook works normally again, and the actors it built while the co-save was still unread get
	// their body now: stored morphs if the co-save had a table for them, else BodyGen's roll.
	void FinishLoad()
	{
		g_loadPendingUntil = 0;
		std::vector<std::uint32_t> ids;
		{
			std::lock_guard lock(g_deferMutex);
			ids.assign(g_deferred.begin(), g_deferred.end());
			g_deferred.clear();
		}
		spdlog::info("[load] save loaded: {} actor(s) were built before the co-save was read, {} table(s) in memory", ids.size(), g_store.TableCount());
		if (ids.empty()) {
			return;
		}
		if (auto* task = F4SE::GetTaskInterface()) {
			task->AddTask([ids] {
				int stored = 0, rolled = 0;
				for (const auto id : ids) {
					try {
						auto* actor = FormById<RE::Actor>(id);
						if (!actor) {
							continue;
						}
						const auto* npc = actor->GetNPC();
						const bool  female = npc && npc->IsFemale();
						if (g_store.Has(id, female)) {
							++stored;
						} else if (autogen::Generate(actor, female, false)) {
							++rolled;
						}
						UpdateActorNow(id);
					} catch (...) {
						spdlog::error("[load] {:08X}: update threw", id);
					}
				}
				spdlog::info("[load] {} actor(s) got their saved morphs, {} a BodyGen roll", stored, rolled);
			});
		}
	}

	// [main] F3: puts [Test] sStoreMorphs into the store of the nearest NPC (what a script's BodyGen.SetMorph calls do) and updates its body.
	void StoreTestSet()
	{
		RefreshAutoSettings();
		float dist = 0.0F;
		auto* actor = NearestActor(g_maxDistance, g_femaleOnly, dist);  // bFemaleOnly: the nearest dog is not a test subject
		if (!actor) {
			spdlog::warn("[test] no {}NPC within {:.0f} units", g_femaleOnly ? "female " : "", g_maxDistance);
			return;
		}
		const auto* npc = actor->GetNPC();
		const bool  female = npc && npc->IsFemale();
		const auto  weights = bodymorph::ParseMorphList(g_storeMorphs);
		for (const auto& w : weights) {
			g_store.Set(actor->GetFormID(), female, w.name, 0, w.weight);
		}
		spdlog::info("[test] actor {:08X} ({}) at {:.0f} units: {} morph(s) from sStoreMorphs '{}' stored, updating its body", actor->GetFormID(), female ? "female" : "male", dist, weights.size(),
			g_storeMorphs);
		UpdateActorNow(actor->GetFormID());
	}

	// [main] F4: empties the store of the nearest NPC and puts its original body back.
	void StoreTestClear()
	{
		float dist = 0.0F;
		auto* actor = NearestActor(g_maxDistance, g_femaleOnly, dist);
		if (!actor) {
			spdlog::warn("[test] no {}NPC within {:.0f} units", g_femaleOnly ? "female " : "", g_maxDistance);
			return;
		}
		g_store.RemoveActor(actor->GetFormID());
		spdlog::info("[test] actor {:08X} at {:.0f} units: stored morphs removed, updating its body", actor->GetFormID(), dist);
		UpdateActorNow(actor->GetFormID());
	}

	// ---- the Papyrus script "BodyGen" (Scripts\BodyGen.pex declares the natives; the functions and signatures are the ones Silhouette's scripts call)
	namespace bodygen
	{
		// The first calls are logged in full (to see how the scripts use us), then one in a thousand.
		bool Loud()
		{
			static std::atomic<int> calls{ 0 };
			const int               n = calls.fetch_add(1);
			return n < 80 || n % 1000 == 0;
		}

		[[nodiscard]] std::uint32_t KeywordId(const RE::BGSKeyword* a_keyword) { return a_keyword ? a_keyword->GetFormID() : 0; }

		void SetMorph(std::monostate, RE::Actor* a_actor, bool a_female, std::string a_morph, RE::BGSKeyword* a_keyword, float a_value)
		{
			if (!a_actor || a_morph.empty()) {
				return;
			}
			g_store.Set(a_actor->GetFormID(), a_female, a_morph, KeywordId(a_keyword), a_value);
			if (Loud()) {
				spdlog::info("[papyrus] SetMorph actor {:08X} female {} '{}' keyword {:08X} value {}", a_actor->GetFormID(), a_female, a_morph, KeywordId(a_keyword), a_value);
			}
		}

		float GetMorph(std::monostate, RE::Actor* a_actor, bool a_female, std::string a_morph, RE::BGSKeyword* a_keyword)
		{
			return a_actor ? g_store.Get(a_actor->GetFormID(), a_female, a_morph, KeywordId(a_keyword)) : 0.0F;
		}

		void RemoveMorphsByName(std::monostate, RE::Actor* a_actor, bool a_female, std::string a_morph)
		{
			if (a_actor) {
				g_store.RemoveByName(a_actor->GetFormID(), a_female, a_morph);
				if (Loud()) {
					spdlog::info("[papyrus] RemoveMorphsByName actor {:08X} female {} '{}'", a_actor->GetFormID(), a_female, a_morph);
				}
			}
		}

		void RemoveMorphsByKeyword(std::monostate, RE::Actor* a_actor, bool a_female, RE::BGSKeyword* a_keyword)
		{
			if (a_actor) {
				g_store.RemoveByKeyword(a_actor->GetFormID(), a_female, KeywordId(a_keyword));
				if (Loud()) {
					spdlog::info("[papyrus] RemoveMorphsByKeyword actor {:08X} female {} keyword {:08X}", a_actor->GetFormID(), a_female, KeywordId(a_keyword));
				}
			}
		}

		void RemoveAllMorphs(std::monostate, RE::Actor* a_actor, bool a_female)
		{
			if (a_actor) {
				g_store.RemoveAll(a_actor->GetFormID(), a_female);
				if (Loud()) {
					spdlog::info("[papyrus] RemoveAllMorphs actor {:08X} female {}", a_actor->GetFormID(), a_female);
				}
			}
		}

		std::vector<RE::BGSKeyword*> GetKeywords(std::monostate, RE::Actor* a_actor, bool a_female, std::string a_morph)
		{
			std::vector<RE::BGSKeyword*> out;
			if (a_actor) {
				for (const auto id : g_store.Keywords(a_actor->GetFormID(), a_female, a_morph)) {
					if (auto* keyword = FormById<RE::BGSKeyword>(id)) {
						out.push_back(keyword);
					}
				}
			}
			return out;
		}

		std::vector<std::string> GetMorphs(std::monostate, RE::Actor* a_actor, bool a_female)
		{
			return a_actor ? g_store.Names(a_actor->GetFormID(), a_female) : std::vector<std::string>{};
		}

		// Clears the actor's morphs and runs BodyGen again (templates.ini / morphs.ini). a_update: put the new body on the actor at once.
		void RegenerateMorphs(std::monostate, RE::Actor* a_actor, bool a_update)
		{
			if (!a_actor) {
				return;
			}
			g_store.RemoveActor(a_actor->GetFormID());
			const auto* npc = a_actor->GetNPC();
			const bool  female = npc && npc->IsFemale();
			const bool  got = autogen::Generate(a_actor, female, true);
			if (Loud()) {
				spdlog::info("[papyrus] RegenerateMorphs actor {:08X} update {}: stored morphs cleared, BodyGen {}", a_actor->GetFormID(), a_update, got ? "gave new morphs" : "gave nothing");
			}
			if (a_update) {
				PostUpdate(a_actor->GetFormID());
			}
		}

		void UpdateMorphs(std::monostate, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return;
			}
			if (Loud()) {
				spdlog::info("[papyrus] UpdateMorphs actor {:08X}", a_actor->GetFormID());
			}
			PostUpdate(a_actor->GetFormID());
		}

		void ClearAll(std::monostate)
		{
			g_store.Clear();
			autogen::ForgetAll();
			spdlog::info("[papyrus] ClearAll: every stored morph removed");
		}

		bool SetSkinOverride(std::monostate, RE::Actor*, std::string)
		{
			static std::atomic<bool> said{ false };
			if (!said.exchange(true)) {
				spdlog::info("[papyrus] SetSkinOverride is not supported (returns false)");
			}
			return false;
		}

		bool RemoveSkinOverride(std::monostate, RE::Actor*) { return false; }
	}

	// The game functions the script binding code of CommonLibF4VR reaches through the VR address library: one missing id would end the game with a message box the
	// first time a script calls us, so they are checked first.
	bool PapyrusIdsPresent()
	{
		return HaveIds({ 996227, 571037, 541793, 1452752, 461710, 445184, 1429302, 897539, 709728, 522763, 648543 }, "the Papyrus BodyGen natives");
	}

	bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			return false;
		}
		a_vm->BindNativeMethod("BodyGen", "SetMorph", bodygen::SetMorph, true);
		a_vm->BindNativeMethod("BodyGen", "GetMorph", bodygen::GetMorph, true);
		a_vm->BindNativeMethod("BodyGen", "RemoveMorphsByName", bodygen::RemoveMorphsByName, true);
		a_vm->BindNativeMethod("BodyGen", "RemoveMorphsByKeyword", bodygen::RemoveMorphsByKeyword, true);
		a_vm->BindNativeMethod("BodyGen", "RemoveAllMorphs", bodygen::RemoveAllMorphs, true);
		a_vm->BindNativeMethod("BodyGen", "GetKeywords", bodygen::GetKeywords, true);
		a_vm->BindNativeMethod("BodyGen", "GetMorphs", bodygen::GetMorphs, true);
		a_vm->BindNativeMethod("BodyGen", "RegenerateMorphs", bodygen::RegenerateMorphs, true);
		a_vm->BindNativeMethod("BodyGen", "UpdateMorphs", bodygen::UpdateMorphs, true);
		a_vm->BindNativeMethod("BodyGen", "ClearAll", bodygen::ClearAll, true);
		a_vm->BindNativeMethod("BodyGen", "SetSkinOverride", bodygen::SetSkinOverride, true);
		a_vm->BindNativeMethod("BodyGen", "RemoveSkinOverride", bodygen::RemoveSkinOverride, true);
		spdlog::info("[papyrus] the 12 BodyGen natives are bound (script 'BodyGen')");
		return true;
	}

	// ---- co-save: one record per morph table
	constexpr std::uint32_t kCoSaveId = 0x424D5652;   // 'BMVR'
	constexpr std::uint32_t kRecordMorphs = 0x4D525048;  // 'MRPH'
	constexpr std::uint32_t kRecordVersion = 1;

	void CoSaveRevert(const F4SE::SerializationInterface*)
	{
		g_store.Clear();
		autogen::ForgetAll();
		SetLoadPending();
		spdlog::info("[save] revert: stored morphs cleared");
	}

	void CoSaveSave(const F4SE::SerializationInterface* a_intfc)
	{
		std::size_t written = 0;
		for (const auto& r : g_store.Serialize()) {
			if (a_intfc->WriteRecord(kRecordMorphs, kRecordVersion, r.bytes.data(), static_cast<std::uint32_t>(r.bytes.size()))) {
				++written;
			}
		}
		spdlog::info("[save] {} morph table(s) written to the co-save", written);
	}

	void CoSaveLoad(const F4SE::SerializationInterface* a_intfc)
	{
		g_store.Clear();
		std::uint32_t          type = 0, version = 0, length = 0;
		std::vector<std::byte> buffer;
		int                    read = 0, other = 0, damaged = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type != kRecordMorphs || version != kRecordVersion || length == 0 || length > (4U << 20)) {
				++other;
				continue;
			}
			buffer.resize(length);
			if (a_intfc->ReadRecordData(buffer.data(), length) != length) {
				++damaged;
				continue;
			}
			const bool ok = g_store.Deserialize(buffer.data(), buffer.size(), [a_intfc](std::uint32_t& a_id) {
				const auto resolved = a_intfc->ResolveFormID(a_id);
				if (!resolved) {
					return false;
				}
				a_id = *resolved;
				return true;
			});
			++(ok ? read : damaged);
		}
		spdlog::info("[save] co-save loaded: {} table(s) read ({} now in memory), {} unknown record(s), {} damaged", read, g_store.TableCount(), other, damaged);
	}

	void PollLoop()
	{
		struct Key
		{
			std::atomic<int>& vk;
			void (*fn)();
			const char*       what;
			bool              down = false;
		};
		Key  keys[] = { { g_probeKey, Probe, "probe" }, { g_applyKey, Apply, "apply" }, { g_restoreKey, Restore, "restore" }, { g_storeSetKey, StoreTestSet, "store set" },
		{ g_storeClearKey, StoreTestClear, "store clear" } };
		auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
		auto lastTick = last;
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			const auto now = std::chrono::steady_clock::now();
			for (auto& k : keys) {
				const int vk = k.vk;
				if (vk <= 0 || vk > 0xFE) {
					k.down = false;
					continue;
				}
				const bool down = (::GetAsyncKeyState(vk) & 0x8000) != 0;
				const bool pressed = down && !k.down;
				k.down = down;
				if (pressed && now - last >= std::chrono::milliseconds(700)) {
					last = now;
					PostTask(k.fn, k.what);
				}
			}
			if (g_active.load() > 0 && now - lastTick >= std::chrono::milliseconds(100)) {
				lastTick = now;
				PostTask(Tick);
			}
		}
	}

	// [main] Reads the .tri at start-up and logs what it holds (shape names, slider names): the names go into sMorphs.
	void SelfTestTri()
	{
		LoadSettings();
		bodymorph::TriFile tri;
		if (!LoadTri(g_triPath, tri)) {
			return;
		}
		spdlog::info("tri self-test: '{}': {} shape(s), {} UV morph(s) skipped", g_triPath, tri.Shapes().size(), tri.UVMorphCount());
		for (const auto& shape : tri.Shapes()) {
			std::size_t moved = 0;
			std::string names;
			for (const auto& [name, morph] : shape.morphs) {
				moved += morph.size();
				names += (names.empty() ? "" : ", ") + name;
			}
			spdlog::info("  shape '{}': {} morph(s), {} moved vertex entries", shape.name, shape.morphs.size(), moved);
			spdlog::info("    morph names (lower case): {}", names);
		}
	}

	void OnMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (a_msg && a_msg->type == F4SE::MessagingInterface::kPreLoadGame) {
			SetLoadPending();
		} else if (a_msg && (a_msg->type == F4SE::MessagingInterface::kPostLoadGame || a_msg->type == F4SE::MessagingInterface::kNewGame)) {
			FinishLoad();
			autogen::NotifyOnce();
		}
		if (a_msg && a_msg->type == F4SE::MessagingInterface::kGameDataReady) {
			spdlog::info("game data ready; keys: probe VK {:#x}, apply VK {:#x}, restore VK {:#x}", g_probeKey.load(), g_applyKey.load(), g_restoreKey.load());
			SelfTestTri();
			RefreshAutoSettings();
			autogen::EnsureLoaded();  // BodyGen's templates.ini / morphs.ini: read now, not in the middle of the first attach
		}
	}
}

extern "C" __declspec(dllexport) constinit auto F4SEPlugin_Version = []() noexcept {
	F4SE::PluginVersionData data{};
	data.PluginName(kName);
	data.PluginVersion(kVersion);
	data.AuthorName("Axel");
	data.UsesAddressLibrary(true);
	data.IsLayoutDependent(true);
	data.CompatibleVersions({ F4SE::RUNTIME_VR_1_2_72 });
	return data;
}();

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = kName;
	a_info->version = 0x00080000;
	if (a_f4se->IsEditor()) {
		return false;
	}
	// F4SE VR reports 1.10.138 (the flat build VR was forked from) instead of 1.2.72 here: accept both (HBCM lesson).
	const auto v = a_f4se->RuntimeVersion();
	return v == F4SE::RUNTIME_VR_1_2_72 || v == REL::Version(1, 10, 138);
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se, false);
	InitLog();  // after Init, or the log lands in My Games\F4SE instead of My Games\Fallout4VR\F4SE
	spdlog::info("BodyMorph VR {} (BodySlide presets as NPC bodies, BodyGen: templates.ini / morphs.ini engine, stored morphs per NPC, Papyrus natives, co-save; garment .tri via BODYTRI; F3/F4 store test, F7/F6 lab) loaded on runtime {}", kVersion.string(), REL::Module::get().version().string());
	LoadKeys();
	F4SE::AllocTrampoline(128);
	InstallAttachHook();
	F4SE::GetMessagingInterface()->RegisterListener(OnMessage);
	if (const auto* serialization = F4SE::GetSerializationInterface()) {
		serialization->SetUniqueID(kCoSaveId);
		serialization->SetRevertCallback(CoSaveRevert);
		serialization->SetSaveCallback(CoSaveSave);
		serialization->SetLoadCallback(CoSaveLoad);
	} else {
		spdlog::error("no F4SE serialization interface: stored morphs will not persist");
	}
	if (const auto* papyrus = F4SE::GetPapyrusInterface(); papyrus && PapyrusIdsPresent()) {
		if (!papyrus->Register(RegisterPapyrus)) {
			spdlog::error("[papyrus] F4SE refused the registration");
		}
	}
	std::thread(PollLoop).detach();
	return true;
}
