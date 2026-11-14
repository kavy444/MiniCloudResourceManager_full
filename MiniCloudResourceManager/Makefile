# GNU make + g++ (Linux, macOS, or MSYS2/MinGW on Windows).  On plain Windows use build_windows.bat instead.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pedantic -O2 -Iinclude
LDFLAGS  ?= -pthread
EXE      := $(if $(findstring Windows,$(OS)),.exe,)

# milestone-1 console simulator
SIM_SRC    := src/job.cpp src/server.cpp src/resource_manager.cpp src/scheduler.cpp src/simulation.cpp src/workload.cpp src/report.cpp
# engine (everything except the two main() files)
ENGINE_SRC := $(filter-out src/main.cpp src/engine_main.cpp,$(wildcard src/*.cpp))

all: build/mcrm$(EXE) build/mcrm_engine$(EXE) build/tests$(EXE) build/engine_tests$(EXE)

build:
	mkdir -p build

build/mcrm$(EXE): $(SIM_SRC) src/main.cpp | build
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# JSON-in / JSON-out program the Python backend starts (see docs/ENGINE_PROTOCOL.md)
build/mcrm_engine$(EXE): $(ENGINE_SRC) src/engine_main.cpp | build
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

build/tests$(EXE): $(SIM_SRC) tests/test_main.cpp | build
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

build/engine_tests$(EXE): $(ENGINE_SRC) tests/engine/test_cpu_scheduler.cpp tests/engine/test_modules.cpp tests/engine/test_runner.cpp | build
	$(CXX) $(CXXFLAGS) -Itests/engine $^ -o $@ $(LDFLAGS)

test: build/tests$(EXE) build/engine_tests$(EXE) build/mcrm_engine$(EXE)
	./build/tests$(EXE)
	./build/engine_tests$(EXE)
	cd backend && python3 -m unittest discover -s tests -t .

demo: build/mcrm$(EXE)
	./build/mcrm$(EXE) --demo

clean:
	rm -rf build
