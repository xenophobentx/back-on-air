// arkscan.h: finds the D3D9 shader programs in the game's own .ark archives (pipewarm.cpp's "pre-compile all
// tracks"). Portable C++ without Windows headers, so test/arkscantest.cpp runs on the build machine.
//
// PC archive (tools/dlc/ark_pc.py has the full description): a 0x18-byte header (u32 0x00010000, u32 0x10 + 20*count,
// u32 count, u32 0x10, u32 0, u32 = the second field), then count entries of 20 bytes {u32 flags (0 stored, 1 LZSS),
// u32 offset, u32 stored size, u32 size, u32 name hash}. A flags-1 member is a u32 size, then an Okumura LZSS stream
// (N 4096, F 18, threshold 2, ring start 0xFEE, zero fill, 8 tokens per flag byte LSB first, 1 = literal).
// Shader members (.fxc) begin with "CXF\0" (or "\0FXC"); they hold the D3D9 byte code of the game's shader combos.
// The programs inside are found by their version token (vs/ps 2.x / 3.0) and a walk over their tokens to the end
// token, so the container's own layout does not matter.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace arkscan {

// Reads n bytes at off; false on any failure.
using ReadFn = std::function<bool(uint64_t off, void* dst, size_t n)>;

struct Stats {
  uint32_t members = 0;   // archive members seen
  uint32_t fxc = 0;       // shader members among them
  uint64_t fxc_bytes = 0; // their unpacked size
  uint32_t bad = 0;       // members that could not be read or unpacked
};

// Unpacks an LZSS stream (without the u32 size prefix) into dst; returns the bytes written (at most cap).
size_t Lzss(const uint8_t* src, size_t n, uint8_t* dst, size_t cap);

// Calls on_fxc(content, size) for every shader member of the archive. False when the file is not a PC archive.
// A member larger than max_member bytes is skipped (counted as bad).
bool ScanArchive(const ReadFn& read, uint64_t file_size, const std::function<void(const uint8_t*, size_t)>& on_fxc,
                 Stats* stats, uint32_t max_member = 64u << 20);

// Calls on_prog(code, dwords, vs) for every vs/ps 2.x or 3.0 program in p[0..n), and a second time with the
// program's comment blocks (constant table, ...) left out when it has any: a game may hand either to the device.
void ScanPrograms(const uint8_t* p, size_t n, const std::function<void(const uint32_t*, size_t, bool)>& on_prog);

// The program hash pipewarm.cpp's records use: 64-bit FNV-1a of the byte code, seeded per stage.
uint64_t ProgramHash(const void* code, size_t bytes, bool vs);

}  // namespace arkscan
