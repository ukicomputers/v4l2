#include "decoder.hpp"
#include <iostream>
#include <fstream>
#include <chrono>
using namespace std;

// required settings
const int inputVideoWidth = 1920;
const int inputVideoHeight = 1080;
const string inputVideoPath = "video.h264";
const int chunkSize = 225280; // 220 KiB

const string outputPath = "video.yuv";

int main() {
    Decoder decoder;

    {
        Decoder::InitStatus initStatus = decoder.initializeDecoder(inputVideoWidth, inputVideoHeight);
        if(initStatus != Decoder::InitStatus::OK) {
            cout << "Failed initializing decoder, error code: " << static_cast<int>(initStatus) << "\n";
            return 1;
        }
    }
    
    ifstream videoFile(inputVideoPath, ios::binary);
    if(!videoFile) {
        cout << "Failed to open video file\n";
        return 2;
    }

    ofstream outputFile(outputPath, ios::binary | ios::app);
    if(!outputFile) {
        cout << "Failed opening output file\n";
        return 3;
    }

    // load the video data from the file fully
    vector<char> data(chunkSize);

    while(videoFile) {
        videoFile.read(data.data(), data.size());
        streamsize read = videoFile.gcount();
        if(read <= 0) break;
        data.resize(read);

        // decoding time measurement
        auto decodingStart = chrono::high_resolution_clock::now();

        // decode the data
        bool isLast = (read < chunkSize || videoFile.peek() == EOF);
        auto decodedFrame = decoder.decode(data, isLast);

        if(decodedFrame.status != Decoder::Status::OK) {
            cout << "Failed decoding, error code: " << static_cast<int>(decodedFrame.status) << "\n";
            return 4;
        }

        auto decodingEnd = chrono::high_resolution_clock::now();
        auto decodingDuration = chrono::duration_cast<chrono::milliseconds>(decodingEnd - decodingStart).count();

        // write chunk to a file
        if(!decodedFrame.output.empty()) {
            cout << "Decoded frame successfully in " << decodedFrame.imageSize.first << "x" << decodedFrame.imageSize.second << " for " << decodingDuration << "ms\n";
            outputFile.write(reinterpret_cast<const char *>(decodedFrame.output.data()), decodedFrame.output.size());
        }
    }

    // unload and close the decoder
    decoder.unload();
    
    return 0;
}