#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

// Windows.h AFTER CommonLibSSE-NG headers (avoids REX macro clashes) — global rule 4-3.
#include <Windows.h>

#include <spdlog/sinks/basic_file_sink.h>

using namespace std::literals;

namespace logger = SKSE::log;
