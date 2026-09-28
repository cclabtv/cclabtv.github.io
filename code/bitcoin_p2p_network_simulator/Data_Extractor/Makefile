# Makefile
# Compiler and flags
CXX := g++
CXXFLAGS := -Wall -Wextra -std=c++17 -I include/ -I /usr/include/eigen3
LDFLAGS := -lz

# Shared sources (no main function)
SHARED_SRCS := src/data_processor.cpp src/file_manager.cpp src/graph.cpp src/parsing_utils.cpp
SHARED_OBJS := $(SHARED_SRCS:.cpp=.o)

# Data extractor executable
MAIN_TARGET := main
MAIN_OBJ    := src/main.o

# Eigenvalue calculator executable (reads from snapshots/ folder)
EIGEN_TARGET := compute_eigenvalues
EIGEN_OBJ    := src/eigen_main.o

.PHONY: all clean
all: $(MAIN_TARGET) $(EIGEN_TARGET)

$(MAIN_TARGET): $(SHARED_OBJS) $(MAIN_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(EIGEN_TARGET): $(EIGEN_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Rule for building object files from .cpp files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Optional: clean rule to remove compiled files
clean:
	rm -f $(SHARED_OBJS) $(MAIN_OBJ) $(EIGEN_OBJ) $(MAIN_TARGET) $(EIGEN_TARGET)

# Export number of jobs for parallel execution
MAKEFLAGS += -j$(shell nproc)
