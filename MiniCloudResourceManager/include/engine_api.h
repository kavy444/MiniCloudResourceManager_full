// engine_api.h - JSON request/response layer around the C++ engine (used by the mcrm_engine executable).
//
// One request in, one response out. Every response is an object with "ok" (bool). On failure "errors" is a
// list of readable messages; the engine never crashes on bad input.
//
// Commands ("command" field):
//   info      -> version, algorithm names, limits
//   schedule  -> run ONE CPU-scheduling algorithm   {algorithm, processes[], config{}}
//   compare   -> run SEVERAL algorithms on the same processes and rank them per metric
//                {algorithms[], processes[], config{}}
//   rank      -> rank already-computed metrics       {items:[{label, metrics{}}]}
//   place     -> least-loaded server choice for ONE job (explains every server)
//                {servers[{..., used_cpu, used_ram_gb, used_storage_gb}], job{}}
//   memory    -> First/Best/Worst Fit simulation     {strategy, total_size | partitions[], operations[]}
//   banker    -> safety / request / detect           {action, ...}
//   sync      -> race-condition and producer/consumer demos {scenario, ...}
//
// See docs/ENGINE_PROTOCOL.md for field-by-field details.
#pragma once

#include <string>

#include "json.h"

// Version of the JSON protocol (bump when fields change incompatibly).
constexpr const char* kEngineProtocolVersion = "1.0";

Json handleRequest(const Json& request);

// Parses `text`, handles it and returns the response as a JSON string. Malformed JSON gives
// {"ok":false,"errors":[...]}.
std::string handleRequestText(const std::string& text);
