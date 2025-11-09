#ifndef MPP_MAIN_H
#define MPP_MAIN_H

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <iterator>
#include <chrono>


extern "C" {
#include "libswscale/swscale.h"
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include <libavutil/mathematics.h>
#include <libavutil/time.h>

}

#include "mpp_enc.h"
#include "mpp_rtsp_server.h"
// 定义一些常量
#define FRAME_WIDTH 1280 // 帧宽度
#define FRAME_HEIGHT 720 // 帧高度
#define FRAME_BGR888_SIZE FRAME_WIDTH*FRAME_HEIGHT*3
#define FRAME_YUV420P_SIZE FRAME_BGR888_SIZE/2
#define FRAME_RATE 30 // 帧率

using namespace std;

class mpp_main
{
private:
    AVFormatContext *out_fmt_ctx = NULL; //输出格式上下文
    AVStream *out_stream = NULL; //输出流
    AVFrame *frame = NULL; //输入帧
    AVCodecContext* codec_ctx = NULL;
    whale::vision::MppEncoder mppenc;

    mpp_rtsp_server *rtsp_server;
    uint8_t *my_yuv420p_buf;

    string get_ipAdress();
    int64_t get_current_time();
    int init_ffmpeg(string output_url);
    int packet_alloc(AVBufferRef **buf, int size);
    void array_to_AVPacket(uint8_t* h264_buf, int buf_size, AVPacket *packet);
    int ori_data_to_yuv420p(uint8_t* ori_data, uint8_t *yuv420p_buf);
    int encoder(uint8_t* ori_data, char *h264_buf);
    void release_ffmpeg();
public:
    mpp_main(/* args */);
    ~mpp_main();

    int push_stream(uint8_t *ori_data);

};


#endif

