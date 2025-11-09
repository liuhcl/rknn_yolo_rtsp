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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include "yolo.h"
#include <stdint.h>
#include "rknn_demo_utils.h"

#include <set>
#include <vector>
#define LABEL_NALE_TXT_PATH "../yolov8_demo/model/fire_labels_list.txt"

static char *labels[OBJ_CLASS_NUM];

double __get_us(struct timeval t) { return (t.tv_sec * 1000000 + t.tv_usec); }

inline static int clamp(float val, int min, int max)
{
    return val > min ? (val < max ? val : max) : min;
}


int model_type_cmp(MODEL_TYPE *t, char* input_str, const char* type_str, MODEL_TYPE asign_type){
    if (strcmp(input_str, type_str)==0){
        *t = asign_type;
        return 1;
    }
    return 0;
}


MODEL_TYPE string_to_model_type(char* model_type_str){
    int ret = 0;
    MODEL_TYPE _t = MODEL_TYPE_ERROR;
    ret = model_type_cmp(&_t, model_type_str, "yolov5", YOLOV5);
    ret = model_type_cmp(&_t, model_type_str, "v5", YOLOV5);
    ret = model_type_cmp(&_t, model_type_str, "yolov6", YOLOV6);
    ret = model_type_cmp(&_t, model_type_str, "v6", YOLOV6);
    ret = model_type_cmp(&_t, model_type_str, "yolov7", YOLOV7);
    ret = model_type_cmp(&_t, model_type_str, "v7", YOLOV7);
    ret = model_type_cmp(&_t, model_type_str, "yolov8", YOLOV8);
    ret = model_type_cmp(&_t, model_type_str, "v8", YOLOV8);
    ret = model_type_cmp(&_t, model_type_str, "yolox", YOLOX);
    ret = model_type_cmp(&_t, model_type_str, "ppyoloe_plus", PPYOLOE_PLUS);
    ret = model_type_cmp(&_t, model_type_str, "ppyoloe", PPYOLOE_PLUS);
    
    if (_t == MODEL_TYPE_ERROR){
        printf("ERROR: Only support yolov5/yolov6/yolov7/yolov8/yolox/ppyoloe_plus model, but got %s\n", model_type_str);
    }
    return _t;
}


char *readLine(FILE *fp, char *buffer, int *len)
{
    int ch;
    int i = 0;
    size_t buff_len = 0;

    buffer = (char *)malloc(buff_len + 1);
    if (!buffer)
        return NULL; // Out of memory

    while ((ch = fgetc(fp)) != '\n' && ch != EOF)
    {
        buff_len++;
        void *tmp = realloc(buffer, buff_len + 1);
        if (tmp == NULL)
        {
            free(buffer);
            return NULL; // Out of memory
        }
        buffer = (char *)tmp;

        buffer[i] = (char)ch;
        i++;
    }
    buffer[i] = '\0';

    *len = buff_len;

    // Detect end
    if (ch == EOF && (i == 0 || ferror(fp)))
    {
        free(buffer);
        return NULL;
    }
    return buffer;
}

int readLines(const char *fileName, char *lines[], int max_line)
{
    FILE *file = fopen(fileName, "r");
    char *s;
    int i = 0;
    int n = 0;
    while ((s = readLine(file, s, &n)) != NULL)
    {
        lines[i++] = s;
        if (i >= max_line)
            break;
    }
    return i;
}


int readFloats(const char *fileName, float* result, int max_line, int* valid_number)
{
    FILE *file = fopen(fileName, "r");
    if (file == NULL) {
        printf("failed to open file\n");
        return 1;
    }

    int n = 0;
    while ((n<=max_line) &&(fscanf(file, "%f", &result[n++]) != EOF));

    /* n-1 float values were successfully read */
    // for (int i=0; i<n-1; i++)
    //     printf("fval[%d]=%f\n", i, result[i]);

    fclose(file);
    *valid_number = n-1;
    return 0;
}

