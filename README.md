# 支持文档

## 适用硬件模式与现有问题

上次更新: Ver. 260618

### DAC 设备

#### 声卡

声卡（与 Symphonic 模式）是我们正在研究的新扫描协议，使用相对便宜的声卡代替昂贵的 DAC 进行振镜的驱动。理论上说任何声卡都可以用来输出驱动信号，但需要注意声卡的编码格式和采样率。\
声卡可以和任何支持声卡的 DAC 设备共同工作。

目前测试的声卡：Creative - **Sound Blaster X AE-5**, 24-bit, 48000 Hz.

#### FCCTEC PCIe3640

北京星烁华创 (FCCTEC) - **PCIe3640** AD/DA 二合一采集卡\
射频 DAC: 16-bit, 5 GS/s, 128K 样点缓存，双通道同信号输出\
低速 DAC: 16-bit, 1 MS/s, 32K 样点缓存 (CH2 1G 样点), 四通道同步独立输出\
支持声卡：**是**

现有问题：

1. 目前 DAC 失灵了，无法输出任何信号！我们正在联系商家进行维修。

#### NI PCIe6353

National Instruments (NI) - **PCIe6353** 多功能 I/O 采集卡\
射频 DAC: **无**\
低速 DAC: 16-bit, 1.25 MS/s (单通道 2.86 MS/s), 2G 样点缓存，四通道同步独立输出。\
<small>&emsp;&emsp; 附共享 8K FIFO 缓存，样点较少时可直接在 FIFO 内循环，不依赖电脑持续传输；输出范围可独立设置。</small>\
模拟输入：32 路单端 / 16 路差分模拟输入 (AI), 16-bit, 1 MS/s (单通道 1.25 MS/s).\
<small>&emsp;&emsp; 输入范围可独立设置；支持 DIFF、RSE、NRSE 接法。</small>\
<small>&emsp;&emsp; 适合低速监测、同步控制、触发/时钟路由和振镜驱动测试。</small>\
数字 I/O: 48 通道，硬件定时 10MHz, 可编程为输入或输出\
支持声卡：**是**

现有问题：

1. 已加入 NI-DAQmx AO 适配层；仍需在真实 PCIe6353 硬件上实测输出和同步触发。
2. 需要实测 AO 输出范围、PFI 触发/时钟路由，以及与声卡 Symphonic 模式同步运行时的触发关系。

### ADC 设备

#### FCCTEC PCIe3640

北京星烁华创 (FCCTEC) - **PCIe3640** AD/DA 二合一采集卡\
性能：14-bit, 1.25 GS/s, 2G 样点 FIFO 缓存，双通道输入

现有问题：

1. 硬件不支持外部时钟，这使得 k-clock (波数线性时钟) 无法使用。这会极大地影响成像质量，而且暂时没有办法在硬件本身的层面进行修复。

## 使用说明

上次更新: Ver. 260616

### 运行环境

1. 如果需要使用 debug 程序，请安装最新版的 Visual Studio.
2. 如果需要使用 CUDA, 请实时更新显卡的驱动程序和 NVIDIA CUDA Toolkit. 目前 CUDA 并不会被实际调用，但是之后可能会增加一些 CUDA 相关的功能。
3. 如果需要使用 Symphonic 模式，请在 `C:\Windows` 中安装 portaudio.
4. 使用 Symphonic 模式时，请注意您的声卡的输出通道，并且将音量调整至 100%.

### 部署运行目录

步骤如下：

1. 使用 Debug 和 Release 两种模式进行编译，确认程序没有问题。
2. 在工程根目录（在 powershell 中使用 `cd` 转到需要的目录）运行：

    ```powershell
    powershell -ExecutionPolicy Bypass -File .\tools\deploy_release.ps1
    ```

3. 把 **整个目录** 复制到另一台电脑运行。

#### 可选项

