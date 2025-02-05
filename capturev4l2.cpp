#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <memory>

#include <jpeglib.h>

#define IMAGE_WIDTH_720P    1280
#define IMAGE_HEIGHT_720P   720

#define IMAGE_WIDTH_360P    640
#define IMAGE_HEIGHT_360P   360

int image_width = IMAGE_WIDTH_360P;
int image_height = IMAGE_HEIGHT_360P;

uint8_t *buffer;

/**
 * converts a YUYV raw buffer to a JPEG buffer.
 * input is in YUYV (YUV 422). output is JPEG binary.
 * from https://linuxtv.org/downloads/v4l-dvb-apis/V4L2-PIX-FMT-YUYV.html:
 *      Each four bytes is two pixels.
 *      Each four bytes is two Y's, a Cb and a Cr.
 *      Each Y goes to one of the pixels, and the Cb and Cr belong to both pixels.
 *
 * inspired by: http://stackoverflow.com/questions/17029136/weird-image-while-trying-to-compress-yuv-image-to-jpeg-using-libjpeg
 */
uint64_t YUYVtoJPEG(const uint8_t* input, const int width, const int height, uint8_t* &outbuffer) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_ptr[1];
    int row_stride;

    // uint8_t* outbuffer = NULL;
    uint64_t outlen = 0;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &outbuffer, &outlen);

    // jrow is a libjpeg row of samples array of 1 row pointer
    cinfo.image_width = width & -1;
    cinfo.image_height = height & -1;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr; //libJPEG expects YUV 3bytes, 24bit, YUYV --> YUV YUV YUV

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 92, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    std::vector<uint8_t> tmprowbuf(width * 3);

    JSAMPROW row_pointer[1];
    row_pointer[0] = &tmprowbuf[0];
    while (cinfo.next_scanline < cinfo.image_height) {
        unsigned i, j;
        unsigned offset = cinfo.next_scanline * cinfo.image_width * 2; //offset to the correct row
        for (i = 0, j = 0; i < cinfo.image_width * 2; i += 4, j += 6) { //input strides by 4 bytes, output strides by 6 (2 pixels)
            tmprowbuf[j + 0] = input[offset + i + 0]; // Y (unique to this pixel)
            tmprowbuf[j + 1] = input[offset + i + 1]; // U (shared between pixels)
            tmprowbuf[j + 2] = input[offset + i + 3]; // V (shared between pixels)
            tmprowbuf[j + 3] = input[offset + i + 2]; // Y (unique to this pixel)
            tmprowbuf[j + 4] = input[offset + i + 1]; // U (shared between pixels)
            tmprowbuf[j + 5] = input[offset + i + 3]; // V (shared between pixels)
        }
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    return outlen;
}

uint64_t YV12toJPEG(const uint8_t* input, const int width, const int height, uint8_t* &outbuffer) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_ptr[1];
    int row_stride;

    // uint8_t* outbuffer = NULL;
    uint64_t outlen = 0;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &outbuffer, &outlen);

    // jrow is a libjpeg row of samples array of 1 row pointer
    cinfo.image_width = width & -1;
    cinfo.image_height = height & -1;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr; //libJPEG expects YUV 3bytes, 24bit, YUYV --> YUV YUV YUV

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 92, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    std::vector<uint8_t> tmprowbuf(width * 3);

    JSAMPROW row_pointer[1];
    row_pointer[0] = &tmprowbuf[0];
    while (cinfo.next_scanline < cinfo.image_height) {
        unsigned i, j;
        for (i = 0, j = 0; i < cinfo.image_width; i += 1, j += 3) { //input strides by 4 bytes, output strides by 6 (2 pixels)
            tmprowbuf[j + 0] = input[i + cinfo.image_width * cinfo.next_scanline]; // Y (unique to this pixel)
            tmprowbuf[j + 1] = input[i / 2 + cinfo.image_width * cinfo.image_height + (cinfo.image_width / 2) * (cinfo.next_scanline / 2)]; // U (shared between pixels)
            tmprowbuf[j + 2] = input[i / 2 + cinfo.image_width * cinfo.image_height + (cinfo.image_width / 2) * (cinfo.image_height / 2) + (cinfo.image_width / 2) * (cinfo.next_scanline / 2)]; // V (shared between pixels)
        }
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    return outlen;
}

