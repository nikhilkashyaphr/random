/*
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006 Open Grid Computing, Inc. All rights reserved.
 * Copyright (c) 2020 Xilinx, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#define _GNU_SOURCE
#include <endian.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <semaphore.h>
#include <pthread.h>
# include <inttypes.h>
#include <sys/mman.h>
#include <rdma/rdma_cma.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <rdma/rdma_verbs.h>
#include <termios.h>
#include <arpa/inet.h>

#define MAX	1 * 1024 *1024
#define PORT	52525
#define SA struct sockaddr
static int sockfd;
/*
 * Pause and Play Implementation required configuration and functions
 */
static struct termios old, current;

/* Initialize new terminal i/o settings */
void initTermios(int echo)
{
	tcgetattr(0, &old); /* grab old terminal i/o settings */
	current = old; /* make new settings same as old settings */
	current.c_lflag &= ~ICANON; /* disable buffered i/o */
	if (echo) {
		current.c_lflag |= ECHO; /* set echo mode */
	} else {
		current.c_lflag &= ~ECHO; /* set no echo mode */
	}
	tcsetattr(0, TCSANOW, &current); /* use these new terminal i/o settings now */
}

/* Restore old terminal i/o settings */
void resetTermios(void)
{
	tcsetattr(0, TCSANOW, &old);
}

/* Read 1 character - echo defines echo mode */
char getch_(int echo)
{
	char ch;
	initTermios(echo);
	ch = getchar();
	resetTermios();
	return ch;
}

/* Read 1 character without echo */
char getch(void)
{
	return getch_(0);
}

/* Read 1 character with echo */
char getche(void)
{
	return getch_(1);
}

uint64_t virtaddr;
uint32_t totalDataTS;
static int debug = 0;
uint16_t dataStart = 0;
uint8_t numOfImages;
#define MAX_IMAGES	10
#define DEBUG_LOG if (debug) printf
#define DRIVER_LOG printf

#define HH_DEV_NAME	"/dev/xib0"
int update_sgl_data(struct ibv_qp *ibvqp, char* buf, int len);
#define XIB_ERNIC
#define XIB_DMA_MEM_ALLOC_FIX

#define XIB_MEM_ALIGN	4096
#if (XIB_MEM_ALIGN == 64)
	#define XIB_BIT_SHIFT 6
#elif (XIB_MEM_ALIGN == 4096)
	#define XIB_BIT_SHIFT 12
#endif

/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV, 
 * and RDMA_WRITE_COMPLETE for each ping.
 */
enum test_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	RDMA_READ_ADV,
	RDMA_READ_COMPLETE,
	RDMA_WRITE_ADV,
	RDMA_WRITE_COMPLETE,
	DISCONNECTED,
	HW_HS_RECV_CMPLT,
	ERROR
};

struct rping_rdma_info {
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
	uint32_t iter_cnt;
	uint32_t qp_cnt;
};

/*
 * Default max buffer size for IO...
 */
#define RPING_BUFSIZE 64*1024

/* Default string for print data and
 * minimum buffer size
 */
#define _stringify( _x ) # _x
#define stringify( _x ) _stringify( _x )

#define RPING_MSG_FMT           "rdma-ping-%d: "
#define RPING_MIN_BUFSIZE       sizeof(stringify(INT_MAX)) + sizeof(RPING_MSG_FMT)

struct hh_send_fmt {
	int	tf_siz;
	int	opcode;
	int	qp_cnt;
	int     wqe_cnt;
};

/*
 * Control block struct.
 */
struct rping_cb {
	pthread_t cq_thread;
	struct ibv_comp_channel *channel;
	struct ibv_cq *cq;
	struct ibv_pd *pd;
	struct ibv_qp *qp;

	struct ibv_recv_wr rq_wr;	/* recv work request record */
	struct ibv_sge recv_sgl;	/* recv single SGE */
	struct hh_send_fmt recv_buf;/* malloc'd buffer */
	struct ibv_mr *recv_mr, *send_mr;

	struct ibv_send_wr sq_wr;	/* send work request record */
	struct ibv_sge send_sgl;
	struct rping_rdma_info send_buf;/* single send buf */

	struct ibv_send_wr rdma_sq_wr;	/* rdma work request record */

	enum test_state state;		/* used for cond/signalling */
	enum test_state disc_state;		/* used for cond/signalling */
	sem_t sem;

	/* CM stuff */
	struct rdma_event_channel *cm_channel;
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on service side. */
	int count;
	struct rdma_cm_event *event;
	struct ibv_wc *wc;
	volatile int cm_fail, cq_fail, cm_chnl_en, cm_established, test_done, disc_rx, disc_check_en;
	volatile int poll_pkt_cnt;
	unsigned int qp_num;

	void	*hh_va;
	uint32_t	hh_data_size, hh_opcode, wqe_cnt;
	struct	ibv_mr*	hh_mr;
	struct hh_send_fmt hh_info;/* malloc'd buffer */
	volatile int		rx_wqe_ready, rq_depth;
	volatile uint32_t	rx_compl, rx_posted;
	int wr_id;
};

struct rping_test_info {
	struct rping_cb	*cb;
	pthread_t cmthread;
	pthread_t disc_thread;
	pthread_t cqthread;
	pthread_t datathread;
//	pthread_t pauseplaythread;
	int	qp_cnt;
	int	size;
	int	count;
	int	verbose;
	int	validate;
	struct sockaddr_storage sin, ssource;
	uint16_t port;			/* dst port in NBO */
	int	cq_init;
	volatile int done_cnt;
	volatile int tf_siz;
	int	dev_fd;	
} rping_test;

#define HH_MAX_Q_DEPTH 16
enum hh_opcode {
	HH_RDMA_WRITE_OP,
	HH_RDMA_READ_OP,
	HH_RDMA_SEND_OP,
};

#define HH_MAX_QUEUE_DEPTH	16
#define HH_CQ_RQ_DEPTH		(8 * 1024 + 32)
#define HH_RX_REPOST_WQE_CNT	256
#define HH_SQ_DEPTH		1000

#if 0
__attribute__((destructor)) void destruct_rping(void)
{
	if (rping_test.dev_fd > 0)
		close(rping_test.dev_fd);
}
#endif

const int BYTES_PER_PIXEL = 3;
const int FILE_HEADER_SIZE = 14;
const int INFO_HEADER_SIZE = 40;

