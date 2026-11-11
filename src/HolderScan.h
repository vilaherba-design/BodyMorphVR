// BodyMorph VR: finds every copy of some pointer values in the process's memory. Lab tool (INI [Lab] iScan): after a mesh swap it tells which other
// structures of the engine still hold the OLD mesh pointer, i.e. where the renderer can keep reading from. Copyright (C) 2026 Axel. GPL-3.0-or-later.
#pragma once

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace bodymorph
{
	struct HolderHit
	{
		std::uintptr_t addr = 0;   // address of the 8-byte slot that holds the value
		std::size_t    which = 0;  // index into the searched values
	};

	struct ScanResult
	{
		std::vector<HolderHit>   hits;    // at most a_maxPerValue per value
		std::vector<std::size_t> counts;  // every hit per value (the ones past the cap are only counted)
		std::uint64_t            bytes = 0;
		std::size_t              regions = 0;
	};

	using AddressRange = std::pair<std::uintptr_t, std::uintptr_t>;  // [first, second)

	inline bool IsScannableProtect(DWORD a_protect)
	{
		if (a_protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_NOCACHE | PAGE_WRITECOMBINE)) {
			return false;
		}
		return (a_protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
	}

	// Scans committed private and image pages, 8-byte aligned. Memory is read with ReadProcessMemory, so an unreadable page just fails the read.
	// a_skip: ranges whose hits are ignored (the caller's own copies of the values). The scan buffer itself is skipped automatically.
	inline ScanResult ScanForPointers(const std::vector<std::uintptr_t>& a_values, std::size_t a_maxPerValue, const std::vector<AddressRange>& a_skip)
	{
		ScanResult r;
		r.counts.assign(a_values.size(), 0);
		if (a_values.empty()) {
			return r;
		}
		constexpr std::size_t kChunk = std::size_t{ 4 } << 20;
		std::vector<std::uint64_t> buf(kChunk / 8);
		const auto                 bufLo = reinterpret_cast<std::uintptr_t>(buf.data());
		const auto                 bufHi = bufLo + kChunk;
		const auto                 minValue = *std::min_element(a_values.begin(), a_values.end());

		SYSTEM_INFO si{};
		::GetSystemInfo(&si);
		auto        addr = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
		const auto  maxAddr = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
		const HANDLE self = ::GetCurrentProcess();
		MEMORY_BASIC_INFORMATION mbi{};
		while (addr < maxAddr && ::VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
			const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			const auto size = static_cast<std::uintptr_t>(mbi.RegionSize);
			if (size == 0 || base + size <= addr) {
				break;
			}
			addr = base + size;
			if (mbi.State != MEM_COMMIT || (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_IMAGE) || !IsScannableProtect(mbi.Protect)) {
				continue;
			}
			++r.regions;
			for (std::uintptr_t p = base; p < base + size; p += kChunk) {
				const std::size_t n = static_cast<std::size_t>(std::min<std::uintptr_t>(kChunk, base + size - p));
				SIZE_T            got = 0;
				if (!::ReadProcessMemory(self, reinterpret_cast<LPCVOID>(p), buf.data(), n, &got) && got < 8) {
					continue;
				}
				r.bytes += got;
				const std::size_t words = static_cast<std::size_t>(got) / 8;
				for (std::size_t i = 0; i < words; ++i) {
					const std::uint64_t v = buf[i];
					if (v < minValue) {
						continue;
					}
					for (std::size_t k = 0; k < a_values.size(); ++k) {
						if (v != a_values[k]) {
							continue;
						}
						const std::uintptr_t at = p + i * 8;
						if (at >= bufLo && at < bufHi) {
							continue;
						}
						if (std::any_of(a_skip.begin(), a_skip.end(), [at](const AddressRange& s) { return at >= s.first && at < s.second; })) {
							continue;
						}
						++r.counts[k];
						if (r.counts[k] <= a_maxPerValue) {
							r.hits.push_back({ at, k });
						}
					}
				}
			}
		}
		return r;
	}
}
