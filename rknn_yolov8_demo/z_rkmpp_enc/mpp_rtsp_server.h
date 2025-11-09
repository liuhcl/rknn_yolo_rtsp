#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <vector>
#include <string>
#include <stdbool.h>
#include <list>
#include <deque>

using namespace std;

#if !defined(WIN32)
	#define __PACKED__ __attribute__ ((__packed__))
#else
	#define __PACKED__
#endif

/*
MTU 是网络中允许的最大数据包大小。对于以太网，标准的 MTU 大小是 1500 字节。
由于 IP 报头（20 字节）、UDP 报头（8 字节）和 RTP 报头（12 字节），实际可用的 RTP 负载大小为：
1500 − 20 − 8 − 12 = 1460字节
通常情况下，RTP 包的有效负载大小设置在 1000 到 1400 字节之间是一个比较合理的范围
*/
#define nalu_sent_len           1400
#define RTP_H264                96
#define RTP_AUDIO               97
// #define RTP_H265                265
#define MAX_RTSP_CLIENT         1
#define RTSP_SERVER_PORT        554
#define RTSP_RECV_SIZE          1024
#define RTSP_MAX_VID            (640*1024)
#define RTSP_MAX_AUD            (15*1024)

#define AU_HEADER_SIZE          4
#define PARAM_STRING_MAX        100

typedef unsigned short u_int16_t;
typedef unsigned char u_int8_t;
typedef u_int16_t portNumBits;
typedef u_int32_t netAddressBits;
typedef long long _int64;
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE  1
#endif
#define AUDIO_RATE    8000
#define PACKET_BUFFER_END            (unsigned int)0x00000000

typedef enum
{
	RTSP_VIDEO=0,
	RTSP_VIDEOSUB=1,
	RTSP_AUDIO=2,
	RTSP_YUV422=3,
	RTSP_RGB=4,
	RTSP_VIDEOPS=5,
	RTSP_VIDEOSUBPS=6
}enRTSP_MonBlockType;

typedef struct
{
	int startblock;         
	int endblock;           
	int BlockFileNum;       
}IDXFILEHEAD_INFO;          

typedef struct
{
	_int64 starttime;       
	_int64 endtime;         
	int startblock;         
	int endblock;           
	int stampnum;           
}IDXFILEBLOCK_INFO;         

typedef struct
{
	int blockindex;         
	int pos;                
	_int64 time;            
}IDXSTAMP_INFO;             

typedef struct
{
	char filename[150];     
	int pos;               
	_int64 time;            
}FILESTAMP_INFO;            

typedef struct
{
	char channelid[9];
	_int64 starttime;   
	_int64 endtime;         
	_int64 session;
	int type;               
	int encodetype;         
}FIND_INFO;                 

typedef enum
{
	RTP_UDP,
	RTP_TCP,
	RAW_UDP
}StreamingMode;

struct _RTP_FIXED_HEADER
{
	/**//* byte 0 */
	unsigned char csrc_len:4;       /**//* expect 0 */
	unsigned char extension:1;      /**//* expect 1, see RTP_OP below */
	unsigned char padding:1;        /**//* expect 0 */
	unsigned char version:2;        /**//* expect 2 */
	/**//* byte 1 */
	unsigned char payload:7;        /**//* RTP_PAYLOAD_RTSP */
	unsigned char marker:1;         /**//* expect 1 */
	/**//* bytes 2, 3 */
	unsigned short seq_no;
	/**//* bytes 4-7 */
	unsigned  long timestamp;
	/**//* bytes 8-11 */
	unsigned long ssrc;             /**//* stream number is used here. */
} __PACKED__;

typedef struct _RTP_FIXED_HEADER RTP_FIXED_HEADER;

struct _NALU_HEADER
{
	//byte 0
	unsigned char TYPE:5;
	unsigned char NRI:2;
	unsigned char F:1;
}__PACKED__; /**//* 1 BYTES */

typedef struct _NALU_HEADER NALU_HEADER;

