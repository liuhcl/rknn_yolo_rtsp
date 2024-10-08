// Copyright (c) 2021 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*-------------------------------------------
                Includes
-------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <vector>
#include <string>
#include <stdbool.h>

#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "rknnPool.hpp"
#include "ThreadPool.hpp"

#include "rknn_api.h"
#include "yolo.h"
#include "resize_function.h"
#include "rknn_demo_utils.h"
#include "mpp_main.h"

#include "jpeglib.h"
#include "jconfig.h"
#include "jmorecfg.h"

using std::queue;
using std::vector;
using namespace cv;	    //OpenCV标准库

char* RKNN_MODEL_PATH = (char *)"../yolov8_demo/model/yolov8n_c2f_drb.rknn";
#define THREAD_COUNT 1
#define VIDEO_DEVICE "/dev/video0" // 视频设备文件
#define FRAMEBUFFER_COUNT 2 //帧缓冲数量
#define IMAGE_WIDTH 1280 // 图像宽度
#define IMAGE_HEIGHT 720 // 图像高度

/*** 描述一个帧缓冲的信息 ***/
typedef struct cam_buf_info {
    unsigned short *start; //帧缓冲起始地址
    unsigned long length; //帧缓冲长度
} cam_buf_info;

static int v4l2_fd = -1; //摄像头设备文件描述符
static cam_buf_info buf_infos[FRAMEBUFFER_COUNT];

bool mjpeg_to_rgb(unsigned char* mjpeg_buf, int mjpeg_buf_len, unsigned char* RGB_buf);
static int v4l2_dev_init(const char *device);
static int v4l2_set_format(void);
static int v4l2_init_buffer(void);
static int v4l2_stream_on(void);

/*rtsp推流线程*/
static pthread_mutex_t rtsp_mutex; //定义互斥锁
static pthread_cond_t rtsp_cond; //定义条件变量
Mat rknn_img;               //全局共享资源
static void *rtsp_thread(void *arg)
{
    mpp_main *my_mpp = new mpp_main();
    printf("creat mpp object success\n");
    for (; ;) {
        pthread_mutex_lock(&rtsp_mutex);//上锁
        while (rknn_img.empty())
            pthread_cond_wait(&rtsp_cond, &rtsp_mutex);//等待条件满足

        my_mpp->push_stream(rknn_img.data);

        rknn_img.release();// 释放内存
        pthread_mutex_unlock(&rtsp_mutex);//解锁
    }
}

static void creat_rtsp_thread()
{
    pthread_t tid;
    int ret;
    /* 初始化互斥锁和条件变量 */
    pthread_mutex_init(&rtsp_mutex, NULL);
    pthread_cond_init(&rtsp_cond, NULL);
    /* 创建新线程 */
    ret = pthread_create(&tid, NULL, rtsp_thread, NULL);
    if (ret) {
        fprintf(stderr, "pthread_create error: %s\n", strerror(ret));
        exit(-1);
    }
}

