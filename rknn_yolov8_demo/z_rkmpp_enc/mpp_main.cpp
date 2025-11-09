#include "mpp_main.h"

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>

#define USE_FFMPEG 0

mpp_main::mpp_main()
{
#if  USE_FFMPEG
    int ret = 0;
    string output_url =  "rtmp://" + get_ipAdress() + "/live/stream";
    ret = init_ffmpeg(output_url);
    if (ret == -1) fprintf(stderr, "init_ffmpeg failed\n");
#else
    rtsp_server = new mpp_rtsp_server();
#endif

    mppenc.MppEncdoerInit(FRAME_WIDTH, FRAME_HEIGHT, 30);
    my_yuv420p_buf = (uint8_t *)malloc(FRAME_YUV420P_SIZE);
}

mpp_main::~mpp_main()
{
    free(my_yuv420p_buf);

#if  USE_FFMPEG
    release_ffmpeg();
#endif
}

#if  USE_FFMPEG
/*获取本机IPv4地址*/
string mpp_main::get_ipAdress()
{
    string ip;
    int sock_fd, intrface;
    struct ifreq buf[INET_ADDRSTRLEN];
    struct ifconf ifc;
    if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {
        ifc.ifc_len = sizeof(buf);
        ifc.ifc_buf = (caddr_t)buf;
        if (!ioctl(sock_fd, SIOCGIFCONF, (char *)&ifc)) {
            intrface = ifc.ifc_len / sizeof(struct ifreq);
            while (intrface-- > 0) {
                if (!(ioctl(sock_fd, SIOCGIFADDR, (char *)&buf[intrface]))) {
                    ip = inet_ntoa(((struct sockaddr_in*)(&buf[intrface].ifr_addr))->sin_addr);
                    if (ip != "127.0.0.1") {
                        break;
                    }
                }
            }
        }
        close(sock_fd);
    }
    return ip;
}

/*获取当前时间（毫秒）*/
int64_t mpp_main::get_current_time()
{
    auto now = chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return chrono::duration_cast<chrono::milliseconds>(duration).count(); // 将duration转换为不同的单位
}

/*初始化输出流格式*/
int mpp_main::init_ffmpeg(string output_url)
{
    int ret;
    printf("output url:%s\n", output_url.c_str());

    avformat_network_init();
    //分配输出格式上下文
    if (avformat_alloc_output_context2(&out_fmt_ctx, NULL, "flv", output_url.c_str()) < 0) {
        fprintf(stderr, "avformat_alloc_output_context2 failed\n");
        return -1;
    }

    // 查找 H264 编码器
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    // 创建编码器上下文
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Could not allocate video codec context\n");
        return -1;
    }
    // 设置编码器的参数
    codec_ctx->codec_id = codec->id;
    codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx->width = FRAME_WIDTH; // 设置输出分辨率的宽度
    codec_ctx->height = FRAME_HEIGHT; // 设置输出分辨率的高度
    codec_ctx->time_base.num = 1;
    codec_ctx->time_base.den = FRAME_RATE; 
    //codec_ctx->gop_size = 12;

    int bit_rate = 300 * 1024 * 8;  //压缩后每秒视频的bit位大小 300kB
    //VBV
    codec_ctx->flags |= AV_CODEC_FLAG_QSCALE;
    codec_ctx->rc_min_rate = bit_rate / 2;
    codec_ctx->rc_max_rate = bit_rate / 2 + bit_rate;
    codec_ctx->bit_rate = bit_rate;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Could not open encoder\n");
        return -1;
    }

    //新建一个输出流
    out_stream = avformat_new_stream(out_fmt_ctx, codec);
    if (out_stream == NULL) {
        fprintf(stderr, "avformat_new_stream failed\n");
        return -1;
    }
    
    //分配内存
    out_stream->codecpar = avcodec_parameters_alloc();
    // 复制编码器参数到输出流
    if (avcodec_parameters_from_context(out_stream->codecpar, codec_ctx) < 0) {
        fprintf(stderr, "Could not copy codec parameters\n");
        return -1;
    }
    out_stream->codecpar->codec_tag = 0;
    out_stream->id = 0;
    //out_stream->index = 0;

    //打印输出格式信息
    printf("output information:\n");
    av_dump_format(out_fmt_ctx, 0, output_url.c_str(), 1);
    //打开输出URL
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        printf("avio_open output_url\n");
        if (avio_open(&out_fmt_ctx->pb, output_url.c_str(), AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "avio_open failed\n");
            return -1;
        }
    }

    // 写入输出文件头
    ret = avformat_write_header(out_fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "avformat_write_header failed\n");
        // 写入失败，打印错误信息
        char buf[1024];
        av_strerror(ret, buf, sizeof(buf));
        printf("Error occurred when writing header: %s\n", buf);
        return -1;
    }

    return 0;
}

