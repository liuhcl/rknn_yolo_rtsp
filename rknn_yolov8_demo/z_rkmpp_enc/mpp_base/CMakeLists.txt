# vim: syntax=cmake
# ----------------------------------------------------------------------------
# add libvpu implement
# ----------------------------------------------------------------------------
include_directories(/home/cat/AI/rknn_yolo_rtsp/rknn_yolov8_demo/z_rkmpp_enc/osal/inc)
include_directories(inc)
add_library(mpp_base STATIC
    mpp_trie.cpp
    )

target_link_libraries(mpp_base osal)
