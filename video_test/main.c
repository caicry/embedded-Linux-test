#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>             /* getopt_long() */

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/dvb/video.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

static void start_capturing(void);
static void stop_capturing(void);
enum io_method {
        IO_METHOD_READ,
        IO_METHOD_MMAP,
        IO_METHOD_USERPTR,
};

struct buffer {
        void   *start;
        size_t  length;
};

static char            *dev_name;
static enum io_method   io = IO_METHOD_MMAP;
static int              fd = -1;
struct buffer          *buffers;
static unsigned int     n_buffers;
static int              out_buf;
static const char      *out_path;
static FILE            *out_fp;
static const char      *fb_buf=NULL;
static int              force_format;
static int              frame_count = 70;

static struct fb_fix_screeninfo finfo;
static struct fb_var_screeninfo vinfo;
static void errno_exit(const char *s)
{
        fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
        exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg)
{
        int r;

        do {
                r = ioctl(fh, request, arg);
        } while (-1 == r && EINTR == errno);

        return r;
}

static float tbl_1_4075X[513];
static float tbl_0_3455X[513];
static float tbl_0_7169X[513];
static float tbl_1_7990X[513];

static float *tbl_1_4075X_Center = &tbl_1_4075X[256];
static float *tbl_0_3455X_Center = &tbl_0_3455X[256];
static float *tbl_0_7169X_Center = &tbl_0_7169X[256];
static float *tbl_1_7990X_Center = &tbl_1_7990X[256];

static int tbl_128[256];
void yuv_CreateFloatTable(float *tbl, float fMul)
{
	for (int i=0; i<512;i++)
	{
		tbl[i] = fMul * (i - 256);
		//tbl[i] = fMul * i;
	}
}

void yuv_float_table_init()
{
	yuv_CreateFloatTable(tbl_1_4075X, 1.4075);
	yuv_CreateFloatTable(tbl_0_3455X, 0.3455);
	yuv_CreateFloatTable(tbl_0_7169X, 0.7169);
	yuv_CreateFloatTable(tbl_1_7990X, 1.7990);

	for (int i=0; i<256; i++)
		tbl_128[i] = (i-128);
}


#define CAL_YUV_R(y, cb, cr) (y + 1.4075*(cr-128))
#define CAL_YUV_G(y, cb, cr) (y - 0.3455 * (cb - 128) - (0.7169 * (cr - 128)))
#define CAL_YUV_B(y, cb, cr) (y + 1.7790 * (cb - 128))

#define CAL_YUV_R1(y, cb, cr) (y + tbl_1_4075X_Center[tbl_128[cr]])
#define CAL_YUV_G1(y, cb, cr) (y - tbl_0_3455X_Center[tbl_128[cb]] - (tbl_0_7169X_Center[tbl_128[cr]]))
#define CAL_YUV_B1(y, cb, cr) (y + tbl_1_7990X_Center[tbl_128[cb]])
//#define CAL_YUV_R(y, u, v) (y+ 0 * u + 1.13983 * v)
//#define CAL_YUV_G(y, u, v) (y -0.39465 * u + -0.58060 * v)
//#define CAL_YUV_B(y, u, v) (y -0.03211 * u + 0 * v)
static inline unsigned char clamp(float v)
{
	if (v>255)
		return 255;
	else if (v<0)
		return 0;
	else
		return (unsigned char)v;
}

static unsigned int get_time_now()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec*1000 + tv.tv_usec/1000;
}

#define CLIP(color) (unsigned char)(((color) > 0xFF) ? 0xff : (((color) < 0) ? 0 : (color)))


