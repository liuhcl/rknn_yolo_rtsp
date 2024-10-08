#include "mpp_enc.h"
#include <stdio.h>
#include <fstream>

void MppEncoder::setUp(int width, int height, int fps) {
	memset(&g_args, 0, sizeof(MpiEncArgs));

	//计算idx是否到了gop数量，如果到了则添加一个关键帧头信息
	countIdx_ = 0;

	//设置的输入帧的格式信息 yuv 420p  即 I420  yyyyyyyyyyyyuuuvvv planer 结构
	g_args.format = MPP_FMT_YUV420P;

	//设置264编码格式， AVC
	g_args.type = MPP_VIDEO_CodingAVC;
	
	g_args.fps_in_num = fps;
    g_args.fps_out_num = fps;
	g_args.width = width;
	g_args.height = height;
	g_args.hor_stride = mpi_enc_width_default_stride(g_args.width, g_args.format);
	g_args.ver_stride = g_args.height;
	
	packet = NULL;
	std::cout<<"in setUp"<<std::endl;

	init();
}

MPP_RET MppEncoder::encode(const void* img, int img_len, char* dst, int *length)
{
    MPP_RET ret = MPP_OK;
    MppApi *mpi; //存储编码器的接口函数指针
    MppCtx ctx; //存储编码器的上下文信息

	char *tmpP = dst;
	*length = 0;
	if (0 == countIdx_) { //(countIdx_ % 60)
		//countIdx_ = 0;
		int len = 0;

		WriteHeadInfo(tmpP, &len);
		tmpP += len;
		*length += len;
	}

	if (NULL == g_enc_data) return MPP_ERR_NULL_PTR;

	mpi = g_enc_data->mpi;
	ctx = g_enc_data->ctx;

	MppFrame frame = NULL; //存储输入的图像数据   
	packet = NULL; //存储输出的视频数据   将packet作为元数据附加到frame上，以便编码器获取输出缓冲区
	MppMeta meta = NULL;
	RK_U32 eoi = 1;

	void *buf = mpp_buffer_get_ptr(g_enc_data->frm_buf);
	memcpy(buf, img, img_len);
    ret = mpp_frame_init(&frame); 
    if (ret) {
        printf("mpp_frame_init failed\n");
        return ret;
    }

    /*设置图像的属性，如宽度、高度、格式等*/
    mpp_frame_set_width(frame, g_enc_data->width);
    mpp_frame_set_height(frame, g_enc_data->height);
    mpp_frame_set_hor_stride(frame, g_enc_data->hor_stride);
    mpp_frame_set_ver_stride(frame, g_enc_data->ver_stride);
    mpp_frame_set_fmt(frame, g_enc_data->fmt);
    mpp_frame_set_eos(frame, g_enc_data->frm_eos);

    mpp_frame_set_buffer(frame, g_enc_data->frm_buf);
    meta = mpp_frame_get_meta(frame);
    mpp_packet_init_with_buffer(&packet, g_enc_data->pkt_buf);
    /* NOTE: 明确输出包的长度是很重要的 */
    mpp_packet_set_length(packet, 0);
    mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);
  
    ret = mpi->encode_put_frame(ctx, frame); //将frame送入编码器进行编码
    if (ret) {
        printf("mpp encode put frame failed\n");
        mpp_frame_deinit(&frame);
        return ret;
    }
    mpp_frame_deinit(&frame); //释放frame
    do {
        ret = mpi->encode_get_packet(ctx, &packet); //从编码器获取编码后的packet

        if (ret) {
            printf("mpp encode get packet failed\n");
            return ret;
        }
        
		//    mpp_assert(packet);
        if (packet) {
            // 在这里写入数据包到文件
            void *ptr   = mpp_packet_get_pos(packet); //获取数据包   获取MppPacket中的数据指针
            size_t len  = mpp_packet_get_length(packet); //数据包大小
            g_enc_data->pkt_eos = mpp_packet_get_eos(packet); //获取一个MppPacket结构体中的结束标志，也就是表示数据包是否是最后一个数据包

			memcpy(tmpP, ptr, len);
			*length += len;

            /* 用于低延迟分区编码 */
            if (mpp_packet_is_partition(packet)) { // MPP 包是否是一个分区包
                eoi = mpp_packet_is_eoi(packet); //判断是否是一个编码帧的结束标志
                printf(" pkt %d", g_enc_data->frm_pkt_cnt);
				//如果是结束标志，就将 g_enc_data->frm_pkt_cnt 计数器清零，否则就将其加一
                g_enc_data->frm_pkt_cnt = (eoi) ? (0) : (g_enc_data->frm_pkt_cnt + 1); //用于记录一个编码帧中有多少个分区包
            }          
#if 0
            if (mpp_packet_has_meta(packet)) { //判断一个 MPP 包是否有元数据
                meta = mpp_packet_get_meta(packet); //获取元数据
                RK_S32 temporal_id = 0; //时间层 ID
                RK_S32 lt_idx = -1; //长期参考帧索引
                RK_S32 avg_qp = -1; //平均量化参数
                if (MPP_OK == mpp_meta_get_s32(meta, KEY_TEMPORAL_ID, &temporal_id))
                	printf(" tid %d", temporal_id);
                if (MPP_OK == mpp_meta_get_s32(meta, KEY_LONG_REF_IDX, &lt_idx))
                    printf(" lt %d", lt_idx);
					/*qp:量化步长，值越大压缩率越高，视频质量越低，H254编码，QP值为0-51之间的整数*/
                if (MPP_OK == mpp_meta_get_s32(meta, KEY_ENC_AVERAGE_QP, &avg_qp))
                    printf(" qp %d\n", avg_qp);
            }
#endif
            mpp_packet_deinit(&packet); //释放packet

            g_enc_data->stream_size += len;
            g_enc_data->frame_count += eoi;
            if (g_enc_data->pkt_eos) {
                printf("%p found last packet\n", ctx);

            }
        }
    } while (!eoi);

	return ret;
}

