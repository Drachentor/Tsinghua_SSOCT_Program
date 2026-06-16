#ifndef _USB3020_DEVICE_
#define _USB3020_DEVICE_

#include<windows.h>
//*************************************************************************************
// 各段信息
typedef struct _USB3020_SEGMENT_INFO
{
	LONG SegLoopCount;		// 每个段在大循环中的小循环次数,取值为[1, 16777215]
	LONG SegmentSize;		// 每个段在RAM中的长度(单位：字/点)
} USB3020_SEGMENT_INFO, *PUSB3020_SEGMENT_INFO;

// DA的工作参数
typedef struct _USB3020_PARA_DA
{
	LONG OutputRange;		// 输出量程
	LONG Frequency;         // 点频率[0.010Hz, 1MHz], 为正数时单位Hz，为负数时单位为0.001Hz
	LONG LoopCount;			// 整过RAM的大循环次数,=0:无限循环, =n:表示n次循环(1<n<32768)
	LONG TriggerMode;		// 触发模式选择
	LONG TriggerSource;		// 触发源选择
	LONG TriggerDir;		// 触发方向选择
	LONG bSingleOut;		// 是否单点输出
	LONG ClockSource;		// 时钟源选择
} USB3020_PARA_DA, *PUSB3020_PARA_DA;

//######################## 常量定义 #################################
// DA输出函数InitDeviceDA的模拟量输出范围参数OutputRange所使用的选项
const long USB3020_OUTPUT_N5000_P5000mV		= 0x00;		// ±5000mV
const long USB3020_OUTPUT_N10000_P10000mV	= 0x01;		// ±10000mV

// DA硬件参数USB3020_PARA_DA中的TriggerMode成员变量触发模式选项
const long USB3020_TRIGMODE_SINGLE			= 0x00;		// 单次触发
const long USB3020_TRIGMODE_CONTINUOUS		= 0x01;		// 连续触发
const long USB3020_TRIGMODE_STEPED			= 0x02;		// 单步触发
const long USB3020_TRIGMODE_BURST			= 0x03;		// 紧急触发

// DA硬件参数USB3020_PARA_DA中的TriggerSource成员变量触发源选项
const long USB3020_TRIGSRC_SOFT_DA			= 0x00;		// 软件触发
const long USB3020_TRIGSRC_DTR_DA			= 0x01;		// DTR硬件模拟触发

//***********************************************************
// AD硬件参数USB3020_PARA_AD中的TriggerDir触发方向所使用的选项
// DA硬件参数USB3020_PARA_DA中的TriggerDir成员触发方向选项
const long USB3020_TRIGDIR_NEGATIVE			= 0x00;		// 负向触发(低脉冲/下降沿触发)
const long USB3020_TRIGDIR_POSITIVE			= 0x01;		// 正向触发(高脉冲/上升沿触发)
const long USB3020_TRIGDIR_POSIT_NEGAT		= 0x02;		// 正负向触发(高/低脉冲或上升/下降沿触发)

//***********************************************************
// AD硬件参数USB3020_PARA_AD中的ClockSource时钟源所使用的选项
// DA硬件参数USB3020_PARA_DA中的ClockSource成员变量时钟源选项
const long USB3020_CLOCKSRC_IN				= 0x00;		// 内部时钟
const long USB3020_CLOCKSRC_OUT				= 0x01;		// 外部时钟

//*************************************************************************************
// 用于DA采样的实际硬件参数
typedef struct _USB3020_STATUS_DA
{
	LONG bTrigFlag;		// 触发标志是否有效，=TRUE表示触点标有效， = FALSE表示无效（即触发点未到）
	LONG bConverting;	// DA是否正在转换， =TRUE:表示正在转换， = FALS表示转换完成
	LONG nCurSegNum;	// 可读取的RAM段号，取值为[0, SegmentCount-1], (注SegmentCount为InitDeviceDA函数的参数)
	LONG nCurSegAddr;	// 可读取的RAM段地址
	LONG nCurLoopCount; // 当前总循环次数
	LONG nCurSegLoopCount; // 当前段循环次数
} USB3020_STATUS_DA, *PUSB3020_STATUS_DA;

//***********************************************************
// 各段的头信息在RAM中的地址空间
const long USB3020_SEGMENT_HEADER_SIZE = 0x0C; // 段头信息的长度

//***********************************************************
// 驱动函数接口
#ifndef _USB3020_DRIVER_
#define DEVAPI __declspec(dllimport)
#else
#define DEVAPI __declspec(dllexport)
#endif

