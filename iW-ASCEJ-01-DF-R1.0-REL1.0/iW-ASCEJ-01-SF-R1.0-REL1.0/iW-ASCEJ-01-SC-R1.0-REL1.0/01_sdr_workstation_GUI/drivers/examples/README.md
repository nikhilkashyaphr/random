# IWFG Display and Streaming Examples

This directory contains examples demonstrating various methods for displaying and streaming captured frames from the IWFG frame grabber, including zero-copy display, low-latency techniques using DMA-BUF, KMS/DRM, OpenGL, and network streaming with optional video encoding.

## Examples Overview

### Display Examples

### 1. `iwfg_opengl_display` - OpenGL Window Display (NEW!)
- **Purpose**: User-friendly OpenGL display in a resizable window with NVIDIA GPU acceleration
- **Features**:
  - **4K RGBA support**: Native 3840x2160 RGBA8888 display
  - OpenGL texture streaming for GPU-accelerated rendering
  - GLFW-based windowed application (no root required)
  - Real-time performance monitoring
  - Fullscreen and windowed modes
  - Runtime controls (ESC to exit, P for performance stats)
  - **NVIDIA GPU optimized** for maximum performance
  - Cross-platform compatibility

### 2. `iwfg_opengl_chunked_display` - OpenGL Chunked Display (NEW!)
- **Purpose**: OpenGL display with chunked DMA transfers for efficient frame building and display
- **Features**:
  - **Frame division streaming**: Process frames divided into configurable chunks (2-256 divisions)
  - **Simple collection approach**: Minimal processing overhead for driver testing
  - Complete frame display - only renders when frame collection is complete
  - Configurable frame divisions for latency vs throughput optimization
  - Real-time performance monitoring with chunk-level statistics
  - Low-latency streaming ideal for real-time applications
  - Fullscreen and windowed modes with runtime controls
  - **Minimal display latency** - renders complete frames immediately
  - Command-line frame division configuration

### 3. `iwfg_zero_copy_display` - True Zero-Copy Display (XRGB8888)
- **Purpose**: Demonstrates true zero-copy display when frame grabber outputs XRGB8888 natively
- **Features**:
  - **True zero-copy**: No format conversion or memory copies
  - Direct DMA-BUF from frame grabber to GPU framebuffer
  - Minimal CPU usage and memory bandwidth
  - Optimal performance and lowest latency
  - Direct KMS page flipping

### 4. `iwfg_direct_display` - Zero-Copy with OpenGL Conversion
- **Purpose**: Zero-copy display with hardware-accelerated format conversion
- **Features**:
  - OpenGL-based RGB888 → XRGB8888 conversion
  - Hardware-accelerated color conversion on GPU
  - EGL/OpenGL ES rendering pipeline
  - Zero-copy data path with GPU-based processing
  - Color channel correction for proper RGB display

### 5. `iwfg_memcpy_display` - Traditional Copy-Based Display
- **Purpose**: Provides comparison baseline using traditional memcpy approach
- **Features**:
  - CPU-based format conversion (RGB888 → XRGB8888)
  - Measures memcpy overhead and latency
  - Uses traditional DRM dumb buffers
  - Performance comparison with zero-copy approaches
  - Detailed latency breakdown

### Transfer and Streaming Examples

### 6. `iwfg_c2h_h2c_example` - C2H to H2C Transfer
- **Purpose**: Demonstrates bidirectional data transfer between C2H and H2C
- **Features**:
  - **Multi-threaded pipeline**: C2H capture thread and H2C transmission thread
  - Frame routing from C2H buffers to H2C buffers
  - Inter-thread communication via lock-free frame queue
  - Performance monitoring for both directions
  - Memory-to-memory data movement through DMA engine

