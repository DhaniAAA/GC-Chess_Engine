#
# 'make'        build executable file 'main'
# 'make clean'  removes all .o and executable files
#

# define the Cpp compiler to use
CXX = g++

# Mode Release (Default)
# CXXFLAGS := -std=c++17 -Wall -Wextra -O3 -march=native -DNDEBUG
# LFLAGS =

# define any compile-time flags
# Mode Release (Optimized for Speed - Bullet Chess)
# -O3: Maximum optimization
# -march=native: Use all CPU features available
# -flto: Link-time optimization for cross-unit inlining
# -funroll-loops: Unroll loops for better performance
CXXFLAGS := -std=c++17 -Wall -Wextra -O3 -march=native -flto -funroll-loops -DNDEBUG

# Link-time optimization flags (must match CXXFLAGS)
# -static: Static linking to avoid DLL dependencies (libgcc, libstdc++, etc)
# -flto: Enable LTO at link stage (MUST match CXXFLAGS)
LFLAGS = -static -flto

# define output directory
OUTPUT	:= output

# define source directory
SRC		:= src

# define include directory
INCLUDE	:= include

# define lib directory
LIB		:= lib

ifeq ($(OS),Windows_NT)
MAIN	:= main.exe
SOURCEDIRS	:= $(SRC)
INCLUDEDIRS	:= $(INCLUDE)
LIBDIRS		:= $(LIB)
FIXPATH = $(subst /,\,$1)
RM			:= del /q /f
MD	:= mkdir
else
MAIN	:= main
SOURCEDIRS	:= $(shell find $(SRC) -type d 2>/dev/null || echo $(SRC))
INCLUDEDIRS	:= $(shell find $(INCLUDE) -type d 2>/dev/null || echo $(INCLUDE))
# Check if lib directory exists before using find
LIBDIRS		:= $(shell if [ -d "$(LIB)" ]; then find $(LIB) -type d 2>/dev/null; fi)
FIXPATH = $1
RM = rm -f
MD	:= mkdir -p
endif

# define any directories containing header files other than /usr/include
INCLUDES	:= $(patsubst %,-I%, $(INCLUDEDIRS:%/=%))

# define the C libs
LIBS		:= $(patsubst %,-L%, $(LIBDIRS:%/=%))

