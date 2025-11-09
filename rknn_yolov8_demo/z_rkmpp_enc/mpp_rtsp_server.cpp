#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <time.h>
#include <netdb.h>
#include <sys/socket.h>

#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <netinet/if_ether.h>
#include <net/if.h>
#include <ifaddrs.h>


#include <linux/if_ether.h>
#include <linux/sockios.h>
#include <netinet/in.h> 
#include <arpa/inet.h> 

#include "mpp_rtsp_server.h"

RTSP_CLIENT g_rtspClients[MAX_RTSP_CLIENT];
int udpfd;
char *localip;

/*这5个参数都是对sendbuf中的元素取地址*/
RTP_FIXED_HEADER	*rtp_hdr; // rtp包头
NALU_HEADER			*nalu_hdr; // nalu包头
FU_INDICATOR		*fu_ind; 
FU_HEADER			*fu_hdr; 
AU_HEADER         *au_hdr;

int g_nSendDataChn = -1;
pthread_mutex_t g_mutex;		//互斥锁创建
pthread_cond_t  g_cond;			//线程同步
pthread_mutex_t g_sendmutex;	//互斥锁创建

pthread_t g_SendDataThreadId = 0;
char g_rtp_playload[20];
int  g_audio_rate = 8000;
int  g_nframerate;

int exitok = 0;
int h264_frame_cnt=0;

typedef struct _rtpbuf {
	int len;
	char *buf;
}rtp_buf_s;
deque<rtp_buf_s*> rtp_buf_deque;

static char const* dateHeader()
{
	static char buf[200];
#if !defined(_WIN32_WCE)
	time_t tt = time(NULL);
	strftime(buf, sizeof(buf), "Date: %a, %b %d %Y %H:%M:%S GMT\r\n", gmtime(&tt));
#endif
	return buf;
}

/*获取本机IPv4地址*/
static char *GetLocalIP()
{
    char * LocalIP = (char *)malloc(20);
    int sock_fd, intrface;
    struct ifreq buf[INET_ADDRSTRLEN];
    struct ifconf ifc;
    if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {
        ifc.ifc_len = sizeof(buf);
        ifc.ifc_buf = (caddr_t)buf;
        if (!ioctl(sock_fd, SIOCGIFCONF, (char *)&ifc)) {
            intrface = ifc.ifc_len / sizeof(struct ifreq);
            while (intrface-- > 0) {
                if (!(ioctl(sock_fd, SIOCGIFADDR, (char *)&buf[intrface]))) {
                    LocalIP = inet_ntoa(((struct sockaddr_in*)(&buf[intrface].ifr_addr))->sin_addr);
                    if (LocalIP != "127.0.0.1") {
                        break;
                    }
                }
            }
        }
        close(sock_fd);
    }
    return LocalIP;
}

static char* strDupSize(char const* str)
{
	if (str == NULL)
		return NULL;
	size_t len = strlen(str) + 1;
	char* copy = (char *)malloc(len);
	return copy;
}