void generateBitmapImage(unsigned char*** image, int height, int width, char* imageFileName);
unsigned char* createBitmapFileHeader(int height, int stride);
unsigned char* createBitmapInfoHeader(int height, int width);

int roundup(int a){
	if(a<0)
		return 0;
	else if(a>255)
		return 255;
	else 
		return a;
}

void generateBitmapImage (unsigned char*** image, int height, int width, char* imageFileName)
{
	int widthInBytes = width * BYTES_PER_PIXEL;

	unsigned char padding[3] = {0, 0, 0};
	int paddingSize = (4 - (widthInBytes) % 4) % 4;

	int stride = (widthInBytes) + paddingSize;

	FILE* imageFile = fopen(imageFileName, "wb");

	unsigned char* fileHeader = createBitmapFileHeader(height, stride);
	fwrite(fileHeader, 1, FILE_HEADER_SIZE, imageFile);

	unsigned char* infoHeader = createBitmapInfoHeader(height, width);
	fwrite(infoHeader, 1, INFO_HEADER_SIZE, imageFile);

	int i,j;
	for (i = 0; i < height; i++) {
		for (j = 0; j <width; ++j) {
			fwrite(image[i][j], BYTES_PER_PIXEL, 1, imageFile);
			fwrite(padding, 1, paddingSize, imageFile);
		}
	}

	fclose(imageFile);
}

unsigned char* createBitmapFileHeader (int height, int stride)
{
	int fileSize = FILE_HEADER_SIZE + INFO_HEADER_SIZE + (stride * height);

	static unsigned char fileHeader[] = {
		0,0,     /// signature
		0,0,0,0, /// image file size in bytes
		0,0,0,0, /// reserved
		0,0,0,0, /// start of pixel array
	};

	fileHeader[ 0] = (unsigned char)('B');
	fileHeader[ 1] = (unsigned char)('M');
	fileHeader[ 2] = (unsigned char)(fileSize      );
	fileHeader[ 3] = (unsigned char)(fileSize >>  8);
	fileHeader[ 4] = (unsigned char)(fileSize >> 16);
	fileHeader[ 5] = (unsigned char)(fileSize >> 24);
	fileHeader[10] = (unsigned char)(FILE_HEADER_SIZE + INFO_HEADER_SIZE);

	return fileHeader;
}

unsigned char* createBitmapInfoHeader (int height, int width)
{
	static unsigned char infoHeader[] = {
		0,0,0,0, /// header size
		0,0,0,0, /// image width
		0,0,0,0, /// image height
		0,0,     /// number of color planes
		0,0,     /// bits per pixel
		0,0,0,0, /// compression
		0,0,0,0, /// image size
		0,0,0,0, /// horizontal resolution
		0,0,0,0, /// vertical resolution
		0,0,0,0, /// colors in color table
		0,0,0,0, /// important color count
	};

	infoHeader[ 0] = (unsigned char)(INFO_HEADER_SIZE);
	infoHeader[ 4] = (unsigned char)(width      );
	infoHeader[ 5] = (unsigned char)(width >>  8);
	infoHeader[ 6] = (unsigned char)(width >> 16);
	infoHeader[ 7] = (unsigned char)(width >> 24);
	infoHeader[ 8] = (unsigned char)(height      );
	infoHeader[ 9] = (unsigned char)(height >>  8);
	infoHeader[10] = (unsigned char)(height >> 16);
	infoHeader[11] = (unsigned char)(height >> 24);
	infoHeader[12] = (unsigned char)(1);
	infoHeader[14] = (unsigned char)(BYTES_PER_PIXEL*8);

	return infoHeader;
}

int hh_poll_cq(struct rping_cb *rdma_ctx)
{
	void* cq_ctx;
	struct ibv_cq* cq_event;
	uint32_t ret = 0;

	while (1) {
		if ((ret = ibv_poll_cq(rdma_ctx->cq, HH_RX_REPOST_WQE_CNT, rdma_ctx->wc)) < 0) {
			printf(">>> CQ Poll FAILED: ret=%d\n", ret);
			return -1;
		}
		if (ret > 0) {
			printf(">>> CQ Poll: Retrieved %u completions\n", ret);
			for (int i = 0; i < ret && i < 3; i++) {  // Print first 3
				printf("    WC[%d]: wr_id=%lu, status=%d (%s), opcode=%d, byte_len=%u\n",
				       i, rdma_ctx->wc[i].wr_id, 
				       rdma_ctx->wc[i].status,
				       ibv_wc_status_str(rdma_ctx->wc[i].status),
				       rdma_ctx->wc[i].opcode,
				       rdma_ctx->wc[i].byte_len);
			}
		}
		return ret;
	}
}

int hh_post_rx_wqes(struct rping_cb *ctx, int size, int cnt)
{
	int i, ret = 0, r = ctx->rx_posted;
	uint32_t free_wqe;
	struct ibv_recv_wr *bad_wr;

	free_wqe = ctx->rq_depth - ctx->rx_posted + ctx->rx_compl;
	if (free_wqe < cnt) {
		DEBUG_LOG("No enuf wqes to post all the requested. Posting only %d\n", free_wqe);
		cnt = free_wqe;
	}
	
	printf("\n=== POST RECEIVE WQEs (rdma_post_recv) ===\n");
	printf(">>> Posting %d receive WQEs:\n", cnt);
	printf("    Buffer size: %d bytes\n", size);
	printf("    Buffer address: %p\n", ctx->hh_va);
	printf("    MR lkey: 0x%x\n", ctx->hh_mr->lkey);
	printf("    MR rkey: 0x%x\n", ctx->hh_mr->rkey);
	printf("    Free WQEs: %u\n", free_wqe);
	printf("    Already posted: %u\n", ctx->rx_posted);
	printf("    Already completed: %u\n", ctx->rx_compl);
	
	for (i = 0; i < cnt; i++) {
		if (i == 0 || i == cnt-1) {  // Print first and last
			printf("    WQE[%d]: wr_id=%lu, addr=%p, length=%d\n",
			       i, (uintptr_t)(r + i), ctx->hh_va, size);
		}
		
		ret = rdma_post_recv(ctx->cm_id, (void *)(uintptr_t)(r + i),
					ctx->hh_va, size, ctx->hh_mr);
		if (ret < 0) {
			printf("HH post rx failed at WQE %d\n", i);
			break;
		}
	}
	ctx->rx_posted += i;
	printf(">>> Successfully posted %d WQEs, Total posted: %d\n", i, ctx->rx_posted);
	return i;
}

