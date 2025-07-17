# Makefile for MailSlot vs Pipe Comparison Test
# This is for cross-platform development, but MailSlots are Windows-only

# Windows compilation (using MinGW or Visual Studio)
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra
LDFLAGS = -static-libgcc -static-libstdc++

# Windows-specific libraries
LIBS = -lwinhttp -lcomctl32 -lpsapi

# Source files for the comparison test
MAILSLOT_SOURCES = mailslot_comparison.cpp mailslot_test.cpp
MAILSLOT_HEADERS = mailslot_comparison.h

# Mock implementation for non-Windows platforms
ifeq ($(OS),Windows_NT)
    # Windows build
    PLATFORM_SOURCES = $(MAILSLOT_SOURCES)
    TARGET = mailslot_test.exe
else
    # Mock build for development on non-Windows
    PLATFORM_SOURCES = mailslot_test_mock.cpp
    TARGET = mailslot_test_mock
    LIBS = 
endif

all: $(TARGET)

$(TARGET): $(PLATFORM_SOURCES) $(MAILSLOT_HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(PLATFORM_SOURCES) $(LIBS)

# Mock implementation for non-Windows platforms
mailslot_test_mock.cpp:
	@echo "Creating mock implementation for non-Windows development..."
	@echo '#include <iostream>' > $@
	@echo 'int main() {' >> $@
	@echo '    std::cout << "MailSlot test requires Windows platform" << std::endl;' >> $@
	@echo '    std::cout << "This is a mock for development purposes only" << std::endl;' >> $@
	@echo '    return 0;' >> $@
	@echo '}' >> $@

clean:
	rm -f $(TARGET) mailslot_test_mock.cpp mailslot_vs_pipe_analysis.txt

test: $(TARGET)
ifeq ($(OS),Windows_NT)
	./$(TARGET)
else
	@echo "MailSlot tests can only run on Windows"
	@echo "Run 'make' to build mock version for development"
endif

.PHONY: all clean test

# Help target
help:
	@echo "MailSlot vs Pipe IPC Comparison Test"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build the test application"
	@echo "  test    - Run the comparison test (Windows only)"
	@echo "  clean   - Remove built files"
	@echo "  help    - Show this help"
	@echo ""
	@echo "Note: MailSlots are Windows-only. On other platforms, a mock is built."