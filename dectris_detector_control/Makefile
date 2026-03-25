# C++ demos (shared eiger_client.cpp).
# Usage: make
#
#   ./connect_and_configure_and_arm_detector [HOST]
#   ./software_trigger_detector [-n N] [HOST]
#   ./wait_idle_and_disarm_detector [HOST]

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
CPPFLAGS += -Icpp

PKG_CONFIG ?= pkg-config
EIGER_SRC = cpp/eiger_client.cpp

TARGETS = connect_and_configure_and_arm_detector \
          software_trigger_detector \
          wait_idle_and_disarm_detector

ifeq ($(shell $(PKG_CONFIG) --exists libcurl 2>/dev/null && echo yes),yes)
  CPPFLAGS += -DUSE_LIBCURL=1
  LDLIBS   += $(shell $(PKG_CONFIG) --libs libcurl)
else
  $(warning libcurl not found (pkg-config). Binaries link but HTTP calls fail until libcurl is installed.)
endif

.PHONY: all clean

all: $(TARGETS)

connect_and_configure_and_arm_detector: cpp/connect_and_configure_and_arm_detector.cpp $(EIGER_SRC)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

software_trigger_detector: cpp/software_trigger_detector.cpp $(EIGER_SRC)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

wait_idle_and_disarm_detector: cpp/wait_idle_and_disarm_detector.cpp $(EIGER_SRC)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

clean:
	$(RM) $(TARGETS)