int yuv2rgb(const unsigned char *pYuv, int length)
{
	char *ptr;
	unsigned char r,g,b;
	unsigned short v, *pShort, *pShort2;
	unsigned char *pMem1;
	int cb0, cr0;
	float y0, y1;
	unsigned int t1;

	static char *pBuf = NULL;
	static char *pBuf2 = NULL;
	if (!pBuf)
	{
		pBuf = (char*)malloc(finfo.smem_len);
		pBuf2 = (char*)malloc(finfo.smem_len);
	}

	ptr = pBuf;
	pShort = (unsigned short*)pBuf;

	t1 = get_time_now();
#if 1
	for (int i=0; i<length;)
	{
		y0  = (unsigned char)pYuv[i++];
		cb0 = (unsigned char)pYuv[i++];
		y1  = (unsigned char)pYuv[i++];
		cr0 = (unsigned char)pYuv[i++];

		//             11             7  6    5         0
		// R5 R4 R3 R2 R1 G6 G5 G4 G3 G2 G1 B5 B4 B3 B2 B1 
		r = clamp(CAL_YUV_R1(y0, cb0, cr0));
		g = clamp(CAL_YUV_G1(y0, cb0, cr0));
		b = clamp(CAL_YUV_B1(y0, cb0, cr0));
		v = ((((r>>3)&0x1f)<<11) | ((g>>2&0x3f)<<5) | ((b>>3)&0x1f));
		*pShort++=v;

		r = clamp(CAL_YUV_R1(y1, cb0, cr0));
		g = clamp(CAL_YUV_G1(y1, cb0, cr0));
		b = clamp(CAL_YUV_B1(y1, cb0, cr0));
		v = ((((r>>3)&0x1f)<<11) | ((g>>2&0x3f)<<5) | ((b>>3)&0x1f));
		*pShort++= v;

	}
#else
	for (int i=0; i<length;i+=4)
	{
		y0  = (unsigned char)*pYuv++;
		int u = (unsigned char)*pYuv++;
		y1  = (unsigned char)*pYuv++;
		int v = (unsigned char)*pYuv++;

		int u1 = (((u - 128) << 7) +  (u - 128)) >> 6;
		int rg = (((u - 128) << 1) +  (u - 128) +
			((v - 128) << 2) + ((v - 128) << 1)) >> 3;
		int v1 = (((v - 128) << 1) +  (v - 128)) >> 1;

		//             11             7  6    5         0
		// R5 R4 R3 R2 R1 G6 G5 G4 G3 G2 G1 B5 B4 B3 B2 B1 
		r = CLIP(y0 + v1);
		g = CLIP(y0 - rg);
		b = CLIP(y0 + u1);
		v = ((((r>>3)&0x1f)<<11) | ((g>>2&0x3f)<<5) | ((b>>3)&0x1f));
		*pShort++=v;

		r = CLIP(y1 + v1);
		g = CLIP(y1 - rg);
		b = CLIP(y1 + u1);
		v = ((((r>>3)&0x1f)<<11) | ((g>>2&0x3f)<<5) | ((b>>3)&0x1f));
		*pShort++= v;
	}
#endif

	printf("fly:%d ", get_time_now()-t1);

	// rorate 90
	//  ------------------           ---------------------
	// |(x,y)                       |              (x1,y1)
	// |                            |
	// |                            |
	// |                            |
	// |                            |
	// |                            |

	t1 = get_time_now();
	pMem1 = pBuf;
	pShort = (unsigned short *)pBuf;
	pShort2 = (unsigned short *)pBuf2;
	for (int i=0; i<320; i++)
		for (int j=240; j>0; j--)
	{
		*pShort2++ = *(unsigned short*)(pMem1 + 2*j*320+ 2*i);
	}

	memcpy(fb_buf, pBuf2, finfo.smem_len);
	printf("rorate:%d\n", get_time_now()-t1);
	return 0;
}

static void process_image(const void *p, int size)
{
	static int i=0;

	printf("Capture %d  ", i++);
	if (out_buf) {
		fseek(out_fp, 0, SEEK_SET);
		fwrite(p, size, 1, out_fp);
		//fwrite(p, size, 1, stdout);
		return ;
	}

	if (fb_buf && size==320*240*2)
	{
		yuv2rgb(p, size);
		return;
	}

        fflush(stderr);
        fprintf(stderr, ".");
        fflush(stdout);
}

static int read_frame(void)
{
        struct v4l2_buffer buf;
        unsigned int i;

        switch (io) {
        case IO_METHOD_READ:
                if (-1 == read(fd, buffers[0].start, buffers[0].length)) {
                        switch (errno) {
                        case EAGAIN:
                                return 0;

                        case EIO:
                                /* Could ignore EIO, see spec. */

                                /* fall through */

                        default:
                                errno_exit("read");
                        }
                }

                process_image(buffers[0].start, buffers[0].length);
                break;

        case IO_METHOD_MMAP:
                CLEAR(buf);

                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;

                if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
                        switch (errno) {
                        case EAGAIN:
                                return 0;

                        case EIO:
                                /* Could ignore EIO, see spec. */

                                /* fall through */

                        default:
                                errno_exit("VIDIOC_DQBUF");
                        }
                }

                assert(buf.index < n_buffers);

                process_image(buffers[buf.index].start, buf.bytesused);

                if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                        errno_exit("VIDIOC_QBUF");
                break;

        case IO_METHOD_USERPTR:
                CLEAR(buf);

                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_USERPTR;

                if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
                        switch (errno) {
                        case EAGAIN:
                                return 0;

                        case EIO:
                                /* Could ignore EIO, see spec. */

                                /* fall through */

                        default:
                                errno_exit("VIDIOC_DQBUF");
                        }
                }

                for (i = 0; i < n_buffers; ++i)
                        if (buf.m.userptr == (unsigned long)buffers[i].start
                            && buf.length == buffers[i].length)
                                break;

                assert(i < n_buffers);

                process_image((void *)buf.m.userptr, buf.bytesused);

                if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                        errno_exit("VIDIOC_QBUF");
                break;
        }

        return 1;
}