### 6b. `iwfg_c2h_h2c_chunked_example` - C2H to H2C Chunked Transfer (NEW!)
- **Purpose**: Advanced 3-thread pipeline for chunked C2H to H2C transfers with driver-managed C2H completion
- **Features**:
  - **3-thread pipeline architecture**: Separate C2H capture, copy processing, and H2C transmission threads
  - **Driver-managed C2H chunking**: Application requests chunks until driver signals completion via IWFG_FLAG_LAST_PACKET
  - **Application-managed H2C chunking**: Application sets IWFG_FLAG_LAST_PACKET for final H2C chunk
  - **Dynamic chunk handling**: No fixed chunk count - driver determines when frame is complete
  - **Proper thread separation**: C2H thread → Copy thread → H2C thread with dual queue system
  - **Frame integrity verification**: Complete frame tracking across all chunks
  - **Performance monitoring**: Chunk-level statistics and throughput analysis
  - **Dual queue system**: Separate queues for C2H→Copy and Copy→H2C communication
  - **Semaphore synchronization**: Non-blocking pipeline with proper thread coordination
  - **Configurable chunk size**: Request size for C2H operations (2-16 divisions supported)
  - **Memory efficiency**: Uses 4 C2H and 4 H2C buffers for optimal throughput
  - **Robust error handling**: Safety checks to prevent infinite loops and handle driver behavior

### 6. `iwfg_gstreamer_streaming` - GStreamer H.264 RTP Streaming (NEW!)
- **Purpose**: Professional H.264 video streaming using GStreamer pipeline with hardware encoding support
- **Features**:
  - **GStreamer integration**: appsrc for frame injection into GStreamer pipeline
  - **Hardware encoding**: NVIDIA NVENC, Intel VA-API support with software fallback
  - **RTP streaming**: Standards-compliant H.264 RTP streaming over UDP
  - Real-time H.264 encoding with configurable bitrate and framerate
  - Multi-threaded pipeline with C2H capture thread
  - **Only 2 C2H buffers**: Minimal memory footprint as requested
  - Performance monitoring and statistics
  - Configurable network destination and encoding parameters
  - Compatible with standard receivers (GStreamer, FFmpeg, VLC)

### 7. `iwfg_network_streaming_example` - Network Video Streaming (NEW!)
- **Purpose**: Real-time video streaming over network with optional H.264 encoding
- **Features**:
  - **Multi-threaded pipeline**: C2H capture thread and encoding/streaming thread
  - Raw frame streaming or H.264 encoded streaming
  - UDP and TCP network protocols support
  - Configurable bitrate, FPS, and encoding parameters
  - Real-time performance monitoring
  - Low-latency streaming optimized for live video
  - Frame queue with overflow protection
  - Network throughput monitoring

### 8. `iwfg_network_receiver` - Network Stream Receiver (NEW!)
- **Purpose**: Companion receiver for network streaming demonstration
- **Features**:
  - UDP and TCP stream reception
  - Raw frame and H.264 stream support
  - Frame saving to files for analysis
  - Reception statistics and monitoring
  - Configurable output directory and frame limits

### 8. `iwfg_chunked_dma_example` - Chunked DMA Streaming
- **Purpose**: Demonstrates chunked DMA for real-time streaming applications
- **Features**:
  - **Chunked DMA**: Process data in configurable chunks as it arrives
  - Low-latency streaming with minimal buffering delay
  - Configurable chunk sizes (64KB to 2MB)
  - Real-time processing simulation
  - Performance monitoring and bandwidth analysis
  - File output capability for data capture
  - Multi-threaded processing with timing analysis
  - **Zero-copy display**: DRM/KMS direct display from chunked data
  - **Vulkan direct-to-display**: Optional Vulkan direct display support

### 6. `iwfg_dmabuf_example` - Basic DMA-BUF Usage
- **Purpose**: Simple example showing DMA-BUF export and import
- **Features**:
  - Basic DMA-BUF export workflow
  - DRM integration example
  - Educational reference

### 9. `iwfg_dmabuf_h2c_pattern` - DMA-BUF H2C Test Pattern Generator
- **Purpose**: CPU-generated test patterns for H2C transfer debugging
- **Features**:
  - Driver-allocated DMA buffers (DMABUF mode) or user buffers (USERPTR mode)
  - CPU-generated test patterns (color bars, gradient, checkerboard, moving bar)
  - H2C DMA transfer to FPGA
  - Pattern animation support
  - Configurable frame rate

### 10. `iwfg_dmabuf_c2h_capture` - DMA-BUF C2H Frame Capture
- **Purpose**: C2H frame capture to files for analysis
- **Features**:
  - Driver-allocated DMA buffers
  - C2H DMA capture from FPGA
  - Frame saving to files
  - Capture statistics

