// shadercache.cpp: the patch's HLSL compiled off the render thread, and the byte code kept on disk.
//
// postfx.cpp and hdr.cpp hand a job (a list of sources) to ss_shader_submit; one worker thread compiles it with
// d3dx9_42's D3DXCompileShader (no device needed), or reads the byte code from <the DLL's folder>\shader-cache\.
// The render thread polls `state` each frame and, once it is done, makes the shader objects from `code` there
// (CreateVertexShader / CreatePixelShader: fast). Until then the previous mode (or the plain frame) stays on screen.
#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SsShaderJob {
  struct Item {
    std::string src, entry, profile;
    uint32_t flags = 0;
    std::vector<uint32_t> code;  // the byte code when done; empty: it did not compile (logged)
  };
  const char* tag = "";  // the log tag, e.g. "[postfx]"
  std::string what;      // what the job is for, for the log
  std::vector<Item> items;
  volatile LONG state = 0;      // 0 waiting or running, 1 done (read items only after that)
  volatile LONG cancelled = 0;  // the render thread no longer wants it: skipped if not started
  DWORD submitted = 0;          // GetTickCount at submit
  DWORD worker_ms = 0;          // time on the worker
  int hits = 0, compiled = 0, failed = 0;
  bool Done() const { return state != 0; }
  void Add(std::string src, const char* entry, const char* profile, uint32_t flags) {
    items.push_back(Item{std::move(src), entry, profile, flags, {}});
  }
};

// Queued for the worker (started on first use). Returns the job.
std::shared_ptr<SsShaderJob> ss_shader_submit(std::shared_ptr<SsShaderJob> job);