/* 解析 RTSP 客户端请求
*
* 格式：
* 方法Method		URI			RTSP版本	CR+LF(回车换行)
* 消息头			CR			LF
* ......
* CR(回车)			LF(换行)
* 消息体			CR			LF
*
* RTSP 关键字段说明：
* ① 关键字OPTION：得到服务器提供的可用方法（OPTION、DESCRIBE、SETUP、TEARDOWN、PLAY、PAUSE、SCALE、GET_PARAMETER、SET_PARAMETER）
* ② 关键字DESCRIBE：请求流的 SDP 信息。注解：此处需要了解 H264 Law Data 如何生成 SPS PPS 信息。
* ③ 关键字SETUP：客户端提醒服务器建立会话，并建立传输模式。注解：此处确定了 RTP 传输交互式采用 TCP（面向连接）还是 UDP（无连接）模式。
* ④ 关键字PLAY：客户端发送播放请求。注解：此处引入 RTP 协议及 RTCP 协议。
* ⑤ 关键字PAUSE：播放暂停请求。注解：此关键字经常用在录像回放当中，实时视频流几乎用不到。
* ⑥ 关键字TEARDOWN:客户端发送关闭请求
* ⑦ 关键字GET_PARAMETER:从服务器获取参数，目前主要获取时间参数（可扩展）
* ⑧ 关键字SET_PARAMETER：给指定的 URL 或者流设置参数（可扩展）
示例---OPTIONS - 用来查询服务器支持的方法:
	OPTIONS rtsp://example.com/media.mp4 RTSP/1.0
	CSeq: 1
*/
static int ParseRequestString(char const* reqStr, // 要解析的字符串
			unsigned reqStrSize, // 字符串大小
			char* resultCmdName, // 解析出来的方法的存储指针
			unsigned resultCmdNameMaxSize, // resultCmdName 的长度
			char* resultURLPreSuffix, // URL 的前缀存储指针
			unsigned resultURLPreSuffixMaxSize, // URLPreSuffix 的长度
			char* resultURLSuffix, // URL 的存储指针
			unsigned resultURLSuffixMaxSize, // URLSuffix 的长度
			char* resultCSeq, // CSeq 的存储指针
			unsigned resultCSeqMaxSize) // CSeq 大小
{
	// 读取到第一个空格之前的所有内容作为命令名：
	int parseSucceeded = FALSE; //首先做了一个flag，来表示是否成功分析
	unsigned i;

	/* 解析方法名 */
	for (i = 0; i < resultCmdNameMaxSize-1 && i < reqStrSize; ++i) 
	{
		char c = reqStr[i];
		if (c == ' ' || c == '\t') { // 遇到空格或制表符，说明已经解析出命令名了
			parseSucceeded = TRUE;
			break;
		}
		resultCmdName[i] = c;
	}
	resultCmdName[i] = '\0'; //由于解析出来的是字符串，就需要将最后一个字符替换为字符串的结束标志
	if (!parseSucceeded) return FALSE;

	/* 跳过 "rtsp://" 或 "rtsp:/" 前缀 */
	unsigned j = i+1;
	while (j < reqStrSize && (reqStr[j] == ' ' || reqStr[j] == '\t')) ++j; //跳过多余的空格
	for (j = i+1; j < reqStrSize-8; ++j) //兼容大小写，作用是跳过rtsp://或rtsp:/
	{
		if ((reqStr[j] == 'r' || reqStr[j] == 'R')
			&& (reqStr[j+1] == 't' || reqStr[j+1] == 'T')
			&& (reqStr[j+2] == 's' || reqStr[j+2] == 'S')
			&& (reqStr[j+3] == 'p' || reqStr[j+3] == 'P')
			&& reqStr[j+4] == ':' && reqStr[j+5] == '/')
		{
			j += 6;
			if (reqStr[j] == '/') { // 然后找到'/'后面的不是'/'或者空格的那一个地址，就是解析出来的URL的首地址
				// This is a "rtsp://" URL; skip over the host:port part that follows:
				++j;
				while (j < reqStrSize && reqStr[j] != '/' && reqStr[j] != ' ') ++j;
			} else {
				// This is a "rtsp:/" URL; back up to the "/":
				--j;
			}
			i = j;
			break;
		}
	}

	/* 解析 URL 后缀 */
	parseSucceeded = FALSE;
	unsigned k;
	for (k = i+1; k < reqStrSize-5; ++k)
	{
		// 继续搜索直到找到 "RTSP/"
		if (reqStr[k] == 'R' && reqStr[k+1] == 'T' &&
			reqStr[k+2] == 'S' && reqStr[k+3] == 'P' && reqStr[k+4] == '/')
		{
			while (--k >= i && reqStr[k] == ' ') {} // go back over all spaces before "RTSP/"
			unsigned k1 = k;
			while (k1 > i && reqStr[k1] != '/' && reqStr[k1] != ' ') --k1;
			// the URL suffix comes from [k1+1,k]

			// 将 URL 后缀复制到 resultURLSuffix 中
			if (k - k1 + 1 > resultURLSuffixMaxSize) return FALSE; // there's no room
			unsigned n = 0, k2 = k1+1;
			while (k2 <= k) resultURLSuffix[n++] = reqStr[k2++];
			resultURLSuffix[n] = '\0';

			// 确定 URL 前缀部分，并将其复制到 resultURLPreSuffix 中
			unsigned k3 = --k1;
			while (k3 > i && reqStr[k3] != '/' && reqStr[k3] != ' ') --k3;
			// the URL pre-suffix comes from [k3+1,k1]
			if (k1 - k3 + 1 > resultURLPreSuffixMaxSize) return FALSE; // there's no room
			n = 0; k2 = k3+1;
			while (k2 <= k1) resultURLPreSuffix[n++] = reqStr[k2++];
			resultURLPreSuffix[n] = '\0';

			i = k + 7; // to go past " RTSP/"
			parseSucceeded = TRUE;
			break;
		}
	}
	if (!parseSucceeded) return FALSE;

	/* 解析出CSeq字段 */
	//CSeq (Command Sequence)：这是一个序列号，用于确保命令的顺序正确执行，并且帮助匹配请求与响应
	// Look for "CSeq:", skip whitespace
	// then read everything up to the next \r or \n as 'CSeq':
	parseSucceeded = FALSE;
	for (j = i; j < reqStrSize-5; ++j)
	{
		if (reqStr[j] == 'C' && reqStr[j+1] == 'S' && reqStr[j+2] == 'e' &&
			reqStr[j+3] == 'q' && reqStr[j+4] == ':')
		{
			j += 5;
			unsigned n;
			while (j < reqStrSize && (reqStr[j] ==  ' ' || reqStr[j] == '\t')) ++j;
			for (n = 0; n < resultCSeqMaxSize-1 && j < reqStrSize; ++n,++j)
			{
				char c = reqStr[j];
				if (c == '\r' || c == '\n') {
					parseSucceeded = TRUE;
					break;
				}
				resultCSeq[n] = c;
			}
			resultCSeq[n] = '\0';
			break;
		}
	}
	if (!parseSucceeded) return FALSE;
	return TRUE;
}