int loadLabelName(const char *locationFilename, char *label[])
{
    printf("loadLabelName %s\n", locationFilename);
    readLines(locationFilename, label, OBJ_CLASS_NUM);
    return 0;
}

static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1, float ymax1)
{
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) + (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
    return u <= 0.f ? 0.f : (i / u);
}

static int nms(int validCount, std::vector<float> &outputLocations, std::vector<int> classIds, std::vector<int> &order,
               int filterId, float threshold)
{
    for (int i = 0; i < validCount; ++i)
    {
        int n = order[i];
        if (n == -1 || classIds[n] != filterId)
        {
            continue;
        }
        for (int j = i + 1; j < validCount; ++j)
        {
            int m = order[j];
            if (m == -1 || classIds[m] != filterId)
            {
                continue;
            }
            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
            float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
            float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

            float iou = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);

            if (iou > threshold)
            {
                order[j] = -1;
            }
        }
    }
    return 0;
}

//快速排序
static int quick_sort_indice_inverse(
    std::vector<float> &input, //得分
    int left, //0
    int right, //边界框数量 - 1
    std::vector<int> &indices) //索引数组(0 至 边界框数量-1)
{
    float key; //基准值得分
    int key_index; //基准值索引
    int low = left; //左值索引
    int high = right; //右值索引
    if (left < right)
    {
        key_index = indices[left];
        key = input[left];
        /*将数组划分为两个部分，左边的元素都大于等于基准值，右边的元素都小于等于基准值*/
        while (low < high)
        {
            /*从右向左 获取小于基准值的元素*/
            while (low < high && input[high] <= key)
            {
                high--;
            }
            input[low] = input[high];
            indices[low] = indices[high];

            /*从左向右 获取大于基准值的元素*/
            while (low < high && input[low] >= key)
            {
                low++;
            }
            input[high] = input[low];
            indices[high] = indices[low];
        }
        input[low] = key;
        indices[low] = key_index;
        //递归地对左右两个部分进行快速排序
        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }
    return low;
}

inline static int32_t __clip(float val, float min, float max)
{
    float f = val <= min ? min : (val >= max ? max : val);
    return f;
}

static float sigmoid(float x)
{
    return 1.0 / (1.0 + expf(-x));
}

static float unsigmoid(float y)
{
    return -1.0 * logf((1.0 / y) - 1.0);
}

/*浮点型转int8*/
static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
{
    float dst_val = (f32 / scale) + zp;
    int8_t res = (int8_t)__clip(dst_val, -128, 127);//调用__clip函数，将dst_val限制在-128到127之间，防止溢出
    return res;
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale)
{
    return ((float)qnt - (float)zp) * scale;
}

/*
    Post process v2 for yolov6, yolov8, ppyoloe_plus
    Feature:
        score,box in diffenrent tensor
        Anchor free
*/
//张量，长度，边界框的位置与大小指针
void compute_dfl(float* tensor, int dfl_len, float* box){
    for (int b=0; b<4; b++){ //获取边界框的4个边界点坐标
        float exp_t[dfl_len];
        float exp_sum=0;
        float acc_sum=0;
        for (int i=0; i< dfl_len; i++){
            exp_t[i] = exp(tensor[i+b*dfl_len]);
            exp_sum += exp_t[i];
        }
        
        for (int i=0; i< dfl_len; i++){
            acc_sum += exp_t[i]/exp_sum *i;
        }
        box[b] = acc_sum;
    }
}

