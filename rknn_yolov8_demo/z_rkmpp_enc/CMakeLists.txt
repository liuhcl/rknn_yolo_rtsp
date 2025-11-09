add_subdirectory(osal)
add_subdirectory(mpp_base)
add_subdirectory(mpp_utils)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/osal/inc)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/mpp_utils/inc)

# rga
include_directories(${CMAKE_CURRENT_SOURCE_DIR}../../libs/rklibs/librga/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}../../libs/rklibs/librga/samples/utils/allocator/include)
set(RGA_DMA_ALLOC ${CMAKE_CURRENT_SOURCE_DIR}../../libs/rklibs/librga/samples/utils/allocator/dma_alloc.cpp)

#ffmpeg
# set(FFMPEG "/usr/local/my/ffmpeg")
# link_directories(${FFMPEG}/lib)
# include_directories(${FFMPEG}/include)

#opencv
include_directories(${OpenCV_INCLUDED_DIRS})

add_library(z_rkmpp_enc STATIC mpp_enc.cpp mpp_enc.h mpp_main.cpp mpp_main.h mpp_rtsp_server.cpp mpp_rtsp_server.h)

target_link_libraries(z_rkmpp_enc
        mpp_utils
        rockchip_mpp
        pthread
        c

        # FFMPEG 静态库
        # 注意这些库的顺序必须按照如下进行 , 否则会报错
        #  这些库之间有依赖的关系 , 必须按照顺序加载库 , 如 avcodec 内部依赖 avformat 库
        avfilter
        avformat
        avcodec
        avutil
        swresample
        swscale

        #FFMPEG 使用到了 libz 库 , 没有会报错
        #   FFMPEG 对于 z 的依赖是以动态库的形式进行的
        #   如果 FFMPEG 是动态库 , 那么可以不链接这个库 , 可以运行时链接这个库
        #   当前 FFMPEG 是静态库 , 需要在编译时将这个库打包到静态库中
        #   推荐使用静态库 , 静态库打包出来的库比动态库小很多 , 一倍左右
        z
)

# install(TARGETS z_rkmpp_enc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