### 11. `iwfg_dmabuf_gpu_h2c_pattern` - GPU H2C Test Pattern Generator (NEW!)
- **Purpose**: GPU-generated moving bar pattern for H2C vsync debugging
- **Features**:
  - **Zero-copy GPU rendering**: EGL/OpenGL ES pipeline with DMA-BUF import
  - **GPU-generated patterns**: Moving bar animation rendered on GPU
  - **H2C transfer**: GPU-rendered frames sent to FPGA via H2C DMA
  - **Vsync debugging**: Frame counter indicator for timing analysis
  - **High performance**: No CPU involvement in pattern generation
  - **Surfaceless rendering**: Headless GPU operation via EGL
  - **Frame rate control**: Configurable delay between frames
  - **Hardware acceleration**: Leverages GPU shader pipeline
  - **Educational**: Demonstrates complete GPU→DMA-BUF→H2C pipeline

## Building

### Prerequisites
```bash
# Install required packages
sudo apt-get update
sudo apt-get install -y libdrm-dev linux-headers-$(uname -r) build-essential
```

### Build Examples
```bash
# From the main driver directory
make examples-build

# Or from this directory
make all

# All build artifacts are placed in build/ directory
```

## Usage

### Setup Driver
```bash
# Build and load the driver (from main directory)
make all
sudo insmod iwfg.ko

# Verify device exists
ls -l /dev/iwfg0
```

### Run True Zero-Copy Display (XRGB8888)
```bash
# Best performance - frame grabber outputs XRGB8888 natively
sudo ./build/iwfg_zero_copy_display

# Or from main directory
make run-zero-copy-display
```

### Run Zero-Copy with Conversion (RGB888→XRGB8888)
```bash
# GPU-accelerated conversion - frame grabber outputs RGB888
sudo ./build/iwfg_direct_display

# Or from main directory
make run-direct-display
```

### Run Traditional Memcpy Display
```bash
# This demonstrates traditional copy-based approach
sudo ./build/iwfg_memcpy_display

# Or from main directory  
make run-memcpy-display
```

### Run Chunked DMA Streaming
```bash
# Basic chunked DMA with default 256KB chunks
sudo ./build/iwfg_chunked_dma_example

# Or from main directory
make run-chunked

# With performance monitoring and processing simulation
sudo ./build/iwfg_chunked_dma_example -m -p

# Or from main directory
make run-chunked-perf

# With zero-copy display (DRM/KMS)
sudo ./build/iwfg_chunked_dma_example -d -m

# Or from main directory
make run-chunked-display

# With Vulkan direct-to-display (if supported)
sudo ./build/iwfg_chunked_dma_example -d -v -m

# Or from main directory
make run-chunked-vulkan
```

### Run C2H to H2C Transfer Pipeline
```bash
# Basic C2H to H2C pipeline
sudo ./build/iwfg_c2h_h2c_example

# Or from main directory
make run-c2h-h2c

# With performance monitoring
sudo ./build/iwfg_c2h_h2c_example -p

# Or from main directory
make run-c2h-h2c-perf
```

### Run C2H to H2C Chunked Transfer Pipeline (NEW!)
```bash
# Basic chunked transfer (4KB chunk requests, driver determines completion)
sudo ./build/iwfg_c2h_h2c_chunked_example

# Or from main directory
make run-c2h-h2c-chunked

# Smaller chunk requests (8 divisions = smaller chunks, more frequent driver interaction)
sudo ./build/iwfg_c2h_h2c_chunked_example 8

# Or from main directory  
make run-c2h-h2c-chunked-8

# Larger chunk requests (2 divisions = larger chunks, less frequent driver interaction)
sudo ./build/iwfg_c2h_h2c_chunked_example 2

# Very small chunk requests for maximum responsiveness
sudo ./build/iwfg_c2h_h2c_chunked_example 16

# Note: The actual number of chunks per frame is determined by the driver
# The parameter only sets the requested chunk size for C2H operations
# Driver will signal completion via IWFG_FLAG_LAST_PACKET when frame is ready
```

