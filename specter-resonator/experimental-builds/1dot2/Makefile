# Project Name
TARGET = specter-resonator-1dot2

# Sources
CPP_SOURCES = main.cpp

# Library Locations
# The libraries are shared by every project in the repo and live three levels
# up in `lib/`, which is gitignored. See the root README to fetch them.
# Override these if yours live elsewhere:
#   make LIBDAISY_DIR=/path/to/libDaisy DAISYSP_DIR=/path/to/DaisySP
LIBDAISY_DIR ?= ../../../lib/libDaisy
DAISYSP_DIR  ?= ../../../lib/DaisySP

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
