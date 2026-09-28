# Makefile for include/src structure
CXX := g++
CXXFLAGS := -Wall -Wextra -std=c++17 -Iinclude 
LDFLAGS := -lboost_program_options -lz

# Debug flags
DEBUG_CXXFLAGS := -g -O0 -DDEBUG
ASAN_CXXFLAGS := -g -O1 -fsanitize=address -fno-omit-frame-pointer
ASAN_LDFLAGS := -fsanitize=address

# Directories
SRCDIR := src
INCDIR := include
OBJDIR := obj
DEBUGDIR := debug_obj

# Target names
TARGET := main
DEBUG_TARGET := main_debug
ASAN_TARGET := main_asan

# Find all .cpp files in src directory
SOURCES := $(wildcard $(SRCDIR)/*.cpp)

# Create object file names for different builds
OBJECTS := $(SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
DEBUG_OBJECTS := $(SOURCES:$(SRCDIR)/%.cpp=$(DEBUGDIR)/%.o)

# Default target
all: $(OBJDIR) $(TARGET)

# Debug target
debug: $(DEBUGDIR) $(DEBUG_TARGET)


# Create directories if they don't exist
$(OBJDIR):
	mkdir -p $(OBJDIR)

$(DEBUGDIR):
	mkdir -p $(DEBUGDIR)


# Link the target executables
$(TARGET): $(OBJECTS)
	$(CXX) -o $@ $(OBJECTS) $(LDFLAGS)

$(DEBUG_TARGET): $(DEBUG_OBJECTS)
	$(CXX) -o $@ $(DEBUG_OBJECTS) $(LDFLAGS)

# Compile source files to object files - Release
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile source files to object files - Debug
$(DEBUGDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(DEBUG_CXXFLAGS) -c $< -o $@

# Clean rule
clean:
	rm -rf $(OBJDIR) $(DEBUGDIR) $(TARGET) $(DEBUG_TARGET)

# Phony targets
.PHONY: all debug clean