# Custom chunk size (128KB) with file output
sudo ./build/iwfg_chunked_dma_example -c 128 -s captured_data.raw

# Ultra low latency (64KB chunks) with display and monitoring
sudo ./build/iwfg_chunked_dma_example -c 64 -d -m -p

# Maximum throughput (2MB chunks) with Vulkan display
sudo ./build/iwfg_chunked_dma_example -c 2048 -d -v
```

### Run OpenGL Display Examples

#### OpenGL Window Display (No root required)
```bash
# Basic OpenGL display (windowed mode)
./build/iwfg_opengl_display

# Or from main directory
make run-opengl

# With performance monitoring
./build/iwfg_opengl_display -p

# Or from main directory
make run-opengl-perf

# Fullscreen mode
./build/iwfg_opengl_display -f

# Or from main directory
make run-opengl-fullscreen
```

#### OpenGL Chunked Display (Progressive frame building)
```bash
# Basic chunked OpenGL display (16 divisions)
./build/iwfg_opengl_chunked_display

# Or from main directory
make run-opengl-chunked

# With performance monitoring
./build/iwfg_opengl_chunked_display -p

# Or from main directory
make run-opengl-chunked-perf

# Custom frame divisions (32 divisions) with performance monitoring
./build/iwfg_opengl_chunked_display -d 32 -p

# Or from main directory
make run-opengl-chunked-32div

# Low latency (8 divisions) in fullscreen
./build/iwfg_opengl_chunked_display -f -d 8 -p

# Or from main directory
make run-opengl-chunked-fullscreen

# Available frame divisions: 2, 4, 8, 16, 32, 64, 128, 256
```

### Run DMA-BUF H2C Test Pattern Examples

#### CPU-Based Pattern Generator
```bash
# Send color bars pattern (DMABUF mode)
sudo ./build/iwfg_dmabuf_h2c_pattern -m dmabuf -p bars

# Send moving bar animation
sudo ./build/iwfg_dmabuf_h2c_pattern -p moving

# Use USERPTR mode (user-allocated buffer)
sudo ./build/iwfg_dmabuf_h2c_pattern -m userptr -p bars

# Custom frame rate (30 fps)
sudo ./build/iwfg_dmabuf_h2c_pattern -p moving -d 33
```

#### GPU-Based Pattern Generator (NEW!)
```bash
# Basic GPU-rendered moving bar (default 60 fps)
sudo ./build/iwfg_dmabuf_gpu_h2c_pattern

# Custom frame rate (30 fps)
sudo ./build/iwfg_dmabuf_gpu_h2c_pattern -d 33

# Maximum frame rate (no delay)
sudo ./build/iwfg_dmabuf_gpu_h2c_pattern -d 0

# The GPU renders a moving white bar with a colored square frame counter
# Perfect for debugging H2C vsync timing and verifying frame ordering
```

### Run Network Streaming Examples

#### GStreamer H.264 RTP Streaming (Recommended)
```bash
# Basic GStreamer streaming to localhost:5004
sudo ./build/iwfg_gstreamer_streaming

# With performance monitoring
sudo ./build/iwfg_gstreamer_streaming -m

# Stream to remote host with high quality
sudo ./build/iwfg_gstreamer_streaming -h 192.168.1.100 -b 10000000 -f 60

# Try hardware encoder (NVIDIA/Intel)
sudo ./build/iwfg_gstreamer_streaming -e nvh264enc -m

# Or use make targets
make run-gstreamer      # Basic streaming
make run-gstreamer-perf # With monitoring
make run-gstreamer-hq   # High quality
make run-gstreamer-hw   # Hardware encoding

# Demo script with interactive options
sudo ./demo_gstreamer_streaming.sh
```

#### Receive GStreamer Stream
```bash
# Basic receiver
gst-launch-1.0 udpsrc port=5004 ! \
  application/x-rtp,encoding-name=H264 ! \
  rtph264depay ! h264parse ! avdec_h264 ! \
  videoconvert ! autovideosink

# Save to file
gst-launch-1.0 udpsrc port=5004 ! \
  application/x-rtp,encoding-name=H264 ! \
  rtph264depay ! h264parse ! mp4mux ! \
  filesink location=iwfg_stream.mp4

