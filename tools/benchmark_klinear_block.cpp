#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<float> readResampleIndices(const std::string &path, int expectedCount)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open " + path);

    std::vector<float> indices;
    std::string line;
    while (std::getline(input, line)) {
        const std::string::size_type comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        std::istringstream stream(line);
        float value = 0.0f;
        while (stream >> value)
            indices.push_back(value);
    }

    if (indices.size() != static_cast<std::vector<float>::size_type>(expectedCount)) {
        std::ostringstream message;
        message << "index count mismatch: got " << indices.size()
                << ", expected " << expectedCount;
        throw std::runtime_error(message.str());
    }
    return indices;
}

void fillSyntheticBlock(std::vector<float> &block, int ascanLen)
{
    for (std::size_t i = 0; i < block.size(); ++i) {
        const int z = static_cast<int>(i % static_cast<std::size_t>(ascanLen));
        const int line = static_cast<int>(i / static_cast<std::size_t>(ascanLen));
        block[i] = static_cast<float>((z * 0.03125) + ((line % 257) * 0.0078125));
    }
}

void applyKLinearizationToSpectra(float *spectra,
                                  int ascanLen,
                                  int lineCount,
                                  const std::vector<float> &resampleIndex)
{
    if (spectra == nullptr || ascanLen <= 0 || lineCount <= 0
        || resampleIndex.size() != static_cast<std::vector<float>::size_type>(ascanLen)) {
        return;
    }

    std::vector<float> source(static_cast<std::size_t>(ascanLen), 0.0f);
    for (int line = 0; line < lineCount; ++line) {
        float *lineData = spectra + static_cast<std::int64_t>(line) * ascanLen;
        std::copy(lineData, lineData + ascanLen, source.begin());

        for (int z = 0; z < ascanLen; ++z) {
            const float sourceIndex = resampleIndex[static_cast<std::size_t>(z)];
            const int left = std::max(0, std::min(ascanLen - 1, static_cast<int>(std::floor(sourceIndex))));
            const int right = std::min(ascanLen - 1, left + 1);
            const float fraction = sourceIndex - static_cast<float>(left);
            lineData[z] = source[static_cast<std::size_t>(left)] * (1.0f - fraction)
                + source[static_cast<std::size_t>(right)] * fraction;
        }
    }
}

double sparseChecksum(const std::vector<float> &block)
{
    double checksum = 0.0;
    const std::size_t stride = 104729;
    for (std::size_t i = 0; i < block.size(); i += stride)
        checksum += block[i] * static_cast<double>((i % 97) + 1);
    return checksum;
}

} // namespace

int main()
{
    constexpr int ascanLen = 1600;
    constexpr int bscanLen = 200;
    constexpr int cscanLen = 200;
    constexpr int lineCount = bscanLen * cscanLen;
    constexpr int repeats = 5;
    const std::string mapPath =
        "parameters/calibration/Thorlabs_SL_134051/ascan_1600/klinear_resample_indices.txt";

    try {
        const std::vector<float> resampleIndex = readResampleIndices(mapPath, ascanLen);
        const std::size_t sampleCount =
            static_cast<std::size_t>(ascanLen) * bscanLen * cscanLen;
        std::vector<float> block(sampleCount);
        fillSyntheticBlock(block, ascanLen);

        std::cout << "block: " << ascanLen << " x " << bscanLen << " x " << cscanLen
                  << " = " << sampleCount << " float samples\n";
        std::cout << "block bytes: " << (sampleCount * sizeof(float))
                  << " (" << std::fixed << std::setprecision(1)
                  << (sampleCount * sizeof(float) / 1024.0 / 1024.0) << " MiB)\n";
        std::cout << "line count: " << lineCount << "\n";
        std::cout << "map: " << mapPath << "\n";

        double totalSeconds = 0.0;
        double bestSeconds = 1.0e100;
        for (int repeat = 0; repeat < repeats; ++repeat) {
            const auto start = std::chrono::steady_clock::now();
            applyKLinearizationToSpectra(block.data(), ascanLen, lineCount, resampleIndex);
            const auto stop = std::chrono::steady_clock::now();
            const double seconds =
                std::chrono::duration<double>(stop - start).count();
            totalSeconds += seconds;
            bestSeconds = std::min(bestSeconds, seconds);
            std::cout << "repeat " << (repeat + 1) << ": "
                      << std::fixed << std::setprecision(6) << seconds << " s, "
                      << (sampleCount / seconds / 1.0e6) << " Msamples/s, checksum "
                      << std::setprecision(3) << sparseChecksum(block) << "\n";
        }

        std::cout << "average: " << std::setprecision(6)
                  << (totalSeconds / repeats) << " s\n";
        std::cout << "best: " << bestSeconds << " s\n";
    } catch (const std::exception &ex) {
        std::cerr << "benchmark failed: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
