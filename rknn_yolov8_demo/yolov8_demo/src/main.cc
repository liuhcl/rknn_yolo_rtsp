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
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define _BASETSD_H

#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#endif 

// #ifndef STB_IMAGE_RESIZE_IMPLEMENTATION
// #define STB_IMAGE_RESIZE_IMPLEMENTATION
// #include <stb/stb_image_resize.h>
// #endif

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#endif

#undef cimg_display
#define cimg_display 0
#undef cimg_use_jpeg
#define cimg_use_jpeg 1
#undef cimg_use_png
#define cimg_use_png 1
#include "CImg/CImg.h"

#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>
#include "rknn_api.h"
#include "yolo.h"
#include "resize_function.h"
#include "rknn_demo_utils.h"
#include "mpp_main.h"
#include "rknnPool.hpp"
#include "ThreadPool.hpp"

#define THREAD_COUNT 1
#define VIDEO_DEVICE "/dev/video11" // 视频设备文件
#define FRAMEBUFFER_COUNT 4 //帧缓冲数量

using std::time;
using std::queue;
using std::vector;

using namespace cv;	    //OpenCV标准库
#define FMT_NUM_PLANES  1
#define RKNN_MODEL_PATH "../yolov8_demo/model/yolov8n_relu.rknn"

/*** 描述一个帧缓冲的信息 ***/
typedef struct cam_buf_info {
    unsigned short *start; //帧缓冲起始地址
    unsigned long length; //帧缓冲长度
} cam_buf_info;

static int v4l2_fd = -1; //摄像头设备文件描述符
static cam_buf_info buf_infos[FRAMEBUFFER_COUNT];

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
    if (ioctl(v4l2_fd, VIDIOC_QUERYCAP, &cap) == 0) {
        printf("------- VIDIOC_QUERYCAP ----\n");
        printf("  driver: %s\n", cap.driver);
        printf("  card: %s\n", cap.card);
        printf("  bus_info: %s\n", cap.bus_info);
        printf("  version: %d.%d.%d\n",
            (cap.version >> 16) & 0xff,
            (cap.version >> 8) & 0xff,
            (cap.version & 0xff));
        printf("  capabilities: %08X\n", cap.capabilities);

        if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
            printf("        Video Capture\n");
        if (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)
            printf("        Video Output\n");
        if (cap.capabilities & V4L2_CAP_VIDEO_OVERLAY)
            printf("        Video Overly\n");
        if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
            printf("        Video Capture Mplane\n");
        if (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE)
            printf("        Video Output Mplane\n");
        if (cap.capabilities & V4L2_CAP_READWRITE)
            printf("        Read / Write\n");
        if (cap.capabilities & V4L2_CAP_STREAMING)
            printf("        Streaming\n");
        printf("\n");
    } else {
        printf("VIDIOC_QUERYCAP error!\n");
        return -1;
    }
    return 0;
}