# Or with FFmpeg/VLC
ffplay udp://127.0.0.1:5004
vlc udp://@:5004
```

#### Basic Network Streaming (Raw Frames)
```bash
# Start receiver first (in one terminal)
./build/iwfg_network_receiver

# Start streaming (in another terminal)
sudo ./build/iwfg_network_streaming_example

# Or use make targets
make run-receiver &    # Start receiver in background
make run-stream        # Start streaming
```

#### Advanced Network Streaming Options
```bash
# Stream with performance monitoring
sudo ./build/iwfg_network_streaming_example -m

# Stream via TCP instead of UDP
sudo ./build/iwfg_network_streaming_example -P tcp -m

# Stream to remote host
sudo ./build/iwfg_network_streaming_example -s 192.168.1.100 -p 9999 -P tcp

# Stream with H.264 encoding (if x264 compiled)
sudo ./build/iwfg_network_streaming_example -e -b 8000 -f 60 -m

# Or use make targets
make run-stream-perf     # Raw frames with performance monitoring
make run-stream-tcp      # TCP streaming
make run-stream-encoded  # H.264 encoding (if available)
```

#### Network Receiver Options
```bash
# Basic UDP receiver
./build/iwfg_network_receiver

# TCP receiver
./build/iwfg_network_receiver -P tcp

# Save received frames to files
./build/iwfg_network_receiver -s -o /tmp/frames -m 100

# Listen on different port
./build/iwfg_network_receiver -p 9999

# Or use make targets
make run-receiver        # Basic UDP receiver
make run-receiver-tcp    # TCP receiver
make run-receiver-save   # Save frames to files
```

#### Complete Demo
```bash
# Run automated demo (both sender and receiver)
make demo-streaming

# This will:
# 1. Start receiver in background
# 2. Stream frames for 10 seconds
# 3. Stop both processes
# 4. Save received frames to received_frames/ directory
```

### Performance Comparison
```bash
# Run both examples for side-by-side comparison
make run-comparison

# Or from main directory
make run-display-comparison
```

## Requirements

### Hardware Requirements
- IWFG frame grabber device
- DisplayPort monitor connected to system
- DRM/KMS compatible graphics hardware

### Software Requirements
- Linux kernel with DRM/KMS support
- libdrm development libraries
- Root privileges (for direct hardware access)
- **For Vulkan display**: Vulkan drivers and direct-to-display support
- **For OpenGL examples**: GLFW3 and OpenGL development libraries
- **For network streaming**: Basic networking libraries (included in glibc)
- **For GStreamer streaming**: GStreamer 1.0 development packages

### Optional Dependencies
- **For H.264 encoding**: libx264 development libraries
  ```bash
  # Install x264 for H.264 encoding support
  sudo apt-get install libx264-dev
  
  # Then enable in Makefile by uncommenting:
  # CFLAGS += -DENABLE_X264_ENCODING
  # LIBS_X264 = -lx264
  ```

- **For GStreamer streaming**: GStreamer development packages
  ```bash
  # Install GStreamer development packages (Ubuntu/Debian)
  sudo apt-get install \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav
  
  # For hardware encoding support (optional)
  sudo apt-get install gstreamer1.0-vaapi  # Intel VA-API
  # NVIDIA NVENC support usually included with NVIDIA drivers
  ```

### Optional: Vulkan Direct Display
- Vulkan development libraries (`libvulkan-dev`)
- GPU with Vulkan direct-to-display support
- Vulkan loader and drivers properly configured
- VK_KHR_display extension support

### Device Files
- `/dev/iwfg` - IWFG frame grabber device
- `/dev/dri/card1` - DRM device for display

## Key Features Demonstrated

### Zero-Copy Display (`iwfg_direct_display`)
1. **DMA-BUF Export**: Exports frame grabber buffers as DMA-BUF file descriptors
2. **Direct Import**: Imports DMA-BUF into DRM as framebuffers  
3. **Page Flipping**: Uses hardware page flipping for smooth display
4. **No Window Manager**: Direct hardware display like kmscube

### Chunked DMA Streaming (`iwfg_chunked_dma_example`)
1. **Chunked Processing**: Data processed in configurable chunk sizes (64KB-2MB)
2. **Low Latency**: Immediate processing as chunks arrive, not waiting for full frames
3. **Zero-Copy Display**: Optional direct display from chunked buffers
4. **Vulkan Integration**: Optional Vulkan direct-to-display support
5. **Performance Analysis**: Detailed timing and throughput monitoring
6. **Real-time Processing**: Simulated processing pipeline with configurable workloads
5. **Performance Monitoring**: Real-time latency and FPS statistics

### Comparison Display (`iwfg_memcpy_display`)
1. **Traditional Buffers**: Uses DRM dumb buffers allocated by driver
2. **Memcpy Overhead**: Measures time spent copying frame data
3. **Latency Breakdown**: Separates copy time from display time
4. **Percentage Analysis**: Shows memcpy overhead as percentage of total latency

## Performance Metrics

Both examples provide detailed performance statistics:

- **Frame Rate**: Actual FPS achieved
- **Display Latency**: Time from frame ready to display flip
- **Copy Overhead**: Time spent in memcpy (memcpy example only)
- **Min/Max/Average**: Statistical analysis of all timings

## Expected Results

The zero-copy approach should demonstrate:
- **Lower Latency**: Significantly reduced frame-to-display time
- **Higher Efficiency**: No CPU overhead from memory copying
- **Better Performance**: Higher sustainable frame rates
- **Resource Savings**: Reduced memory bandwidth usage

## Troubleshooting

### Device Not Found
```bash
# Check if driver is loaded
lsmod | grep iwfg

