% 读取当前工程 parameters/scan_path 中的 scanX.txt / scanY.txt，并显示路径与频谱。

voltage_0V = 32768;
scriptPath = mfilename("fullpath");
if strlength(scriptPath) == 0
    projectDir = pwd;
else
    projectDir = fileparts(fileparts(scriptPath));
end

scanXPath = fullfile(projectDir, "parameters", "scan_path", "scanX.txt");
scanYPath = fullfile(projectDir, "parameters", "scan_path", "scanY.txt");
settingsPath = fullfile(projectDir, "parameters", "settings.ini");

ScanX = load(scanXPath);
ScanY = load(scanYPath);
ScanX = ScanX(:);
ScanY = ScanY(:);

if numel(ScanX) ~= numel(ScanY)
    error("scanX.txt 和 scanY.txt 点数不一致：ScanX=%d, ScanY=%d。", numel(ScanX), numel(ScanY));
end

ascanFreq = readNumberSetting(settingsPath, "mainWidget", "AscanFreq");
bscanCycleLen = readNumberSetting(settingsPath, "mainWidget", "BscanCycleLen");
bscanLength = readNumberSetting(settingsPath, "mainWidget", "BscanLength");

if isnan(ascanFreq) || ascanFreq <= 0
    error("无法从 settings.ini 的 [mainWidget] 中读取有效的 AscanFreq。");
end

n = numel(ScanX);
timeMs = (0:n-1).' / ascanFreq * 1000;
fftRepeatCount = 100;
fftPointCount = n * fftRepeatCount;
df = ascanFreq / fftPointCount;

fprintf("ScanX/ScanY: %d 点。\n", n);
fprintf("AscanFreq: %.6g Hz；FFT 重复次数: %d；FFT 点数: %d；FFT 频率分辨率: %.6g Hz。\n", ...
    ascanFreq, fftRepeatCount, fftPointCount, df);
if ~isnan(bscanCycleLen) && bscanCycleLen > 0 && bscanCycleLen ~= n
    warning("scanX/scanY 点数(%d) 与 settings.ini 中的 BscanCycleLen(%g) 不一致。", n, bscanCycleLen);
end
if ~isnan(bscanLength) && bscanLength > 0
    fprintf("BscanLength: %.0f；BscanCycleLen: %.0f。\n", bscanLength, bscanCycleLen);
end

removeDcForFft = true;
fftX = repmat(double(ScanX), fftRepeatCount, 1);
fftY = repmat(double(ScanY), fftRepeatCount, 1);
if removeDcForFft
    fftX = fftX - mean(fftX);
    fftY = fftY - mean(fftY);
end

[freqHz, ampX, max_ampX] = oneSidedAmplitudeSpectrum(fftX, ascanFreq);
[~, ampY, max_ampY] = oneSidedAmplitudeSpectrum(fftY, ascanFreq);

figure("Name", "ScanX / ScanY");

subplot(2, 2, 1);
plot(timeMs, ScanX, "LineWidth", 1);
grid on;
xlabel("Time (ms)");
ylabel("DA code");
title("ScanX");

subplot(2, 2, 2);
plot(timeMs, ScanY, "LineWidth", 1);
grid on;
xlabel("Time (ms)");
ylabel("DA code");
title("ScanY");

subplot(2, 2, 3);
plot(freqHz/1000, log10(ampX), "LineWidth", 1);
grid on;
xlabel("Frequency (kHz)");
ylabel("log10(|X(f)|)");
xlim([0, freqHz(end)/5000]);
ylim([-max_ampX/2, max_ampX]);
title("X(f), logarithmic");

subplot(2, 2, 4);
plot(freqHz/1000, log10(ampY), "LineWidth", 1);
grid on;
xlabel("Frequency (kHz)");
ylabel("log10(|Y(f)|)");
xlim([0, freqHz(end)/5000]);
ylim([-max_ampY/2, max_ampY]);
title("Y(f), logarithmic");

if exist("sgtitle", "file") == 2
    titleText = sprintf("AscanFreq = %.6g Hz, FFT repeat = %d, FFT N = %d, df = %.6g Hz", ...
        ascanFreq, fftRepeatCount, fftPointCount, df);
    if removeDcForFft
        titleText = titleText + " (FFT removed DC)";
    end
    sgtitle(titleText);
end

figure("Name", "Scan Path");
plot(ScanX, ScanY, "LineWidth", 1);
grid on;
axis equal;
xlabel("ScanX (DA code)");
ylabel("ScanY (DA code)");
title("Scan Path");

lowpass10kX = fftLowpass(ScanX, ascanFreq, 10000);
lowpass10kY = fftLowpass(ScanY, ascanFreq, 10000);
lowpass50kX = fftLowpass(ScanX, ascanFreq, 50000);
lowpass50kY = fftLowpass(ScanY, ascanFreq, 50000);

