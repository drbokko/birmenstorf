# Build simple_acquisition_with_stream2 (C++ sources in ./cpp).
# Usage: make && ./simple_acquisition_with_stream2

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
CPPFLAGS += -Icpp

PKG_CONFIG ?= pkg-config
SOURCES = cpp/simple_acquisition_with_stream2.cpp cpp/eiger_client.cpp
TARGET  = simple_acquisition_with_stream2

ifeq ($(shell $(PKG_CONFIG) --exists libcurl 2>/dev/null && echo yes),yes)
  CPPFLAGS += -DUSE_LIBCURL=1
  LDLIBS   += $(shell $(PKG_CONFIG) --libs libcurl)
else
  $(warning libcurl not found (pkg-config). Binary builds but HTTP calls will fail until you install libcurl and rebuild.)
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

clean:
	$(RM) $(TARGET)