/*_FU_INDICATOR
TYPE:
	表示 NAL 单元的类型。对于 FU-A 和 FU-B，TYPE 的值分别为 28 和 29。
	用于将一个 NAL 单元分片成多个 RTP 包。每个分片包包含一个 FU-A 指示符和一部分 NAL 单元的数据。
	第一个分片包的 FU-A 指示符中的 S 位（Start Bit）设置为 1，表示这是分片的开始。
	最后一个分片包的 FU-A 指示符中的 E 位（End Bit）设置为 1，表示这是分片的结束。
	中间的分片包的 S 位和 E 位都设置为 0。
NRI:
	重要性指示符（NAL Ref IDC），表示该 NAL 单元的重要性级别。可能的值：
	00：不重要的数据。
	01：中等重要的数据。
	10：非常重要的数据。
	11：保留。
F:
	禁止位（Forbidden Zero Bit），必须设置为 0。如果检测到这个位为 1，接收端可能会丢弃该 NAL 单元
*/
struct _FU_INDICATOR
{
	//byte 0
	unsigned char TYPE:5;
	unsigned char NRI:2;
	unsigned char F:1;
}__PACKED__; /**//* 1 BYTES */

/*
这个结构体主要用于 H.264 编码的视频流中
当一个 NAL 单元（NAL Unit）太大而不能放在一个 RTP 包中时，需要将其分片并封装成多个 RTP 包进行传输
*/
typedef struct _FU_INDICATOR FU_INDICATOR;

struct _FU_HEADER
{
	//byte 0
	unsigned char TYPE:5;
	unsigned char R:1; // 保留位（Reserved），必须设置为 0。在当前版本的 H.264 标准中，这个位没有使用
	unsigned char E:1; // 如果 E = 1，则表示这是最后一个分片
	unsigned char S:1; // S = 1 表示这是第一个分片	
} __PACKED__; /**//* 1 BYTES */
typedef struct _FU_HEADER FU_HEADER;

struct _AU_HEADER
{
	//byte 0, 1
	unsigned short au_len;
	//byte 2,3
	unsigned  short frm_len:13;
	unsigned char au_index:3;
} __PACKED__; /**//* 1 BYTES */
typedef struct _AU_HEADER AU_HEADER;   

typedef enum
{
	RTSP_IDLE = 0,
	RTSP_CONNECTED = 1,
	RTSP_SENDING = 2,
}RTSP_STATUS;

typedef struct
{
	int  nVidLen;
	int  nAudLen;
	int bIsIFrm;
	int bWaitIFrm;
	int bIsFree;
	char vidBuf[RTSP_MAX_VID];
	char audBuf[RTSP_MAX_AUD];
}RTSP_PACK;

typedef struct
{
	int index;
	int socket;
	int reqchn;
	int seqnum;
	int seqnum2;
	unsigned int tsvid;
	unsigned int tsaud;
	int status;
	int sessionid;
	int rtpport[2];
	int rtcpport;
	char IP[20];
	char urlPre[PARAM_STRING_MAX];
}RTSP_CLIENT;

typedef struct
{
	int  vidLen;
	int  audLen;
	int  nFrameID;
	char vidBuf[RTSP_MAX_VID];
	char audBuf[RTSP_MAX_AUD];
}FRAME_PACK;

typedef struct
{
	int startcodeprefix_len;      //! 4 for parameter sets and first slice in picture, 3 for everything else (suggested)
	unsigned len;                 //! Length of the NAL unit (Excluding the start code, which does not belong to the NALU)
	unsigned max_size;            //! Nal Unit Buffer size
	int forbidden_bit;            //! should be always FALSE
	int nal_reference_idc;        //! NALU_PRIORITY_xxxx
	int nal_unit_type;            //! NALU_TYPE_xxxx
	char *buf;                    //! contains the first byte followed by the EBSP
	unsigned short lost_packets;  //! true, if packet loss is detected
} NALU_t;

class mpp_rtsp_server 
{
public:
	mpp_rtsp_server() { rtsp_server_init(); }
	~mpp_rtsp_server() { rtsp_server_exit(); }

    void rtsp_server_init(void);
    void rtsp_server_exit(void);

	void push_h264_stream(char *h264_buf, uint16_t buf_len);

};

#endif

