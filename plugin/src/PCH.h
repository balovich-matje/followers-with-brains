#pragma once
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace logger = SKSE::log;
using namespace std::literals;

#include <algorithm>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <map>
#include <tuple>
#include <atomic>