/* 应答 RTSP 客户端请求
* 格式：
* RTSP版本			状态码		解释	CR+LF(回车换行)
* 消息头			CR			LF
* ......
* CR(回车)			LF(换行)
* 消息体			CR			LF
*/

/* Describe 应答 - 得到服务器提供的可用方法 */
static int OptionAnswer(char *cseq, int sock)
{
	if (sock != 0) {
		char buf[1024];
		memset(buf,0,1024);
		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sPublic: %s\r\n\r\n",
						cseq,dateHeader(),"OPTIONS,DESCRIBE,SETUP,PLAY,PAUSE,TEARDOWN");

		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0) 
			return FALSE;
		else	
			printf(">>>>>%s\n",buf);
	
		return TRUE;
	}
	return FALSE;
}

/* Describe 应答 - 请求流的 SDP 信息 */
static int DescribeAnswer(char *cseq,int sock,char * urlSuffix,char* recvbuf)
{
	if (sock != 0) {
		char sdpMsg[1024];
		char buf[2048];
		memset(buf,0,2048);
		memset(sdpMsg,0,1024);
		localip = GetLocalIP();

		/* 接下来，就按照RTSP的DESCRIBE标准进行回复 */
		char *pTemp = buf;
		pTemp += sprintf(pTemp, "RTSP/1.0 200 OK\r\nCSeq: %s\r\n", cseq);
		pTemp += sprintf(pTemp, "%s", dateHeader());
		pTemp += sprintf(pTemp, "Content-Type: application/sdp\r\n");

		/*
		RTP协议中，使用VLC进行播放，需要有一个SDP文件进行解析才能播放，RTP传的是裸流，没有控制信息，需要本地配置，
		而RTSP协议中，SDP包含在交互信息中，不需要用VLC打开SDP文件，通过解析交互信息，客户端就能够知道怎么去解析视频流
		*/
		char *pTemp2 = sdpMsg;
		pTemp2 += sprintf(pTemp2, "v=0\r\n");
		pTemp2 += sprintf(pTemp2, "o=StreamingServer 3331435948 1116907222000 IN IP4 %s\r\n", localip);
		pTemp2 += sprintf(pTemp2, "s=H.264\r\n");
		pTemp2 += sprintf(pTemp2, "c=IN IP4 0.0.0.0\r\n");
		pTemp2 += sprintf(pTemp2, "t=0 0\r\n");
		pTemp2 += sprintf(pTemp2, "a=control:rtsp://%s/live/stream/\r\n", localip); // 拉流地址

		/*H264 TrackID=0 RTP_PT 96*/
		pTemp2 += sprintf(pTemp2, "m=video 0 RTP/AVP 96\r\n");
		pTemp2 += sprintf(pTemp2, "a=control:trackID=0\r\n");
		pTemp2 += sprintf(pTemp2, "a=rtpmap:96 H264/90000\r\n");
		pTemp2 += sprintf(pTemp2, "a=fmtp:96 packetization-mode=1; sprop-parameter-sets=%s\r\n", "AAABBCCC");

#if defined(AUDIO_ENABLE)
		/*G726*/
		pTemp2 += sprintf(pTemp2,"m=audio 0 RTP/AVP 97\r\n");
		pTemp2 += sprintf(pTemp2,"a=control:trackID=1\r\n");
		if(strcmp(g_rtp_playload,"AAC")==0)
		{
			pTemp2 += sprintf(pTemp2,"a=rtpmap:97 MPEG4-GENERIC/%d/2\r\n",16000);
			pTemp2 += sprintf(pTemp2,"a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1410\r\n");
		}
		else
		{
			pTemp2 += sprintf(pTemp2,"a=rtpmap:97 G726-32/%d/1\r\n",8000);
			pTemp2 += sprintf(pTemp2,"a=fmtp:97 packetization-mode=1\r\n");
		}
#endif

		pTemp += sprintf(pTemp, "Content-length: %d\r\n", strlen(sdpMsg));
		pTemp += sprintf(pTemp, "Content-Base: rtsp://%s/%s/\r\n\r\n", localip, urlSuffix);

		//printf("mem ready\n");
		strcat(pTemp, sdpMsg);
		//printf("Describe ready sent\n");
		int re = send(sock, buf, strlen(buf),0);
		if(re <= 0)
			return FALSE;
		else
			printf(">>>>>%s\n",buf);
	}
	return TRUE;
}

