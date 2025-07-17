# Makefile for MailSlot vs Pipe Comparison Test and Alternative IPC Demo
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

# Source files for alternative IPC demo
ALTERNATIVE_IPC_SOURCES = alternative_ipc_demo.cpp alternative_ipc_test.cpp
ALTERNATIVE_IPC_HEADERS = alternative_ipc_demo.h

# Mock implementation for non-Windows platforms
ifeq ($(OS),Windows_NT)
    # Windows build
    PLATFORM_SOURCES = $(MAILSLOT_SOURCES)
    ALT_IPC_PLATFORM_SOURCES = $(ALTERNATIVE_IPC_SOURCES)
    TARGET = mailslot_test.exe
    ALT_IPC_TARGET = alternative_ipc_test.exe
else
    # Mock build for development on non-Windows
    PLATFORM_SOURCES = mailslot_test_mock.cpp
    ALT_IPC_PLATFORM_SOURCES = alternative_ipc_test_mock.cpp
    TARGET = mailslot_test_mock
    ALT_IPC_TARGET = alternative_ipc_test_mock
    LIBS = 
endif

all: $(TARGET) $(ALT_IPC_TARGET)

$(TARGET): $(PLATFORM_SOURCES) $(MAILSLOT_HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(PLATFORM_SOURCES) $(LIBS)

$(ALT_IPC_TARGET): $(ALT_IPC_PLATFORM_SOURCES) $(ALTERNATIVE_IPC_HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(ALT_IPC_PLATFORM_SOURCES) $(LIBS)

# Mock implementation for non-Windows platforms
mailslot_test_mock.cpp:
	@echo "Creating mock implementation for non-Windows development..."
	@echo '#include <iostream>' > $@
	@echo 'int main() {' >> $@
	@echo '    std::cout << "MailSlot test requires Windows platform" << std::endl;' >> $@
	@echo '    std::cout << "This is a mock for development purposes only" << std::endl;' >> $@
	@echo '    return 0;' >> $@
	@echo '}' >> $@

alternative_ipc_test_mock.cpp:
	@echo "Creating mock implementation for alternative IPC test..."
	@echo '#include <iostream>' > $@
	@echo 'int main() {' >> $@
	@echo '    std::cout << "Alternative IPC test requires Windows platform" << std::endl;' >> $@
	@echo '    std::cout << "MailSlots and Named Pipes are Windows-only features" << std::endl;' >> $@
	@echo '    return 0;' >> $@
	@echo '}' >> $@

clean:
	rm -f $(TARGET) $(ALT_IPC_TARGET) mailslot_test_mock.cpp alternative_ipc_test_mock.cpp mailslot_vs_pipe_analysis.txt

test: $(TARGET)
ifeq ($(OS),Windows_NT)
	./$(TARGET)
else
	@echo "MailSlot tests can only run on Windows"
	@echo "Run 'make' to build mock version for development"
endif

test-alternative: $(ALT_IPC_TARGET)
ifeq ($(OS),Windows_NT)
	./$(ALT_IPC_TARGET)
else
	@echo "Alternative IPC tests can only run on Windows"
	@echo "Run 'make' to build mock version for development"
endif

.PHONY: all clean test test-alternative help

# Help target
help:
	@echo "MailSlot vs Pipe IPC Comparison Test and Alternative IPC Demo"
	@echo ""
	@echo "Targets:"
	@echo "  all               - Build all test applications"
	@echo "  test              - Run the MailSlot vs Pipe comparison test (Windows only)"
	@echo "  test-alternative  - Run the alternative IPC methods demo (Windows only)"
	@echo "  clean             - Remove built files"
	@echo "  help              - Show this help"
	@echo ""
	@echo "Note: MailSlots and Named Pipes are Windows-only. On other platforms, mocks are built."