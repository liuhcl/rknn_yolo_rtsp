cmake_minimum_required(VERSION 3.4.1)

project(rknn_yolov8_demo)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-pthread")

set(CMAKE_BUILD_TYPE "Debug")
SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb") #debug模式下开启-g选项
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall") #如果set(CMAKE_BUILD_TYPE RELEASE)则使用该配置


set(CMAKE_SKIP_INSTALL_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")

set(MZ_ROOT "/home/cat/AI/rknn_yolo_rtsp")

# GENERAL LIB
# rknn api

if(TARGET_SOC STREQUAL "rk3588")
  set(PLATFORM RK3588)
  add_definitions(-DPLATFORM_RK3588)
  set(LIB_ARCH aarch64)
  set(RKNN_API_PATH ${MZ_ROOT}/libs/rklibs/rknpu2/runtime/RK3588/Linux/librknn_api)
  set(RKNN_RT_LIB ${RKNN_API_PATH}/${LIB_ARCH}/librknnrt.so)
  set(RGA_LIB ${MZ_ROOT}/libs/rklibs/librga/libs/Linux/gcc-aarch64/librga.a)
else()
  message(FATAL_ERROR "TARGET_SOC is not set, ref value: rk356x or rk3588 ")
endif()
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
set(CMAKE_INSTALL_RPATH "lib")

add_subdirectory(z_rkmpp_enc)

# YOLO DEMO LIB
# rknn_yolov8_demo
set(RKDEMO_UTILS ${MZ_ROOT}/libs/common/rkdemo_utils)
set(RKDEMO_UTILS_CC ${RKDEMO_UTILS}/rknn_demo_utils.cc)
include_directories( ${RKDEMO_UTILS})

include_directories( 
  ${CMAKE_SOURCE_DIR}/yolov8_demo/include 
  ${CMAKE_SOURCE_DIR}/yolo_utils
  ${CMAKE_SOURCE_DIR}/z_rkmpp_enc
  ${CMAKE_SOURCE_DIR}/z_rkmpp_enc/mpp_utils/inc
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
  z_rkmpp_enc
  ${RKNN_RT_LIB}
  ${RGA_LIB}
  ${LIBJPEG_LIB}
  ${LIBPNG_LIB}
  ${ZLIB_LIB}
  dl
  ${PTHREAD}
  ${OpenCV_LIBS}
)

# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install)

install(PROGRAMS ${RKNN_RT_LIB} DESTINATION lib)
install(PROGRAMS ${RGA_LIB} DESTINATION lib)
install(DIRECTORY ./yolov8_demo/model DESTINATION ./)
