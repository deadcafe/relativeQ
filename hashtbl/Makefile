PROJ_ROOT ?= ../
BUILD_DIR ?= $(PROJ_ROOT)/_build
INSTALL_DIR ?= $(PROJ_ROOT)/_install
PKG_CONFIG_PATH ?= $(INSTALL_DIR)/lib/pkgconfig
#export PROJ_ROOT BUILD_DIR INSTALL_DIR JOBS PKG_CONFIG_PATH

TARGET := fcache

PKGCONF := pkg-config
#OPTFLAGS := -g -O3 -mavx2 -mavx512f -msse4.1 -msse4.2 -Wall -Wextra -DENABLE_TRACER
OPTFLAGS := -g -O2 -march=native -fomit-frame-pointer -fpie -fpic -Werror -Wall -Wextra -DENABLE_TRACER

CFLAGS += $(OPTFLAGS) $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS += $(shell $(PKGCONF) --libs libdpdk)

SRC_DIR ?= ./

OBJ_DIR := $(BUILD_DIR)/fcache
APP := $(OBJ_DIR)/$(TARGET)

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean depend
all: $(APP)

$(APP): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR):
	-@ mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	-@ rm -rf $(OBJ_DIR)

depend: $(SRCS) Makefile
	-@ $(CC) $(CFLAGS) -MM -MG $(SRCS) > $(DEPENDS)

-include $(DEPENDS)