static void ParseTransportHeader(char const* buf,
						StreamingMode* streamingMode,
						char** streamingModeString,
						char** destinationAddressStr,
						u_int8_t* destinationTTL,
						portNumBits* clientRTPPortNum, // if UDP
						portNumBits* clientRTCPPortNum,// if UDP
						unsigned char* rtpChannelId,	// if TCP
						unsigned char* rtcpChannelId	// if TCP
						)
{
	// Initialize the result parameters to default values:
	*streamingMode = RTP_UDP;
	*streamingModeString = NULL;
	*destinationAddressStr = NULL;
	*destinationTTL = 255;
	*clientRTPPortNum = 0;
	*clientRTCPPortNum = 1;
	*rtpChannelId = *rtcpChannelId = 0xFF;

	portNumBits p1, p2;
	unsigned ttl, rtpCid, rtcpCid;

	// First, find "Transport:"
	while (1) {
		if (*buf == '\0') return; // not found
		if (strncasecmp(buf, "Transport: ", 11) == 0) break;
		++buf;
	}

	// Then, run through each of the fields, looking for ones we handle:
	char const* fields = buf + 11;
	char* field = strDupSize(fields);
	while (sscanf(fields, "%[^;]", field) == 1) {
		if (strcmp(field, "RTP/AVP/TCP") == 0) {
			*streamingMode = RTP_TCP;

		} else if (strcmp(field, "RAW/RAW/UDP") == 0 ||
			strcmp(field, "MP2T/H2221/UDP") == 0) {
			*streamingMode = RAW_UDP;
			//*streamingModeString = strDup(field);

		} else if (strncasecmp(field, "destination=", 12) == 0)
		{
			//delete[] destinationAddressStr;
			free(destinationAddressStr);
			//destinationAddressStr = strDup(field+12);

		} else if (sscanf(field, "ttl%u", &ttl) == 1) {
			*destinationTTL = (u_int8_t)ttl;

		} else if (sscanf(field, "client_port=%hu-%hu", &p1, &p2) == 2) {
			*clientRTPPortNum = p1;
			*clientRTCPPortNum = p2;

		} else if (sscanf(field, "client_port=%hu", &p1) == 1) {
			*clientRTPPortNum = p1;
			*clientRTCPPortNum = (*streamingMode == RAW_UDP) ? 0 : p1 + 1;

		} else if (sscanf(field, "interleaved=%u-%u", &rtpCid, &rtcpCid) == 2) {
			*rtpChannelId = (unsigned char)rtpCid;
			*rtcpChannelId = (unsigned char)rtcpCid;
		}

		fields += strlen(field);
		while (*fields == ';') ++fields; // skip over separating ';' chars
		if (*fields == '\0' || *fields == '\r' || *fields == '\n') break;
	}
	free(field);
}

/* Setup 应答 - 客户端提醒服务器建立会话，并建立传输模式 */
static int SetupAnswer(char *cseq,int sock,int SessionId,char * urlSuffix,char* recvbuf,int* rtpport, int* rtcpport)
{
	if (sock != 0) {
		char buf[1024];
		memset(buf,0,1024);

		StreamingMode streamingMode;
		char* streamingModeString; // set when RAW_UDP streaming is specified
		char* clientsDestinationAddressStr;
		u_int8_t clientsDestinationTTL;
		portNumBits clientRTPPortNum, clientRTCPPortNum;
		unsigned char rtpChannelId, rtcpChannelId;
		ParseTransportHeader(recvbuf,&streamingMode, &streamingModeString,
					&clientsDestinationAddressStr, &clientsDestinationTTL,
					&clientRTPPortNum, &clientRTCPPortNum,
					&rtpChannelId, &rtcpChannelId);

		//Port clientRTPPort(clientRTPPortNum);
		//Port clientRTCPPort(clientRTCPPortNum);
		*rtpport = clientRTPPortNum;
		*rtcpport = clientRTCPPortNum;

		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sTransport: RTP/AVP;unicast;destination=%s;client_port=%d-%d;server_port=%d-%d\r\nSession: %d\r\n\r\n",
						cseq, dateHeader(), localip,
						ntohs(htons(clientRTPPortNum)),
						ntohs(htons(clientRTCPPortNum)),
						ntohs(2000),
						ntohs(2001),
						SessionId);

		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s",buf);
		}
		return TRUE;
	}
	return FALSE;
}

/* Play 应答 - 客户端发送播放请求 */
static int PlayAnswer(char *cseq, int sock,int SessionId,char* urlPre,char* recvbuf)
{
	//客户端play请求后，服务端开启一个UDP的SOCKET通道进行裸流的传输，命令走的是TCP通道。这就是RTSP的设计
	if (sock != 0) {
		char buf[1024];
		memset(buf, 0, 1024);
		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sRange: npt=0.000-\r\nSession: %d\r\nRTP-Info: url=rtsp://%s/%s;seq=0\r\n\r\n",
			cseq, dateHeader(), SessionId, localip, urlPre);

		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0) {
			return FALSE;
		} else {
			printf(">>>>>%s",buf);
			udpfd = socket(AF_INET, SOCK_DGRAM, 0);//UDP
			struct sockaddr_in server;
			server.sin_family = AF_INET;
			server.sin_port = htons(g_rtspClients[0].rtpport[0]);
			server.sin_addr.s_addr = inet_addr(g_rtspClients[0].IP);
			connect(udpfd, (struct sockaddr *)&server, sizeof(server));
			printf("udp up\n");
		}
		return TRUE;
	}
	return FALSE;
}

