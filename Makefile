RACK_DIR ?= ../..

FLAGS +=
CFLAGS +=
CXXFLAGS += -std=c++17 -I./src

LDFLAGS +=

# Explicitly specify active sources to prevent accidental wildcard issues
SOURCES := src/plugin.cpp src/SpacesCommand.cpp

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard README*)

include $(RACK_DIR)/plugin.mk
