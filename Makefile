.PHONY: all configure build test check

all: build

configure:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

build: configure
	cmake --build build --parallel

test: build
	ctest --test-dir build --output-on-failure

check:
	./scripts/check.sh

