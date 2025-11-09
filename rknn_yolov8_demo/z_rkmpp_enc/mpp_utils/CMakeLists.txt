# vim: syntax=cmake
# ----------------------------------------------------------------------------
# add libvpu implement
# ----------------------------------------------------------------------------
include_directories(/home/cat/AI/rknn_yolo_rtsp/rknn_yolov8_demo/z_rkmpp_enc/mpp_base/inc)
include_directories(inc)
add_library(mpp_utils STATIC
    mpi_enc_utils.c
    mpp_utils.c
    dictionary.c
    iniparser.c
    mpp_opt.c
    )

target_link_libraries(mpp_utils mpp_base)
