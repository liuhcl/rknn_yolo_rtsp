add_library(mpp_enc STATIC mpp_enc.cc mpp_enc.h mpp_main.cc mpp_main.h) 

target_link_libraries(mpp_enc
        ${mpp_utils}
        ${RC_MPP}
        c
        avfilter
        avformat
        avcodec
        avutil
        swresample
        swscale
        z
)