int poll_cq(struct rping_cb *cb, int id)
{
        struct ibv_wc wc;
        int ret;
	struct ibv_cq* cq_event;
	void* cq_ctx;

        while(1) {
		if (ibv_get_cq_event(cb->channel, &cq_event, &cq_ctx))
			continue;
		if (ibv_req_notify_cq(cb->cq, 0))
			continue;
		if ((ret = ibv_poll_cq(cb->cq, 1, &wc)) != 1)
			continue;

		if (wc.status != IBV_WC_SUCCESS) {
			printf("ernic wc status = %#x (%s), id = %lx:%d\n",
				wc.status,
				ibv_wc_status_str(wc.status),
				wc.wr_id, id);
			return -1;
		}

		if (wc.wr_id == id)
			break;
        }
	return 0;
}

int rping_test_client(struct rping_cb *cb);
static int rping_cma_event_handler(struct rdma_cm_id *cma_id,
				    struct rdma_cm_event *event)
{
	int ret = 0;
	struct rping_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %s cma_id %p (%s)\n",
		  rdma_event_str(event->event), cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			cb->state = ERROR;
			perror("rdma_resolve_route");
			sem_post(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("Connection established for QP #%d\n",
			cma_id->qp->qp_num);
		/*
		 * Server will wake up when first RECV completes.
		 */
		cb->state = CONNECTED;
		cb->cm_chnl_en = 0;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		DEBUG_LOG("cma event %s, error %d\n",
			rdma_event_str(event->event), event->status);
		sem_post(&cb->sem);
		ret = -1;
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		cb->state = DISCONNECTED;
		cb->disc_state = DISCONNECTED;
		rping_test.done_cnt++;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printf("cma detected device removal!!!!\n");
		cb->state = ERROR;
		sem_post(&cb->sem);
		ret = -1;
		break;

	default:
		DEBUG_LOG("unhandled event: %s, ignoring\n",
			rdma_event_str(event->event));
		break;
	}
	return ret;
}


static int rping_setup_wr(struct rping_cb *cb)
{
	printf("\n=== MEMORY REGISTRATION (ibv_reg_mr) ===\n");
	
	// Receive buffer registration
	printf(">>> Registering RECV buffer:\n");
	printf("    Address: %p\n", &cb->recv_buf);
	printf("    Size: %zu bytes\n", sizeof cb->recv_buf);
	printf("    Access flags: IBV_ACCESS_LOCAL_WRITE (0x%x)\n", IBV_ACCESS_LOCAL_WRITE);
	
	cb->recv_mr = ibv_reg_mr(cb->pd, &cb->recv_buf, sizeof cb->recv_buf,
				IBV_ACCESS_LOCAL_WRITE);
	if (!cb->recv_mr) {
		fprintf(stderr, "recv_buf reg_mr failed\n");
		return -EFAULT;
	}
	
	printf(">>> RECV MR registered successfully:\n");
	printf("    lkey: 0x%x\n", cb->recv_mr->lkey);
	printf("    rkey: 0x%x\n", cb->recv_mr->rkey);
	printf("    addr: %p\n", cb->recv_mr->addr);
	printf("    length: %zu\n", cb->recv_mr->length);
	
	// Send buffer registration
	printf("\n>>> Registering SEND buffer:\n");
	printf("    Address: %p\n", &cb->send_buf);
	printf("    Size: %zu bytes\n", sizeof cb->send_buf);
	printf("    Access flags: 0 (no remote access)\n");
	
	cb->send_mr = ibv_reg_mr(cb->pd, &cb->send_buf, sizeof cb->send_buf, 0);
	if (!cb->send_mr) {
		fprintf(stderr, "send_buf reg_mr failed\n");
		ibv_dereg_mr(cb->recv_mr);
		return -EFAULT;
	}
	printf(">>> SEND MR registered successfully:\n");
	printf("    lkey: 0x%x\n", cb->send_mr->lkey);
	printf("    rkey: 0x%x\n", cb->send_mr->rkey);
	printf("    addr: %p\n", cb->send_mr->addr);
	printf("    length: %zu\n", cb->send_mr->length);

	// Setup scatter-gather lists
	printf("\n>>> Setting up Scatter-Gather Lists:\n");
	cb->recv_sgl.addr = (uint64_t) (unsigned long) &cb->recv_buf;
	cb->recv_sgl.length = sizeof cb->recv_buf;
	cb->recv_sgl.lkey =  cb->recv_mr->lkey;
	printf("    RECV SGL: addr=0x%lx, length=%u, lkey=0x%x\n",
	       cb->recv_sgl.addr, cb->recv_sgl.length, cb->recv_sgl.lkey);
	
	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = (uint64_t) (unsigned long) &cb->send_buf;
	cb->send_sgl.length = sizeof cb->send_buf;
	cb->send_sgl.lkey = cb->send_mr->lkey;
	printf("    SEND SGL: addr=0x%lx, length=%u, lkey=0x%x\n",
	       cb->send_sgl.addr, cb->send_sgl.length, cb->send_sgl.lkey);

	cb->sq_wr.opcode = IBV_WR_SEND;
	cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;
	
	printf("    Send WR: opcode=%d (IBV_WR_SEND), flags=0x%x (IBV_SEND_SIGNALED)\n",
	       cb->sq_wr.opcode, cb->sq_wr.send_flags);
	
	return 0;
}

static int rping_setup_buffers(struct rping_cb *cb)
{
	int ret, r = rand();

	if (ret = rping_setup_wr(cb))
		return ret;

	cb->wr_id = rand();
	ret = rdma_post_recv(cb->cm_id, (void *)(uintptr_t)cb->wr_id,
					&cb->recv_buf, sizeof(cb->recv_buf), cb->recv_mr);
	if (ret < 0) {
		printf("Failed to post recv %s:%d\n", __func__, __LINE__);
		return ret;
	}

	DEBUG_LOG("allocated & registered buffers...\n");
	return 0;
}