int main(int argc, char **argv)
{
    /*时间*/
    struct timeval time;
    gettimeofday(&time, nullptr);
    long tmpTime, lopTime = time.tv_sec * 1000 + time.tv_usec / 1000;

    /* 初始化摄像头 */
    if (v4l2_dev_init(VIDEO_DEVICE))
        exit(EXIT_FAILURE);
    printf("camera init success\n");
    /* 设置格式 */
    if (v4l2_set_format())
        exit(EXIT_FAILURE);
    printf("camera set format success\n");
    /* 初始化帧缓冲：申请、内存映射、入队 */
    if (v4l2_init_buffer())
        exit(EXIT_FAILURE);
    printf("V4L2 init buffer success\n");
    /* 开启视频采集 */
    if (v4l2_stream_on())
        exit(EXIT_FAILURE);
    printf("V4L2 stream on\n");

    struct v4l2_buffer buf = {0};
    uint8_t *mjpeg_buf = (uint8_t *)malloc(IMAGE_WIDTH * IMAGE_HEIGHT * 3);
    uint8_t *rgb_buf = (uint8_t *)malloc(IMAGE_HEIGHT * IMAGE_WIDTH * 3);
    Mat img_rgb(IMAGE_HEIGHT, IMAGE_WIDTH, CV_8UC3);
    //namedWindow("rknn test");

    /*数据拷贝*/
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP; //mmap

    int frame_num = 0;
    int i = 0;

    // 存储多个rknn模型的实例
    vector<rknn_lite *> rkpool;
    // 线程池
    dpool::ThreadPool pool(THREAD_COUNT);
    // 线程队列
    queue<std::future<Mat>> futs;

    //初始化
    ioctl(v4l2_fd, VIDIOC_DQBUF, &buf); //出队
    memcpy(mjpeg_buf, buf_infos[0].start, buf.length);  
    mjpeg_to_rgb(mjpeg_buf, buf.length, rgb_buf);
    img_rgb.data = rgb_buf;
    ioctl(v4l2_fd, VIDIOC_QBUF, &buf); // 数据处理完之后、再入队、往复
    for (int j = 0; j < THREAD_COUNT; j++)
    {
        rknn_lite *ptr = new rknn_lite(RKNN_MODEL_PATH, j % 3);
        rkpool.push_back(ptr);
        ptr->ori_img = img_rgb;
        futs.push(pool.submit(&rknn_lite::interf, ptr)); //&(*ptr)-->ptr
        sleep(1);
    }
    printf("ThreadPool init success\n");
    creat_rtsp_thread();
    sleep(3);
    //printf("creat rtsp thread success\n");
    for( ; ; ) {
        for(buf.index = 0; buf.index < FRAMEBUFFER_COUNT; buf.index++) {
            ioctl(v4l2_fd, VIDIOC_DQBUF, &buf); //出队

            i = frame_num % THREAD_COUNT;

            /*把jpeg图像转换为RGB888图像*/
            bool to_rgb_success = true;
            memcpy(mjpeg_buf, buf_infos[buf.index].start, buf.length);  
            to_rgb_success = mjpeg_to_rgb(mjpeg_buf, buf.length, rgb_buf);
            if (to_rgb_success) {
                img_rgb.data = rgb_buf;

                pthread_mutex_lock(&rtsp_mutex);//上锁
                // 获取 futs 队列中的第一个元素，并等待它的异步操作完成，然后返回结果或者抛出异常
                rknn_img = futs.front().get();
                if (rknn_img.empty()) 
                    break;
                pthread_mutex_unlock(&rtsp_mutex);//解锁
                pthread_cond_signal(&rtsp_cond);//向条件变量发送信号

                //imwrite("rknn.jpg", rknn_img);
                futs.pop(); // 从队列中移除第一个元素
                rkpool[i]->ori_img = img_rgb;
                futs.push(pool.submit(&rknn_lite::interf, rkpool[i])); //加入一个元素
                frame_num++;
            }

            if(frame_num % 30 == 0){
                gettimeofday(&time, nullptr);
                tmpTime = time.tv_sec * 1000 + time.tv_usec / 1000;
                printf("平均帧率:\t%f帧\n", 30000.0 / (float)(tmpTime - lopTime));
                lopTime = tmpTime;
            }

            ioctl(v4l2_fd, VIDIOC_QBUF, &buf); // 数据处理完之后、再入队、往复
        }
    }

    printf("main thread exit\n");
    gettimeofday(&time, nullptr);

    // 释放剩下的资源
    while (!futs.empty())
    {
        // if (futs.front().get())
        // break;
        futs.pop();
    }

    for (int j = 0; j < THREAD_COUNT; j++)
        delete rkpool[j];
    //destroyAllWindows();

    return 0;
}

/*jpeg 模数 FF D8 FF DB*/
bool mjpeg_to_rgb(unsigned char* mjpeg_buf, int mjpeg_buf_len, unsigned char* RGB_buf)
{
    bool ret = true;
    int read_flag = 0;

    /*防止出现类似这样的异常: Not a JPEG file: starts with 0xc7 0x04*/
    if (mjpeg_buf[0] != 0xFF || mjpeg_buf[1] != 0xD8) {
        ret = false;
        fprintf(stderr, "Not a JPEG file: starts with %#x %#x\n", mjpeg_buf[0], mjpeg_buf[1]);
        return ret;
    }

    // 创建和初始化JPEG解码对象
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    //  绑定默认错误处理函数
    cinfo.err = jpeg_std_error(&jerr);
    // 创建JPEG解码对象
    jpeg_create_decompress(&cinfo); 
    // 设置MJPEG数据源
    jpeg_mem_src(&cinfo, mjpeg_buf, mjpeg_buf_len);

    // 读取头部信息，并设置默认的解压参数 
    try { 
        read_flag = jpeg_read_header(&cinfo, TRUE); 
    } catch (exception& e) { // 处理异常 
        fprintf(stderr, "jpeg read header exception: %s\n", e.what()); 
        ret = false;
    }
    if (!ret || read_flag != JPEG_HEADER_OK) {
        jpeg_finish_decompress(&cinfo); 
        jpeg_destroy_decompress(&cinfo); 
        return ret; 
    }

    // 设置解压参数
    cinfo.out_color_space = JCS_RGB; // 设置输出颜色空间为RGB
    // cinfo.raw_data_out = TRUE; // 设置输出原始数据
    cinfo.dct_method = JDCT_IFAST; // 设置快速离散余弦变换方法

    // 开始解压图像
    try {
        ret = jpeg_start_decompress(&cinfo);
    } catch (exception& e) { // 处理异常 
        fprintf(stderr, "jpeg decompress error:%s\n", e.what());
        ret = false;
    }
    if (!ret) {
        jpeg_finish_decompress(&cinfo); 
        jpeg_destroy_decompress(&cinfo); 
        return ret;
    }

    //读取解压后的RGB图像
    int buffer_size = cinfo.output_components * cinfo.output_width;
    unsigned char * buffer = (unsigned char *)malloc(buffer_size);
    while (cinfo.output_scanline < cinfo.output_height) {
        try {
            jpeg_read_scanlines(&cinfo, (unsigned char **)&buffer, 1);//每次读取一行数据
        } catch (exception& e){
            fprintf(stderr, "jpeg read data error:%s\n", e.what());
            ret = false;
            jpeg_finish_decompress(&cinfo); 
            jpeg_destroy_decompress(&cinfo); 
            free(buffer);
            return ret;
        }
        memcpy(RGB_buf, buffer, buffer_size);
        RGB_buf += buffer_size;
    }

    // 结束解压图像
    jpeg_finish_decompress(&cinfo); 
    // 销毁JPEG解码对象
    jpeg_destroy_decompress(&cinfo); 
    free(buffer);

    return ret;
}

