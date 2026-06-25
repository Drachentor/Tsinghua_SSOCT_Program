# 软件波数线性化标定说明

程序中的 `CB_softwareKLinear` 约定如下：

- 勾选：使用软件波数线性化。程序会加载当前扫频光源和 `AscanLen` 对应目录下的 `klinear_resample_indices.txt`，在去本底/加窗之后、色散补偿和 FFT 之前，对每条 A-line 做插值重采样。
- 不勾选：不做软件波数线性化，保持现有外部/硬件线性化路径。

程序保存到 `.json` 和 `settings.ini` 时使用 `softwareKLinearEnabled=true/false` 记录这个开关。

程序中的 `CB_calibratedDispersion` 约定如下：

- 勾选：使用标定色散修正。程序会加载当前扫频光源和 `AscanLen` 对应目录下的 `dispersion_phase.txt`，在波数线性化之后、FFT 之前，把每条 A-line 乘以 `exp(i*phi_disp)`。
- 不勾选：继续使用界面上的 `W0/A1/A2` 手工色散补偿参数。

程序保存到 `.json` 和 `settings.ini` 时使用 `calibratedDispersionEnabled=true/false` 记录这个开关。

## 推荐采集方式

请在与实际成像完全相同的系统参数下采集两组镜面数据：

1. 将镜面调到零光程差一侧的一个固定位置，保持不动，连续采 64 条 A-line，保存为一个 `.3d` 文件。
2. 将镜面调到零光程差另一侧的一个固定位置，保持不动，连续采 64 条 A-line，保存为另一个 `.3d` 文件。
3. 如果方便，挡光或移开镜面后再采一组本底，保存为第三个 `.3d` 文件。

重要：同一个文件内部的 64 条 A-line 必须是同一个光程差位置的重复采样，不能是不同光程差位置的扫描序列。不同光程差的 A-line 相位斜率不同，不能直接平均。

当前设置下 `AscanLen=1600`，所以每个 `.3d` 文件应包含 `1600 * 64` 个采样点。程序保存 `.3d` 时若生成了同名 `.json`，脚本可以自动读取 `AscanLen` 和数据类型。

推荐命名：

```text
parameters/calibration/
  mirror_pos_fixed.3d
  mirror_pos_fixed.json
  mirror_neg_fixed.3d
  mirror_neg_fixed.json
  mirror_background.3d      可选
  mirror_background.json    可选
```

`pos/neg` 只表示零光程差两侧，不要求判断真实物理正负；只要两组在零点两侧即可。

## 生成重采样表

点击程序中的 “手动标定” 时，对话框会提供两个选项：

- `进行波数线性化映射`：生成 `klinear_resample_indices.txt` 和 `klinear_resample_diagnostics.json`。
- `计算色散修正`：生成 `dispersion_phase.txt` 和 `dispersion_diagnostics.json`。

如果只勾选 `计算色散修正`、不勾选 `进行波数线性化映射`，程序会把正/负光程差数据当作“已经波数线性化后的数据”来计算色散相位；这种前提会记录在 `dispersion_diagnostics.json` 的 `input_already_klinear=true` 中。

点击 “从文件读取” 时，对话框中的四个文件都是可选项：

- `k-linear 重采样表 .txt`：导入 `klinear_resample_indices.txt` 或外部重采样表。
- `k-linear 诊断 .json`：可选；留空时程序会补齐当前扫频光源、`AscanLen` 等基本诊断字段。
- `色散相位 .txt`：导入 `dispersion_phase.txt` 或外部色散相位表。
- `色散诊断 .json`：可选；留空时程序会补齐当前扫频光源、`AscanLen` 等基本诊断字段。

因此可以只导入 k-linear 映射、只导入色散补偿，或二者一起导入。诊断文件不能单独导入，必须和对应的主文件一起使用。k-linear 重采样表长度不匹配时仍可选择 X/Y 等比例放缩；色散相位表长度不匹配时不会自动放缩，需要选择匹配当前 `AscanLen` 的文件或重新标定。

如果 `.3d` 旁边有程序保存的同名 `.json`：

```powershell
python .\tools\generate_klinear_map.py `
  --positive .\parameters\calibration\mirror_pos_fixed.3d `
  --negative .\parameters\calibration\mirror_neg_fixed.3d `
  --background .\parameters\calibration\mirror_background.3d
```

如果没有本底文件，删除 `--background ...` 这一行即可。

如果没有同名 `.json`，请显式指定当前 A-line 长度和数据类型：

```powershell
python .\tools\generate_klinear_map.py `
  --positive .\parameters\calibration\mirror_pos_fixed.3d `
  --negative .\parameters\calibration\mirror_neg_fixed.3d `
  --background .\parameters\calibration\mirror_background.3d `
  --ascan-len 1600 `
  --dtype uint16 `
  --swept-source-id Thorlabs_SL_134051
