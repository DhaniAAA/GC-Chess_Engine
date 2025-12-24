#
# 'make'        build executable file 'main'
# 'make clean'  removes all .o and executable files
#

# define the Cpp compiler to use
# Untuk g++ 64-bit
CXX = x86_64-w64-mingw32-g++

# Mode Release (Default)
# CXXFLAGS := -std=c++17 -Wall -Wextra -O3 -march=native -DNDEBUG
# LFLAGS =

# define any compile-time flags
# Mode Release (Optimized for Speed - Bullet Chess)
# -O3: Maximum optimization
# -march=native: Use all CPU features available
# -flto: Link-Time Optimization for whole-program optimization
# -fno-exceptions: Disable exceptions for faster code (if not used)
# -funroll-loops: Unroll loops for speed
# -fomit-frame-pointer: Free up a register for faster code
CXXFLAGS := -std=c++17 -Wall -Wextra -O3 -march=native -DNDEBUG \
            -flto -funroll-loops -fomit-frame-pointer

# Link-time optimization flags (must match CXXFLAGS)
LFLAGS = -flto

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
SOURCEDIRS	:= $(shell find $(SRC) -type d)
INCLUDEDIRS	:= $(shell find $(INCLUDE) -type d)
LIBDIRS		:= $(shell find $(LIB) -type d)
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