int mpp_main::packet_alloc(AVBufferRef **buf, int size)
{
    int ret;
    if (size < 0 || size >= INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE)
        return AVERROR(EINVAL);

    ret = av_buffer_realloc(buf, size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (ret < 0)
        return ret;

    memset((*buf)->data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    return 0;
}

/*将h264数组转换为packet包*/
void mpp_main::array_to_AVPacket(uint8_t* h264_buf, int buf_size, AVPacket *packet)
{
    static int64_t last_time = 0;
    static int64_t pts = 0; //单位us
    int64_t current_time = 0;
    int64_t time = 0;
    
    if (!packet) {
        fprintf(stderr, "Could not allocate packet\n");
        exit(-1);
    }
    packet_alloc(&packet->buf, buf_size);
    memcpy(packet->buf->data, h264_buf, buf_size);
    packet->data = packet->buf->data;
    packet->size = buf_size;
    packet->stream_index = 0;

    if (mppenc.countIdx_ % 60 == 0) packet->flags = 1;
    else packet->flags = 0;

    current_time = get_current_time(); //获取当前时间（ms）
    if (last_time != 0) time = current_time - last_time;
    // 设置数据包的流索引和时间戳
    pts += time * 1000;
    packet->pts = pts;
    packet->dts = pts;
    last_time = current_time;
    /*时间戳转换*/
    av_packet_rescale_ts(packet, AV_TIME_BASE_Q, out_stream->time_base);

    //printf("set pts success, current pts:%ld\n", time);
}

/*释放内存*/
void mpp_main::release_ffmpeg()
{
    // 写入输出文件尾
    if (av_write_trailer(out_fmt_ctx) < 0) {
        fprintf(stderr, "av_write_trailer failed\n");
    }
    // 关闭输出URL
    if (out_fmt_ctx && !(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_close(out_fmt_ctx->pb);
    }
    // 释放输出格式上下文
    if (out_fmt_ctx) {
        avformat_free_context(out_fmt_ctx);
    }
    // 释放输入帧
    if (frame) {
        av_frame_free(&frame);
    }
}
#endif

/*推流*/
int mpp_main::push_stream(uint8_t *ori_data)
{
    static int i = 0;
    int ret = 0;
    int h264_buf_size;
    char *h264_buf = (char *)malloc(150000); //已知1280*720 普遍处于6000-8000
    AVPacket* pkt = NULL;
    /*mpp编码*/
    h264_buf_size = encoder(ori_data, h264_buf);
    // if (h264_buf_size > 12000) fprintf(stderr, "mpp encoder failed, buf size:%d i:%d\n", h264_buf_size, i);
    // else printf("mpp encoder success h264 buf size:%d\n", h264_buf_size);
    if (h264_buf_size == 0) {
        ret = -1;
        goto enc_failed;
    }
    i++;
    
#if USE_FFMPEG
    /*将h264数组转换为AVPacket包*/
    pkt = av_packet_alloc();
    array_to_AVPacket((uint8_t *)h264_buf, h264_buf_size, pkt);

    /*写入数据包到输出*/
    if (av_interleaved_write_frame(out_fmt_ctx, pkt) < 0) {
        fprintf(stderr, "Could not write packet to output\n");
        ret = -1;
    }     

    /*释放内存*/
    av_packet_free(&pkt);
#else
    rtsp_server->push_h264_stream(h264_buf, h264_buf_size); // 推流
#endif

enc_failed:
    free(h264_buf);
    return ret;
}


/*编码器*/
int mpp_main::encoder(uint8_t* ori_data, char *h264_buf)
{
    int ret = 0;
    int h264_buf_size = 0;
    /*使用RGA将原始数据转化为YUV420P*/
    //uint8_t *yuv420p_buf = new uint8_t[FRAME_YUV420P_SIZE];
    ret = ori_data_to_yuv420p(ori_data, my_yuv420p_buf);
    if (ret < 0) {
        fprintf(stderr, "ori_data_to_yuyv422 failed\n");
        return h264_buf_size;
    }

    /*将YUYV422使用mpp编码为h264*/
    mppenc.encode(my_yuv420p_buf, FRAME_YUV420P_SIZE, h264_buf, &h264_buf_size); //2-3ms //获取编码数据大小

    return h264_buf_size;
}

/*转换为yuyv420格式*/
#include "RgaUtils.h"
#include "im2d.hpp"
#include "dma_alloc.h"
int mpp_main::ori_data_to_yuv420p(uint8_t* ori_data, uint8_t *yuv420p_buf)
{
    int ret = 0;
    int src_width, src_height, src_format;
    int dst_width, dst_height, dst_format;
    uint8_t *src_buf, *dst_buf;
    int src_buf_size = FRAME_BGR888_SIZE, dst_buf_size = FRAME_YUV420P_SIZE;

    int src_dma_fd, dst_dma_fd;
    rga_buffer_t src_img, dst_img;
    rga_buffer_handle_t src_handle, dst_handle;
    querystring(RGA_VERSION);

    memset(&src_img, 0, sizeof(src_img));
    memset(&dst_img, 0, sizeof(dst_img));

    src_width = FRAME_WIDTH;
    src_height = FRAME_HEIGHT;
    src_format = RK_FORMAT_BGR_888;

    dst_width = FRAME_WIDTH;
    dst_height = FRAME_HEIGHT;
    dst_format = RK_FORMAT_YCbCr_420_P;

     /*
     * Allocate dma_buf within 4G from dma32_heap,
     * return dma_fd and virtual address.
     */
    ret = dma_buf_alloc(DMA_HEAP_DMA32_UNCACHE_PATCH, src_buf_size, &src_dma_fd, (void **)&src_buf);
    if (ret < 0) {
        printf("alloc src dma_heap buffer failed!\n");
        return -1;
    }

    ret = dma_buf_alloc(DMA_HEAP_DMA32_UNCACHE_PATCH, dst_buf_size, &dst_dma_fd, (void **)&dst_buf);
    if (ret < 0) {
        printf("alloc dst dma_heap buffer failed!\n");
        dma_buf_free(src_buf_size, &src_dma_fd, src_buf);
        return -1;
    }

    memset(dst_buf, 0x33, dst_buf_size);

    /*
     * Import the allocated dma_fd into RGA by calling
     * importbuffer_fd, and use the returned buffer_handle
     * to call RGA to process the image.
     */
    src_handle = importbuffer_fd(src_dma_fd, src_buf_size);
    dst_handle = importbuffer_fd(dst_dma_fd, dst_buf_size);
    if (src_handle == 0 || dst_handle == 0) {
        printf("import dma_fd error!\n");
        ret = -1;
        goto free_buf;
    }

    memcpy(src_buf, ori_data, src_buf_size);

    /*将输入输出的图像参数转化为统一的 rga_buffer_t 结构作为user API的输入参数*/
    src_img = wrapbuffer_handle(src_handle, src_width, src_height, src_format);
    dst_img = wrapbuffer_handle(dst_handle, dst_width, dst_height, dst_format);

    /*校验当前参数是否合法，并根据当前硬件情况判断硬件是否支持*/
    ret = imcheck(src_img, dst_img, {}, {});
    if (IM_STATUS_NOERROR != ret) {
        printf("%d, check error! %s", __LINE__, imStrError((IM_STATUS)ret));
        return -1;
    }

    /*图像格式转换*/
    ret = imcvtcolor(src_img, dst_img, src_format, dst_format);
    if (ret == IM_STATUS_SUCCESS) {
        // printf("RGA running success!\n");
    } else {
        printf("RGA running failed, %s\n", imStrError((IM_STATUS)ret));
        goto release_buffer;
    }

    memcpy(yuv420p_buf, dst_buf, dst_buf_size);

/*释放内存*/
release_buffer:
    if (src_handle)
        releasebuffer_handle(src_handle);
    if (dst_handle)
        releasebuffer_handle(dst_handle);

free_buf:
    dma_buf_free(src_buf_size, &src_dma_fd, src_buf);
    dma_buf_free(dst_buf_size, &dst_dma_fd, dst_buf);

    return ret;
}