static int xioctl(int fd, int request, void *arg)
{
    int r;

    do
    {
        r = ioctl(fd, request, arg);
    }
    while (-1 == r && EINTR == errno);

    return r;
}

int print_caps(int fd, int pixel_format)
{
    struct v4l2_capability caps = {};
    if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &caps))
    {
        perror("Querying Capabilities");
        return 1;
    }

    printf( "Driver Caps:\n"
            "  Driver: \"%s\"\n"
            "  Card: \"%s\"\n"
            "  Bus: \"%s\"\n"
            "  Version: %d.%d\n"
            "  Capabilities: %08x\n",
            caps.driver,
            caps.card,
            caps.bus_info,
            (caps.version>>16)&&0xff,
            (caps.version>>24)&&0xff,
            caps.capabilities);

    int support_grbg10 = 0;

    struct v4l2_fmtdesc fmtdesc = {0};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    char fourcc[5] = {0};
    char c, e;
    printf("  FMT : CE Desc\n--------------------\n");
    while (0 == xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc))
    {
        strncpy(fourcc, (char *)&fmtdesc.pixelformat, 4);
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_SGRBG10)
            support_grbg10 = 1;
        c = fmtdesc.flags & 1? 'C' : ' ';
        e = fmtdesc.flags & 2? 'E' : ' ';
        printf("  %s: %c%c %s\n", fourcc, c, e, fmtdesc.description);
        fmtdesc.index++;
    }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = image_width;
    fmt.fmt.pix.height = image_height;

    if(pixel_format == 0) fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    if(pixel_format == 1) fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    if(pixel_format == 2) fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;

    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
    {
        perror("Setting Pixel Format");
        return 1;
    }

    strncpy(fourcc, (char *)&fmt.fmt.pix.pixelformat, 4);
    printf( "Selected Camera Mode:\n"
            "  Width: %d\n"
            "  Height: %d\n"
            "  PixFmt: %s\n"
            "  Field: %d\n"
            "  support_grbg10: %s\n",
            fmt.fmt.pix.width,
            fmt.fmt.pix.height,
            fourcc,
            fmt.fmt.pix.field,
            support_grbg10 ? "support_grbg10" : "not support_grbg10");
    return 0;
}