/* Pause 应答 - 播放暂停请求 */
static int PauseAnswer(char *cseq,int sock,char *recvbuf)
{
	if (sock != 0) {
		char buf[1024];
		memset(buf,0,1024);
		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%s\r\n\r\n",
			cseq,dateHeader());

		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0) {
			return FALSE;
		} else {
			printf(">>>>>%s",buf);
		}
		return TRUE;
	}
	return FALSE;
}

/* Teardown 应答 - 客户端发送关闭请求 */
static int TeardownAnswer(char *cseq,int sock,int SessionId,char *recvbuf)
{
	if (sock != 0)
	{
		char buf[1024];
		memset(buf,0,1024);
		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sSession: %d\r\n\r\n",
			cseq,dateHeader(),SessionId);

		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s",buf);
			close(udpfd);
		}
		return TRUE;
	}
	return FALSE;
}

/* 服务器响应客户端消息线程 */
static void * repose_rtsp_clinet_msg_thread(void*pParam)
{
	pthread_detach(pthread_self());
	int nRes;
	char pRecvBuf[RTSP_RECV_SIZE];
	RTSP_CLIENT * pClient = (RTSP_CLIENT*)pParam;

	memset(pRecvBuf,0,sizeof(pRecvBuf));
	printf("RTSP:-----Create Client %s\n",pClient->IP);
	while(pClient->status != RTSP_IDLE)
	{
		nRes = recv(pClient->socket, pRecvBuf, RTSP_RECV_SIZE,0);
		//printf("--------- %d\n",nRes);
		if(nRes < 1) // 如果读出来的字节数小于0，那么是错误的，把状态重新改为置空
		{
			//usleep(1000);
			printf("RTSP:Recv Error--- %d\n",nRes);
			g_rtspClients[pClient->index].status = RTSP_IDLE;
			g_rtspClients[pClient->index].seqnum = 0;
			g_rtspClients[pClient->index].tsvid = 0;
			g_rtspClients[pClient->index].tsaud = 0;
			close(pClient->socket);
			break;
		}

		/* 解析 RTSP 客户端请求 */
		char cmdName[PARAM_STRING_MAX];
		char urlPreSuffix[PARAM_STRING_MAX];
		char urlSuffix[PARAM_STRING_MAX];
		char cseq[PARAM_STRING_MAX];
		/* 对RTSP消息头进行解析 */
		ParseRequestString(pRecvBuf,nRes,cmdName,sizeof(cmdName),urlPreSuffix,sizeof(urlPreSuffix),
							urlSuffix,sizeof(urlSuffix),cseq,sizeof(cseq));

		char *p = pRecvBuf;
		printf("<<<<<%s\n",p);
		//printf("---------- %s %s ----------\n",urlPreSuffix,urlSuffix);

		/* 应答 RTSP 客户端请求 */
		if(strstr(cmdName, "OPTIONS")) {
			OptionAnswer(cseq,pClient->socket);

		} else if(strstr(cmdName, "DESCRIBE")) {
			DescribeAnswer(cseq,pClient->socket,urlSuffix,p);
			printf("---------DescribeAnswer %s %s\n", urlPreSuffix,urlSuffix);

		} else if(strstr(cmdName, "SETUP")) {
			int rtpport,rtcpport;
			int trackID=0;
			SetupAnswer(cseq,pClient->socket,pClient->sessionid,urlPreSuffix,p,&rtpport,&rtcpport);

			sscanf(urlSuffix, "trackID=%u", &trackID);
			//printf("---------TrackId %d\n",trackID);
			if(trackID<0 || trackID>=2)trackID=0;
			g_rtspClients[pClient->index].rtpport[trackID] = rtpport;
			g_rtspClients[pClient->index].rtcpport= rtcpport;
			g_rtspClients[pClient->index].reqchn = atoi(urlPreSuffix);
			if(strlen(urlPreSuffix)<100)
				strcpy(g_rtspClients[pClient->index].urlPre,urlPreSuffix);
			printf("---------SetupAnswer %s-%d-%d\n", urlPreSuffix,g_rtspClients[pClient->index].reqchn,rtpport);

		} else if(strstr(cmdName, "PLAY")) {
			PlayAnswer(cseq,pClient->socket,pClient->sessionid,g_rtspClients[pClient->index].urlPre,p);
			g_rtspClients[pClient->index].status = RTSP_SENDING;
			printf("Start Play\n", pClient->index);
			printf("----------------PlayAnswer %d %d\n",pClient->index);
			usleep(100);

		} else if(strstr(cmdName, "PAUSE")) {
			PauseAnswer(cseq,pClient->socket,p);
		}

		else if(strstr(cmdName, "TEARDOWN")) {
			TeardownAnswer(cseq,pClient->socket,pClient->sessionid,p);
			g_rtspClients[pClient->index].status = RTSP_IDLE;
			g_rtspClients[pClient->index].seqnum = 0;
			g_rtspClients[pClient->index].tsvid = 0;
			g_rtspClients[pClient->index].tsaud = 0;
			close(pClient->socket);
		}
	}
	printf("RTSP:-----Exit Client %s\n",pClient->IP);
	return NULL;
}

