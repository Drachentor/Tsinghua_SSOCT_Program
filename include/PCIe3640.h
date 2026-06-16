#ifndef	PCIE_3630_DLL_H
#define	PCIE_3630_DLL_H

#include <tchar.h>
#include <windows.h>
typedef void* FWHANDLE;
typedef FWHANDLE* PFWHANDLE;
typedef PFWHANDLE LPFWHANDLE;

//AD采集初始化参数
#ifndef _PCIe3640_PARA_INIT
typedef struct _PCIe3640_PARA_INIT    
{
	LONG    lClkDiv;            //分频，取值1,2,4,8,16...256
    LONG    lChCnt;             //通道数1-CH1使能；2-CH1 CH2使能；4-CH1 CH2 CH3 CH4使能; 8-CH1 CH2 CH3...CH8使能 
	LONG	TriggerMode;        //触发模式
	LONG	TriggerSource;	    //触发源 
	LONG    TriggerDelay;       //触发延时（触发后移）
	LONG    TriggerPre;			//前触发  （触发前移）
	LONG    TriggerLength;      //触发长度
	LONG    TriggerLevel;       //触发门限
	LONG    lSelDataSrc;        //数据源选择，0-AD数据源 1-计数器数据源 
	LONG    lADFmt;             //AD数据输出格式，0表示直接二进制输出  1表示补码输出  	
	LONG    lLineNum;           //触发去抖动参数

} PCIe3640_PARA_INIT,*PPCIe3640_PARA_INIT;
#endif

//采样钟选择，内钟或外钟 
typedef enum EmADClkSel
{
	ADCLK_INT        = 0, 
	ADCLK_EXT        = 1
} ADCLK_SEL;

//DA源选择
typedef enum EmADDASrcSel
{
	DA_SRC_RF          = 0,  //射频DA
	DA_SRC_DACH1       = 1,  //1M DA1
	DA_SRC_DACH2       = 2,  //1M DA2	
	DA_SRC_DACH3       = 3,  //1M DA3
	DA_SRC_DACH4       = 4   //1M DA4
} DASRC_SEL;


//AD数据输出格式
typedef enum EmADFormat
{
	ADFMT_STBIN    = 0, //直接二进制输出
	ADFMT_2SBIN    = 1  //二进制补码输出
} AD_FORMAT;

//AD模式
typedef enum EmADMODE
{	
	ADMODE_1X        = 0,  //1X模式
	ADMODE_2X        = 1   //2X模式
} AD_MODE;

//触发模式
typedef enum EmTriggerMode
{
	TRIG_MODE_CONTINUE        = 0, //连续采集
	TRIG_MODE_POST            = 1, //后触发或行同步触发		
	TRIG_MODE_1TRIG			  = 2, //单触发			
	TRIG_MODE_FTRIG			  = 3  //场同步触发
} TRIGGER_MODE;

//触发源
typedef enum EmTriggerSource
{
	TRIG_SRC_EXT_RISING      = 0,  //外正沿
	TRIG_SRC_EXT_FALLING     = 1,  //外负沿	
	TRIG_SRC_SOFT            = 2,  //软件触发
	TRIG_SRC_INT_RISING      = 3,  //内正沿
	TRIG_SRC_INT_FALLING     = 4,  //内负沿
	TRIG_SRC_CH1_RISING      = 5,  //CH1正沿
	TRIG_SRC_CH1_FALLING     = 6,  //CH1负沿
	TRIG_SRC_CH2_RISING      = 7,  //CH2正沿
	TRIG_SRC_CH2_FALLING     = 8,  //CH2负沿
	TRIG_SRC_CH3_RISING      = 9,  //CH3正沿
	TRIG_SRC_CH3_FALLING     = 10, //CH3负沿
	TRIG_SRC_CH4_RISING      = 11, //CH4正沿
	TRIG_SRC_CH4_FALLING     = 12, //CH4负沿
	TRIG_SRC_CH5_RISING      = 13, //CH5正沿
	TRIG_SRC_CH5_FALLING     = 14, //CH5负沿
	TRIG_SRC_CH6_RISING      = 15, //CH6正沿
	TRIG_SRC_CH6_FALLING     = 16, //CH6负沿
	TRIG_SRC_CH7_RISING      = 17, //CH7正沿
	TRIG_SRC_CH7_FALLING     = 18, //CH7负沿
	TRIG_SRC_CH8_RISING      = 19, //CH8正沿
	TRIG_SRC_CH8_FALLING     = 20  //CH8负沿
} TRIGGER_SOURCE;

