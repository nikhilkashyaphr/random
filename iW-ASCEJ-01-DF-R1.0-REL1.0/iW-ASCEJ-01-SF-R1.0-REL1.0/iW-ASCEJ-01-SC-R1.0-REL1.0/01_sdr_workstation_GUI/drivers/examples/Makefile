# Makefile for IWFG userspace examples
# Can be used standalone or called from parent Makefile

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -D_GNU_SOURCE -I.. -I../.. -I/usr/include/libdrm
LIBS_DMABUF = -ldrm
LIBS_OPENGL = -ldrm -lgbm -lEGL -lGLESv2
LIBS_OPENGL_GLFW = -lGL -lglfw -lpthread -lm
LIBS_AV_DISPLAY  = -lGL -lglfw -lpthread -lm -lasound
LIBS_VULKAN = -ldrm -lvulkan
LIBS_NETWORK = -lpthread

# GStreamer support
GSTREAMER_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0)
GSTREAMER_LIBS = $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0) -lpthread

# Optional VA-API encoding support (uncomment if VA-API is available)
CFLAGS += -DENABLE_VAAPI_ENCODING
LIBS_VAAPI = -lva -lva-drm

# Legacy x264/x265 support (commented out - use VA-API instead)
# CFLAGS += -DENABLE_X264_ENCODING
# LIBS_X264 = -lx264
# CFLAGS += -DENABLE_X265_ENCODING
# LIBS_X265 = -lx265

# Build directory
BUILD_DIR = build

# Source files
SOURCES = iwfg_opengl_display.c iwfg_av_display.c iwfg_audio_capture.c iwfg_data_capture.c iwfg_h2c_test.c iwfg_c2h_h2c_test.c iwfg_c2h2c_old.c iwfg_dmabuf_cube.c iwfg_dmabuf_c2h_h2c.c iwfg_dmabuf_h2c_pattern.c iwfg_dmabuf_c2h_capture.c iwfg_dmabuf_gpu_h2c_pattern.c

# Targets (with build directory prefix)
TARGETS = $(BUILD_DIR)/iwfg_opengl_display $(BUILD_DIR)/iwfg_av_display $(BUILD_DIR)/iwfg_audio_capture $(BUILD_DIR)/iwfg_data_capture $(BUILD_DIR)/iwfg_h2c_test $(BUILD_DIR)/iwfg_c2h_h2c_test $(BUILD_DIR)/iwfg_c2h2c_old $(BUILD_DIR)/iwfg_dmabuf_cube $(BUILD_DIR)/iwfg_dmabuf_c2h_h2c $(BUILD_DIR)/iwfg_dmabuf_h2c_pattern $(BUILD_DIR)/iwfg_dmabuf_c2h_capture $(BUILD_DIR)/iwfg_dmabuf_gpu_h2c_pattern

# Default target
all: $(BUILD_DIR) $(TARGETS)

# Create build directory
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# OpenGL display example (GLFW + OpenGL)
$(BUILD_DIR)/iwfg_opengl_display: iwfg_opengl_display.c | $(BUILD_DIR)
	@echo "Building OpenGL display example (GLFW + OpenGL)..."
	$(CC) $(CFLAGS) -o $@ $< $(LIBS_OPENGL_GLFW)
	@echo "Built: $@"

# Audio+Video display example (GLFW + OpenGL + ALSA)
$(BUILD_DIR)/iwfg_av_display: iwfg_av_display.c | $(BUILD_DIR)
	@echo "Building Audio+Video display example (GLFW + OpenGL + ALSA)..."
	$(CC) $(CFLAGS) -o $@ $< $(LIBS_AV_DISPLAY)
	@echo "Built: $@"

# Audio-only capture + playback example (ALSA)
$(BUILD_DIR)/iwfg_audio_capture: iwfg_audio_capture.c | $(BUILD_DIR)
	@echo "Building audio capture example (ALSA)..."
	$(CC) $(CFLAGS) -o $@ $< -lasound
	@echo "Built: $@"

# Raw C2H data capture example
$(BUILD_DIR)/iwfg_data_capture: iwfg_data_capture.c | $(BUILD_DIR)
	@echo "Building raw data capture example..."
	$(CC) $(CFLAGS) -o $@ $<
	@echo "Built: $@"

# H2C test example
$(BUILD_DIR)/iwfg_h2c_test: iwfg_h2c_test.c | $(BUILD_DIR)
	@echo "Building H2C test example..."
	$(CC) $(CFLAGS) -o $@ $< $(LIBS_OPENGL_GLFW) 
	@echo "Built: $@"

# C2H + H2C loopback test example
$(BUILD_DIR)/iwfg_c2h_h2c_test: iwfg_c2h_h2c_test.c | $(BUILD_DIR)
	@echo "Building C2H + H2C loopback test example..."
	$(CC) $(CFLAGS) -o $@ $< -lpthread
	@echo "Built: $@"

# C2H to H2C old test example
$(BUILD_DIR)/iwfg_c2h2c_old: iwfg_c2h2c_old.c | $(BUILD_DIR)
	@echo "Building C2H to H2C old test example..."
	$(CC) $(CFLAGS) -o $@ $< -lpthread
	@echo "Built: $@"

# DMA-BUF zero-copy cube overlay example (EGL + GBM + OpenGL ES)
$(BUILD_DIR)/iwfg_dmabuf_cube: iwfg_dmabuf_cube.c | $(BUILD_DIR)
	@echo "Building DMA-BUF zero-copy cube overlay example..."
	$(CC) $(CFLAGS) -o $@ $< $(LIBS_OPENGL) -lpthread -lm
	@echo "Built: $@"

# DMA-BUF C2H to H2C loopback example (no GPU, just DMA-BUF allocation)
$(BUILD_DIR)/iwfg_dmabuf_c2h_h2c: iwfg_dmabuf_c2h_h2c.c | $(BUILD_DIR)
	@echo "Building DMA-BUF C2H to H2C loopback example..."
	$(CC) $(CFLAGS) -o $@ $< -lpthread
	@echo "Built: $@"

# DMA-BUF H2C test pattern generator (H2C only, no C2H)
$(BUILD_DIR)/iwfg_dmabuf_h2c_pattern: iwfg_dmabuf_h2c_pattern.c | $(BUILD_DIR)
	@echo "Building DMA-BUF H2C test pattern generator..."
	$(CC) $(CFLAGS) -o $@ $< -lpthread
	@echo "Built: $@"

# DMA-BUF C2H frame capture to files (C2H only, no H2C)
$(BUILD_DIR)/iwfg_dmabuf_c2h_capture: iwfg_dmabuf_c2h_capture.c | $(BUILD_DIR)
	@echo "Building DMA-BUF C2H frame capture..."
	$(CC) $(CFLAGS) -o $@ $<
	@echo "Built: $@"

# DMA-BUF GPU H2C test pattern generator (GPU-rendered moving bar)
$(BUILD_DIR)/iwfg_dmabuf_gpu_h2c_pattern: iwfg_dmabuf_gpu_h2c_pattern.c | $(BUILD_DIR)
	@echo "Building DMA-BUF GPU H2C test pattern generator..."
	$(CC) $(CFLAGS) -o $@ $< $(LIBS_OPENGL) -lm
	@echo "Built: $@"

clean:
	@echo "Cleaning build directory..."
	@rm -rf $(BUILD_DIR)
	@echo "Cleaned."

.PHONY: all run clean deps