/* RTSP服务器监听客户端线程 */
static void *rtsp_server_listen_thread(void *arg)
{
	/*这里主要就是网络编程中配置端口号，修改网络字节序的内容，端口号使用的是554，这是RTSP的默认端口号
	  setsockopt来设置socket属性，设置了SO_REUSEADDR可以用来复用*/
	int s32Socket;
	struct sockaddr_in servaddr;
	int s32CSocket;
	int s32Rtn;
	int s32Socket_opt_value = 1;
	unsigned int nAddrLen;
	struct sockaddr_in addrAccept;
	int bResult;

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port = htons(RTSP_SERVER_PORT);

	s32Socket = socket(AF_INET, SOCK_STREAM, 0);

	/* 设置 socket 属性 */
	if (setsockopt(s32Socket, SOL_SOCKET, SO_REUSEADDR, &s32Socket_opt_value, sizeof(int)) == -1) {
		printf("setsockopt error\n");
		return (void *)(-1);
	}

	s32Rtn = bind(s32Socket, (struct sockaddr *)&servaddr, sizeof(sockaddr_in));
	if(s32Rtn < 0) {
		printf("bind error\n");
		return (void *)(-2);
	}

	s32Rtn = listen(s32Socket, 50);
	if(s32Rtn < 0) {
		printf("listen error\n");
		return (void *)(-2);
	}

	nAddrLen = sizeof(sockaddr_in);
	int nSessionId = 1000;
	printf("<<<<wait RTSP Client Connect...\n");
	/* 阻塞等待客服端连接 */
	while ((s32CSocket = accept(s32Socket, (struct sockaddr*)&addrAccept, &nAddrLen)) >= 0) {
		printf("<<<<RTSP Client %s Connected...\n", inet_ntoa(addrAccept.sin_addr));

		int nMaxBuf = 10 * 1024; //接着再进行socket的设置，选项是SO_SNDBUF，即设置发送缓冲区的大小
		if(setsockopt(s32CSocket, SOL_SOCKET, SO_SNDBUF, (char*)&nMaxBuf, sizeof(nMaxBuf)) == -1)
			printf("RTSP:!!!!!! Enalarge socket sending buffer error !!!!!!\n");

		int i;
		int bAdd=FALSE;
		for(i = 0; i < MAX_RTSP_CLIENT; i++) {  	//然后对新连接的rtspClient的status进行判断，如果等于RTSP_IDLE就进行处理
			if(g_rtspClients[i].status == RTSP_IDLE) { 	//进if后，就把他的状态换成connected，避免反复操作
				memset(&g_rtspClients[i], 0, sizeof(RTSP_CLIENT));
				g_rtspClients[i].index = i;
				g_rtspClients[i].socket = s32CSocket;
				g_rtspClients[i].status = RTSP_CONNECTED ;//RTSP_SENDING;
				g_rtspClients[i].sessionid = nSessionId++;
				strcpy(g_rtspClients[i].IP,inet_ntoa(addrAccept.sin_addr));
				pthread_t threadIdlsn = 0;

				struct sched_param sched;
				sched.sched_priority = 1;
				pthread_create(&threadIdlsn, NULL, repose_rtsp_clinet_msg_thread, &g_rtspClients[i]);
				//pthread_setschedparam(threadIdlsn,SCHED_RR,&sched);
				bAdd = TRUE;
				break;
			}
		}
		if(bAdd==FALSE) {  //如果超过最大限制，那么就将超过的覆盖到原来的0，这样循环覆盖下去（设计方面的思路，也可以超过直接不再连接）
			memset(&g_rtspClients[0], 0, sizeof(RTSP_CLIENT));
			g_rtspClients[0].index = 0;
			g_rtspClients[0].socket = s32CSocket;
			g_rtspClients[0].status = RTSP_CONNECTED ;//RTSP_SENDING;
			g_rtspClients[0].sessionid = nSessionId++;
			strcpy(g_rtspClients[0].IP, inet_ntoa(addrAccept.sin_addr));

			pthread_t threadIdlsn = 0;
			struct sched_param sched;
			sched.sched_priority = 1;
			pthread_create(&threadIdlsn, NULL, repose_rtsp_clinet_msg_thread, &g_rtspClients[0]);
			//pthread_setschedparam(threadIdlsn,SCHED_RR,&sched);
			bAdd = TRUE;
		}
	}
	if(s32CSocket < 0) {
	//printf(0, "RTSP listening on port %d,accept err, %d\n", RTSP_SERVER_PORT, s32CSocket);
	}
	printf("----- INIT_RTSP_Listen() Exit !! \n");
	return NULL;
}