static void mainloop(void)
{
        unsigned int count;

        count = frame_count;

        while (count-- > 0) {
                for (;;) {
                        fd_set fds;
                        struct timeval tv;
                        int r;

                        FD_ZERO(&fds);
                        FD_SET(fd, &fds);

                        /* Timeout. */
                        tv.tv_sec = 2;
                        tv.tv_usec = 0;
						int t1 = get_time_now();

                        r = select(fd + 1, &fds, NULL, NULL, &tv);
						printf("select:%d ", get_time_now() - t1);

                        if (-1 == r) {
                                if (EINTR == errno)
                                        continue;
                                errno_exit("select");
                        }

                        if (0 == r) {
                                fprintf(stderr, "select timeout\n");
                                exit(EXIT_FAILURE);
                        }

                        if (read_frame())
                                break;
                        /* EAGAIN - continue select loop. */
                }
        }
}

static void stop_capturing(void)
{
        enum v4l2_buf_type type;

        switch (io) {
        case IO_METHOD_READ:
                /* Nothing to do. */
                break;

        case IO_METHOD_MMAP:
        case IO_METHOD_USERPTR:
                type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
                        errno_exit("VIDIOC_STREAMOFF");
                break;
        }
}

static void start_capturing(void)
{
        unsigned int i;
        enum v4l2_buf_type type;

        switch (io) {
        case IO_METHOD_READ:
                /* Nothing to do. */
                break;

        case IO_METHOD_MMAP:
                for (i = 0; i < n_buffers; ++i) {
                        struct v4l2_buffer buf;

                        CLEAR(buf);
                        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                        buf.memory = V4L2_MEMORY_MMAP;
                        buf.index = i;

                        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                                errno_exit("VIDIOC_QBUF");
                }
                type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
                        errno_exit("VIDIOC_STREAMON");
                break;

        case IO_METHOD_USERPTR:
                for (i = 0; i < n_buffers; ++i) {
                        struct v4l2_buffer buf;

                        CLEAR(buf);
                        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                        buf.memory = V4L2_MEMORY_USERPTR;
                        buf.index = i;
                        buf.m.userptr = (unsigned long)buffers[i].start;
                        buf.length = buffers[i].length;

                        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                                errno_exit("VIDIOC_QBUF");
                }
                type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
                        errno_exit("VIDIOC_STREAMON");
                break;
        }
}

static void uninit_device(void)
{
        unsigned int i;

        switch (io) {
        case IO_METHOD_READ:
                free(buffers[0].start);
                break;

        case IO_METHOD_MMAP:
                for (i = 0; i < n_buffers; ++i)
                        if (-1 == munmap(buffers[i].start, buffers[i].length))
                                errno_exit("munmap");
                break;

        case IO_METHOD_USERPTR:
                for (i = 0; i < n_buffers; ++i)
                        free(buffers[i].start);
                break;
        }

        free(buffers);
}

static void init_read(unsigned int buffer_size)
{
        buffers = calloc(1, sizeof(*buffers));

        if (!buffers) {
                fprintf(stderr, "Out of memory\n");
                exit(EXIT_FAILURE);
        }

        buffers[0].length = buffer_size;
        buffers[0].start = malloc(buffer_size);

        if (!buffers[0].start) {
                fprintf(stderr, "Out of memory\n");
                exit(EXIT_FAILURE);
        }
}

