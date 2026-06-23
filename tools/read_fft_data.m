% 这个程序可以让您读取已经存储的，经过了 FFT 的数据。

CHOICE = 2; % 1: 读取所有数据；2: 按 Bscan 读取 & 显示数据。
BscanIndex = 2; % 选择要读取的 Bscan 的索引（从 1 开始）。

jsonPath = ""; % 留空则弹出对话框选择 FFT 设置 JSON 文件。

if jsonPath == ""
    [jsonFile, jsonFolder] = uigetfile( ...
        {'*_fft.json', 'FFT 设置 JSON (*_fft.json)'; ...
         '*.json', 'JSON 文件 (*.json)'; ...
         '*.*', '所有文件 (*.*)'}, ...
        '选择 FFT 设置 JSON 文件');
    if isequal(jsonFile, 0)
        return;
    end
    jsonPath = string(fullfile(jsonFolder, jsonFile));
end


info = jsondecode(fileread(jsonPath));
binPath = string(info.fftProcessing.outputFile);
ascanLen = info.source.ascanLen;
totalLines = info.source.storedAlineCount;
bscanLen = info.source.bscanLen;

fid = fopen(binPath, "r", "ieee-le");

if CHOICE == 1
    % 读取所有数据。
    % 注意：如果数据量较大，可能会占用较多内存。

    fftData = fread(fid, [ascanLen, totalLines], "single=>single");

elseif CHOICE == 2
    % 按 Bscan 读取 & 显示数据。

    offset = (BscanIndex-1) * bscanLen * ascanLen * 4; % 每个单精度浮点数占 4 字节。
    fseek(fid, offset, "bof");

    bscan = fread(fid, [ascanLen, bscanLen], "single=>single");

    imagesc(bscan(1:ascanLen/2, :));
    colormap gray;
    axis image;
end

fclose(fid);
