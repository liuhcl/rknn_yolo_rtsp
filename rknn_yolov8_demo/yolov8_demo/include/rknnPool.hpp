#ifndef _rknnPool_H
#define _rknnPool_H

#include <queue>
#include <vector>
#include <iostream>
#include "RgaUtils.h"
#include "im2d.h"
#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>

#include "yolo.h"
#include "rga.h"
#include "rknn_demo_utils.h"
#include "resize_function.h"
#include "rknn_api.h"

#include "ThreadPool.hpp"

using namespace cv;	    //OpenCV标准库
using std::queue;
using std::vector;

class rknn_lite
{
private:
    MODEL_INFO               m_info;
    LETTER_BOX               letter_box;
    int                      dfl_len = -1;
    int                      ret;

public:
    Mat ori_img;
    Mat interf();
    rknn_lite(char *model_path, int npu_index);
    ~rknn_lite();
};

rknn_lite::rknn_lite(char *model_path, int npu_index)
{   
    /* 加载rknn文件，创建网络 */
    m_info.m_path = model_path; //RKNN模型路径
    rkdemo_init(&m_info);
    rkdemo_init_input_buffer_all(&m_info, NORMAL_API, RKNN_TENSOR_UINT8);

    rkdemo_init_output_buffer_all(&m_info, NORMAL_API, 0); //初始化输出缓冲区
  
    dfl_len = (int)(m_info.out_attr[0].dims[1]/4); //置为模型信息中第一个输出层维度的四分之一
    letter_box.target_height = m_info.in_attr[0].dims[1];
    letter_box.target_width = m_info.in_attr[0].dims[2];

    /*设置 NPU*/
    rknn_core_mask core_mask = RKNN_NPU_CORE_0;
    if (npu_index == 0)
        core_mask = RKNN_NPU_CORE_0;
    else if(npu_index == 1)
        core_mask = RKNN_NPU_CORE_1;
    else if(npu_index == 2)
        core_mask = RKNN_NPU_CORE_2;

    ret = rknn_set_core_mask(m_info.ctx, core_mask);
    if (ret < 0){
        printf("rknn_init core error ret=%d\n", ret);
        exit(-1);
    }
}

rknn_lite::~rknn_lite()
{

}

Mat rknn_lite::interf()
{
#if 1
    letter_box.in_width  = ori_img.cols;
    letter_box.in_height = ori_img.rows;
 
    //模型的输入通道数、宽、高  
    static int channel = 3;
    unsigned char *resize_buf = (unsigned char *)malloc(letter_box.target_height* letter_box.target_width* channel);

    // Letter box resize 
    if ((letter_box.in_height == letter_box.target_height) && (letter_box.in_width == letter_box.target_width)){
        m_info.inputs[0].buf = ori_img.data;
    }
    else{ // 输入图像宽高与目标尺寸不符
        compute_letter_box(&letter_box);
        ret = rga_letter_box_resize(ori_img.data, resize_buf, &letter_box); //使用RGA调整尺寸

        if (ret != 0){
            printf("RGA letter box resize failed, use stb to resize\n");
            stb_letter_box_resize(ori_img.data, resize_buf, letter_box);
        }
        letter_box.reverse_available = true;
        m_info.inputs[0].buf = resize_buf;
    }
 
    // input set 
    rknn_inputs_set(m_info.ctx, m_info.n_input, m_info.inputs); //设置模型的输入数据

    // rknn run
    ret = rknn_run(m_info.ctx, NULL); //模型推理

    // output get
    ret = rknn_outputs_get(m_info.ctx, m_info.n_output, m_info.outputs, NULL); //获取模型推理的输出

    /* Post process */
    detect_result_group_t detect_result_group;
    void* output_buf_list[m_info.n_output];
    for (int i=0; i< m_info.n_output; i++){
        output_buf_list[i] = m_info.outputs[i].buf;
    }
    //printf("post process\n");
    post_process(output_buf_list, &m_info, dfl_len, &detect_result_group); //后处理

    // Draw Objects
    char score_result[64];   
    static const Scalar rec_color(96,200,7);
    static const Scalar font_color(20,20,230);
    for (int i = 0; i < detect_result_group.count; i++)
    { 
        //尺寸转换 reverse：从原始尺寸转换为目标尺寸，同时保持图像的比例和内容
        detect_result_group.results[i].box.left = w_reverse(detect_result_group.results[i].box.left, letter_box);
        detect_result_group.results[i].box.right = w_reverse(detect_result_group.results[i].box.right, letter_box);
        detect_result_group.results[i].box.top = h_reverse(detect_result_group.results[i].box.top, letter_box);
        detect_result_group.results[i].box.bottom = h_reverse(detect_result_group.results[i].box.bottom, letter_box);

        detect_result_t *det_result = &(detect_result_group.results[i]);
        int x1 = det_result->box.left;
        int y1 = det_result->box.top;
        int x2 = det_result->box.right;
        int y2 = det_result->box.bottom;
        int ret = snprintf(score_result, sizeof(score_result), "%f", det_result->prop);

        //draw box
        // 定义矩形的左上角和右下角坐标
        Point left_top(x1, y1);
        Point left_bottom(x2, y2);
        Point name_result_pt(x1, y1 + 24);
        Point score_result_pt(x1, y1 + 48);
        rectangle(ori_img, left_top, left_bottom, rec_color, 4, LINE_8); //框
        putText(ori_img, det_result->name, name_result_pt, FONT_HERSHEY_PLAIN, 2, font_color, 2, LINE_8); //类别
        putText(ori_img, score_result, score_result_pt, FONT_HERSHEY_PLAIN, 2, font_color, 2, LINE_8); //置信度
    }
   
    //释放内存
    ret = rknn_outputs_release(m_info.ctx, m_info.n_output, m_info.outputs);
    free(resize_buf);

 #endif
    return ori_img;
}

#endif