/**
 * VENC_Sent函数用于向RTSP客户端发送视频编码数据。
 * 该函数通过UDP协议，根据RTSP协议规范，封装并发送视频数据。
 * 
 * @param buffer 指向待发送的视频数据缓冲区。
 * @param buflen 视频数据缓冲区的长度。
 * @return 返回发送结果的整数值，0表示成功，非零表示失败。
 */
static int send_h264_for_rtsp_client(char *buffer, int buflen)
{
	int i;
	int is = 0;
	int nChanNum = 0;

	 // 遍历所有RTSP客户端，MAX_RTSP_CLIENT为最大客户端数量
	for(is = 0; is < MAX_RTSP_CLIENT; is++) {
		 // 检查客户端状态，只有在发送状态下的客户端才会被处理
		if(g_rtspClients[is].status != RTSP_SENDING) {
			continue;
		}
		// 计算心跳包序列号
		int heart = g_rtspClients[is].seqnum % 10000;

		char* nalu_payload;
		int nAvFrmLen = 0; // 视频帧长度
		int nIsIFrm = 0; // 是否为I帧
		int nNaluType = 0;
		char sendbuf[500*1024 + 32];

		nAvFrmLen = buflen; // 设置视频帧长度

		// 初始化服务器地址结构
		struct sockaddr_in server;
		server.sin_family = AF_INET;
		server.sin_port = htons(g_rtspClients[is].rtpport[0]);
		server.sin_addr.s_addr = inet_addr(g_rtspClients[is].IP);
		int	bytes = 0;
		unsigned int timestamp_increse = 0;

		// 计算时间戳增量
		timestamp_increse = (unsigned int)(90000.0 / 25); // H.264 编码的一个时间间隔标准

		// 初始化RTP头
		rtp_hdr =(RTP_FIXED_HEADER*)&sendbuf[0];

		rtp_hdr->payload = RTP_H264; // 编码类型
		rtp_hdr->version = 2; // 版本号
		rtp_hdr->marker  = 0; // 标记位:通常表示该包是否是某个序列的最后一个包
		rtp_hdr->ssrc    = htonl(10); // 同步源标识符

		// 根据视频帧长度处理数据
		if(nAvFrmLen <= nalu_sent_len) {
			/* 设置RTP包头：占12个字节 */
			rtp_hdr->marker=1;
			// 用于标识 RTP 包的顺序，每发送一个 RTP 包，序列号递增 1。接收端可以使用序列号来检测丢包和重排序
			rtp_hdr->seq_no = htons(g_rtspClients[is].seqnum++); // 序列号
			rtp_hdr->timestamp = htonl(g_rtspClients[is].tsvid); // 时间戳

			g_rtspClients[is].tsvid = g_rtspClients[is].tsvid + timestamp_increse; // 更新时间戳

			/* 设置NALU头：占一个字节 */
			nalu_hdr = (NALU_HEADER*)&sendbuf[12];
			nalu_hdr->F = 0; //这个位必须设置为 0。如果检测到这个位为 1，接收端可能会丢弃该 NAL 单元
			nalu_hdr->NRI = nIsIFrm; //NAL 单元的重要性级别。值越大，表示该 NAL 单元越重要(0-3)
			nalu_hdr->TYPE = nNaluType; //表示 NAL 单元的具体类型。不同的 NAL 单元类型有不同的用途

			/* 拷贝数据 */ 
			nalu_payload = &sendbuf[13];
			memcpy(nalu_payload, buffer, nAvFrmLen);  
			
			/* 发送数据 */
			bytes = nAvFrmLen + 13 ;
			sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));

		} else if(nAvFrmLen > nalu_sent_len) {
			// 处理大于MTU的数据，需要分片：包头、包中、包尾
			int k = 0, l = 0;
			k = nAvFrmLen / nalu_sent_len;
			l = nAvFrmLen % nalu_sent_len;
			int t = 0;

			/*时间戳*/
			g_rtspClients[is].tsvid = g_rtspClients[is].tsvid+timestamp_increse;
			rtp_hdr->timestamp = htonl(g_rtspClients[is].tsvid);

			/*循环*/
			while(t <= k) {
				rtp_hdr->seq_no = htons(g_rtspClients[is].seqnum++); //序号
				if(t == 0) {
					// 包头
					rtp_hdr->marker=0; // 表示不是最后一个包

					fu_ind = (FU_INDICATOR*)&sendbuf[12];
					fu_ind->F    = 0; // 必须设置为 0
					fu_ind->NRI  = 2; // 表示该 NAL 单元的重要性级别
					fu_ind->TYPE = 28; // 表示 NAL 单元的类型为FU-A

					/* 表示第一个包 */
					fu_hdr = (FU_HEADER*)&sendbuf[13];
					fu_hdr->E    = 0; // 不是最后一个包
					fu_hdr->R    = 0; // 无用
					fu_hdr->S    = 1; // 是第一个包
					fu_hdr->TYPE = nNaluType;

					nalu_payload = &sendbuf[14];
					memcpy(nalu_payload, buffer, nalu_sent_len);

					bytes = nalu_sent_len + 14;
					sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server, sizeof(server));
					t++;

				} else if(k == t) {
					// 包尾
					rtp_hdr->marker=1;
					fu_ind =(FU_INDICATOR*)&sendbuf[12];
					fu_ind->F= 0 ;
					fu_ind->NRI= nIsIFrm ;
					fu_ind->TYPE=28;

					/* 表示最后一个包 */
					fu_hdr =(FU_HEADER*)&sendbuf[13];
					fu_hdr->R=0;
					fu_hdr->S=0;
					fu_hdr->TYPE= nNaluType;
					fu_hdr->E=1; 

					nalu_payload=&sendbuf[14];
					memcpy(nalu_payload,buffer+t*nalu_sent_len,l);
					bytes=l+14;
					sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
					t++;

				} else if(t<k && t!=0) {
					// 中间包
					rtp_hdr->marker=0;

					fu_ind =(FU_INDICATOR*)&sendbuf[12];
					fu_ind->F=0; 
					fu_ind->NRI=nIsIFrm;
					fu_ind->TYPE=28;				

					/* 表示中间包 */
					fu_hdr =(FU_HEADER*)&sendbuf[13];
					fu_hdr->R=0;
					fu_hdr->S=0;
					fu_hdr->E=0;
					fu_hdr->TYPE=nNaluType;

					nalu_payload=&sendbuf[14];
					memcpy(nalu_payload,buffer+t*nalu_sent_len,nalu_sent_len);
					bytes=nalu_sent_len+14;
					sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
					t++;
				}
			}
		}
	}
}