static void init_mmap(void)
{
        struct v4l2_requestbuffers req;

        CLEAR(req);

        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
                if (EINVAL == errno) {
                        fprintf(stderr, "%s does not support "
                                 "memory mapping\n", dev_name);
                        exit(EXIT_FAILURE);
                } else {
                        errno_exit("VIDIOC_REQBUFS");
                }
        }

        if (req.count < 2) {
                fprintf(stderr, "Insufficient buffer memory on %s\n",
                         dev_name);
                exit(EXIT_FAILURE);
        }

        buffers = calloc(req.count, sizeof(*buffers));

        if (!buffers) {
                fprintf(stderr, "Out of memory\n");
                exit(EXIT_FAILURE);
        }

        for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
                struct v4l2_buffer buf;

                CLEAR(buf);

                buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory      = V4L2_MEMORY_MMAP;
                buf.index       = n_buffers;

                if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
                        errno_exit("VIDIOC_QUERYBUF");

                buffers[n_buffers].length = buf.length;
                buffers[n_buffers].start =
                        mmap(NULL /* start anywhere */,
                              buf.length,
                              PROT_READ | PROT_WRITE /* required */,
                              MAP_SHARED /* recommended */,
                              fd, buf.m.offset);

                if (MAP_FAILED == buffers[n_buffers].start)
                        errno_exit("mmap");
        }
}

static void init_userp(unsigned int buffer_size)
{
        struct v4l2_requestbuffers req;

        CLEAR(req);

        req.count  = 4;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_USERPTR;

        if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
                if (EINVAL == errno) {
                        fprintf(stderr, "%s does not support "
                                 "user pointer i/o\n", dev_name);
                        exit(EXIT_FAILURE);
                } else {
                        errno_exit("VIDIOC_REQBUFS");
                }
        }

        buffers = calloc(4, sizeof(*buffers));

        if (!buffers) {
                fprintf(stderr, "Out of memory\n");
                exit(EXIT_FAILURE);
        }

        for (n_buffers = 0; n_buffers < 4; ++n_buffers) {
                buffers[n_buffers].length = buffer_size;
                buffers[n_buffers].start = malloc(buffer_size);

                if (!buffers[n_buffers].start) {
                        fprintf(stderr, "Out of memory\n");
                        exit(EXIT_FAILURE);
                }
        }
}

static void init_device(void)
{
        struct v4l2_capability cap;
        struct v4l2_cropcap cropcap;
        struct v4l2_crop crop;
        struct v4l2_format fmt;
        unsigned int min;

        if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
                if (EINVAL == errno) {
                        fprintf(stderr, "%s is no V4L2 device\n",
                                 dev_name);
                        exit(EXIT_FAILURE);
                } else {
                        errno_exit("VIDIOC_QUERYCAP");
                }
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
                fprintf(stderr, "%s is no video capture device\n",
                         dev_name);
                exit(EXIT_FAILURE);
        }

        switch (io) {
        case IO_METHOD_READ:
                if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
                        fprintf(stderr, "%s does not support read i/o\n",
                                 dev_name);
                        exit(EXIT_FAILURE);
                }
                break;

        case IO_METHOD_MMAP:
        case IO_METHOD_USERPTR:
                if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
                        fprintf(stderr, "%s does not support streaming i/o\n",
                                 dev_name);
                        exit(EXIT_FAILURE);
                }
                break;
        }


        /* Select video input, video standard and tune here. */


        CLEAR(cropcap);

        cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
                crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                crop.c = cropcap.defrect; /* reset to default */

                if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
                        switch (errno) {
                        case EINVAL:
                                /* Cropping not supported. */
                                break;
                        default:
                                /* Errors ignored. */
                                break;
                        }
                }
        } else {
                /* Errors ignored. */
        }


        CLEAR(fmt);

        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (force_format) {
                fmt.fmt.pix.width       = 240;
                fmt.fmt.pix.height      = 320;
                fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
                fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;
				fprintf(stderr, "\033[35m== %s %s %d ==\033[0m\n", __FILE__, __FUNCTION__, __LINE__);

				fprintf(stderr, "ret:%d\n", ioctl(fd, VIDIOC_TRY_FMT, &fmt));
                if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
                        errno_exit("VIDIOC_S_FMT");

                /* Note VIDIOC_S_FMT may change width and height. */
        } else {
                /* Preserve original settings as set by v4l2-ctl for example */
                if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
                        errno_exit("VIDIOC_G_FMT");
        }

        /* Buggy driver paranoia. */
        min = fmt.fmt.pix.width * 2;
        if (fmt.fmt.pix.bytesperline < min)
                fmt.fmt.pix.bytesperline = min;
        min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
        if (fmt.fmt.pix.sizeimage < min)
                fmt.fmt.pix.sizeimage = min;

		printf("width:%d height:%d pixelformat:%#x\n", fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);

        switch (io) {
        case IO_METHOD_READ:
                init_read(fmt.fmt.pix.sizeimage);
                break;

        case IO_METHOD_MMAP:
                init_mmap();
                break;

        case IO_METHOD_USERPTR:
                init_userp(fmt.fmt.pix.sizeimage);
                break;
        }
}

