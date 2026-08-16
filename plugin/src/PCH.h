#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

// Windows.h AFTER CommonLibSSE-NG headers (avoids REX macro clashes) — global rule 4-3.
#include <Windows.h>

// ★★wingdi.h defines GetObject as GetObjectW/GetObjectA, which silently
// renames any member of that name -- RE::BGSDefaultObjectManager::GetObject
// comes back as "'GetObjectA' is not a member of". Parentheses do not help
// (it is an OBJECT-like macro), and the error names a symbol that appears
// nowhere in our source, so it reads as a CommonLibSSE fault rather than a
// macro one. Dropped here, once, beside the include that causes it: nothing
// in this plugin calls the GDI function.
#ifdef GetObject
#    undef GetObject
#endif

#include <spdlog/sinks/basic_file_sink.h>

using namespace std::literals;

namespace logger = SKSE::log;