//触发长度单位
#define   TRIG_UNIT	 8//8个采样点

//读/写用户内存操作
#define WRITEFLASH 0 //写
#define READFLASH  1 //读

//最小读长度
#define  MIN_READ_LEN  0x200//512样点

//最大读长度
#define  MAX_READ_LEN 0x2000000//32M 样点

//累加最大触发长度
#define  MAX_ADD_TRIG_LEN  0x100000//1M样点

#define ADD_LEN_UNIT    256
#define FIFO_UNIT		16 


//PCIe 缓冲区定义
#ifndef  _PCIE_BUF
typedef struct _PCIE_BUF
{
	PVOID64 pDABufA;//下行（PC到板卡）缓冲区A
	PVOID64 pDABufB;//下行缓冲区B
	PVOID64 pADBufA;//上行（板卡到PC）缓冲区A
	PVOID64 pADBufB;//上行缓冲区B
} PCIE_BUF,*PPCIE_BUF;
#endif

//板卡信息
#ifndef _CARD_INFO
typedef struct _CARD_INIT    
{
	LONG  CARD_VER;   //版本号
	LONG  AD_BIT;     //AD位数   
	LONG  AD_CHCNT;   //AD通道数
	LONG  AD_SPEED;   //AD速度   
	LONG  AD_FIFO;    //AD板载FIFO
} CARD_INFO,*PCARD_INFO;
#endif

//***********************************************************

#ifndef DEFINING
#define DEVAPI __declspec(dllimport)
#else
#define DEVAPI __declspec(dllexport)
#endif


