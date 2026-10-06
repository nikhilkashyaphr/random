# RoCEv2 IQ streaming demo
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11 -Isrc
LDLIBS  = -lrdmacm -libverbs -lm -lrt

CUDA_HOME ?= /usr/local/cuda
GPUFLAGS   = -I$(CUDA_HOME)/include
GPULIBS    = -L$(CUDA_HOME)/lib64 -lcudart

all: rdma_tx rdma_rx scale_tx scale_rx

rdma_tx: src/rdma_tx.c src/rdma_common.h
	$(CC) $(CFLAGS) -o $@ src/rdma_tx.c $(LDLIBS)

rdma_rx: src/rdma_rx.c src/rdma_common.h
	$(CC) $(CFLAGS) -o $@ src/rdma_rx.c $(LDLIBS)

scale_tx: src/scale_tx.c src/scale_common.h src/rdma_common.h
	$(CC) $(CFLAGS) -o $@ src/scale_tx.c $(LDLIBS)

scale_rx: src/scale_rx.c src/scale_common.h src/rdma_common.h
	$(CC) $(CFLAGS) -o $@ src/scale_rx.c $(LDLIBS)

# GPUDirect receiver: needs CUDA toolkit (section 4 of README) and, at run
# time, the nvidia-peermem module (scripts/05_gpudirect_setup.sh).
gpu: rdma_rx_gpu
rdma_rx_gpu: src/rdma_rx_gpu.c src/rdma_common.h
	$(CC) $(CFLAGS) $(GPUFLAGS) -o $@ src/rdma_rx_gpu.c $(LDLIBS) $(GPULIBS)

clean:
	rm -f rdma_tx rdma_rx rdma_rx_gpu scale_tx scale_rx

.PHONY: all gpu clean