static int rping_create_qp(struct rping_cb *cb)
{
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = HH_SQ_DEPTH;
	init_attr.cap.max_recv_wr = HH_CQ_RQ_DEPTH;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;

	printf("\n=== QUEUE PAIR CREATION (rdma_create_qp) ===\n");
	printf(">>> QP Init Attributes:\n");
	printf("    max_send_wr: %u\n", init_attr.cap.max_send_wr);
	printf("    max_recv_wr: %u\n", init_attr.cap.max_recv_wr);
	printf("    max_send_sge: %u\n", init_attr.cap.max_send_sge);
	printf("    max_recv_sge: %u\n", init_attr.cap.max_recv_sge);
	printf("    qp_type: %d (IBV_QPT_RC = Reliable Connection)\n", init_attr.qp_type);
	printf("    send_cq: %p\n", cb->cq);
	printf("    recv_cq: %p\n", cb->cq);

	ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
	if (!ret) {
		cb->qp = cb->cm_id->qp;
		printf(">>> QP created successfully:\n");
		printf("    QP number: %u\n", cb->qp->qp_num);
		printf("    QP context: %p\n", cb->qp->context);
		printf("    QP state: (initial state)\n");
	} else {
		printf(">>> QP creation FAILED: %d\n", ret);
	}

	return ret;
}


static void rping_free_qp(struct rping_cb *cb)
{
	ibv_destroy_qp(cb->qp);
	ibv_destroy_cq(cb->cq);
	ibv_destroy_comp_channel(cb->channel);
	ibv_dealloc_pd(cb->pd);
}

