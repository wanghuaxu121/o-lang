# O 语言 · 构建与测试入口
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -w
BIN      := olang
SRC      := olang.cpp

GIT     := $(shell git describe --tags --always --dirty 2>/dev/null)
VERSION := 1.1.0-dev$(if $(GIT),+$(GIT))

.PHONY: all build test fast audit coverage check clean version demo

all: build

build: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) -DOLANG_VERSION='"$(VERSION)"' -o $@ $<

test: $(BIN)
	./run_tests.sh --no-build

fast: $(BIN)
	./run_tests.sh --fast --no-build

audit: $(BIN)
	./tools/audit_spec15.sh ./$(BIN)

coverage:
	./tools/coverage.py

check: fast audit coverage

version: $(BIN)
	./$(BIN) --version

demo: $(BIN)
	./tools/demo.sh

clean:
	rm -f $(BIN) examples/*.gen.exe tests/*/*.gen.exe
