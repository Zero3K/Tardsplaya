# Makefile for Tardsplaya Pipeline Integration
# Alternative build system for the Pipeline library integration

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pedantic -O2
INCLUDES = -I. -Ipipeline/include
LIBS = -lpthread

# Directories
PIPELINE_SRC_DIR = pipeline/src
PIPELINE_INC_DIR = pipeline/include
BUILD_DIR = build
BIN_DIR = bin

# Source files
PIPELINE_SOURCES = $(wildcard $(PIPELINE_SRC_DIR)/*.cpp)
SIMPLE_EXAMPLE_SOURCE = simple_pipeline_example.cpp

# Object files
PIPELINE_OBJECTS = $(PIPELINE_SOURCES:$(PIPELINE_SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
SIMPLE_EXAMPLE_OBJECT = $(BUILD_DIR)/simple_pipeline_example.o

# Targets
PIPELINE_LIB = $(BUILD_DIR)/libpipeline.a
SIMPLE_EXAMPLE_EXEC = $(BIN_DIR)/simple_pipeline_example

.PHONY: all clean test install directories help

all: directories $(PIPELINE_LIB) $(SIMPLE_EXAMPLE_EXEC)

# Create necessary directories
directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)

# Build Pipeline library
$(PIPELINE_LIB): $(PIPELINE_OBJECTS)
	@echo "Creating Pipeline library..."
	ar rcs $@ $^
	@echo "Pipeline library created: $@"

# Build example executable
$(SIMPLE_EXAMPLE_EXEC): $(SIMPLE_EXAMPLE_OBJECT) $(PIPELINE_LIB)
	@echo "Linking simple example executable..."
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)
	@echo "Simple example executable created: $@"

# Compile Pipeline source files
$(BUILD_DIR)/%.o: $(PIPELINE_SRC_DIR)/%.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Compile Tardsplaya integration source files
$(BUILD_DIR)/%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Run the example
test: $(SIMPLE_EXAMPLE_EXEC)
	@echo "Running Pipeline example..."
	./$(SIMPLE_EXAMPLE_EXEC)

# Install (simplified - copies to /usr/local)
install: all
	@echo "Installing Pipeline library and headers..."
	sudo cp $(PIPELINE_LIB) /usr/local/lib/
	sudo mkdir -p /usr/local/include/pipeline
	sudo cp $(PIPELINE_INC_DIR)/pipeline/*.h /usr/local/include/pipeline/
	sudo mkdir -p /usr/local/include/tardsplaya
	sudo cp pipeline_*.h /usr/local/include/tardsplaya/
	sudo cp $(EXAMPLE_EXEC) /usr/local/bin/
	@echo "Installation completed."

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR)
	rm -rf $(BIN_DIR)
	@echo "Clean completed."

# Build with debug information
debug: CXXFLAGS += -g -DDEBUG
debug: all

# Build with release optimizations
release: CXXFLAGS += -O3 -DNDEBUG
release: all

# Check code with static analysis (if cppcheck is available)
check:
	@echo "Running static code analysis..."
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all --std=c++17 $(INCLUDES) $(PIPELINE_SRC_DIR) *.cpp *.h; \
	else \
		echo "cppcheck not found, skipping static analysis"; \
	fi

# Format code (if clang-format is available)
format:
	@echo "Formatting code..."
	@if command -v clang-format >/dev/null 2>&1; then \
		find . -name "*.cpp" -o -name "*.h" | grep -E "(pipeline|tardsplaya)" | xargs clang-format -i; \
		echo "Code formatting completed."; \
	else \
		echo "clang-format not found, skipping code formatting"; \
	fi

# Show help
help:
	@echo "Tardsplaya Pipeline Integration Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all      - Build everything (default)"
	@echo "  test     - Build and run the example"
	@echo "  install  - Install library and headers to /usr/local"
	@echo "  clean    - Remove all build artifacts"
	@echo "  debug    - Build with debug information"
	@echo "  release  - Build with release optimizations"
	@echo "  check    - Run static code analysis (requires cppcheck)"
	@echo "  format   - Format code (requires clang-format)"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Example usage:"
	@echo "  make all     # Build everything"
	@echo "  make test    # Build and run example"
	@echo "  make clean   # Clean build"

# Dependencies (automatic dependency generation)
-include $(PIPELINE_OBJECTS:.o=.d)
-include $(TARDSPLAYA_OBJECTS:.o=.d)
-include $(EXAMPLE_OBJECT:.o=.d)

$(BUILD_DIR)/%.d: $(PIPELINE_SRC_DIR)/%.cpp
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MM -MT $(@:.d=.o) $< > $@

$(BUILD_DIR)/%.d: %.cpp
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MM -MT $(@:.d=.o) $< > $@