static int rping_setup_qp(struct rping_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;

	cb->pd = ibv_alloc_pd(cm_id->verbs);
	if (!cb->pd) {
		printf("ibv_alloc_pd failed\n");
		return errno;
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	cb->channel = ibv_create_comp_channel(cm_id->verbs);
	if (!cb->channel) {
		printf("ibv_create_comp_channel failed\n");
		ret = errno;
		goto err1;
	}
	DEBUG_LOG("created channel %p\n", cb->channel);

	cb->cq = ibv_create_cq(cm_id->verbs, HH_CQ_RQ_DEPTH, NULL,
				cb->channel, 0);
	if (!cb->cq) {
		printf( "ibv_create_cq failed\n");
		ret = errno;
		goto err2;
	}

	cb->rq_depth = HH_CQ_RQ_DEPTH;
	DEBUG_LOG("created cq %p\n", cb->cq);

	ret = ibv_req_notify_cq(cb->cq, 0);
	if (ret) {
		printf( "ibv_create_cq failed\n");
		ret = errno;
		goto err3;
	}

	ret = rping_create_qp(cb);
	if (ret) {
		perror("rdma_create_qp");
		goto err3;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;

err3:
	ibv_destroy_cq(cb->cq);
err2:
	ibv_destroy_comp_channel(cb->channel);
err1:
	ibv_dealloc_pd(cb->pd);
	return ret;
}

static void *data_thread(void *arg)
{
	struct rping_test_info *rping_info = arg;
	struct rping_cb *cb;
	int ret, i;

	while(1) {
		for (i = 0; i < rping_info->qp_cnt; i++) {
			cb = &rping_info->cb[i];
			if (cb->cm_established && (!cb->test_done)) {
				/* start the data transfers */
       				ret = rping_test_client(cb);
       				if (!ret)
					printf("Successfully exchanged context of app QP #%d\n", i);
#if 0
				cb->cm_established = 0;
#endif
				cb->test_done = 1;
				/* Once the test is over, just disable the channel to
					avoid hearing for events on the channel */
				cb->cm_chnl_en = 0;
			}
		}
		pthread_yield();
	}
}

static void *disc_thread(void *arg)
{
	struct rping_test_info *rping_info = arg;
	struct rping_cb *cb;
	int ret, i, qp_cnctd_cnt = 0;

	DEBUG_LOG("%s started\n", __func__);
	while (1) {
		for (i = 0; i < rping_info->qp_cnt; i++) {
			cb = &rping_info->cb[i];
			if ((!cb->disc_rx) && cb->disc_check_en) {
				ret = rdma_get_cm_event(cb->cm_channel, &cb->event);
				if (ret) {
					printf("rdma_get_cm_event failed CB=%#x", (uint32_t) (uintptr_t)cb);
					cb->disc_rx = 1;
					continue;
				}

				ret = rping_cma_event_handler(cb->event->id, cb->event);
				rdma_ack_cm_event(cb->event);
				if (ret) {
					printf("Ack cm event failed for %#x\n", (uint32_t)(uintptr_t)cb);
					cb->disc_rx = 1;
				}
				if (cb->disc_state == DISCONNECTED) {
					cb->disc_rx = 1;
					qp_cnctd_cnt++;
					if (qp_cnctd_cnt >= rping_info->qp_cnt) {
						return NULL;
					}
				}
			}
		}
	}
}

static void *cm_thread(void *arg)
{
	struct rping_test_info *rping_info = arg;
	struct rping_cb *cb;
	int ret, i, qp_cnctd_cnt = 0;

	DEBUG_LOG("cm thread started\n");
	while (1) {
		for (i = 0; i < rping_info->qp_cnt; i++) {
			cb = &rping_info->cb[i];
			if ((!cb->cm_fail) && cb->cm_chnl_en) {
				ret = rdma_get_cm_event(cb->cm_channel, &cb->event);
				if (ret) {
					printf("rdma_get_cm_event failed CB=%#x", (uint32_t) (uintptr_t)cb);
					cb->cm_fail = 1;
					continue;
				}

				ret = rping_cma_event_handler(cb->event->id, cb->event);
				rdma_ack_cm_event(cb->event);
				if (ret) {
					printf("Ack cm event failed for %#x\n", (uint32_t)(uintptr_t)cb);
					cb->cm_fail = 1;
				}
				if (cb->state == CONNECTED) {
					qp_cnctd_cnt++;
					cb->disc_check_en = 1;
					if (qp_cnctd_cnt >= rping_info->qp_cnt) {
						return NULL;
					}
				}
			}
		}
	}
}

static void *cq_thread(void *arg)
{
	struct rping_cb *cb = arg;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret, i;

	while (!cb->rx_wqe_ready);
	DEBUG_LOG("cq_thread started.\n");
	while (1) {
		 if (!cb->rx_posted && !cb->rx_compl) {
			pthread_yield();
		}
		if ((!cb->cq_fail) && cb->cm_established) {
			ret = hh_poll_cq(cb);
			if (ret <= 0) {
				continue;
			}

			cb->rx_compl += ret;
			DEBUG_LOG("Total completions rx are %d\n", cb->rx_compl);
			ret = hh_post_rx_wqes(cb, cb->hh_data_size, HH_RX_REPOST_WQE_CNT);
		}
	}
}
/*
static void *pauseplaythread(void* arg)
{
	char *ipaddr = (char *)arg;
	int err;
	static int toggle=0;
	int connfd, i;
	char buff[MAX], c = 0;
	//int sockfd;
	struct sockaddr_in servaddr, cli;

	// socket create and verification
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, (const char*)1, sizeof(int));
	if (sockfd == -1) {
		printf("TCP: socket creation failed...\n");
		exit(0);
	}
	else
		printf("TCP: Socket successfully created..\n");
	bzero(&servaddr, sizeof(servaddr));
	// assign IP, PORT
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(ipaddr);
	servaddr.sin_port = htons(PORT);

	// connect the client socket to server socket
	if (connect(sockfd, (SA*)&servaddr, sizeof(servaddr)) != 0) {
		printf("TCP: connection with the server failed...\n");
		exit(0);
	}
	else
		printf("TCP: connected to the server..\n");
	bzero(buff, sizeof(buff));
	while(1) {
		i=0;
		uint8_t inc_Data = 0;
		c=getche();
		if(c=='S' || c=='s'){
			buff[0] = 'S';
			for( i = 1; i < MAX; i++){
				buff[i] = '0' + inc_Data++;
				inc_Data %=10;
			}
			toggle ^= 1;
			if(toggle)
				printf("-Data transfer paused.\n");
			else
				printf("-Data transfer resumed.\n");
			int er = send(sockfd, buff, MAX,0);
			if(er < 0)
				printf("Unable to send message\n");
		}
		else if (c =='Q' || c == 'q'){
			if(!toggle){
				printf("-Stop data transfer and Press Q to exit\n");
				continue;
			}
			buff[0] = 'Q';
			for( i = 1; i < MAX; i++){
                                buff[i] = '0' + inc_Data++;
                                inc_Data %=10;
                        }
			printf("-Sending connection close request\n");
			int er = send(sockfd, buff, MAX,0);
                        if(er < 0)
                                printf("Unable to send message\n");
			else
				printf("Connection closed!!!\n");
		}
		else{
			printf("-Invalid Keyboard Interrupt.\n");
		}
	}
}
*/
static void rping_format_send(struct rping_cb *cb, char *buf, struct ibv_mr *mr)
{
	struct rping_rdma_info *info = &cb->send_buf;

	info->buf = htobe64((uint64_t) (unsigned long) buf);
	info->rkey = htobe32(mr->rkey);
	info->size = htobe32(rping_test.size);
	info->iter_cnt = htobe32(cb->count);
	info->qp_cnt = htobe32(rping_test.qp_cnt);
	DEBUG_LOG("RDMA addr %" PRIx64" rkey %x len %d\n",
		  be64toh(info->buf), be32toh(info->rkey), be32toh(info->size));
}

static void free_cb(struct rping_cb *cb)
{
	free(cb);
}

void set_wr_id(uint64_t *buf, int id)
{
	/* for both send & recv wr, first 64B are wr id */
	*buf = id;
}
int rping_test_client(struct rping_cb *cb)
{
	int i, ret = 0;
	int r = cb->wr_id, val = 1;
	struct hh_send_fmt *hh_data;

	printf("\n=== CLIENT DATA EXCHANGE SEQUENCE ===\n");
	
	// Wait for initial receive
	printf(">>> Waiting for initial data from server...\n");
	ret = poll_cq(cb, r);
	if (ret < 0) {
		printf("Poll CQ failed %s:%d\n", __func__, __LINE__);
		return ret;
	}

	hh_data = &cb->recv_buf;
	printf(">>> Received server parameters:\n");
	printf("    Transfer size: %d kB (%d bytes)\n", 
	       hh_data->tf_siz, hh_data->tf_siz * 1024);
	printf("    Opcode: %d\n", hh_data->opcode);
	printf("    QP count: %d\n", hh_data->qp_cnt);
	printf("    WQE count: %d\n", hh_data->wqe_cnt);
	
	totalDataTS = hh_data->tf_siz * 1024;
	r++;

	if ((!hh_data->tf_siz) || (hh_data->tf_siz > 8388608)) { //original value is 8192
		printf("Requested data transfer size %d kB is not supported\n",
			hh_data->tf_siz);
		return -EINVAL;
	}
	
	// Calculate buffer size
	if( hh_data->tf_siz != 4050)
		cb->hh_data_size = hh_data->tf_siz * 1024 * 10;
	else{
		dataStart = 64;
		cb->hh_data_size = ((hh_data->tf_siz * 1024 ) + dataStart ) * 10;
	}
	
	printf(">>> Allocating data buffer:\n");
	printf("    Calculated size: %u bytes (%.2f MB)\n", 
	       cb->hh_data_size, cb->hh_data_size / (1024.0 * 1024.0));
	printf("    Data start offset: %u bytes\n", dataStart);
	
	/* Fill Send SGE */
	cb->hh_va = malloc(cb->hh_data_size);
	if (!cb->hh_va) {
		printf("Failed to alloc hw hs buffer\n");
		return -ENOMEM;
	}
	printf("    Allocated address: %p\n", cb->hh_va);
	
	printf("\n>>> Registering data buffer with driver:\n");
	printf("    Address: %p\n", cb->hh_va);
	printf("    Size: %u bytes\n", cb->hh_data_size);
	printf("    Access flags: 0x%x (LOCAL_WRITE | REMOTE_READ | REMOTE_WRITE)\n",
	       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	
	cb->hh_mr = ibv_reg_mr(cb->cm_id->pd, cb->hh_va, cb->hh_data_size,
				IBV_ACCESS_LOCAL_WRITE |
				IBV_ACCESS_REMOTE_READ |
				IBV_ACCESS_REMOTE_WRITE);
	if (!cb->hh_mr) {
		printf("reg mr failed for hh buffer\n");
		return -EFAULT;
	}
	
	printf(">>> Data buffer MR registered:\n");
	printf("    lkey: 0x%x\n", cb->hh_mr->lkey);
	printf("    rkey: 0x%x\n", cb->hh_mr->rkey);
	printf("    Virtual address: 0x%lx\n", (uint64_t)(uintptr_t)cb->hh_va);
	
	virtaddr =(uint64_t) cb->hh_va;
	
	// Prepare send buffer with RDMA info
	cb->send_buf.buf = htobe64(((uint64_t)(uintptr_t)cb->hh_va));
	cb->send_buf.rkey = ntohl(cb->hh_mr->rkey);
	
	printf("\n>>> Preparing to send RDMA info to server:\n");
	printf("    Buffer address (network byte order): 0x%lx\n", htobe64(((uint64_t)(uintptr_t)cb->hh_va)));
	printf("    RKey (host byte order): 0x%x\n", cb->hh_mr->rkey);
	printf("    RKey (network byte order): 0x%x\n", ntohl(cb->hh_mr->rkey));
	
	if (hh_data->opcode == HH_RDMA_SEND_OP) {
		printf("\n>>> Setting up for RDMA SEND operation:\n");
		cb->wc = malloc(HH_RX_REPOST_WQE_CNT * sizeof (struct ibv_wc));
		if (!cb->wc) {
			printf("Failed to create WC entries\n");
			return -ENOMEM;
		}
		printf("    Allocated %d work completion entries\n", HH_RX_REPOST_WQE_CNT);
		
		/* create a per QP cq thread */
		ret = pthread_create(&cb->cq_thread, NULL, cq_thread,
                                        (void *)cb);
		if (ret) {
			printf("Failed to create cq thread\n");
			return ret;
		}
		printf("    Created CQ polling thread\n");

		ret = hh_post_rx_wqes(cb, cb->hh_data_size, cb->rq_depth);
		if (ret < 0) {
			printf("Unable to post %d Rx WQEs\n", cb->rq_depth);
			return ret;
		}
		printf("    Posted %d receive WQEs\n", ret);
	}

	cb->hh_opcode = hh_data->opcode;

	printf("\n=== SENDING RDMA INFO (rdma_post_send) ===\n");
	printf(">>> Send Work Request:\n");
	printf("    wr_id: %lu\n", (uintptr_t)r);
	printf("    Buffer: %p (send_buf)\n", &cb->send_buf);
	printf("    Length: %zu bytes\n", sizeof(cb->send_buf));
	printf("    MR: %p (lkey=0x%x)\n", cb->send_mr, cb->send_mr->lkey);
	printf("    Flags: IBV_SEND_SIGNALED (0x%x)\n", IBV_SEND_SIGNALED);
	printf(">>> Send buffer contents:\n");
	printf("    buf (remote addr): 0x%lx\n", be64toh(cb->send_buf.buf));
	printf("    rkey: 0x%x\n", be32toh(cb->send_buf.rkey));
	printf("    size: %u\n", be32toh(cb->send_buf.size));
	printf("    iter_cnt: %u\n", be32toh(cb->send_buf.iter_cnt));
	printf("    qp_cnt: %u\n", be32toh(cb->send_buf.qp_cnt));
	
	ret = rdma_post_send(cb->cm_id, (void *)(uintptr_t)r, &cb->send_buf,
					sizeof(cb->send_buf),
					cb->send_mr, IBV_SEND_SIGNALED);
	if (ret < 0) {
		printf("send failed %s:%d\n", __func__, __LINE__);
		return ret;
	}
	printf(">>> Send posted successfully, waiting for completion...\n");

	ret = poll_cq(cb, r);
	if (ret < 0) {
		printf("Poll CQ failed %s:%d\n", __func__, __LINE__);
		return ret;
	}
	printf(">>> Send completed successfully\n");

	cb->rx_wqe_ready = 1;
	printf(">>> Client ready to receive data\n");

	return ret;
}

static int rping_connect_client(struct rping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 16;
	conn_param.initiator_depth = 16;
	conn_param.retry_count = 7;

	printf("\n=== RDMA CONNECTION (rdma_connect) ===\n");
	printf(">>> Connection parameters:\n");
	printf("    Responder resources: %u\n", conn_param.responder_resources);
	printf("    Initiator depth: %u\n", conn_param.initiator_depth);
	printf("    Retry count: %u\n", conn_param.retry_count);
	printf("    CM ID: %p\n", cb->cm_id);
	printf("    QP number: %u\n", cb->cm_id->qp->qp_num);

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		perror("rdma_connect");
		return ret;
	}
	printf(">>> Connection request sent, waiting for establishment...\n");

	sem_wait(&cb->sem);
	if (cb->state != CONNECTED) {
		printf("wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	/* Enable data transasctions */
	cb->cm_established = 1;
	printf(">>> Connection ESTABLISHED\n");
	printf("    Local QP: %u\n", cb->cm_id->qp->qp_num);
	printf("    State: CONNECTED (%d)\n", cb->state);
	DEBUG_LOG("rmda_connect successful\n");
	return 0;
}

static int rping_bind_client(struct rping_cb *cb)
{
	int ret;

	if (rping_test.sin.ss_family == AF_INET)
		((struct sockaddr_in *) &rping_test.sin)->sin_port = rping_test.port;
	else
		((struct sockaddr_in6 *) &rping_test.sin)->sin6_port = rping_test.port;

	if (rping_test.ssource.ss_family) 
		ret = rdma_resolve_addr(cb->cm_id, (struct sockaddr *) &rping_test.ssource,
					(struct sockaddr *) &rping_test.sin, 2000);
	else
		ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *) &rping_test.sin, 2000);

	if (ret) {
		printf("rdma_resolve_addr failed");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != ROUTE_RESOLVED) {
		printf( "waiting for addr/route resolution state %d\n",
			cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static int rping_setup_connection(struct rping_cb *cb)
{
	struct ibv_recv_wr *bad_wr;
	int ret;
	static int i = 0;

	cb->cm_channel = rdma_create_event_channel();
	if (!cb->cm_channel) {
		ret = errno;
		printf("event channel creation failed for app qp #%d\n", i);
		rping_test.done_cnt++;
		return -EFAULT;
	}

	ret = rdma_create_id(cb->cm_channel, &cb->cm_id, cb, RDMA_PS_TCP);
	if (ret) {
		printf("ID creation failed for app qp #%d\n", i);
		rping_test.done_cnt++;
		return -EFAULT;
	}
	DEBUG_LOG("created cm_id %p\n", cb->cm_id);
	cb->cm_chnl_en = 1;

	ret = rping_bind_client(cb);
	if (ret)
		return ret;

	ret = rping_setup_qp(cb, cb->cm_id);
	if (ret)
		return ret;

	ret = rping_setup_buffers(cb);
	if (ret)
		goto err1;

#if 0
	ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printf("ibv_post_recv failed: %d at %s:%d\n", ret, __func__, __LINE__);
		goto err1;
	}
#endif

	ret = rping_connect_client(cb);
	if (ret) {
		printf("connect error %d\n", ret);
		goto err1;
	}

	i++;
	return 0;
err4:
	rdma_disconnect(cb->cm_id);
err1:
	rping_free_qp(cb);

	i++;
	return ret;
}

static int get_addr(char *dst, struct sockaddr *addr)
{
	struct addrinfo *res;
	int ret;

	ret = getaddrinfo(dst, NULL, NULL, &res);
	if (ret) {
		printf("getaddrinfo failed (%s) - invalid hostname or IP address\n", gai_strerror(ret));
		return ret;
	}

	if (res->ai_family == PF_INET)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
	else if (res->ai_family == PF_INET6)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in6));
	else
		ret = -1;
	
	freeaddrinfo(res);
	return ret;
}

static void usage(const char *name)
{
	printf("\t-I\t:Source address to bind to for client.\n");
	printf("\t-d\t:debug prints\n");
	printf("\t-a <ip-addr> \t: Server IP address\n");
	printf("\t-p <port> \t:port number\n");
	printf("\t-D <data integrity> \t:0-No data integrity check, 1-Raw Data Pattern, Default:0\n");
}

int main(int argc, char *argv[])
{
	struct rping_cb *cb;
	char *end;
	int op, i = 0;
	int ret = 0, data[3];
	float qp_bw, tot_bw = 0;
	int persistent_server = 0;
	unsigned long long int clk_cnt;
	
	static int dataIntegrity=0;
	char *tcpIP;

	printf("\n");
	printf("========================================\n");
	printf("RDMA CLIENT APPLICATION STARTING\n");
	printf("========================================\n");

	rping_test.size = 64;
	rping_test.sin.ss_family = PF_INET;
	rping_test.port = htobe16(7176);
	rping_test.qp_cnt = 1;
	rping_test.cq_init = 0;
	rping_test.count = 0;

	opterr = 0;

	if (argc == 1) {
		usage(argv[0]);
		return 0;
	}

	printf("\n>>> Parsing command line arguments:\n");
	while ((op=getopt(argc, argv, "I:p:d:a:h:D:")) != -1) {
		switch (op) {
		case 'a':
			ret = get_addr(optarg, (struct sockaddr *) &rping_test.sin);
			tcpIP = (char *)optarg;
			printf("    Server address: %s\n", optarg);
		break;
		case 'h':
			usage(argv[0]);
			return 0;
		case 'I':
			ret = get_addr(optarg, (struct sockaddr *) &rping_test.ssource);
			printf("    Source address: %s\n", optarg);
			break;
		case 'p':
			rping_test.port = htobe16(atoi(optarg));
			printf("    Port: %d\n", atoi(optarg));
			DEBUG_LOG("port %d\n", rping_test.port);
			break;
		case 'D':
			dataIntegrity=atoi(optarg);
			if( dataIntegrity > 2 || dataIntegrity < 0){
				usage("rping");
				return -EINVAL;
			}
			printf("    Data integrity mode: %d\n", dataIntegrity);
			break;
		case 'd':
			debug++;
			printf("    Debug enabled: level %d\n", debug);
			break;
		default:
			printf("No option found %c\n", op);
			usage("rping");
			return -EINVAL;
		}
	}

	printf("\n>>> Application configuration:\n");
	printf("    Number of QPs: %d\n", rping_test.qp_cnt);
	printf("    Default buffer size: %d bytes\n", rping_test.size);
	printf("    Data integrity: %d\n", dataIntegrity);

	i = sizeof(struct rping_cb) * rping_test.qp_cnt;
	printf("\n>>> Allocating %d bytes for %d control blocks\n", i, rping_test.qp_cnt);
	
	rping_test.cb = malloc(i);
	if (!rping_test.cb) {
		printf("Failed to allocate memory for contexts\n");
		return -ENOMEM;
	}
	memset(rping_test.cb, 0, i);

	printf("\n>>> Creating worker threads:\n");
	
	/* create reqd threads */
	ret = pthread_create(&rping_test.cmthread, NULL, cm_thread, &rping_test);
	if (ret) {
		perror("failed to create cm thread");
		free(rping_test.cb);
		return -1;
	}
	printf("    CM thread created\n");

	ret = pthread_create(&rping_test.disc_thread, NULL, disc_thread, &rping_test);
	if (ret) {
		perror("failed to create cm thread");
		free(rping_test.cb);
		return -1;
	}
	printf("    Disconnect thread created\n");

	ret = pthread_create(&rping_test.datathread, NULL, data_thread, &rping_test);
	if (ret) {
		perror("Failed to create data transfer threads\n");
		free(rping_test.cb);
		return -1;
	}
	printf("    Data transfer thread created\n");
	
/*	ret = pthread_create(&rping_test.pauseplaythread, NULL, pauseplaythread, tcpIP);
	if (ret) {
		printf("Failed to create pause play threads\n");
		free(rping_test.cb);
		return -1;
        }
	printf("    Pause/Play control thread created\n");
*/	
	printf("\n>>> Setting up %d Queue Pair(s):\n", rping_test.qp_cnt);
	for (i = 0; i < rping_test.qp_cnt; i++) {
		printf("\n--- Setting up QP #%d ---\n", i);
		cb = &rping_test.cb[i];
		memset(cb, 0, sizeof(struct rping_cb));
		/* reset to defaults */
		sem_init(&cb->sem, 0, 0);
		cb->qp_num = i;
		cb->cm_fail = cb->cq_fail = 0;
		cb->cm_chnl_en = 0;
		cb->cm_established = 0;
		cb->rx_compl = cb->rx_posted = 0;
		cb->count = rping_test.count;
		cb->test_done = 0;
		cb->disc_rx = 0;
		cb->disc_state = 0;
		cb->disc_check_en = 0;

		ret = rping_setup_connection(cb);
		if (ret) {
			printf("QP connection failed for application QP#%d\n", i);
			/* Disable looking for events on this channel */
			cb->cm_chnl_en = 0;
			rping_test.done_cnt++;
		} else {
			printf("QP #%d setup successful\n", i);
		}
		DEBUG_LOG("init done for app qp #%d\n", i);
	}
	
	printf("\n>>> Waiting for all QPs to complete...\n");
	while(rping_test.done_cnt < rping_test.qp_cnt);
	
	printf("\n========================================\n");
	printf("ALL QPs COMPLETED - Processing data\n");
	printf("========================================\n");
	
	close(sockfd);
	if(dataIntegrity){
		printf("\n>>> Data integrity check mode: %d\n", dataIntegrity);
		printf("    Total data received: %u bytes\n", totalDataTS);
		printf("    Virtual address: 0x%lx\n", virtaddr);
		printf("    Data start offset: %u bytes\n", dataStart);
		printf("    Number of frames: %d\n", MAX_IMAGES);
		FILE *fptr;
		uint8_t *ptrVirtAddr =(uint8_t *)virtaddr;
		uint8_t imageId=0;
		if(dataIntegrity==1){
                        char* textFileName[10] = {
                                (char*)"Frame0.txt",
                                (char*)"Frame1.txt",
                                (char*)"Frame2.txt",
                                (char*)"Frame3.txt",
                                (char*)"Frame4.txt",
                                (char*)"Frame5.txt",
                                (char*)"Frame6.txt",
                                (char*)"Frame7.txt",
                                (char*)"Frame8.txt",
                                (char*)"Frame9.txt",
                        };

			while(imageId < MAX_IMAGES){
				fptr=fopen(textFileName[imageId],"w");	
				for( i = dataStart ; i < totalDataTS+dataStart ; i++){
					fprintf(fptr,"%02x ", ptrVirtAddr[i]);
				}

				fclose(fptr);
				imageId++;
				ptrVirtAddr+=(totalDataTS+dataStart);
			}
		}
		else if (dataIntegrity == 2){
			char* imageFileName[10] = { 
				(char*)"Frame0.bmp",
				(char*)"Frame1.bmp",
				(char*)"Frame2.bmp",
				(char*)"Frame3.bmp",
				(char*)"Frame4.bmp",
				(char*)"Frame5.bmp",
				(char*)"Frame6.bmp",
				(char*)"Frame7.bmp",
				(char*)"Frame8.bmp",
				(char*)"Frame9.bmp",
			};
			int height = 1080;
			int width = 1920;
			int ptpLength = 10, secCounter=4;
			int k,j=0,i=0;
			FILE *fptr;
			uint8_t* hex_rgb=(uint8_t*) malloc(width*height*3*sizeof(uint8_t*));
			if (hex_rgb == NULL)
			{
				fprintf(stderr, "Out of memory");
				exit(0);
			}
			unsigned char*** image = (unsigned char***)malloc(height * sizeof(unsigned char**));
			if (image == NULL)
			{
				fprintf(stderr, "Out of memory");
				exit(0);
			}
			for (k = 0; k < height; k++)
			{
				image[k] = (unsigned char**)malloc(width * sizeof(unsigned char*));

				if (image[k] == NULL)
				{
					fprintf(stderr, "Out of memory");
					exit(0);
				}

				for (j = 0; j < width; j++)
				{
					image[k][j] = (unsigned char*)malloc(BYTES_PER_PIXEL * sizeof(unsigned char));
					if (image[k][j] == NULL)
					{
						fprintf(stderr, "Out of memory");
						exit(0);
					}
				}
			}
			while(imageId < MAX_IMAGES){
				j=0;
				uint64_t seconds=0;
				uint32_t nanoSeconds=0;
				printf("%s\tPTP: Seconds=", imageFileName[imageId]);
				for (i = ptpLength-1; i >= secCounter; i--)
					seconds=(seconds<<8)|ptrVirtAddr[i];
				printf("%u", seconds);
				printf("\tNano Seconds=");
				for (i = secCounter-1; i >=0 ; i--)
					nanoSeconds=(nanoSeconds<<8)|ptrVirtAddr[i];
				printf("%u", nanoSeconds);
				printf("\n");
				for (i = dataStart; i < totalDataTS+dataStart; i+=4){

					uint16_t Y1=ptrVirtAddr[i];
					uint16_t Cb=ptrVirtAddr[i+1];
					uint16_t Y2=ptrVirtAddr[i+2];
					uint16_t Cr=ptrVirtAddr[i+3];

					*(hex_rgb+j+0)=(uint8_t)roundup((Y1 + 1.4075 * (Cr-128)));//R
					*(hex_rgb+j+1)=(uint8_t)roundup((Y1 - 0.3455 * (Cb-128) - (0.7169 * (Cr-128))));//G
					*(hex_rgb+j+2)=(uint8_t)roundup((Y1 + 1.7790 * (Cb-128)));//B

					*(hex_rgb+j+3)=(uint8_t)roundup((Y2 + 1.4075 * (Cr-128)));//R
					*(hex_rgb+j+4)=(uint8_t)roundup((Y2 - 0.3455 * (Cb-128) - (0.7169 * (Cr-128))));//G
					*(hex_rgb+j+5)=(uint8_t)roundup((Y2 + 1.7790 * (Cb-128)));//B
					j+=6;
				}
				for (k = 0; k <height; k++) {
					for (j = 0; j <width; j++) {
						uint32_t off=(k*width*BYTES_PER_PIXEL)+(j*BYTES_PER_PIXEL);

						image[k][j][2] = (unsigned char) *(hex_rgb+off+0);          	//red
						image[k][j][1] = (unsigned char) *(hex_rgb+off+1);           	//green
						image[k][j][0] = (unsigned char) *(hex_rgb+off+2);		//blue
					}
				}
				generateBitmapImage((unsigned char***) image, height, width, imageFileName[imageId]);
				printf("%s Image generated!!\n", imageFileName[imageId]);
				imageId++;
				ptrVirtAddr+=(totalDataTS+dataStart);
			}
			for (k = 0; k < height; k++){
				for (j = 0; j < width; j++) {
					free(image[k][j]);
				}
				free(image[k]);
			}
			free(image);
			free(hex_rgb);
		}
	}
	printf("\n========================================\n");
	printf("APPLICATION COMPLETED\n");
	printf("========================================\n\n");
	return 0;
}
