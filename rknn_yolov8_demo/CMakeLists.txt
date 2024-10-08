cmake_minimum_required(VERSION 3.4.1)

project(rknn_yolov8_demo)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-pthread")

set(MZ_ROOT "/home/cat/mpp/rknn_yolo_rtsp")

# GENERAL LIB
# rknn api
set(PLATFORM RK3588)
add_definitions(-DPLATFORM_RK3588)
set(LIB_ARCH aarch64)
set(RKNN_API_PATH ${MZ_ROOT}/libs/rklibs/rknpu2/runtime/RK3588/Linux/librknn_api)
set(RKNN_RT_LIB ${RKNN_API_PATH}/${LIB_ARCH}/librknnrt.so)
set(RGA_LIB ${MZ_ROOT}/libs/rklibs/librga/libs/Linux/gcc-aarch64/librga.a)

include_directories(${RKNN_API_PATH}/include)

# drm
set(DRM_DIR ${MZ_ROOT}/libs/common/drm)
include_directories(${DRM_DIR}/include)
include_directories(${DRM_DIR}/include/libdrm)

# rga
include_directories(${MZ_ROOT}/libs/rklibs/librga/include)
include_directories(${MZ_ROOT}/libs/rklibs/librga/samples/utils/allocator/include)
set(RGA_DMA_ALLOC ${MZ_ROOT}/libs/rklibs/librga/samples/utils/allocator/dma_alloc.cpp)
# include(${MZ_ROOT}/libs/rklibs/librga/samples/utils/CMakeLists.txt)

#stb CImg
set(STB_DIR ${MZ_ROOT}/libs/common/)
include_directories(${STB_DIR})

#mpp
set(rkmpp ${MZ_ROOT}/libs/rkmpp)
include_directories(
    ${rkmpp}/include/mpp
    ${rkmpp}/include/osal
    ${rkmpp}/include/utils
)
set(mpp_utils ${rkmpp}/libutils.so)
set(RC_MPP ${rkmpp}/librockchip_mpp.so)
add_subdirectory(mpp_enc)

# libpng
set(LIBPNG_DIR ${MZ_ROOT}/libs/platform/${PLATFORM}/libpng)
include_directories(${LIBPNG_DIR}/include)
set(LIBPNG_LIB ${LIBPNG_DIR}/lib/Linux/libpng16.a)
# libjpeg
set(LIBJPEG_DIR ${MZ_ROOT}/libs/platform/${PLATFORM}/libjpeg)
include_directories(${LIBJPEG_DIR}/include)
set(LIBJPEG_LIB ${LIBJPEG_DIR}/lib/Linux/libjpeg.a)
# zlib(used by libpng)
set(ZLIB_DIR ${MZ_ROOT}/libs/platform/${PLATFORM}/zlib)
include_directories(${ZLIB_DIR}/include)
set(ZLIB_LIB ${ZLIB_DIR}/lib/Linux/libz.a)

string(REPLACE "-DANDROID" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "-DANDROID" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

#OpenCV
set(OpenCV_DIR /home/cat/opt/opencv-4.6.0/build) # 设置 OpenCVConfig.cmake 文件所在的目录
find_package(OpenCV REQUIRED) # 找到 opencv 库

# YOLO DEMO LIB
# rknn_yolov8_demo
set(RKDEMO_UTILS ${MZ_ROOT}/libs/common/rkdemo_utils)
set(RKDEMO_UTILS_CC ${RKDEMO_UTILS}/rknn_demo_utils.cc)
include_directories( ${RKDEMO_UTILS})

include_directories( 
  ${CMAKE_SOURCE_DIR}/yolov8_demo/include 
  ${CMAKE_SOURCE_DIR}/yolo_utils
  ${CMAKE_SOURCE_DIR}/mpp_enc
  ${OpenCV_INCLUDED_DIRS}
)

set (PTHREAD pthread)

add_executable(rknn_yolov8_demo
        yolov8_demo/src/main.cc yolov8_demo/src/yolo.cc
        yolo_utils/resize_function.cc
        ${RKDEMO_UTILS_CC}
        ${RGA_DMA_ALLOC}
        )

target_link_libraries(rknn_yolov8_demo
  ${RKNN_RT_LIB}
  ${RGA_LIB}
  ${LIBJPEG_LIB}
  ${LIBPNG_LIB}
  ${ZLIB_LIB}
  dl
  ${PTHREAD}
  ${OpenCV_LIBS}

  mpp_enc
)
