# Makefile — builds the C++ port of spirit.py with a plain `make`.
#
#   make            build ./spirit
#   make test       build, then run the differential test against spirit.py
#   make clean      remove build artefacts

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

# spirit.py lives one directory up; override with PYSPIRIT=... if needed.
PYSPIRIT ?= ../spirit.py
PYTHON   ?= python3

all: spirit

spirit: spirit.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test: spirit
	PYSPIRIT="$(PYSPIRIT)" PYTHON="$(PYTHON)" ./run_tests.sh

clean:
	rm -f spirit

.PHONY: all test clean