#ifdef __cplusplus
extern "C" {
#endif
	//连接设备
	DEVAPI HANDLE  PCIe3640_Link(LONG devNum,PPCIE_BUF pcieBuf);
	//断开设备
	DEVAPI BOOL  PCIe3640_UnLink(HANDLE hdl);
	//初始化AD参数，并开始采集
	DEVAPI BOOL  PCIe3640_initAD(HANDLE hdl, PPCIe3640_PARA_INIT ppara_init);
	//读取AD数据
	DEVAPI BOOL  PCIe3640_ReadAD(HANDLE hdl,LONG bufIndex,LONG nCount,LONG timeout);
	//停止AD采集
	DEVAPI BOOL  PCIe3640_StopAD(HANDLE hdl);
	//设置DO
	DEVAPI BOOL  PCIe3640_SetDO(HANDLE hdl, LONG nDO);
	//读取DI
	DEVAPI BOOL  PCIe3640_GetDI(HANDLE hdl, LONG* nDI);
	//读取FIFO中采样点个数
	DEVAPI ULONG  PCIe3640_GetBufCnt(HANDLE hdl);
	//软件触发
	DEVAPI BOOL  PCIe3640_ExeSoftTrig(HANDLE hdl);
	//读取设备信息
	DEVAPI BOOL  PCIe3640_GetDevInfo(HANDLE hdl,PCARD_INFO pCardInfo);
	//PWM(内触发)发生器
	DEVAPI BOOL  PCIe3640_SetPulGen(HANDLE hdl, ULONG32 lAllcnt, ULONG32 lHighCnt, BOOL bEnable);
	//读写用户空间
	DEVAPI BOOL  PCIe3640_WrRdFlash(HANDLE hdl, BOOL bWtRd, PLONG pMem);
	//初始化复位所有DAC
	DEVAPI BOOL  PCIe3640_intDA(HANDLE hdl);
	//设置DA
	DEVAPI BOOL  PCIe3640_SetDA(HANDLE hdl,LONG selDA,LONG cycCnt,PUSHORT buf,LONG delay,LONG lAGC);
	//启动DA
	DEVAPI BOOL  PCIe3640_StartDA(HANDLE hdl,LONG stDA,LONG pwmDelay);
	//读取触发个数
	DEVAPI ULONG  PCIe3640_GetTrigCnt(HANDLE hdl);
	//软件触发
	DEVAPI float  PCIe3640_GetTemp(HANDLE hdl);

	/************************************写盘函数******************************************/
	/*
		Create File for fast write
		lpFileName: 指定文件名
		dwShareMode: 共享模式，参考Win32 API "CreateFile"
		nQueueMax: 请求队列最大深度，为0或者负数表示不限制队列深度。在调用FWFastWrite写入数据时，在数据实际被写入磁盘之前在这里缓存，如果请求队列深度达到这里设置的最大值，则FWFastWrite函数会阻塞，直到队列深度减小（数据被写入磁盘），FWFastWrite调用才会返回
		lpHandle: 返回打开的文件的句柄，注意此句柄不同于Win32 API "CreateFile"所返回的句柄
		lpSector: 返回当前文件所在卷的扇区大小。注意：调用FWFastWrite时传递的第二个参数lpBuffer缓冲区起始地址必须是扇区大小的整数倍，并且第三个参数dwLenght写入数据长度也必须是扇区大小的整数倍！
	*/
	HRESULT FWCreateFile(/*IN*/ LPCTSTR lpFileName, /*IN*/ DWORD dwShareMode, /*IN*/ INT nQueueMax, /*OUT*/ LPFWHANDLE lpHandle, /*OUT*/ LPDWORD lpSector);

	/*
		Close File
		dwMilliseconds: 等待正在进行的写入结束的超时时间（实际关闭文件之前会等待所有缓存的请求执行完毕），如果超时发生，则数据可能没有被完整写入
	*/
	HRESULT FWCloseFile(/*IN*/ FWHANDLE handle, /*IN*/ DWORD dwMilliseconds);
	/*
		Write File
		lpBuffer: 要写入数据的地址（起始地址必须对齐到扇区大小的整数倍）
		dwLength: 数据长度（写入长度必须为扇区大小的整数倍）
		hNotify: 数据完全写入文件之后或者是当前请求写入发生错误的时候需要触发的事件
		lpResult: 当请求完成时hNotify被触发，同时lpResult指向的地址会被填入请求的执行结果，结果为HRESULT类型
		dwMilliseconds: 如果请求队列到达最大深度，则本函数会阻塞，直到请求队列的深度减小（数据被写入文件）或者此超时时间到期
		备注：每次调用FWFastWrite时传入的参数lpBuffer/dwLength/hNotify/lpResult形成一个请求，请求被异步的处理，因此FWFastWrite能够马上返回（非阻塞模式）；
			  在一个请求完成的时候，请求中的hNotify事件会被触发，同时请求中的lpResult指向的地址会被写入请求的执行结果。
			  因此，在确定请求已经完成之前（请求中的hNotify事件已经被触发，或者成功调用FWCloseFile），不能关闭hNotify事件，并且要保持lpResult指向的地址有效！
			  （例如：如果lpResult指向一个在栈上分配的自动变量，则函数返回时自动变量的空间被释放，此时将导致lpResult指向的地址无效，如果此时请求完成，将有可能导致程序异常！）
	*/
	HRESULT FWFastWrite(/*IN*/ FWHANDLE handle, /*IN*/ LPCVOID lpBuffer, /*IN*/ DWORD dwLength, /*IN*/ HANDLE hNotify, /*IN*/ HRESULT *lpResult, /*IN*/ DWORD dwMilliseconds);
	//计数器
	DWORD GetMS();

	BOOL  PCIe3640_RdWtEprom(HANDLE hdl,BOOL bWtRd,USHORT length,USHORT addr,PUSHORT pBuf);

#ifdef __cplusplus
}
#endif

#endif
