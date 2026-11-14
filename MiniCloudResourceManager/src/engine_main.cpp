// engine_main.cpp - the mcrm_engine executable: ONE JSON request on stdin -> ONE JSON response on stdout.
//
//   echo {"command":"info"} | mcrm_engine
//   mcrm_engine --version
//
// Exit codes: 0 = a response was written (it may still say "ok": false); 2 = could not read input.
// Nothing else is ever written to stdout, so the caller can parse it directly.
#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>

#include "engine_api.h"

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--version") {
        std::cout << "mcrm_engine protocol " << kEngineProtocolVersion << "\n";
        return 0;
    }
    if (argc > 1) {
        std::cerr << "usage: mcrm_engine   (reads one JSON request from stdin)\n";
        return 2;
    }
    std::ios::sync_with_stdio(false);
    constexpr std::size_t kMaxInput = 16u * 1024u * 1024u;  // 16 MB is far more than any realistic request
    std::string input;
    char buffer[65536];
    std::size_t got;
    while ((got = std::fread(buffer, 1, sizeof buffer, stdin)) > 0) {
        input.append(buffer, got);
        if (input.size() > kMaxInput) {
            std::cerr << "request too large\n";
            return 2;
        }
    }
    std::cout << handleRequestText(input) << "\n";
    return 0;
}