#ifdef __cplusplus
extern "C" {
#endif
	//######################## 常规通用函数 #################################
    HANDLE DEVAPI FAR PASCAL USB3020_CreateDevice(int DeviceLgcID = 0); // 创建设备对象(该函数使用系统内逻辑设备ID）
	int DEVAPI FAR PASCAL USB3020_GetDeviceCount(HANDLE hDevice);      // 取得USB3020在系统中的设备数量
	BOOL DEVAPI FAR PASCAL USB3020_GetDeviceCurrentID(HANDLE hDevice, PLONG DeviceLgcID, PLONG DevicePhysID); // 取得当前设备的逻辑ID号和物理ID号
	BOOL DEVAPI FAR PASCAL USB3020_ListDeviceDlg(void); // 用对话框列表系统当中的所有USB3020设备
	BOOL DEVAPI FAR PASCAL USB3020_ResetDevice(HANDLE hDevice);		 // 复位整个USB设备
    BOOL DEVAPI FAR PASCAL USB3020_ReleaseDevice(HANDLE hDevice);    // 设备句柄

	//####################### DA数据读取函数 #################################
	// 适于大多数普通用户，这些接口最简单、最快捷、最可靠，让用户不必知道设备
	// 低层复杂的硬件控制协议和繁多的软件控制编程，仅用下面的初始化设备和
	// DA数据输出两个函数便能轻松高效地实现高速、连续的波形输出
	// DA通用函数
	BOOL DEVAPI FAR PASCAL USB3020_InitDeviceDA(			// 初始化设备，当返回TRUE后, 设备即准备就绪.
									HANDLE hDevice,			// 设备对象句柄,它由CreateDevice函数创建
									LONG SegmentCount,		// 总工作段数[1, 65536]
									USB3020_SEGMENT_INFO SegmentInfo[], // 段信息集合
									PUSB3020_PARA_DA pDAPara, // 硬件参数, 它仅在此函数中决定硬件状态
									int nDAChannel);		// DA通道号[0, 3]

	BOOL DEVAPI FAR PASCAL USB3020_WriteDeviceBulkDA(		// 在DA输出前，用此函数将DA数据写入板载RAM中(程序方式)
									HANDLE hDevice,			// 设备对象句柄,它由CreateDevice函数创建
									USHORT DABuffer[],		// 携带原始DA数据的用户缓冲区
									LONG nWriteSizeWords,   // 相对于偏位点后写入的数据长度(字)
									PLONG nRetSizeWords,	// 返回实际写出的长度(字)
									int nDAChannel);		// DA通道号[0, 3]	

    BOOL DEVAPI FAR PASCAL USB3020_EnableDeviceDA(			// 在初始化之后，使能设备
									HANDLE hDevice,			// 设备对象句柄,它由CreateDevice函数创建
									int nDAChannel);		// DA通道号[0, 3]

    BOOL DEVAPI FAR PASCAL USB3020_SetDeviceTrigDA(			// 当设备使能允许后，产生软件触发事件（只有触发源为软件触发时有效）
									HANDLE hDevice,			// 设备对象句柄,它由CreateDevice函数创建
									BOOL bSetSyncTrig,		// 是否置同步触发
									int nDAChannel);		// DA通道号[0, 3]

	BOOL DEVAPI FAR PASCAL USB3020_GetDevStatusDA(			// 在DA采样过程中取得设备的各种状态,返回值表示函数是否成功
									HANDLE hDevice,			// 设备对象句柄,它由CreateDevice函数创建
									PUSB3020_STATUS_DA pDAStatus, // DA的各种信息结构体
									int nDAChannel);		// DA通道号[0, 3]

    BOOL DEVAPI FAR PASCAL USB3020_DisableDeviceDA(			// 在使设备之后，禁止设备
									HANDLE hDevice,			// 设备对象句柄,它由CreateDevice函数创建
									int nDAChannel);		// DA通道号[0, 3]
	
	BOOL DEVAPI FAR PASCAL USB3020_ReleaseDeviceDA(			// 禁止DA设备, 且释放资源
									HANDLE hDevice,			// 设备对象句柄,它由CreateDevice函数创建			
									int nDAChannel);		// DA通道号[0, 3]

	// DA校准
	BOOL DEVAPI FAR PASCAL USB3020_StartCalibration(		// 启动DA校准
									HANDLE hDevice);		// 设备对象

	BOOL DEVAPI FAR PASCAL USB3020_GetDACalibration(		// 设备DA校准		
									HANDLE hDevice,			// 设备对象
									LONG OutputRange,		// 输出量程,分别控制两个通道
									LONG CalMode,			// 0为零点校准，1为满度校准
									PLONG pCalData,			// 校准值
									int nDAChannel);		// DA通道号[0, 3]

	BOOL DEVAPI FAR PASCAL USB3020_SetDACalibration(		// 设备DA校准		
									HANDLE hDevice,			// 设备对象
									LONG OutputRange,		// 输出量程,分别控制两个通道
									LONG CalMode,			// 0为零点校准，1为满度校准
									LONG CalData,			// 校准值
									int nDAChannel);		// DA通道号[0, 3]

	BOOL DEVAPI FAR PASCAL USB3020_StopCalibration(			// 停止DA校准
									HANDLE hDevice);		// 设备对象
	
   	//################# DA的硬件参数操作函数 ########################
	BOOL DEVAPI FAR PASCAL USB3020_SaveParaDA(HANDLE hDevice, PUSB3020_PARA_DA pDAPara, int nDAChannel);
    BOOL DEVAPI FAR PASCAL USB3020_LoadParaDA(HANDLE hDevice, PUSB3020_PARA_DA pDAPara, int nDAChannel);
    BOOL DEVAPI FAR PASCAL USB3020_ResetParaDA(HANDLE hDevice, PUSB3020_PARA_DA pDAPara, int nDAChannel);

	//############################################################################
	BOOL DEVAPI FAR PASCAL USB3020_GetDevVersion(				// 获取设备固件及程序版本
									HANDLE hDevice,				// 设备对象句柄,它由CreateDevice函数创建
									PULONG pulFmwVersion,		// 固件版本
									PULONG pulDriverVersion);	// 驱动版本

#ifdef __cplusplus
}
#endif

// 自动包含驱动函数导入库
#ifndef _USB3020_DRIVER_
#ifndef _WIN64
#pragma comment(lib, "USB3020_32.lib")
#pragma message("======== Welcome to use our art company's products!")
#pragma message("======== Automatically linking with USB3020_32.dll...")
#pragma message("======== Successfully linked with USB3020_32.dll")
#else
#pragma comment(lib, "USB3020_64.lib")
#pragma message("======== Welcome to use our art company's products!")
#pragma message("======== Automatically linking with USB3020_64.dll...")
#pragma message("======== Successfully linked with USB3020_64.dll")
#endif
#endif

#endif; // _USB3020_DEVICE_