# define the C source files
# Define the C source files (Original)
SOURCES_ALL := $(wildcard $(patsubst %,%/*.cpp, $(SOURCEDIRS)))

# Tentukan file yang ingin DIHAPUS dari build (exclude)
EXCLUDES    := src/testing.cpp \
               src/testing_benchmark.cpp \
               src/testing_regression.cpp \
               src/testing_tactical.cpp \
               src/tests.cpp

# Filter SOURCES_ALL untuk membuang file EXCLUDES
SOURCES     := $(filter-out $(EXCLUDES), $(SOURCES_ALL))

# define the C object files
OBJECTS		:= $(SOURCES:.cpp=.o)

# define the dependency output files
DEPS		:= $(OBJECTS:.o=.d)

#
# The following part of the makefile is generic; it can be used to
# build any executable just by changing the definitions above and by
# deleting dependencies appended to the file from 'make depend'
#

OUTPUTMAIN	:= $(call FIXPATH,$(OUTPUT)/$(MAIN))

all: $(OUTPUT) $(MAIN)
	@echo Executing 'all' complete!

$(OUTPUT):
	$(MD) $(OUTPUT)

$(MAIN): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(OUTPUTMAIN) $(OBJECTS) $(LFLAGS) $(LIBS)

# include all .d files
-include $(DEPS)

# this is a suffix replacement rule for building .o's and .d's from .c's
# it uses automatic variables $<: the name of the prerequisite of
# the rule(a .c file) and $@: the name of the target of the rule (a .o file)
# -MMD generates dependency output files same name as the .o file
# (see the gnu make manual section about automatic variables)
.cpp.o:
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -MMD $<  -o $@

.PHONY: clean
clean:
	$(RM) $(OUTPUTMAIN)
	$(RM) $(call FIXPATH,$(OBJECTS))
	$(RM) $(call FIXPATH,$(DEPS))
	@echo Cleanup complete!

run: all
	./$(OUTPUTMAIN)
	@echo Executing 'run: all' complete!

# ============================================================================
# Performance Optimized Builds
# ============================================================================

# Build with PEXT/PDEP support for BMI2 CPUs (Intel Haswell+, AMD Zen 3+)
# This provides faster sliding piece attack lookups
pext: CXXFLAGS += -mbmi2 -DUSE_PEXT
pext: clean all
	@echo Built with PEXT/BMI2 support!

# Profile-Guided Optimization (PGO) - Step 1: Instrument
# Build with profiling enabled, then run benchmarks
pgo-generate: CXXFLAGS += -fprofile-generate
pgo-generate: LFLAGS += -fprofile-generate
pgo-generate: clean all
	@echo PGO instrumented build complete! Run benchmarks now.

# Profile-Guided Optimization (PGO) - Step 2: Use profile
# Build using collected profile data for optimized code
pgo-use: CXXFLAGS += -fprofile-use -fprofile-correction
pgo-use: LFLAGS += -fprofile-use
pgo-use: clean all
	@echo PGO optimized build complete!

# Build with both PEXT and PGO for maximum performance
pext-pgo-generate: CXXFLAGS += -mbmi2 -DUSE_PEXT -fprofile-generate
pext-pgo-generate: LFLAGS += -fprofile-generate
pext-pgo-generate: clean all
	@echo Built with PEXT + PGO profiling! Run benchmarks now.

pext-pgo-use: CXXFLAGS += -mbmi2 -DUSE_PEXT -fprofile-use -fprofile-correction
pext-pgo-use: LFLAGS += -fprofile-use
pext-pgo-use: clean all
	@echo Built with PEXT + PGO optimization complete!

# Debug build for development
debug: CXXFLAGS := -std=c++17 -Wall -Wextra -g -O0 -DDEBUG
debug: clean all
	@echo Debug build complete!

# Profiling build for gprof analysis
# profile: CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pg -g -march=native -DNDEBUG
# profile: LFLAGS += -pg
# profile: clean all
# 	@echo Profiling build complete! Run the engine then use gprof.

# Internal profiling build (uses built-in timer-based profiler)
# This enables PROFILE_SCOPE macros throughout the code
# Run 'bench' command to see profiling results
internal-profile: CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -march=native -DNDEBUG -DPROFILING
internal-profile: clean all
	@echo Internal profiling build complete! Run 'bench' to see results.

# ============================================================================
# Texel Tuner Build
# ============================================================================

# Source files needed for tuner (excluding main.cpp)
TUNER_SOURCES := tuner/texel_tuner.cpp \
                 src/board.cpp \
                 src/magic.cpp \
                 src/zobrist.cpp \
                 src/bitboard.cpp \
                 src/eval.cpp \
                 src/tuning.cpp \
                 src/movegen.cpp

TUNER_OBJECTS := $(TUNER_SOURCES:.cpp=.o)

ifeq ($(OS),Windows_NT)
TUNER_MAIN := tuner.exe
else
TUNER_MAIN := tuner
endif

TUNER_OUTPUT := $(call FIXPATH,$(OUTPUT)/$(TUNER_MAIN))

tuner: $(OUTPUT) $(TUNER_OBJECTS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(TUNER_OUTPUT) $(TUNER_OBJECTS) $(LFLAGS) $(LIBS)
	@echo Texel Tuner build complete!
	@echo Usage: $(TUNER_OUTPUT) [epd_file] [max_positions] [iterations]

tuner-clean:
	$(RM) $(TUNER_OUTPUT)
	$(RM) $(call FIXPATH,$(TUNER_OBJECTS))
	@echo Tuner cleanup complete!