/* 功能 : 视频流发送 - RTP缓存方式发送(消费者) */
static void* rtp_send_thread(void *arg)
{
	while(1) {   //每隔5微妙在RTPbuf_head链表里查找数据，有数据就进行发送
		if(!rtp_buf_deque.empty()) {
			rtp_buf_s *rtp_buf = rtp_buf_deque.front();
			send_h264_for_rtsp_client(rtp_buf->buf, rtp_buf->len);
			rtp_buf_deque.pop_front();
			free(rtp_buf->buf);
			free(rtp_buf);
			rtp_buf = nullptr;
			h264_frame_cnt--;
		}
		usleep(5000);
	}
}

/* 功能 : 视频流缓存 - 存入链表(生产者) 一般在外部调用，使用这个函数存放数据流*/
void mpp_rtsp_server::push_h264_stream(char *h264_buf, uint16_t buf_len)
{
	for(uint8_t j = 0; j < MAX_RTSP_CLIENT; j++) { //客服端连接个数判断
		if(g_rtspClients[j].status == RTSP_SENDING) {
			rtp_buf_s *rtp_buf = (rtp_buf_s*)malloc(sizeof(rtp_buf_s));
			rtp_buf->buf = (char *)malloc(buf_len);
			rtp_buf->len = buf_len;
			memcpy(rtp_buf->buf, h264_buf, buf_len);
			rtp_buf_deque.push_back(rtp_buf);
			h264_frame_cnt++; // h264_frame_cnt是用来记录剩余编码完的帧的数量，发送的话就去减减，到0说明编码完的帧用完了
		}
	}
}

/*服务器一定是先运行，然后像socket那样初始化完成后阻塞监听，等待客户端连接*/
void mpp_rtsp_server::rtsp_server_init(void)
{
	pthread_t threadId = 0;
	pthread_t gs_RtpPid = 0;

	/* RTSP音频相关 */
	/*接着为payload的存放申请空间，以RTP开头，是应为rtsp与rtp之间有联系，rtsp实际上是以rtp包来发送的，所以以rtp开头
	  g_rtp_playload是一个20个字节的数组，后面将G726-32放入数组，这表示是一种音频标准的payload*/
	memset(g_rtp_playload, 0, sizeof(g_rtp_playload));
	strcpy(g_rtp_playload, "G726-32");
	g_audio_rate = 8000; // 音频的采样率，后续没有用到，如果做音频相关的东西就可以进行使用

	/* 变量初始化 */
	pthread_mutex_init(&g_sendmutex, NULL);
	pthread_mutex_init(&g_mutex, NULL);
	pthread_cond_init(&g_cond, NULL);
	memset(&g_rtspClients, 0, sizeof(RTSP_CLIENT) * MAX_RTSP_CLIENT); //初始化客户端信息存储变量

	pthread_create(&threadId, NULL, rtsp_server_listen_thread, NULL); //开启了一个线程进行监听
	printf("RTSP:-----Init Rtsp server\n");
	/* RTP视频流发送线程 - 缓存方式发送 */
	pthread_create(&gs_RtpPid, 0, rtp_send_thread, NULL);

}

void mpp_rtsp_server::rtsp_server_exit(void)
{
	return;
}