static void close_device(void)
{
        if (-1 == close(fd))
                errno_exit("close");

        fd = -1;
}

static void open_device(void)
{
        struct stat st;

        if (-1 == stat(dev_name, &st)) {
                fprintf(stderr, "Cannot identify '%s': %d, %s\n",
                         dev_name, errno, strerror(errno));
                exit(EXIT_FAILURE);
        }

        if (!S_ISCHR(st.st_mode)) {
                fprintf(stderr, "%s is no device\n", dev_name);
                exit(EXIT_FAILURE);
        }

        fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

        if (-1 == fd) {
                fprintf(stderr, "Cannot open '%s': %d, %s\n",
                         dev_name, errno, strerror(errno));
                exit(EXIT_FAILURE);
        }
}

static void usage(FILE *fp, int argc, char **argv)
{
        fprintf(fp,
                 "Usage: %s [options]\n\n"
                 "Version 1.3\n"
                 "Options:\n"
                 "-d | --device name   Video device name [%s]\n"
                 "-h | --help          Print this message\n"
                 "-m | --mmap          Use memory mapped buffers [default]\n"
                 "-r | --read          Use read() calls\n"
                 "-u | --userp         Use application allocated buffers\n"
                 "-o | --output        Outputs stream to stdout\n"
                 "-f | --format        Force format to 640x480 YUYV\n"
                 "-c | --count         Number of frames to grab [%i]\n"
                 "",
                 argv[0], dev_name, frame_count);
}

static const char short_options[] = "d:hmruo:fc:v:";

static const struct option
long_options[] = {
        { "device", required_argument, NULL, 'd' },
        { "help",   no_argument,       NULL, 'h' },
        { "mmap",   no_argument,       NULL, 'm' },
        { "read",   no_argument,       NULL, 'r' },
        { "userp",  no_argument,       NULL, 'u' },
        { "output", required_argument,       NULL, 'o' },
        { "format", no_argument,       NULL, 'f' },
        { "count",  required_argument, NULL, 'c' },
        { "video",  required_argument, NULL, 'v' },
        { 0, 0, 0, 0 }
};

static int vt_set_mode(int graphics)
{
	int fd, r;
	fd = open("/dev/tty0", O_RDWR | O_SYNC);
	if (fd < 0)
		return -1;
	r = ioctl(fd, KDSETMODE, (void*) (graphics ? KD_GRAPHICS : KD_TEXT));
	close(fd);
	return r;
}

int main(int argc, char **argv)
{
	int fb_fd;
	
	yuv_float_table_init();
	const char *fb_path=NULL;
        dev_name = "/dev/video0";

        for (;;) {
                int idx;
                int c;

                c = getopt_long(argc, argv,
                                short_options, long_options, &idx);

                if (-1 == c)
                        break;

                switch (c) {
                case 0: /* getopt_long() flag */
                        break;

                case 'd':
                        dev_name = optarg;
                        break;

                case 'h':
                        usage(stdout, argc, argv);
                        exit(EXIT_SUCCESS);

                case 'm':
                        io = IO_METHOD_MMAP;
                        break;

                case 'r':
                        io = IO_METHOD_READ;
                        break;

                case 'u':
                        io = IO_METHOD_USERPTR;
                        break;

                case 'o':
                        out_buf++;
						out_path = strdup(optarg);
                        break;

                case 'f':
                        force_format++;
                        break;

                case 'c':
                        errno = 0;
                        frame_count = strtol(optarg, NULL, 0);
                        if (errno)
                                errno_exit(optarg);
                        break;

				case 'v':
						fb_path = strdup(optarg);
						break;
                default:
                        usage(stderr, argc, argv);
                        exit(EXIT_FAILURE);
                }
        }

		if (access(out_path, F_OK))
			remove(out_path);
		out_fp = fopen(out_path, "w+");

		if (fb_path)
		{

			fb_fd= open(fb_path, O_RDWR);
			if (fb_fd>0)
			{
				ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
				ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);
				printf("[FB]x:%d y:%d bits_per_pixel:%d\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
				fb_buf = (char*)mmap(NULL, finfo.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
				vt_set_mode(1);
			}
		}

        open_device();
        init_device();
        start_capturing();
        mainloop();
        stop_capturing();
        uninit_device();
        close_device();
        fprintf(stderr, "\n");
		if (out_fp)
			fclose(out_fp);
		if (fb_fd>0)
		{
			if (fb_buf)
				munmap(fb_buf, finfo.smem_len);
			close(fb_fd);
		}
        return 0;
}