static int process_v2_i8(
                   int8_t *t_box, int32_t box_zp, float box_scale,
                   int8_t *t_score, int32_t score_zp, float score_scale,
                   int8_t* t_score_sum, int32_t sum_zp, float sum_scale,
                   int dfl_len, int grid_h, int grid_w, int stride,
                   std::vector<float> &boxes, std::vector<float> &boxScores, std::vector<int> &classId,
                   float threshold)
{
    int validCount = 0; //有效框数量
    int grid_len = grid_h * grid_w; //网格大小
    int8_t score_thres_i8 = qnt_f32_to_affine(threshold, score_zp, score_scale); //分数阈值 量化为int8
    int8_t score_sum_thres_i8 = qnt_f32_to_affine(threshold, sum_zp, sum_scale);

    /*遍历网格*/
    for (int i= 0; i< grid_h; i++)
    {
        for (int j= 0; j< grid_w; j++)
        {
            float sum = 0; //分数和
            int offset = i* grid_w + j;
            
            // 通过 score sum 起到快速过滤的作用
            if (t_score_sum != nullptr){
                if (t_score_sum[offset] < score_sum_thres_i8){
                    continue;
                }
            }

            int max_class_id = -1; //最大类别编号
            int8_t max_score = -score_zp;
            for (int c = 0; c < OBJ_CLASS_NUM; c++){  //判断每个网格的类别概率，取概率最大的一个
                if ((t_score[offset] > score_thres_i8) && (t_score[offset] > max_score))
                {
                    max_score = t_score[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            if (max_score> score_thres_i8) {  //分数是否大于分数阈值，获取有效的目标检测
                float box[4];

                int box_offset = i* grid_w + j; //偏移量修正 [1,64,80,80] 
  
                /// dfl
                float before_dfl[dfl_len*4]; //dfl_len = 16
                for (int k=0; k< dfl_len*4; k++){
                    before_dfl[k] = deqnt_affine_to_f32(t_box[box_offset], box_zp, box_scale); //反量化t_box值
                    box_offset += grid_len; 
                }

                /*获取边界框坐标*/
                compute_dfl(before_dfl, dfl_len, box); 

                float x1,y1,x2,y2,w,h; //边界框位置与宽高
                //box[0]和box2表示的是边界框的中心点到左右边缘的距离
                x1 = (-box[0] + j + 0.5)*stride;
                y1 = (-box[1] + i + 0.5)*stride;
                x2 = (box[2] + j + 0.5)*stride;
                y2 = (box[3] + i + 0.5)*stride;
                w = x2 - x1;
                h = y2 - y1;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(w);
                boxes.push_back(h);
                boxScores.push_back(deqnt_affine_to_f32(max_score, score_zp, score_scale));
                classId.push_back(max_class_id);
                validCount ++;

            }
        }
    }
    return validCount;
}

int post_process(void** rk_outputs, MODEL_INFO* m_info, int dfl_len, detect_result_group_t* group)
{
    static int init = -1;
    if (init == -1)
    {
        int ret = 0;
        ret = loadLabelName(LABEL_NALE_TXT_PATH, labels); //读取类别文件
        if (ret < 0)
        {
            printf("Failed in loading label\n");
            return -1;
        }

        init = 0;
        printf("post_process load lable finish\n");
    }
    memset(group, 0, sizeof(detect_result_group_t));

    std::vector<float> filterBoxes;
    std::vector<float> boxesScore;
    std::vector<int> classId;
    int validCount = 0; //有效边界框数量
    int stride = 0;
    int grid_h = 0;
    int grid_w = 0;

    int output_per_branch = m_info->n_output / 3; //为3
    for (int i=0; i < 3; i++) {

        void* score_sum = nullptr; //分数总和
        int32_t sum_zp = 0; //零点
        float sum_scale = 0.; //缩放因子
        if (output_per_branch == 3) {
            score_sum = rk_outputs[i*output_per_branch + 2];
            sum_zp = m_info->out_attr[i*output_per_branch + 2].zp; //-128
            sum_scale = m_info->out_attr[i*output_per_branch + 2].scale; //0
        }

        int box_idx = i*output_per_branch;
        int score_idx = i*output_per_branch + 1;

        grid_h = m_info->out_attr[box_idx].dims[2]; //网格高
        grid_w = m_info->out_attr[box_idx].dims[3]; //网格宽
        stride = m_info->in_attr[0].dims[1] / grid_h; //输入张量在高度方向上的步长
        //printf("score_sum_zp:%d, score_sum_scale:%.2f\n", sum_zp, sum_scale);
        /*获取有效识别框数*/
        validCount = validCount + process_v2_i8(
            (int8_t*)rk_outputs[box_idx], m_info->out_attr[box_idx].zp, m_info->out_attr[box_idx].scale,
            (int8_t*)rk_outputs[score_idx], m_info->out_attr[score_idx].zp, m_info->out_attr[score_idx].scale,
            (int8_t*)score_sum, sum_zp, sum_scale,
            dfl_len, grid_h, grid_w, stride, 
            filterBoxes, boxesScore, classId, CONF_THRESHOLD);

    }

    // no object detect
    if (validCount <= 0) return 0;

    std::vector<int> indexArray;
    for (int i = 0; i < validCount; ++i) {
        indexArray.push_back(i); //索引
    }

    //对得分进行从高到低的排序 且保证元素的索引不会发生改变
    quick_sort_indice_inverse(boxesScore, 0, validCount - 1, indexArray);

    std::set<int> class_set(std::begin(classId), std::end(classId));
    //非极大抑制，保留最大置信度最大的一个
    for (auto c : class_set) {
        nms(validCount, filterBoxes, classId, indexArray, c, NMS_THRESHOLD);
    }
    //printf("validCount:%d\n", validCount);

    int last_count = 0;
    group->count = 0;
    /* box valid detect target */
    for (int i = 0; i < validCount; ++i)
    {
        if (indexArray[i] == -1 || boxesScore[i] < CONF_THRESHOLD || last_count >= OBJ_NUMB_MAX_SIZE)
        {
            continue;
        }
        int n = indexArray[i];

        float x1 = filterBoxes[n * 4 + 0];
        float y1 = filterBoxes[n * 4 + 1];
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];
        int id = classId[n];

        group->results[last_count].box.left = x1;
        group->results[last_count].box.top = y1;
        group->results[last_count].box.right = x2;
        group->results[last_count].box.bottom = y2;
        group->results[last_count].prop = boxesScore[i];
        group->results[last_count].class_index = id;
        char *label = labels[id];
        strncpy(group->results[last_count].name, label, OBJ_NAME_MAX_SIZE);

        // printf("result %2d: (%4d, %4d, %4d, %4d), %s\n", i, group->results[last_count].box.left, group->results[last_count].box.top,
        //        group->results[last_count].box.right, group->results[last_count].box.bottom, label);
        last_count++;
    }
    group->count = last_count;

    return 0;
}

/*转换格式*/
#include "RgaUtils.h"
#include "im2d.hpp"
#include "dma_alloc.h"
#include "mpp_main.h"
int yuv420_to_bgr888(uint8_t* yuv420p_buf, uint8_t *bgr888_buf)
{
    int ret = 0;
    int src_width, src_height, src_format;
    int dst_width, dst_height, dst_format;
    uint8_t *src_buf, *dst_buf;
    int src_buf_size = FRAME_YUV420P_SIZE, dst_buf_size = FRAME_BGR888_SIZE;

    int src_dma_fd, dst_dma_fd;
    rga_buffer_t src_img, dst_img;
    rga_buffer_handle_t src_handle, dst_handle;
    querystring(RGA_VERSION);

    memset(&src_img, 0, sizeof(src_img));
    memset(&dst_img, 0, sizeof(dst_img));

    src_width = FRAME_WIDTH;
    src_height = FRAME_HEIGHT;
    src_format = RK_FORMAT_YCbCr_420_SP;

    dst_width = FRAME_WIDTH;
    dst_height = FRAME_HEIGHT;
    dst_format = RK_FORMAT_BGR_888;

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

    memcpy(src_buf, yuv420p_buf, src_buf_size);

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
    if (ret != IM_STATUS_SUCCESS) {
        printf("RGA running failed, %s\n", imStrError((IM_STATUS)ret));
        goto release_buffer;
    }

    memcpy(bgr888_buf, dst_buf, dst_buf_size);

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