```

脚本默认输出到 `parameters/calibration/<swept_source_id>/ascan_<AscanLen>/`，并在同一目录写入 `klinear_resample_diagnostics.json`。如果同名 `.json` 是旧版本、没有扫频光源信息，请显式添加 `--swept-source-id`。

程序保存的原始 ADC 数据一般使用 `--dtype uint16`。脚本默认会对 `uint16` 数据右移 4 bit，以匹配程序中的 `Aline >> 4` 处理路径。

## 备选：逐条 txt 保存

如果确实只能逐条保存，也可以每条 A-line 存成一个 `.txt` 文件，但同一文件夹里的 A-line 仍然必须来自同一个固定光程差位置：

```text
parameters/calibration/
  mirror_pos/
    pos_001.txt
    pos_002.txt
    ...
  mirror_neg/
    neg_001.txt
    neg_002.txt
    ...
```

每个 `.txt` 文件保存一条 A-line 的 1600 个数值，可以是一行空格分隔，也可以多行保存。

```powershell
python .\tools\generate_klinear_map.py `
  --positive .\parameters\calibration\mirror_pos `
  --negative .\parameters\calibration\mirror_neg `
  --ascan-len 1600 `
  --dtype text `
  --swept-source-id Thorlabs_SL_134051
```

## 算法

脚本 `tools/generate_klinear_map.py` 按参考文献和图片中的两侧镜面相位法生成重采样表：

1. 分别平均两侧固定位置的镜面 A-line。
2. 减本底，并可选做高通基线校正。
3. 对实数干涉谱做 Hilbert 变换，得到解析复信号。
4. 对两条相位曲线 unwrap，并统一到同一斜率方向。
5. 计算抵消色散项后的相位：

   `phase_linear = (phase_positive + phase_negative) / 2`

6. 用低阶多项式拟合 `phase_linear` 与原始采样索引的关系。
7. 生成均匀相位坐标，并对拟合曲线反插值，得到每个线性 k 采样点对应的原始小数索引。
8. 保存为 `parameters/calibration/<swept_source_id>/ascan_<AscanLen>/klinear_resample_indices.txt`。

如果同时计算色散修正，程序会继续在后半段做：

1. 如果本次也生成了重采样表，则先用该重采样表把正/负镜面谱转换到线性 k 坐标；如果只计算色散修正，则假设输入已经是线性 k 坐标。
2. 分别对正/负镜面谱做 Hilbert 相位提取，并用 FFT 主峰位置估计两侧镜面峰所在深度。
3. 将两侧相位数值平移到同一目标峰位置，计算 `phi_disp = (phase_positive_shifted - phase_negative_shifted) / 2`。
4. 去掉常数/线性趋势，并用低阶多项式平滑，保存完整相位数组到 `dispersion_phase.txt`。

## 输出放置位置

程序会优先查找当前扫频光源和当前 `AscanLen` 对应的 `klinear_resample_indices.txt`：

- exe 所在目录下的 `parameters/calibration/<swept_source_id>/ascan_<AscanLen>/`。
- 源码目录下的 `parameters/calibration/<swept_source_id>/ascan_<AscanLen>/`。

旧版根目录 `klinear_resample_indices.txt` 不再作为自动候选；如需使用旧文件，请通过 “从文件读取” 导入到 `parameters/calibration`。

标定色散文件 `dispersion_phase.txt` 使用同样的扫频光源/`AscanLen` 目录，但不使用旧版根目录兼容路径。旁边的 `dispersion_diagnostics.json` 必须能通过 `ascan_len` 和 `swept_source_id` 校验。

表中必须有且只有 `AscanLen` 个数字。每个数字表示“线性化后的该采样点，应从原始频谱的哪个小数索引插值得到”。如果导入的表长度和当前 `AscanLen` 不一致，程序会询问是否将 X 和 Y 都等比例放缩到当前长度。

`dispersion_phase.txt` 中也必须有且只有 `AscanLen` 个数字。每个数字表示该线性 k 采样点的色散修正相位，单位为弧度。

`klinear_resample_diagnostics.json` 现在也是程序校验文件。程序会用其中的 `ascan_len` 和 `swept_source_id` 检查标定结果是否与当前 A-line 长度和扫频光源匹配；如果缺少或不匹配，会拒绝启用软件波数线性化。

## 常见错误

如果出现类似下面的错误：

```text
ValueError: fitted phase is not strictly increasing
```

意思是脚本提取出的相位曲线局部反向，无法直接反插值生成线性 k 采样表。脚本会先自动尝试降低多项式拟合阶数，例如从 5 阶降到 4、3、2、1 阶；如果自动降阶成功，命令行会提示最终使用的阶数，`klinear_resample_diagnostics.json` 里也会记录 `requested_poly_degree`、`poly_degree`、`fit_attempts`。

如果自动降阶后仍失败，请优先检查：

- 正侧 `.3d` 内部是否真的是同一个固定光程差位置重复采样。
- 负侧 `.3d` 内部是否真的是同一个固定光程差位置重复采样。
- 正/负两组是否确实位于零光程差两侧。
- 镜面干涉条纹是否清晰，是否没有被本底或饱和信号淹没。
- 数据类型是否正确。程序保存的原始 ADC 数据通常是 `--dtype uint16`。

也可以尝试增加边缘裁剪或高通参数：

```powershell
python .\tools\generate_klinear_map.py `
  --positive .\parameters\calibration\mirror_pos_fixed.3d `
  --negative .\parameters\calibration\mirror_neg_fixed.3d `
  --background .\parameters\calibration\mirror_background.3d `
  --trim-left 50 `
  --trim-right 50 `
  --high-pass-samples 101
```
