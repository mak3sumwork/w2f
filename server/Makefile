CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -fsanitize=address,undefined
DEFINES  := -DW2F_DATA_DIR='"$(CURDIR)/data"' -DW2F_TEST_DATA_DIR='"$(CURDIR)/tests/data"' -DW2F_SCHEMA_DIR='"$(CURDIR)/docs/schemas"' -DW2F_GOLDEN_DIR='"$(CURDIR)/tests/golden"'

# Two libraries, kept apart on purpose:
#   engine  = src/ + include/w2f/          the game rules. Knows nothing about networking; compiled WITHOUT net's include path.
#   net     = net/src/ + net/include/w2f/net/   sockets, WebSocket, lobby, protocol. Uses only the engine's public headers.
ENGINE_INC := -Iinclude
NET_INC    := -Iinclude -Inet/include
TEST_INC   := -Itests/support

ENGINE_SRC := $(wildcard src/*.cpp)
ENGINE_OBJ := $(patsubst src/%.cpp,build/engine/%.o,$(ENGINE_SRC))
NET_SRC    := $(wildcard net/src/*.cpp)
NET_OBJ    := $(patsubst net/src/%.cpp,build/net/%.o,$(NET_SRC))
HDR        := $(wildcard include/w2f/*.h) $(wildcard net/include/w2f/net/*.h) $(wildcard tests/support/*.h)

TEST     := build/w2f_tests
NET_TEST := build/w2f_net_tests
DEMO     := build/w2f_demo
SERVER   := build/w2f_server
BALANCE  := build/w2f_balance
RECORD   := build/w2f_record_fights

.PHONY: all test test-engine test-net check-isolation demo server balance golden sample-fights blockouts crafted clean
all: $(TEST) $(NET_TEST) $(DEMO) $(SERVER)

build/engine/%.o: src/%.cpp $(HDR)
	@mkdir -p build/engine
	$(CXX) $(CXXFLAGS) $(ENGINE_INC) -c $< -o $@

build/net/%.o: net/src/%.cpp $(HDR)
	@mkdir -p build/net
	$(CXX) $(CXXFLAGS) $(NET_INC) -c $< -o $@

# The engine's own tests link ONLY the engine objects: if the engine ever needed the network layer, this would stop linking.
$(TEST): $(ENGINE_OBJ) tests/tests.cpp $(HDR)
	$(CXX) $(CXXFLAGS) $(ENGINE_INC) $(TEST_INC) $(DEFINES) tests/tests.cpp $(ENGINE_OBJ) -o $@

$(NET_TEST): $(ENGINE_OBJ) $(NET_OBJ) net/tests/net_tests.cpp $(HDR)
	$(CXX) $(CXXFLAGS) -pthread $(NET_INC) $(TEST_INC) $(DEFINES) net/tests/net_tests.cpp $(NET_OBJ) $(ENGINE_OBJ) -o $@

$(DEMO): $(ENGINE_OBJ) tools/sim_demo.cpp $(HDR)
	$(CXX) $(CXXFLAGS) $(ENGINE_INC) $(TEST_INC) $(DEFINES) tools/sim_demo.cpp $(ENGINE_OBJ) -o $@

$(SERVER): $(ENGINE_OBJ) $(NET_OBJ) tools/w2f_server.cpp $(HDR)
	$(CXX) $(CXXFLAGS) $(NET_INC) $(DEFINES) tools/w2f_server.cpp $(NET_OBJ) $(ENGINE_OBJ) -o $@

# The engine must stay free of anything network-related: no socket headers, no reference to the net layer.
check-isolation:
	@if grep -rEn 'sys/socket|netinet|arpa/inet|winsock|WinSock|<poll\.h>|w2f/net/|w2f::net' src include/w2f; then \
	  echo "ISOLATION VIOLATION: the engine (src/, include/w2f/) refers to networking"; exit 1; \
	else echo "isolation ok: src/ and include/w2f/ contain no networking"; fi

test-engine: check-isolation $(TEST)
	./$(TEST)

# Re-record the golden fights (tests/golden/fights.golden) after an INTENDED change to the combat rules or the champion / trait / item data.
golden: $(TEST)
	./$(TEST) --update-golden

test-net: $(NET_TEST)
	./$(NET_TEST)

test: test-engine test-net

server: $(SERVER)

# Re-record the sample combat logs for the UE5 developers (docs/sample_fights/): real `combat` messages + the catalog.
$(RECORD): $(ENGINE_OBJ) $(NET_OBJ) tools/record_fights.cpp $(HDR)
	$(CXX) $(CXXFLAGS) $(NET_INC) $(DEFINES) tools/record_fights.cpp $(NET_OBJ) $(ENGINE_OBJ) -o $@
# Placeholder 3D models (glTF) for every champion / monster, from data/ (standard-library Python; verifies its own output). docs/blockouts/README.md
blockouts:
	python3 tools/make_blockouts.py

# The crafted (Blender) models: needs a Python 3.11 venv with the bpy module at ~/w2f_bpy/venv (docs/models/README.md)
crafted:
	$(HOME)/w2f_bpy/venv/bin/python tools/blender/alesk.py

sample-fights: $(RECORD)
	@mkdir -p docs/sample_fights
	./$(RECORD) docs/sample_fights

# The bulk match simulator behind the balance passes (docs/balance.md). Built optimised and WITHOUT sanitizers: it plays hundreds of matches.
#   make balance ARGS="--matches 200 --seed 1000 --json report.json"
BALANCE_FLAGS ?= -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow -Wconversion
$(BALANCE): $(ENGINE_SRC) tools/balance_sim.cpp $(HDR)
	@mkdir -p build
	$(CXX) $(BALANCE_FLAGS) $(ENGINE_INC) $(DEFINES) tools/balance_sim.cpp $(ENGINE_SRC) -o $@
balance: $(BALANCE)
	./$(BALANCE) $(ARGS)

# Headless 8-player match (7 bots + 1 scripted seat): `make demo`, `make demo SEED=7`, or with another data file:
# `make demo SEED=7 DATA=path/to/champions.json`
SEED ?= 2024
DATA ?= $(CURDIR)/data/champions.json
TRAITS ?= $(CURDIR)/data/traits.json
demo: $(DEMO)
	./$(DEMO) $(SEED) $(DATA) $(TRAITS)

clean:
	rm -rf build