默认会同时部署 `build\QT_5_15_2d-Release\release` 和 `build\QT_5_15_2d-Debug\debug`；如果只想部署其中一个，可以使用：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\deploy_release.ps1 -Configuration Debug
powershell -ExecutionPolicy Bypass -File .\tools\deploy_release.ps1 -Configuration Release
```

#### 注意事项

1. 一定要在编译用的电脑上部署运行目录之后，再在别的电脑上运行。
2. CUDA/cuFFT 已经是延迟加载；默认不复制 `cufft64_12.dll`。如果希望把 CUDA 运行库也复制进运行目录，可以加 `-IncludeCudaRuntime`。
3. Debug 目录可以部署 Qt/OpenCV 的 debug DLL，但目标电脑仍然需要匹配的 MSVC debug runtime；通常安装 Visual Studio 或 Build Tools 最省心。
4. 可能会输出警告 `Warning: Could not find any translations in D:\QT\5.15.2-Build2\translations (developer build?)`. 这不会有实际影响，因为程序内置了 `qtbase_zh_CN.qm` 翻译。

## 更新信息

### -- Ver. 260625 --

1. `mainwidget`: 新增了软件色散修正。
    1. 可以可只计算色散修正，此时程序按输入数据已经波数线性化处理。
    2. 色散标定结果按 `parameters/calibration/<扫频光源ID>/ascan_<AscanLen>/dispersion_phase.txt` 保存，并用 `dispersion_diagnostics.json` 校验扫频光源和 A-line 长度。
    3. 参数文件全都放到了 `./parameters` 目录下。

### -- Ver. 260623 --

1. 添加新文件 `DeviceSettings`, 用于在启动之前确定使用的设备。
    1. 现在还会询问使用的扫频光源。扫频光源的型号对应所有 Ascan 相关设置，以及波数线性化标定坐标文件。
2. 添加新文件 `KLinearCalibration`, 用于直接用软件实现波数线性化。
    1. 勾选 “软件波数线性化” 时，程序会使用当前扫频光源和 `AscanLen` 对应目录下的 `klinear_resample_indices.txt` 对采集的 A-line 进行重采样；不勾选时跳过软件重采样。
    2. 首次操作的时候，您需要手动标定。请将样品臂和参考臂都接反射镜，在正光程差、负光程差、（挡住样品臂的）无干涉本底三个情况下分别进行 `64` 个 Aline 的定点采集；之后，请点击 “手动标定” 按钮进行计算。
    3. `klinear_resample_indices.txt` 现在按 `parameters/calibration/<扫频光源ID>/ascan_<AscanLen>/` 独立保存，可以通过 “手动标定” 按钮、“从文件读取” 按钮或者 `tools/generate_klinear_map.py` 脚本生成；标定结果会按扫频光源和 `AscanLen` 校验，不匹配时会报错。导入外部 `.txt` 且长度不同时，可选择对 X/Y 等比例放缩。
    4. “手动标定” 和 “从文件读取” 均改为独立对话框，可在开始计算前分别修改每个输入文件地址。

### -- Ver. 260618 --

1. `NiPcie6353Dac`: 新增 NI PCIe6353 AO 后端，动态加载 `nicaiu.dll`，通过 NI-DAQmx 准备 `AO0/AO1` 的 X/Y 振镜波形输出；默认设备名为 `Dev1`，通道为 `ao0:1`，输出范围为 `±5 V`。
2. `mainwidget` / `mythread`: 当启动时选择 `NI PCIe6353` 作为 DAC 时，X/Y 振镜信号由 NI AO 输出；PCIe3640 仍保留 RF 时基和 ADC 采集路径。3D/Cscan 下会跳过 PCIe3640 每路 1M 点上限分段检查，改为准备完整 NI AO 缓冲并启用 DAQmx regeneration。
3. `mainwidget.ui`: 新增了 "DAC 硬件设置" 分组框，包含适用于各个硬件的参数设置。
4. DA 输出：NI 卡的 DA FIFO 总容量为 8191 样点，如果 DA 的扫描路径长度不多于 8191 样点，则可以开启 FIFO regeneration 模式，直接由硬件 FIFO 自己循环输出，不依赖电脑内存持续传输，提升稳定性。1D 和 2D 扫描默认采用 FIFO regeneration, 仅当样点数量过多时才使用标准模式；3D 扫描总是采用标准模式。

### -- Ver. 260616a --

1. `main`: 程序启动后、主界面显示前新增 DAC/ADC 设备选择对话框。DAC 可选择 `FCCTEC PCIe3640` 或 `NI PCIe6353`，ADC 当前保留 `FCCTEC PCIe3640`。
2. `mainwidget`: 将设置拆分为 DAC、ADC 和共享参数分组；每个 DAC/ADC 设备会保存自己的硬件相关参数，同时保留 `[mainWidget]` 作为当前激活配置快照，兼容命令行转换和已保存数据 sidecar。
3. `mainwidget`: 初步加入 `NI PCIe6353` DAC 后端的占位保护，避免在适配层完成前误走 `PCIe3640` DA 输出路径。

### -- Ver. 260616 --

1. （重要）我们现在更新了程序的名字：`Tsinghua_SSOCT`. 之后，我们将会致力于同时开发并维护适用于不同硬件的版本。
2. `README`: 根据 `documents/NI PCIe6353 用户说明书.pdf` 补充了 NI PCIe6353 的 AO/AI、时钟和触发能力说明。

### -- Ver. 260614 --

1. `mainwidget`: 调整了 k-clock 和触发的模式。
2. `mainwidget`: 现在 1D 模式连续采集的情况下保存的 `.3d` 数据也能记忆储存路径了。
3. （重要）调试修复了实际扫描下可能出现的 bug.
    1. 修复了采样率错误（之前为 1000 MHz, 实际应为 1250 MHz）导致波包无法固定的 bug.
    2. 修复了由于点击 “开始扫描” 时未正确清理 FIFO, 导致扫描前几秒 Ascan 图像会跳变的 bug.
    3. 增加了 “触发偏移” 可调参数。
4. 增加了独立 DA 自检脚本 `./tools/pcie3640_da_selftest.ps1`, 它绕开 Qt 程序，直接按 SDK 示例调用 `PCIe3640.dll`: RF 输出正弦，LDA1 - LDA4 输出约 ±0.5V 方波；按 Enter 后复位 DAC. 这样可以判断是应用流程问题，还是板卡 / 接线 / 驱动层问题。如果商家附送的调试程序 `./VC x64/Release/FCAD.exe` 无法正常使用，可以在 Powershell 上 `cd` 到程序文件夹，然后执行：

    ```powershell
    powershell -ExecutionPolicy Bypass -File .\tools\pcie3640_da_selftest.ps1
    ```

5. [TODO] 现在 DA 出了故障，需要重新调试。

### -- Ver. 260611 --

1. `mainwidget`: 修复了大量的 bug.
    1. 修复了 Ascan 长度可以任意大，从而和采集卡的实际采集评率不匹配的问题。
    2. 修复了在已连接 DA, 未扫描时改变 DA 设置可能不会正确地断连 DA, 导致程序卡死的 bug.
    3. 现在 `Vessel scan` 和 `Symphonic` 模式也能生成实时 Bscan 图像了。在修改程序的时候错误地让这两个模式的扫描周期重复了 `AngioRep` 遍，现在已经改回来了。
2. `mythread`: 修正 `Vessel scan` 对 `scanX.txt`/`scanY.txt` 的长度判断；路径文件可以是完整 `BscanCycleLen` 周期，抛掉线性运动段后有效路径长度才需要等于 `BscanLength`。旧的仅包含有效路径段的文件仍会自动补线性运动段。
3. `mythread`: 修正了混淆 DAC 和 ADC 零点编码的 bug. 同时，程序启动后会将 PCIe3640 位置输出通道 DACH1/DACH2 初始化为 0V；停止 DA 扫描后也会重新回到 0V，避免位置通道停留在未初始化或负满量程附近。
4. `mythread`: 重大 bug 修复：将 AD 采集的硬件读取块和程序逻辑缓存帧解耦。`PCIe3640_ReadAD` 仍按 2048 样点整数倍读取，但读取到的样点会先进入电脑端 pending 缓冲，再拼接成完整 Aline / Bscan 周期 / Angio 组等逻辑帧写入 `m_volumeMemBuffer`。这样 `AscanLen * BscanCycleLen` 不再需要与 2048 形成很大的最小公倍数，也避免为了对齐一次读取大量 Bscan 后只使用其中一小部分。
5. [TODO] 外触发测试，将很快进行。
6. [TODO] 目前声卡的输出有很强的同频噪声，所以先买两个低通滤波器看看情况。

### -- Ver. 260610 --

1. 增加了声卡操控的功能。
    1. `VesselFindingDialog`: 勾选“生成音频”后，导出血管扫描路径时会同步生成 `vessel_scan_path.wav`，格式固定为 24-bit, 48000 Hz 双声道 PCM；左右声道分别对应 X/Y 路径，幅度限制为满量程的 80%。`scanX.txt` 和 `scanY.txt` 保持 DA/Aline 轨迹协议，点数等于完整 `BscanCycleLen`；有效路径段为 `BscanLength`，剩余点数按返回模式作为线性运动段。音频文件则由同一完整周期按 `48000 / AscanFreq` 重采样得到，不再要求与 DA 文本逐点等长。
    2. `mainwidget`: 新增 `Symphonic` 扫描模式，启动扫描时会读取工程目录下的 `vessel_scan_path.wav`，并使用 PortAudio + WASAPI exclusive mode 从 3.5mm 耳机孔循环输出血管扫描音频驱动信号；输出格式固定为 24-bit, 48000 Hz 双声道。开始扫描前会检查 `scanX.txt`/`scanY.txt` 的点数是否等于 `BscanCycleLen`，并检查 WAV 时长按 `AscanFreq` 换算后是否对应同一个完整 Bscan 周期；采集线程会按返回模式跳过线性运动段，只保存有效路径数据；手动停止扫描时会同步停止声卡输出。相关函数放在了 `VesselAudioPlayer` 文件中。
    3. `mainwidget`: 新增 `CB_enableDAInSymphonic` 选项；仅在 `Symphonic` 模式中生效。勾选后，准备扫描时会把完整周期的 `scanX.txt`/`scanY.txt` 写入 DAC，开始扫描时会同时启动声卡循环输出和 PCIe3640 DA 输出，用于信号测试；其他扫描模式勾选该选项不会改变输出行为。
    4. `mainwidget`: 启动程序时会检查 PortAudio、WASAPI Host API、默认输出设备，以及 WASAPI exclusive 24-bit/48000Hz 双声道格式是否可用；如果检查失败，会在主日志框中输出具体错误原因。
    5. [TODO] 实际的输出测试需要等待外触发连接线到货之后进行调试。

### -- Ver. 260605 --

1. `FlowSpeedCalculation`: 为了适应目前血流速度成像结果不够好的问题，新增了 $\alpha$ 参数拟合的相关系数 R, 作为灰度图存储为 `(filename)_flow_speed_fit_correlation.tiff`. 该图显示时使用 $1-\sqrt{1-R^2}$ 对 R 做圆周方式放缩，越亮说明拟合系数越好。现在 segment-wise 血流速度图在显示时会用这个相关系数排除低可信血管段，并对剩余区域重新拉伸色阶，以便增强原本压在蓝色区域里的 alpha 差异。
2. `FlowSpeedCalculation`: 血流速度图转换对话框新增“自动血管骨架抑制噪点”模式，以及上/下/左/右裁剪像素设置。自动骨架模式现在优先从 `flowVolume` 的 surface 对齐投影中寻找血管骨架，再用该骨架约束速度图掩膜；如果投影掩膜失败，则退回原来的 confidence 掩膜。裁剪和骨架抑噪只影响速度图掩膜、亮度和显示范围，不改变原始 TAC 拟合出的 $\alpha$ 数值。
    1. 测试图的 R 中位数大约在 0.984, 对应的亮度为 0.822.
3. `FlowSpeedCalculation`: 血流速度图转换对话框新增“手动速度图掩膜/裁剪窗口”。勾选后，程序会在生成速度图前显示当前速度图掩膜；可以用画笔擦除噪点、补回血管区域，或在窗口中继续裁剪。确认后的掩膜会用于 pixel-wise、快速平均、segment-wise 和拟合相关系数图的显示范围。启用裁剪、自动骨架或手动掩膜时，程序会额外保存最终使用的 `(filename)_flow_speed_mask.tiff`, 便于检查噪点是否进入最终掩膜。
    1. 请注意，“使用自动血管骨架抑制速度图噪点” 和 “打开手动速度图掩膜/裁剪窗口” 两个选项是不互斥的，手动速度图掩膜可以修正自动血管骨架生成的掩膜。
    2. 这个主要作用是修正血流速度图中由于噪点导致的错误亮斑，以及由于错位等影响产生的白线。
4. `main`: 新增命令行诊断入口 `--convert-angio3d <file.3d>`, 可配合 `--flow-skeleton`, `--flow-crop-bottom`, `--flow-fit-correlation` 等参数无人值守生成血流速度图，便于复现实验数据上的掩膜效果；不带该参数时仍进入正常 GUI。

### -- Ver. 260604b --

1. `FlowSpeedCalculation`: 进行了算法的优化。
    1. 计算血流速度时会压掉非血管的区域。
    2. 由于之前按像素计算血流速度 (pixel-wise) 的方法会导致图像的信息被隐藏在噪点里面，所以现在我们推出了 3 种不同的血流图计算模式：原本的按像素计算模式；先给血管分段（`VesselSegmenter`），然后对整段拟合 $\alpha$ 参数的按分段计算 (segment-wise) 模式（这个模式需要较高的 Angio 重复数，对于 `AngioRep = 4` 不适用）；以及按像素计算之后再每一个血管段内做加权平均的 “快速”（因为直接从按像素计算的图像中平均出来）模式。
    3. 请注意！目前采用的 $\alpha$ 参数只是一个速度指标，并不代表实际的速度或其在 (x, y) 平面上的投影。
2. 在程序运行时记录的日志不会再显示文件地址（但是 `.log` 文件中仍然会完整显示）。
3. `VesselProjectionProcessor`: 更新了伪影修复的算法。
    1. 修复了一个 bug: 表面寻找时，边缘仍然非常容易被伪影干扰。图片的亮度（对比度）得到了些微提升。
    2. 修复了一个 bug: Bscan 板块内横向延伸的退相关噪声、配准残差、或者表面窗口轻微错位，都可能会导致血管图中出现横向的亮条。现在程序将试着将这些横条暴力拉回。效果不是很好，可能暂时无法完全处理。

### -- Ver. 260604a --

1. 现在主界面的日志将会记录到 `info.log` 和 `capture.log` 中（和程序的 `.exe` 文件在同一个目录下），分别对应程序运行日志和采集相关的日志。
2. `FlowSpeedCalculation`: 进行了一些速度提升的修改。
    1. 将生成血管图和血流速度图两个步骤中共享的功能写入了新的 `VesselFlowShared` 里面，避免代码和计算重复。
    2. 将更多的功能放在 GPU 上，将处理一个 640 x 800 x 800 x 4 Cscan 数据的血流速度的时间从 480 秒减少到了 12 秒。太伟大了英伟达。

### -- Ver. 260604 --

1. 新增 `FlowSpeedCalculation`: 以 Hwang 等人的论文 <small>(Retinal blood flow speed quantification at the capillary level using temporal autocorrelation fitting OCTA, BOE, 2023.6)</small> 中的算法为基础，增加了血流速度成像的功能。
    1. 可以在 “采集结果->血管图” 对话框中选择是否生成血流速度图。
    2. 首先，程序会使用轴向配准的方法生成血管图（这一块大部分和之前生成血管图的算法一样；这里的傅里叶变换已经和血管图 OpenCL 路径一样，使用 `cv::UMat + cv::dft` 触发 OpenCL/GPU FFT）。
    3. 接着，对每一个体素，程序会计算其退相干 (decorrelation) 系数和重复次数间的关系。
    4. 最后，使用 Temporal autocorrelation fitting 的方法，拟合出速度参数 $\alpha$, 加权平均之后得到（非完全定量的）血流速度图。
    5. 存储路径是 `(filename)_flow_speed.tiff`, 其中亮度代表了置信度，这一项的数值也会保存到 `(filename)_flow_speed_confidence.tiff` 中。

### -- Ver. 260603 --

1. `VesselFindingDialog`: 增加了手绘模式。
2. `mainwidget`: 3D / 3D Angio 模式中，如果 Y 方向的 DAC 位置点数超过上限 `MAX_ADD_TRIG_LEN = 1048576`, 那么程序会询问用户是否想要分段扫描。
3. 现在血管成像的重复数 `AngioRep` 可以调整为 `1 ~ 8`, 以适应将来血流速度成像功能的需求。

### -- Ver. 260602 --

1. `mainwidget`: 对 Bscan, Cscan 测试，进行了一系列修改。
    1. 增加了触发模式选择的功能，支持内触发、外触发和连续扫描模式。
    2. 现在 2D 模式会显示每一个 Bscan 中一条 Aline 的图像和消光比。
    3. 现在 3D 模式可以切换是否显示实时 Bscan 图像，并且会进行计时。不过似乎也没啥太大用处，因为一次 Cscan 用时不到 1 秒。
    4. 修复了 3D 模式扫描之后储存时没有记忆储存路径的 bug.

### -- Ver. 260601b --

1. `mainwidget`: 改善了 UI 的信息输出。
2. 增加了 `read_fft_data.m`，用于读取程序生成的 FFT 数据文件，并把它们转换成 Matlab 中的矩阵，以便之后进行数据分析。

### -- Ver. 260601a --

1. `mainwidget`: 增加了 UI 的可读性。
2. 现在存储文件的时候会记录采集这些数据时的设置。
3. 增加了一个执行傅里叶变换的按钮，可以把已有的数据进行傅里叶变换后存储到 `(filename)_fft.bin` （使用 `(filename)_fft.json` 作为设置文件），以便之后进行数据分析。
    1. 请注意，由于写盘时间较长，所以会比生成血管图所需的时间要长一些。处理一个 640 x 800 x 800 的数据块大约需要 40 秒。
    2. 对数放缩之后的位置域数据为 `float32` 格式的实值矩阵，因此会占用本来 `uint16` 格式的原始数据 2 倍的磁盘空间。

### -- Ver. 250601 --

1. `mainwidget`: 修复了一些采集相关的 bug.
    1. 修复了一个 bug: 1D 模式下，由于每次只读取 1D scan 所使用的数据，所以在 FIFO 中会积压大量未读取数据。
    2. 修复了一个 bug: 2D 模式下，未跳过前面运动到起始点时的 `len_Transition` 无效 Ascan 数据。
2. `mythread`: 由于手动选择血管路径的必要性，我们完全移除了 Vessel 3D 模式。Vessel auto 模式被改名为 Vessel scan 模式，并修复了其扫描路径未传输给 AD 驱动的 bug.
3. 增加了连续采集模式，可以在采集大量的数据之后保存为 3D 文件，以供之后数据分析使用。

### -- Ver. 260529a --

1. `mainwidget`: 修复了更多硬件相关的 bug.
    1. 修复了 1D 模式下，由于采集样点数量小于 2048, 所以可能会不输出任何图像的 bug. 注意现在 1D 模式会启用连续扫描。
    2. 修复了保存数据出错时，不会弹出任何提示的 bug.
    3. 修复了停止采集后重新连接时，有时候程序仍然会卡死的 bug.
    4. 增加采集诊断日志，并修改了相应的 UI 显示。

### -- Ver. 260529 --

1. `mainwidget`: 修复了非常多硬件相关的 bug.
    1. 修复了 Ascan 长度最大值被限制为 784 的 bug.
    2. 修复了 “更新数据并初始化 DAC” 之后，采集卡没有正确初始化、进入断联状态的 bug.
    3. 修复了停止采集后重新连接时，有时候程序会卡死的 bug.
2. 增加了射频 DAC 操控。
    1. 请注意，射频 DAC 无法输出方波，输入方波之后输出的是尖峰波。

### -- Ver. 260528a --

1. `VesselFindingDialog`: 修复了在进行骨架精修时，把 T 形连接误当成短毛刺剪断的 bug.

### -- Ver. 260528 --

1. `VesselFindingDialog`: 在从血管图生成扫描路径的过程中，增加“深度选择”功能，只保留深度差（相邻骨架点允许的最大深度跳变范围）不超过 `depthDependencyRange` 个颜色映射图指标的血管掩膜点。
    1. `VesselProjectionProcessor`: 现在在从 `.3d` 文件生成血管图时，会额外输出一张无灰度调制的深度颜色图：`(filename)_depth.tiff`.
    2. 程序先准备完整骨架，再根据种子点深度筛选血管。
    3. 如果想研究的血管被浅层血管覆盖，那么程序会试图做一次小半径连接修补。
    4. 如果某些骨架点附近深度无效，不会立刻把血管切断，避免颜色估计噪声导致路径破碎。
2. `VesselFindingDialog`: 增加了精修骨架的功能，可以手动修复未正确生成的骨架。

### -- Ver. 260527d --

1. 程序已经移至电脑主机，因此把相关的文件路径改变了。
2. `VesselFindingDialog`: 修复了读取文件时，如果路径包含中文时就会无法读取的 bug.
3. 增加 `AppVersion.h` 文件，用于在程序中显示当前版本号，方便更新。

### -- Ver. 260527c --

1. `VesselProjectionProcessor`: 血管图和可选的血管灰度图改为保存为 600 dpi 的 `.tiff` 文件；多帧平均预览图和表面检测检查图仍然保存为 `.png`. 这是为了论文提交时的图片质量要求。
2. `VesselFindingDialog`: 从血管图生成路径后，`widget_path` 预览会在路径起点显示黄点，在路径终点显示蓝点。

### -- Ver. 260527b --

1. `VesselProjectionProcessor`: 修复了读取文件时，如果路径包含中文时就会无法读取的 bug.

### -- Ver. 260527a --

1. `deploy_release.ps1`: 修正了部署运行目录的指令。

### -- Ver. 260527 --

1. 现在所有的血管图的横轴为 X 轴，纵轴为 Y 轴。
2. 修复了没有 `cufft64_12.dll` 的电脑无法打开程序的 bug.
3. 新增 `deploy_release.ps1`，用于把 `release`/`debug` 运行目录补齐 `Qt`, `OpenCV`, `PCIe3640`, `OpenMP` 等运行时 DLL。具体的使用方法请参见上方的 **部署运行目录** 部分。
    1. debug 版本要求电脑拥有 Visual Studio 以提供 debug runtime.
    2. [TODO] 由于目前 PCIe3640 硬件正在被使用，所以无法测试 release 版本的适配性。

### -- Ver. 260526 --

1. 血管图到路径功能的优化：
    1. 修复了某些种子点位于分叉附近时，方向权重失效、最终路径没有按端点整体方向选择，甚至原路折返的问题。
2. 血管图程序的优化：
    1. 修复了表面检测有时候失败的 bug. 目前，会优先寻找“足够厚、足够连续的亮组织带”的起点，尽量减少上方伪影的影响。但即使如此，只要有伪影，成像就必定会受到影响。
    2. 使用了一个自定义对话框选择 3D 文件，增加了“生成多帧平均预览图”、“生成血管灰度图”的开关选项。

### -- Ver. 260525b --

1. 修复表面寻找算法的 bug 以及相关的问题（比如说使得血流图中间出现没有信号的黑条等等）。
    1. 单张 Bscan 表面检测之后，codex 加了一层沿 Cscan/Y 方向的连续性修补，会识别这种“整行表面突然变成近似直线”的异常行，并用相邻有效表面插值修复，再限制相邻 Bscan 的表面跳变。
2. 血管图到路径功能的优化：
    1. 修复了一个 bug: adaptiveThreshold 在亮度参数超过 0.50 后会把局部阈值压得太低，导致近黑的无信号区域也变成前景、生成过分多的白色区域。
    2. 优化了生成路径时改变参数后，重新执行生成二值掩膜 -> 待选择血管骨架 -> 最终路径时的事件流，并修改了 UI.
    3. 终点权重现在在点击左侧 “确定” 重新生成最终扫描路径时保存；路径更新按钮不会保存这两个权重。

### -- Ver. 260525a --

1. 修复了表面曲线中的一个 bug: 某些列的上表面信号缺失，阈值法会去寻找底部强反射。在这种情况下，新的程序将会使用一个线性插值去连接这些无法找到表面的部分。
2. 大大减少了血管图程序的处理时间。
    1. 增加了线程上限。
    2. 给 OpenCL 的 `extractMagnitude` 输出 `UMat` 显式分配尺寸，修复了 `extractMagnitude` 阶段 OpenCL 路径必定回退到 CPU 处理的 bug. 这使得 FFT + 配准时间从 20 秒减少到了 13 秒。
    3. 把之后的数据处理步骤并行化。
    4. 现在处理一个 640 x 800 x 800 的数据块大约只需要 25 秒。
3. 优化了血管图程序的日志输出。
4. [TODO] 目前发现表面寻找算法中有时候会完全寻找不到表面。之后会进行修复。

### -- Ver. 260525 --

1. 调试血管图程序结果。
    1. 调整了配准操作的步骤。
    2. 修正了在把血流图像根据表面拉平并进行数值放缩时的一个严重 bug: 未逐体素进行背景扣除、而是在沿 Z 方向平均之后，每一个 Aline 都扣除了同一个值。
    3. 修正了最终投影的矩阵方向弄反的严重 bug。
    4. 修正了在从配准后的数据中生成血流信号（对应 matlab 中 `OCTA_F_ED_Clutter_EigFeed` 函数）时的一个严重 bug: 由于矩阵乘法位置错误，导致特征向量的计算完全错误。
2. 现在血管图算法在生成表面预览图之前，会先删掉之前重名的表面预览图。
3. 加入了运行时间统计。
    1. 之前的 CUDA 路径会把每个 FFT block 拷回 CPU 做配准，通常会慢。
    2. 处理一个 640 x 800 x 800 的数据块，Matlab 程序（10 个并行池）大约需要 2 分钟。
    3. OpenCL 大约需要 2 分钟，在删除了 FFT block 来回复制的机制之后时间仍然不变。现在更大的耗时大概率在逐板块的 FFT / flow kernel、结果回拷和后续体数据处理。
    4. CUDA 大约也需要 2 分钟。因此，仍然使用 OpenCL 作为默认的血管图计算路径，CUDA 作为可选项。

### -- Ver. 260523a --

1. 对 UI 进行了小修改：将一些不需要随时调整的参数置于 `tab_2` “内部参数” 中。
    1. 现在可以编辑 Ascan 长度了！一般 Ascan 长度和激光的扫频点数有关。
    2. 现在可以编辑 Angio 跳过 Bscan 的帧数了！这个参数会影响血流成像的结果。
2. 修正了光谱加窗的长度固定为 384 像素的 bug. 现在加窗的长度会根据 Ascan 长度自动调整。

### -- Ver. 260523 --

1. 对于已经安装 CUDA 的电脑，添加了 CUDA 适配。现在重建体数据的优先级是 CUDA -> OpenCL -> CPU.
    1. CUDA 版本目前把 cuFFT 和 flow 计算放到 CUDA 上，registration 仍复用当前 CPU/OpenCV 逻辑。
    2. 运行 CUDA 要求血流成像时的重复数 `AngioRep = 4`。
    3. 速度测试：处理一个 640 x 800 x 800 的数据块大约需要 3 分钟，加上前后的操作，总计需要 4 分钟左右。这个速度并不讨喜，之后将会想办法改善。
2. 正在测试从 3D Angio 数据到血管图的算法。
    1. 修正了 codex 编程时，将 matlab 程序中的一些参数当成常量所导致的 bug.
    2. 修正了生成预览图时，信号在放缩和对数处理中均未正确放缩，导致预览图全白的 bug.
    3. 修正了寻找表面的算法中，sigma = 0 导致检测得到的表面忽上忽下的 bug.
    4. [TODO] 目前血管图仍然不理想，正在调试中。

### -- Ver. 260520a --

1. 开始修改从 3D Angio 数据到血管图的算法。
    1. 之前从 matlab 复刻到 C++ 時，程序并没有自带并行、GPU 加速这两个关键功能。因此，我们现在试图加入这两个功能，以期望能够达到 matlab 版本的性能水平。
    2. 在生成血管图时，会采用目前程序内置的 Ascan 长度（暂不支持修改）和 Bscan 长度。如果和 Cscan 长度不匹配，之前会完全不进行处理。现在，则会询问您是否需要改变 Cscan 长度，进行后续处理。
    3. 程序会额外记住选择 3D Angio 数据的路径，而非和保存血管图的路径一致。
    4. [TODO] 对于已经安装 CUDA 的电脑，将会添加 CUDA 适配。

### -- Ver. 260520 --

1. 对从血管图生成扫描路径的功能，进行了如下优化：
    1. 增强了生成扫描路径时的平滑操作。
    2. 完成路径生成之后，会在主界面显示 `血管模式导出路径成功！`。

### -- Ver. 260519a --

1. 修正了血管长度有时候被显示得过分小的 bug。
2. [TODO] 下一步的目标是尽量减少生成扫描路径的时间。

### -- Ver. 260519 --

1. 略微修改了主对话框和血管图对话框的大小。
2. 增加了配置文件 `settings.ini`（当前保存位置为 `parameters/settings.ini`）。 程序启动时会自动读取配置文件中的参数（以及存储路径），在修改参数时也会被记住。
3. 对从血管图生成扫描路径的功能，进行了如下优化：
    1. 修改了在已经存在扫描路径的情况下更新二值掩膜参数时系统的运行逻辑：现在，更新二值掩膜参数之后，系统不会自动再生成一次扫描路径，而是会重新开启选点模式（防止在骨架出问题时仍然试图生成扫描路径、浪费时间）。当然，如果您不需要改变种子点，可以直接点击确定。
    2. 在构建完扫描路径之后，会自动对扫描路径进行一次平滑操作。
    3. 优化了文字界面的显示。
    4. 把图片显示的画布从 600x400 增加到了 720x400. 这主要是为了参数能够被完整地显示出来。

### -- Ver. 260515c --

1. [重要] 加入了 “血管图 -> 路径” 功能。
    1. 核心流程：选择彩色血管图、生成 initialVesselMask、显示 rgbImage + skeletonImage、鼠标选择种子点、确认后生成主血管路径叠加图，并支持两行参数的重置/更新。
    2. 点击右下角的确定之后，自动按照 `mythread` 中的路径保存格式，生成 `scanX.txt` 和 `scanY.txt` 两个文件，保存在所选血管图同目录下。
    3. [TODO] 下周准备拿已有的图片进行一些血管图 -> 路径的测试。目前由于采集卡正在被其他人使用，所以采集卡相关的测试还无法进行。

### -- Ver. 260515b --

1. [重要] 新增加了 `VesselProjectionProcessor` 文件，使用 codex 编写了 `V_ConvertAngioToImage` 中采集结果 -> 血流图的功能。
    1. 现在点击 `V_ConvertAngioToImage` 会弹出 `.3d` 文件选择框，然后使用新增的范围参数进行转换。
    2. 输出文件放在所选 `.3d` 同目录：`(filename)_preview.png`; `(filename)_surface_*.png`: 7 张表面检测检查图; `(filename).tiff`，最终 `colorProjection` 血管图；可选的灰度血管图为 `(filename)_grayscale.tiff`。

### -- Ver. 260515a --

1. 增加了几个按钮，基本搭好了框架。

### -- Ver. 260515 --

1. [重要] 为血管模式增加了一个崭新的对话框 `VesselFindingDialog`，用于从血管图生成扫描路径。[TODO] 目前仍然只有框架，预计实现的功能如下：
    1. 输入一个二维的彩色血管图，根据给定的参数，生成相应的中轴线骨架。
    2. 用户选择骨架中的某一点，以这一点为种子点，生成扫描路径。

### -- Ver. 260513a --

1. 修正了 2D angio 模式下保存数据量错误的问题。
2. 为之后即将添加的血管模式更新了 UI, 并增加了新模式的框架（内容还没有填充）。

### -- Ver. 260513 --

1. 将血管扫描重复数 (`AngioRep`) 定为常量 = `4`。
2. 将一些变量的类型从 `ulong` 改为 `int`，防止无符号整数在运算时可能出现的问题。

### -- Ver. 260508 --

1. 根据程序功能流程图，使用 codex 进行了以下更新：
    1. 增加了 DA 状态机：未准备 / 已准备 / 正在扫描，避免只靠按钮状态判断。
    2. 开始扫描前会检查 DA 是否已准备；AD 未初始化时会自动初始化。
    3. 停止扫描现在走统一流程：停止 DA、停止 AD、释放线程，但保留电脑缓存中的最近扫描数据。
    4. 修正 StopADCapture() 清空缓存索引的问题，避免停止后无法保存刚才的数据。
    5. 3D / 3D angio 扫描完成后会自动停止并弹出保存流程。
    6. 3D 类扫描中断时，会把未完成的采集缓冲区填成 ADC 零点。
    7. 保存按钮在扫描中点击时，会先按停止流程结束扫描，再进入保存。
    8. 补充了 2D cross scan 和 2D angio 的保存分支。
    9. 修正了保存对话框取消时直接访问 `selected_paths.at(0)` 可能崩溃的问题。
    10. 修正了一个 `new[]` 分配后用 `mkl_free` 释放的错误。
    11. 修正了对话框黑底黑字的问题。图标仍然有黑底，如果之后对美观问题有更多要求，只能考虑改成黑底白字。
    12. `mainWidgetUISetup()` 中未使用的 `thisWidget` 参数已删除。
2. [TODO] 实际程序测试没有发现代码 bug, 但是数据处理的准确性需要进一步考察。

### -- Ver. 260429a --

1. 融合了 `DAcard` 与 `mythread`. `DAcard` 已经不再使用，但是为了保险起见，仍然会留在这里。
2. [TODO] 进行实际的程序测试，看有何处仍然打不开。

### -- Ver. 260429 --

1. 完成了 Ver. 260428 的 [TODO]. 更新了 `DAcard`, 为了之后将其融入 `mythread` 做准备。

### -- Ver. 260428 --

1. 华创公司更新了 `PCIe3640` 的程序代码和说明书，我在此处也进行了更新。
2. 修改了 `DAcard` 中的部分逻辑，链接了 `mainWidget`，为之后融合 `DAcard` 和 `mythread` 做好准备。
3. [TODO] `DAcard` 中的数据生成部分仍然没有复原，这个明天再做。

### -- Ver. 260427 --

1. 删除了所有无关的图片文件。复原了上周在 `mythread` 上的修改。

### -- Ver. 260417 --

1. 由于 U 盘损坏，我把之前的文件传输到了这里，作为备份.