int init_mmap(int fd)
{
    struct v4l2_requestbuffers req = {0};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req))
    {
        perror("Requesting Buffer");
        return 1;
    }

    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if(-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
    {
        perror("Querying Buffer");
        return 1;
    }

    buffer = mmap (NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    printf("Length: %d\nAddress: %p\n", buf.length, buffer);
    // printf("Image Length: %d\n", buf.bytesused);

    return 0;
}

int capture_image(int fd, int format)
{
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;

    if(-1 == xioctl(fd, VIDIOC_QBUF, &buf))
    {
        perror("Query Buffer");
        return 1;
    }

    if(-1 == xioctl(fd, VIDIOC_STREAMON, &buf.type))
    {
        perror("Start Capture");
        return 1;
    }

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = {0};
    tv.tv_sec = 2;

    int r = select(fd+1, &fds, NULL, NULL, &tv);
    if(-1 == r)
    {
        perror("Waiting for Frame");
        return 1;
    }

    if(-1 == xioctl(fd, VIDIOC_DQBUF, &buf))
    {
        perror("Retrieving Frame");
        return 1;
    }

    cv::Mat cv_img;

    if (format == 0 ){
        // Decode YUYV
        cv::Mat img = cv::Mat(cv::Size(image_width, image_height), CV_8UC2, buffer);
        cv::cvtColor(img, cv_img, cv::COLOR_YUV2RGB_YVYU);
    }

    if (format == 1) {
        // Decode MJPEG
        cv::_InputArray pic_arr(buffer, image_width * image_height * 3);
        cv_img = cv::imdecode(pic_arr, cv::IMREAD_UNCHANGED);
    }

    if (format == 2) {
        // Decode RGB3
        cv_img = cv::Mat(cv::Size(image_width, image_height), CV_8UC3, buffer);
    }

//    cv::imshow("view", cv_img);

    // You may do image processing here
    // Begin OpenCV Code
    // ..........
    // End  OpenCV Code

    cv::Mat raw_img;

    // RGB to YV12
    cv::cvtColor(cv_img, raw_img, cv::COLOR_RGB2YUV_YV12);

    // YV12 to JPEG
    uint8_t* outbuffer = NULL;
    cv::Mat input = raw_img.reshape(1, raw_img.total() * raw_img.channels());
    std::vector<uint8_t> vec = raw_img.isContinuous()? input : input.clone();
    uint64_t outlen = YV12toJPEG(vec.data(), image_width, image_height, outbuffer);

    // printf("libjpeg produced %ld bytes\n", outlen);

    // Write JPEG to file
    std::vector<uint8_t> output = std::vector<uint8_t>(outbuffer, outbuffer + outlen);
    std::ofstream ofs("output.jpg", std::ios_base::binary);
    ofs.write((const char*) &output[0], output.size());
    ofs.close();

//    cv::waitKey(1);

    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s [options]\n", argv0);
    fprintf(stderr, "Available options are\n");
    fprintf(stderr,
            " -f <format>    Select frame format\n\t"
            "0 = V4L2_PIX_FMT_YUYV\n\t"
            "1 = V4L2_PIX_FMT_MJPEG\n\t"
            "2 = V4L2_PIX_FMT_RGB24\n");
    fprintf(stderr,
            " -r <resolution> Select frame resolution:\n\t"
            "0 = 360p, VGA (640x360)\n\t"
            "1 = 720p, (1280x720)\n");
    fprintf(stderr, " -v device V4L2 Video Capture device\n");
    fprintf(stderr, " -h Print this help screen and exit\n");
}

int main(int argc, char* argv[])
{
    int fd, opt;

    int pixel_format = 0;     /* V4L2_PIX_FMT_YUYV */
    char *v4l2_devname = "/dev/video0";

    while ((opt = getopt(argc, argv, "f:hv:r:")) != -1) {
        switch (opt) {
            case 'f':
                if (atoi(optarg) < 0 || atoi(optarg) > 2) {
                    usage(argv[0]);
                    return 1;
                }

                pixel_format = atoi(optarg);
                break;

            case 'h':
                usage(argv[0]);
                return 1;

            case 'v':
                v4l2_devname = optarg;
                printf("Use video device: %s\n", v4l2_devname);
                break;

            case 'r':
                if (atoi(optarg) < 0 || atoi(optarg) > 1) {
                    usage(argv[0]);
                    return 1;
                }

                if (atoi(optarg) == 0) {
                    image_width = IMAGE_WIDTH_360P;
                    image_height = IMAGE_HEIGHT_360P;
                } else {
                    image_width = IMAGE_WIDTH_720P;
                    image_height = IMAGE_HEIGHT_720P;
                }
                break;

            default:
                printf("Invalid option '-%c'\n", opt);
                usage(argv[0]);
                return 1;
        }
    }

    fd = open(v4l2_devname, O_RDWR);
    if (fd == -1)
    {
        perror("Opening video device");
        return 1;
    }

    if(print_caps(fd, pixel_format))
    {
        return 1;
    }

    if(init_mmap(fd))
    {
        return 1;
    }

    if(capture_image(fd, pixel_format))
    {
        return 1;
    }

    if(capture_image(fd, pixel_format))
    {
      return 1;
    }

    close(fd);

    return 0;
}
