CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -fsanitize=address,undefined
DEFINES  := -DW2F_DATA_DIR='"$(CURDIR)/data"' -DW2F_TEST_DATA_DIR='"$(CURDIR)/tests/data"'

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

.PHONY: all test test-engine test-net check-isolation demo server clean
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

test-net: $(NET_TEST)
	./$(NET_TEST)

test: test-engine test-net

server: $(SERVER)

# Headless 8-player match (7 bots + 1 scripted seat): `make demo`, `make demo SEED=7`, or with another data file:
# `make demo SEED=7 DATA=path/to/champions.json`
SEED ?= 2024
DATA ?= $(CURDIR)/data/champions.json
TRAITS ?= $(CURDIR)/data/traits.json
demo: $(DEMO)
	./$(DEMO) $(SEED) $(DATA) $(TRAITS)

clean:
	rm -rf build