void MppEncoder::init() {

	MPP_RET ret = MPP_OK;

	MppPollType timeout = MPP_POLL_BLOCK;

	printf("mpi_enc_test start\n");

	ret = enc_ctx_init(&g_enc_data, &g_args);
	if (ret) {
		printf("test data init failed ret %d\n", ret);
		deinit();
	}
	ret = mpp_buffer_group_get_internal(&g_enc_data->buf_grp, MPP_BUFFER_TYPE_DRM);
    if (ret) {
        printf("failed to get mpp buffer group ret %d\n", ret);
        deinit();
    }

    ret = mpp_buffer_get(g_enc_data->buf_grp, &g_enc_data->frm_buf, g_enc_data->frame_size + g_enc_data->header_size);
	if (ret) {
		printf("failed to get buffer for input frame ret %d\n", ret);
		deinit();
	}

    ret = mpp_buffer_get(g_enc_data->buf_grp, &g_enc_data->pkt_buf, g_enc_data->frame_size);
	if (ret) {
		printf("failed to get buffer for input osd index ret %d\n", ret);
		deinit();
	}

	printf("mpi_enc_test encoder test start w %d h %d type %d\n", g_enc_data->width,
				 g_enc_data->height, g_enc_data->type);

	// encoder demo
	ret = mpp_create(&g_enc_data->ctx, &g_enc_data->mpi);
	if (ret) {
		printf("mpp_create failed ret %d\n", ret);
		deinit();
	}

	ret = g_enc_data->mpi->control(g_enc_data->ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
	if (MPP_OK != ret) {
		printf("mpi control set output timeout %d ret %d\n", timeout, ret);
		deinit();
	}

	ret = mpp_init(g_enc_data->ctx, MPP_CTX_ENC, g_enc_data->type);
	if (ret) {
		printf("mpp_init failed ret %d\n", ret);
		deinit();
	}

	ret = mpp_enc_cfg_init(&g_enc_data->cfg);
	if (ret) {
		printf("mpp_enc_cfg_init failed ret %d\n", ret);
		deinit();
	}


	ret = test_mpp_enc_cfg_setup(g_enc_data);
	if (ret) {
		printf("test mpp setup failed ret %d\n", ret);
		deinit();
	}
}

MPP_RET MppEncoder::enc_ctx_init(MpiEncData **data, MpiEncArgs *cmd) {
	MpiEncData *p_enc_data = NULL;
	MPP_RET ret = MPP_OK;

	if (!data || !cmd) {
		printf("invalid input data %p cmd %p\n", data, cmd);
		return MPP_ERR_NULL_PTR;
	}

	p_enc_data = mpp_calloc(MpiEncData, 1);
	if (!p_enc_data) {
		printf("create MpiEncTestData failed\n");
		ret = MPP_ERR_MALLOC;
		*data = p_enc_data;
		return ret;
	}

	// get paramter from cmd
    p_enc_data->width        = cmd->width;
    p_enc_data->height       = cmd->height;
    p_enc_data->hor_stride   = (cmd->hor_stride) ? (cmd->hor_stride) :
                      (MPP_ALIGN(cmd->width, 16));
    p_enc_data->ver_stride   = (cmd->ver_stride) ? (cmd->ver_stride) :
                      (MPP_ALIGN(cmd->height, 16));
    p_enc_data->fmt          = cmd->format;
    p_enc_data->type         = cmd->type;
    p_enc_data->bps          = cmd->bps_target;
    p_enc_data->bps_min      = cmd->bps_min;
    p_enc_data->bps_max      = cmd->bps_max;
    p_enc_data->rc_mode      = cmd->rc_mode;
    p_enc_data->num_frames   = cmd->num_frames;
    if (cmd->type == MPP_VIDEO_CodingMJPEG && p_enc_data->num_frames == 0) {
        printf("jpege default encode only one frame. Use -n [num] for rc case\n");
        p_enc_data->num_frames   = 1;
    }
    p_enc_data->gop_mode     = cmd->gop_mode;
    p_enc_data->gop_len      = cmd->gop_len;
    p_enc_data->vi_len       = cmd->vi_len;

    p_enc_data->fps_in_flex  = cmd->fps_in_flex;
    p_enc_data->fps_in_den   = cmd->fps_in_den;
    p_enc_data->fps_in_num   = cmd->fps_in_num;
    p_enc_data->fps_out_flex = cmd->fps_out_flex;
    p_enc_data->fps_out_den  = cmd->fps_out_den;
    p_enc_data->fps_out_num  = cmd->fps_out_num;

	// update resource parameter
    switch (p_enc_data->fmt & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP:
    case MPP_FMT_YUV420P: {
        p_enc_data->frame_size = MPP_ALIGN(p_enc_data->hor_stride, 64) * MPP_ALIGN(p_enc_data->ver_stride, 64) * 3 / 2;
    } break;

    case MPP_FMT_YUV422_YUYV :
    case MPP_FMT_YUV422_YVYU :
    case MPP_FMT_YUV422_UYVY :
    case MPP_FMT_YUV422_VYUY :
    case MPP_FMT_YUV422P :
    case MPP_FMT_YUV422SP :
    case MPP_FMT_RGB444 :
    case MPP_FMT_BGR444 :
    case MPP_FMT_RGB555 :
    case MPP_FMT_BGR555 :
    case MPP_FMT_RGB565 :
    case MPP_FMT_BGR565 : {
        p_enc_data->frame_size = MPP_ALIGN(p_enc_data->hor_stride, 64) * MPP_ALIGN(p_enc_data->ver_stride, 64) * 2;
    } break;

    default: {
        p_enc_data->frame_size = MPP_ALIGN(p_enc_data->hor_stride, 64) * MPP_ALIGN(p_enc_data->ver_stride, 64) * 4;
    } break;
    }

    if (MPP_FRAME_FMT_IS_FBC(p_enc_data->fmt))
        p_enc_data->header_size = MPP_ALIGN(MPP_ALIGN(p_enc_data->width, 16) * MPP_ALIGN(p_enc_data->height, 16) / 16, SZ_4K);
    else
        p_enc_data->header_size = 0;

	*data = p_enc_data;
	return ret;
}

void MppEncoder::deinit() {

    g_enc_data->mpi->reset(g_enc_data->ctx);

	if (g_enc_data->ctx) {
		mpp_destroy(g_enc_data->ctx);
		g_enc_data->ctx = NULL;
	}

	if (g_enc_data->cfg) {
		mpp_enc_cfg_deinit(g_enc_data->cfg);
		g_enc_data->cfg = NULL;
	}

	if (g_enc_data->frm_buf) {
		mpp_buffer_put(g_enc_data->frm_buf);
		g_enc_data->frm_buf = NULL;
	}

	enc_ctx_deinit(&g_enc_data);
}

MPP_RET MppEncoder::enc_ctx_deinit(MpiEncData **data) {
	MpiEncData *p_enc_data = NULL;
	if (!data) {
		printf("invalid input data %p\n", data);
		return MPP_ERR_NULL_PTR;
	}

	p_enc_data = *data;
	if (p_enc_data) {
		MPP_FREE(p_enc_data);
		*data = NULL;
	}

	return MPP_OK;
}

MPP_RET MppEncoder::WriteHeadInfo(char *dst, int *length) {
	MPP_RET ret = MPP_OK;
	MppApi *mpi;
	MppCtx ctx;

	if (NULL == g_enc_data) return MPP_ERR_NULL_PTR;

	mpi = g_enc_data->mpi;
	ctx = g_enc_data->ctx;

	//
	if (g_enc_data->type == MPP_VIDEO_CodingAVC || g_enc_data->type == MPP_VIDEO_CodingHEVC) {
		packet = NULL;
		ret = mpi->control(ctx, MPP_ENC_GET_EXTRA_INFO, &packet);
		if (ret) {
			printf("mpi control enc get extra info failed\n");
			return ret;
		}

		/* get and write sps/pps for H.264 */
		if (packet) {
			void *ptr = mpp_packet_get_pos(packet);
			*length = mpp_packet_get_length(packet);

			memcpy(dst, ptr, *length);

			packet = NULL;
		}
	}
	return ret;
}

MPP_RET MppEncoder::test_mpp_enc_cfg_setup(MpiEncData *fp_enc_data) {
	MPP_RET ret;
	MppApi *mpi;
	MppCtx ctx;
	MppEncCfg cfg;	
	MppEncRcMode rc_mode = MPP_ENC_RC_MODE_AVBR;

	if (NULL == fp_enc_data) return MPP_ERR_NULL_PTR;

	mpi = fp_enc_data->mpi;
	ctx = fp_enc_data->ctx;
	cfg = fp_enc_data->cfg;

	/* setup default parameter */
	if (fp_enc_data->fps_in_den == 0) fp_enc_data->fps_in_den = 1;
	if (fp_enc_data->fps_in_num == 0) fp_enc_data->fps_in_num = 30;
	if (fp_enc_data->fps_out_den == 0) fp_enc_data->fps_out_den = 1;
	if (fp_enc_data->fps_out_num == 0) fp_enc_data->fps_out_num = 30;
	// fp_enc_data->gop = 60;

	if (!fp_enc_data->bps)
		fp_enc_data->bps = fp_enc_data->width * fp_enc_data->height / 8 * (fp_enc_data->fps_out_num / fp_enc_data->fps_out_den);

	mpp_enc_cfg_set_s32(cfg, "prep:width", fp_enc_data->width);
	mpp_enc_cfg_set_s32(cfg, "prep:height", fp_enc_data->height);
	mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", fp_enc_data->hor_stride);
	mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", fp_enc_data->ver_stride);
	mpp_enc_cfg_set_s32(cfg, "prep:format", fp_enc_data->fmt);

	mpp_enc_cfg_set_s32(cfg, "rc:mode", rc_mode);

	/* fix input / output frame rate */
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", fp_enc_data->fps_in_flex);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fp_enc_data->fps_in_num);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", fp_enc_data->fps_in_den);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", fp_enc_data->fps_out_flex);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fp_enc_data->fps_out_num);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", fp_enc_data->fps_out_den);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", fp_enc_data->gop_len ? fp_enc_data->gop_len : fp_enc_data->fps_out_num * 2);

	    /* drop frame or not when bitrate overflow */
    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_thd", 20);        /* 20% of max bps */
    mpp_enc_cfg_set_u32(cfg, "rc:drop_gap", 1);         /* Do not continuous drop frame */

    /* setup bitrate for different rc_mode */
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", fp_enc_data->bps);
    switch (fp_enc_data->rc_mode) {
    case MPP_ENC_RC_MODE_FIXQP : {
        printf("FIXQp\n");
        /* do not setup bitrate on FIXQP mode */
    } break;
    case MPP_ENC_RC_MODE_CBR : {
        printf("CBR\n");
        /* CBR mode has narrow bound */
        mpp_enc_cfg_set_s32(cfg, "rc:bps_max", fp_enc_data->bps_max ? fp_enc_data->bps_max : fp_enc_data->bps * 17 / 16);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_min", fp_enc_data->bps_min ? fp_enc_data->bps_min : fp_enc_data->bps * 15 / 16);
    } break;
	//表示一种码率控制模式，即平均变比特率（Average Variable Bitrate）模式。
    case MPP_ENC_RC_MODE_VBR :
    case MPP_ENC_RC_MODE_AVBR : {
        printf("AVBR \n");
        /* VBR mode has wide bound */
        mpp_enc_cfg_set_s32(cfg, "rc:bps_max", fp_enc_data->bps_max ? fp_enc_data->bps_max : fp_enc_data->bps * 17 / 16);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_min", fp_enc_data->bps_min ? fp_enc_data->bps_min : fp_enc_data->bps * 1 / 16);
    } break;
    default : {
        /* default use CBR mode */
        printf("default");
        mpp_enc_cfg_set_s32(cfg, "rc:bps_max", fp_enc_data->bps_max ? fp_enc_data->bps_max : fp_enc_data->bps * 17 / 16);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_min", fp_enc_data->bps_min ? fp_enc_data->bps_min : fp_enc_data->bps * 15 / 16);
    } break;
    }

    /* setup qp for different codec and rc_mode */
    switch (fp_enc_data->type) {
    case MPP_VIDEO_CodingAVC :
    case MPP_VIDEO_CodingHEVC : {
        switch (fp_enc_data->rc_mode) {
        case MPP_ENC_RC_MODE_FIXQP : {
            mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 20);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 20);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 20);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 20);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 20);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 2);
        } break;
        case MPP_ENC_RC_MODE_CBR :
        case MPP_ENC_RC_MODE_VBR :
        case MPP_ENC_RC_MODE_AVBR : {
            mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 26);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 51);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 10);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 51);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 10);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 2);
        } break;
        default : {
            printf("unsupport encoder rc mode %d\n", fp_enc_data->rc_mode);
        } break;
        }
    } break;
    case MPP_VIDEO_CodingVP8 : {
        /* vp8 only setup base qp range */
        mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 40);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_max",  127);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_min",  0);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 127);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 0);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 6);
    } break;
    case MPP_VIDEO_CodingMJPEG : {
        /* jpeg use special codec config to control qtable */
        mpp_enc_cfg_set_s32(cfg, "jpeg:q_factor", 80);
        mpp_enc_cfg_set_s32(cfg, "jpeg:qf_max", 99);
        mpp_enc_cfg_set_s32(cfg, "jpeg:qf_min", 1);
    } break;
    default : {
    } break;
    }

    /* setup codec  */
    mpp_enc_cfg_set_s32(cfg, "codec:type", fp_enc_data->type);
    switch (fp_enc_data->type) {
    case MPP_VIDEO_CodingAVC : {
        /*
         * H.264 profile_idc parameter
         * 66  - Baseline profile
         * 77  - Main profile
         * 100 - High profile
         */
        mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
        /*
         * H.264 level_idc parameter
         * 10 / 11 / 12 / 13    - qcif@15fps / cif@7.5fps / cif@15fps / cif@30fps
         * 20 / 21 / 22         - cif@30fps / half-D1@@25fps / D1@12.5fps
         * 30 / 31 / 32         - D1@25fps / 720p@30fps / 720p@60fps
         * 40 / 41 / 42         - 1080p@30fps / 1080p@30fps / 1080p@60fps
         * 50 / 51 / 52         - 4K@30fps
         */
        mpp_enc_cfg_set_s32(cfg, "h264:level", 40);
        mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
        mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
        mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);
    } break;
    case MPP_VIDEO_CodingHEVC :
    case MPP_VIDEO_CodingMJPEG :
    case MPP_VIDEO_CodingVP8 : {
    } break;
    default : {
        printf("unsupport encoder coding type %d\n", fp_enc_data->type);
    } break;
    }

    fp_enc_data->split_mode = 0;
    fp_enc_data->split_arg = 0;

    mpp_env_get_u32("split_mode", &fp_enc_data->split_mode, MPP_ENC_SPLIT_NONE);
    mpp_env_get_u32("split_arg", &fp_enc_data->split_arg, 0);

    if (fp_enc_data->split_mode) {
        printf("%p split_mode %d split_arg %d\n", ctx, fp_enc_data->split_mode, fp_enc_data->split_arg);
        mpp_enc_cfg_set_s32(cfg, "split:mode", fp_enc_data->split_mode);
        mpp_enc_cfg_set_s32(cfg, "split:arg", fp_enc_data->split_arg);
    }

    ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    if (ret) {
        printf("mpi control enc set cfg failed ret %d\n", ret);
        return ret;
    }

    /* optional */
    fp_enc_data->sei_mode = MPP_ENC_SEI_MODE_ONE_FRAME;
    ret = mpi->control(ctx, MPP_ENC_SET_SEI_CFG, &fp_enc_data->sei_mode);
    if (ret) {
        printf("mpi control enc set sei cfg failed ret %d\n", ret);
        return ret;
    }

    if (fp_enc_data->type == MPP_VIDEO_CodingAVC || fp_enc_data->type == MPP_VIDEO_CodingHEVC) {
        fp_enc_data->header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
        ret = mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &fp_enc_data->header_mode);
        if (ret) {
            printf("mpi control enc set header mode failed ret %d\n", ret);
            return ret;
        }
    }

    RK_U32 gop_mode = fp_enc_data->gop_mode;

    mpp_env_get_u32("gop_mode", &gop_mode, gop_mode);
    if (gop_mode) {
        MppEncRefCfg ref;

        mpp_enc_ref_cfg_init(&ref);

        if (fp_enc_data->gop_mode < 4)
            mpi_enc_gen_ref_cfg(ref, gop_mode);
        else
            mpi_enc_gen_smart_gop_ref_cfg(ref, fp_enc_data->gop_len, fp_enc_data->vi_len);

        ret = mpi->control(ctx, MPP_ENC_SET_REF_CFG, ref);
        if (ret) {
            printf("mpi control enc set ref cfg failed ret %d\n", ret);
            return ret;
        }
        mpp_enc_ref_cfg_deinit(&ref);
    }

    /* setup test mode by env */
    mpp_env_get_u32("osd_enable", &fp_enc_data->osd_enable, 0);
    mpp_env_get_u32("osd_mode", &fp_enc_data->osd_mode, MPP_ENC_OSD_PLT_TYPE_DEFAULT);
    mpp_env_get_u32("roi_enable", &fp_enc_data->roi_enable, 0);
    mpp_env_get_u32("user_data_enable", &fp_enc_data->user_data_enable, 0);

	// RET:
	return ret;
}