static int v4l2_dev_init(const char *device)
{
    struct v4l2_capability cap = {0};
    /* 打开摄像头 */
    v4l2_fd = open(device, O_RDWR);
    if (0 > v4l2_fd) {
        fprintf(stderr, "open error: %s: %s\n", device, strerror(errno));
        return -1;
    } else {
        printf("open camera success!\n");
    }
    /* 查询设备功能 */
    ioctl(v4l2_fd, VIDIOC_QUERYCAP, &cap);
    /* 判断是否是视频采集设备 */
    if (!(V4L2_CAP_VIDEO_CAPTURE & cap.capabilities)) {
        fprintf(stderr, "Error: %s: No capture video device!\n", device);
        close(v4l2_fd);
        return -1;
    } else {
        printf("capture video device success!\n");
    }
    return 0;
}

static int v4l2_set_format(void)
{
    struct v4l2_format fmt = {0};
    struct v4l2_streamparm streamparm = {0};
    int frm_width, frm_height; //视频帧宽度和高度

    /* 设置帧格式 */
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;//type 类型
    fmt.fmt.pix.width = IMAGE_WIDTH; //视频帧宽度
    fmt.fmt.pix.height = IMAGE_HEIGHT;//视频帧高度
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG; //像素格式

    if (0 > ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt)) {
        fprintf(stderr, "ioctl error: VIDIOC_S_FMT: %s\n", strerror(errno));
        return -1;
    }

    /*判断是否已经设置为我们要求的 MJPEG 像素格式*/
    if (V4L2_PIX_FMT_MJPEG != fmt.fmt.pix.pixelformat) {
        fprintf(stderr, "Error: the device does not support MJPEG format!\n");
        return -1;
    } 
    printf("the camera support MJPEG format!\n");
    

    frm_width = fmt.fmt.pix.width; //获取实际的帧宽度
    frm_height = fmt.fmt.pix.height;//获取实际的帧高度
    printf("视频帧大小<%d * %d>\n", frm_width, frm_height);

    /* 获取 streamparm */
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(v4l2_fd, VIDIOC_G_PARM, &streamparm);

    /** 判断是否支持帧率设置 **/
    if (V4L2_CAP_TIMEPERFRAME & streamparm.parm.capture.capability) {
        streamparm.parm.capture.timeperframe.numerator = 1;
        streamparm.parm.capture.timeperframe.denominator = 30;//30fps
        if (0 > ioctl(v4l2_fd, VIDIOC_S_PARM, &streamparm)) {
            fprintf(stderr, "ioctl error: VIDIOC_S_PARM: %s\n", strerror(errno));
            return -1;
        } 
    } else {
        printf("the camera does not support to set fps\n");
    }
    return 0;
}

static int v4l2_init_buffer(void)
{
    struct v4l2_requestbuffers reqbuf = {0};
    struct v4l2_buffer buf = {0};

    /* 申请帧缓冲 */
    reqbuf.count = FRAMEBUFFER_COUNT; //帧缓冲的数量
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    if (0 > ioctl(v4l2_fd, VIDIOC_REQBUFS, &reqbuf)) {
        fprintf(stderr, "ioctl error: VIDIOC_REQBUFS: %s\n", strerror(errno));
        return -1;
    }

    /* 建立内存映射 */
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    for (buf.index = 0; buf.index < FRAMEBUFFER_COUNT; buf.index++) {
        ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf); // 获取缓冲区信息
        buf_infos[buf.index].length = buf.length; //缓冲区的长度
        //映射缓冲区地址
        buf_infos[buf.index].start = (unsigned short *)(mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l2_fd, buf.m.offset));
        if (MAP_FAILED == buf_infos[buf.index].start) {
            perror("mmap error");
            return -1;
        }
    }

    /*入队*/
    for (buf.index = 0; buf.index < FRAMEBUFFER_COUNT; buf.index++) {
        if (0 > ioctl(v4l2_fd, VIDIOC_QBUF, &buf)) {
            fprintf(stderr, "ioctl error: VIDIOC_QBUF: %s\n", strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int v4l2_stream_on(void)
{
    /* 打开摄像头、摄像头开始采集数据 */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (0 > ioctl(v4l2_fd, VIDIOC_STREAMON, &type)) {
        fprintf(stderr, "ioctl error: VIDIOC_STREAMON: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}