static int v4l2_set_format(void)
{
    struct v4l2_format fmt = {0};
    struct v4l2_streamparm streamparm = {0};
    int frm_width, frm_height; //视频帧宽度和高度

    /* 设置帧格式 */
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;//type 类型
    fmt.fmt.pix.width = FRAME_WIDTH; //视频帧宽度
    fmt.fmt.pix.height = FRAME_HEIGHT;//视频帧高度
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12; //像素格式

    if (0 > ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt)) {
        fprintf(stderr, "ioctl error: VIDIOC_S_FMT: %s\n", strerror(errno));
        return -1;
    }

    /*判断是否已经设置为我们要的像素格式*/
    if (V4L2_PIX_FMT_NV12 != fmt.fmt.pix.pixelformat) {
        fprintf(stderr, "Error: the device does not support yuv420 format!\n");
        return -1;
    } 
    printf("the camera support NV12 format!\n");

    frm_width = fmt.fmt.pix.width; //获取实际的帧宽度
    frm_height = fmt.fmt.pix.height;//获取实际的帧高度
    printf("视频帧大小<%d * %d>\n", frm_width, frm_height);

    /* 获取 streamparm */
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
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
    struct v4l2_plane planes[FMT_NUM_PLANES];
    for (int i = 0; i < FMT_NUM_PLANES; i++)
        memset(&planes[i], 0, sizeof(v4l2_plane));

    /* 申请帧缓冲 */
    reqbuf.count = FRAMEBUFFER_COUNT; //帧缓冲的数量
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    if (0 > ioctl(v4l2_fd, VIDIOC_REQBUFS, &reqbuf)) {
        fprintf(stderr, "ioctl error: VIDIOC_REQBUFS: %s\n", strerror(errno));
        return -1;
    }

    /* 建立内存映射 */
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = FMT_NUM_PLANES;
    for (buf.index = 0; buf.index < FRAMEBUFFER_COUNT; buf.index++) {
        ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf); // 获取缓冲区信息
        buf_infos[buf.index].length = buf.m.planes[0].length; //缓冲区的长度
        //映射缓冲区地址
        buf_infos[buf.index].start = (unsigned short *)(mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l2_fd, buf.m.planes[0].m.mem_offset));
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
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (0 > ioctl(v4l2_fd, VIDIOC_STREAMON, &type)) {
        fprintf(stderr, "ioctl error: VIDIOC_STREAMON: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/*rtsp推流线程*/
static pthread_mutex_t rtsp_mutex; //定义互斥锁
static pthread_cond_t rtsp_cond; //定义条件变量
Mat rknn_img;               //全局共享资源
// 存储多个rknn模型的实例
vector<rknn_lite *> rkpool;
// 线程池
dpool::ThreadPool pool(THREAD_COUNT);
// 线程队列
queue<std::future<Mat>> futs;

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
    int to_bgr_success = 0;
    int frame_num = 0;
    int thread_index = 0;

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
    struct v4l2_plane planes[FMT_NUM_PLANES];
    uint8_t *nv12_buf;
    uint8_t *bgr_buf = (uint8_t *)malloc(FRAME_BGR888_SIZE);
    Mat ori_img_bgr(FRAME_HEIGHT, FRAME_WIDTH, CV_8UC3);

    /*数据拷贝*/
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP; //mmap
    buf.m.planes = planes;
    buf.length = FMT_NUM_PLANES;

    //初始化
    ioctl(v4l2_fd, VIDIOC_DQBUF, &buf); //出队
    printf("yuv420 buf length:%d\n", buf_infos[0].length);
    nv12_buf = (uint8_t *)buf_infos[0].start;
    yuv420_to_bgr888(nv12_buf, bgr_buf);
    ori_img_bgr.data = bgr_buf;
    //imwrite("output_image.jpg", ori_img_bgr);
    ioctl(v4l2_fd, VIDIOC_QBUF, &buf); // 数据处理完之后、再入队、往复
    for (int j = 0; j < THREAD_COUNT; j++) {
        rknn_lite *ptr = new rknn_lite(RKNN_MODEL_PATH, j % THREAD_COUNT);
        rkpool.push_back(ptr);
        ptr->ori_img = ori_img_bgr;
        futs.push(pool.submit(&rknn_lite::interf, ptr)); 
        sleep(1);
    }
    printf("ThreadPool init success\n");

    creat_rtsp_thread();
    sleep(3);
    for( ; ; ) {
        for(buf.index = 0; buf.index < FRAMEBUFFER_COUNT; buf.index++) {
            ioctl(v4l2_fd, VIDIOC_DQBUF, &buf); //出队

            thread_index = frame_num % THREAD_COUNT;
            /*把yuv420图像转换为bgr888图像*/
            nv12_buf = (uint8_t *)buf_infos[buf.index].start;
            to_bgr_success = yuv420_to_bgr888(nv12_buf, bgr_buf);
            if (to_bgr_success == 1) {
                ori_img_bgr.data = bgr_buf;

                pthread_mutex_lock(&rtsp_mutex);//上锁
                // 获取 futs 队列中的第一个元素，并等待它的异步操作完成，然后返回结果或者抛出异常
                rknn_img = futs.front().get();
                if (rknn_img.empty()) 
                    break;
                pthread_mutex_unlock(&rtsp_mutex);//解锁
                pthread_cond_signal(&rtsp_cond);//向条件变量发送信号

                futs.pop(); // 从队列中移除第一个元素
                rkpool[thread_index]->ori_img = ori_img_bgr.clone();
                futs.push(pool.submit(&rknn_lite::interf, rkpool[thread_index])); //加入一个元素
                frame_num++;
            }
            /*计算帧率*/
            if(frame_num % 120 == 0){
                gettimeofday(&time, nullptr);
                tmpTime = time.tv_sec * 1000 + time.tv_usec / 1000;
                printf("平均帧率:\t%f帧\n", 120000.0 / (float)(tmpTime - lopTime));
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