figure("Name", "Low-pass Comparison");

subplot(2, 2, 1);
plotLowpassComparison(timeMs, ScanX, lowpass10kX, "ScanX", 10000, ascanFreq, voltage_0V);

subplot(2, 2, 2);
plotLowpassComparison(timeMs, ScanY, lowpass10kY, "ScanY", 10000, ascanFreq, voltage_0V);

subplot(2, 2, 3);
plotLowpassComparison(timeMs, ScanX, lowpass50kX, "ScanX", 50000, ascanFreq, voltage_0V);

subplot(2, 2, 4);
plotLowpassComparison(timeMs, ScanY, lowpass50kY, "ScanY", 50000, ascanFreq, voltage_0V);

if exist("sgtitle", "file") == 2
    sgtitle("Low-pass filter comparison");
end

function value = readNumberSetting(settingsPath, sectionName, keyName)
    value = NaN;
    if ~isfile(settingsPath)
        return;
    end

    text = fileread(settingsPath);
    lines = regexp(text, "\r\n|\n|\r", "split");
    currentSection = "";

    for i = 1:numel(lines)
        line = strtrim(lines{i});
        if strlength(line) == 0 || startsWith(line, ";") || startsWith(line, "#")
            continue;
        end

        sectionMatch = regexp(line, "^\[(.+)\]$", "tokens", "once");
        if ~isempty(sectionMatch)
            currentSection = string(sectionMatch{1});
            continue;
        end

        if currentSection ~= sectionName
            continue;
        end

        keyValue = regexp(line, "^([^=]+)=(.*)$", "tokens", "once");
        if isempty(keyValue)
            continue;
        end

        key = strtrim(string(keyValue{1}));
        if key ~= keyName
            continue;
        end

        value = str2double(strtrim(keyValue{2}));
        return;
    end
end

function [freqHz, amplitude, maxAmplitude] = oneSidedAmplitudeSpectrum(signal, sampleRateHz)
    signal = double(signal(:));
    n = numel(signal);
    spectrum = fft(signal);
    amplitude = abs(spectrum) / n;

    halfCount = floor(n / 2) + 1;
    amplitude = amplitude(1:halfCount);

    if n > 1
        if mod(n, 2) == 0
            amplitude(2:end-1) = 2 * amplitude(2:end-1);
        else
            amplitude(2:end) = 2 * amplitude(2:end);
        end
    end

    freqHz = (0:halfCount-1).' * sampleRateHz / n;
    maxAmplitude = ceil( max(log10(amplitude)) );
end

function filtered = fftLowpass(signal, sampleRateHz, cutoffHz)
    signal = double(signal(:));
    n = numel(signal);

    if cutoffHz <= 0
        filtered = mean(signal) * ones(size(signal));
        return;
    end

    if cutoffHz >= sampleRateHz / 2
        filtered = signal;
        return;
    end

    spectrum = fft(signal);
    frequencyBins = (0:n-1).' * sampleRateHz / n;
    frequencyBins(frequencyBins > sampleRateHz / 2) = ...
        frequencyBins(frequencyBins > sampleRateHz / 2) - sampleRateHz;

    spectrum(abs(frequencyBins) > cutoffHz) = 0;
    filtered = real(ifft(spectrum));
end

function plotLowpassComparison(timeMs, rawSignal, filteredSignal, signalName, cutoffHz, sampleRateHz, voltage_0V)
    rawSignal = double(rawSignal(:));
    filteredSignal = double(filteredSignal(:));
    difference = rawSignal - filteredSignal;
    differenceScale = 100;
    noiseAmplitude = 100;
    noiseFreqHz = 19000;
    noisyDifferenceScale = 100;

    timeSec = timeMs(:) / 1000;
    noiseSignal = noiseAmplitude * sin(2 * pi * noiseFreqHz * timeSec);
    noisyFilteredSignal = fftLowpass(rawSignal + noiseSignal, sampleRateHz, cutoffHz);
    noisyFilteredDifference = noisyFilteredSignal - rawSignal;

    plot(timeMs, rawSignal - voltage_0V, "LineWidth", 1);
    hold on;
    plot(timeMs, filteredSignal - voltage_0V, "LineWidth", 1);
    plot(timeMs, differenceScale * difference, "LineWidth", 1);
    plot(timeMs, noisyDifferenceScale * noisyFilteredDifference, "LineWidth", 1);
    hold off;
    grid on;

    xlabel("Time (ms)");
    ylabel("DA code");
    title(sprintf("%s, %.0f kHz low-pass", signalName, cutoffHz / 1000));
    legend("Raw", ...
        "Low-pass", ...
        sprintf("%d x (Raw - low-pass)", differenceScale), ...
        sprintf("%d x (LP(Raw + %.0f kHz noise) - Raw)", noisyDifferenceScale, noiseFreqHz / 1000), ...
        "Location", "best");
end