# Check device permissions
ls -l /dev/iwfg0

# Check DRM device
ls -l /dev/dri/card0
```

### Permission Denied
```bash
# Examples require root privileges for DRM access
sudo ./iwfg_direct_display
```

### No Display Output
- Ensure monitor is connected to correct DisplayPort
- Check that DRM/KMS is enabled in kernel
- Verify graphics driver supports direct rendering

### Build Errors
```bash
# Install missing dependencies
make deps

# Clean and rebuild
make clean
make all
```

## Architecture

```
Frame Grabber → DMA-BUF → DRM Framebuffer → DisplayPort Monitor
                ↑
            Zero-Copy Path (iwfg_direct_display)

Frame Grabber → CPU Copy → DRM Dumb Buffer → DisplayPort Monitor  
                   ↑
              Copy Overhead (iwfg_memcpy_display)
```

### 6. `iwfg_c2h_h2c_example` - C2H to H2C Transfer Pipeline (NEW! - Multi-threaded)
- **Purpose**: Demonstrates bidirectional DMA transfers using both C2H and H2C operations in separate threads
- **Features**:
  - **Multi-threaded pipeline**: Separate threads for C2H capture and H2C transmission
  - **Dual buffer setup**: Separate C2H (receive) and H2C (transmit) buffers
  - **Frame capture via C2H**: Uses existing method same as zero_copy example in dedicated thread
  - **Frame transmission via H2C**: Uses new `IWFG_IOCTL_DMA_DATA` ioctl in dedicated thread
  - **Inter-thread communication**: Lock-free frame queue with semaphores for synchronization
  - **Non-blocking operation**: C2H and H2C operations run independently without blocking each other
  - **Pipeline processing**: Captures frames, processes them, then transmits via H2C
  - **Performance monitoring**: Tracks both C2H and H2C transfer times with thread-safe statistics
  - **Memory-to-memory transfers**: Routes captured data to H2C buffers for transmission
  - **Frame processing**: Demonstrates data modification between C2H and H2C stages
  - **Watermark addition**: Example frame processing that adds watermark pattern
  - **Queue management**: Configurable frame queue size with overflow handling
  - **Graceful shutdown**: Proper thread cleanup and resource management

## Integration with Other Projects

These examples can be adapted for:
- **Video Processing Pipelines**: Zero-copy integration with GPU compute
- **Real-Time Systems**: Low-latency video display applications  
- **Embedded Systems**: Efficient resource usage for constrained platforms
- **Benchmarking**: Performance comparison baseline for optimization
- **Bidirectional Data Processing**: Using both C2H and H2C DMA paths

## License

Same as the main IWFG driver project.
