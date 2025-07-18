# Makefile for Tardsplaya with TSDuck Transport Router
# Cross-compilation for Windows using MinGW

CXX = x86_64-w64-mingw32-g++
WINDRES = x86_64-w64-mingw32-windres
CXXFLAGS = -std=c++17 -Wall -O2 -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601
LDFLAGS = -static-libgcc -static-libstdc++ -mwindows
LIBS = -lwinhttp -lcomctl32 -lwininet -lpsapi -lws2_32

# Source files
SOURCES = Tardsplaya.cpp \
          favorites.cpp \
          playlist_parser.cpp \
          stream_memory_map.cpp \
          stream_pipe.cpp \
          stream_resource_manager.cpp \
          stream_thread.cpp \
          tlsclient/tlsclient.cpp \
          tlsclient/tlsclient_source.cpp \
          tsduck_hls_wrapper.cpp \
          tsduck_transport_router.cpp \
          twitch_api.cpp \
          urlencode.cpp

# Object files
OBJECTS = $(SOURCES:.cpp=.o)

# Resource file
RESOURCE_RC = resource.rc
RESOURCE_O = resource.o

# Target executable
TARGET = Tardsplaya.exe

# Default target
all: $(TARGET)

# Build executable
$(TARGET): $(OBJECTS) $(RESOURCE_O)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Build completed: $(TARGET)"

# Compile C++ source files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile resource file
$(RESOURCE_O): $(RESOURCE_RC)
	$(WINDRES) $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(RESOURCE_O) $(TARGET)
	@echo "Clean completed"

# Test build only (syntax check)
test-compile:
	$(CXX) $(CXXFLAGS) -fsyntax-only $(SOURCES)
	@echo "Syntax check completed successfully"

# Create DLL version of transport router
tsduck_transport.dll: tsduck_transport_router.cpp tsduck_transport_router.h tsduck_hls_wrapper.cpp tsduck_hls_wrapper.h
	$(CXX) $(CXXFLAGS) -shared -DBUILD_DLL -o $@ \
		tsduck_transport_router.cpp tsduck_hls_wrapper.cpp \
		-lwinhttp -lws2_32
	@echo "DLL build completed: $@"

.PHONY: all clean test-compile