#include "decoder.hpp"
#include <iostream>
#include <fstream>
using namespace std;

const int videoWidth = 1920;
const int videoHeight = 1048;
const string videoPath = "video.h264";
const string outputPath = "video.nv12";

int main() {
    Decoder decoder;

    {
        Decoder::InitStatus initStatus = decoder.initializeDecoder(videoWidth, videoHeight);
        if(initStatus != Decoder::InitStatus::OK) {
            cout << "Failed initializing decoder, error code: " << static_cast<int>(initStatus) << "\n";
            return 1;
        }
    }
    
    ifstream videoFile(videoPath, ios::binary);
    if(!videoFile) {
        cout << "Failed to open video file\n";
        return 2;
    }

    ofstream outputFile(outputPath, ios::binary);
    if(!outputFile) {
        cout << "Failed opening output file\n";
        return 3;
    }

    vector<uint8_t> data;
    data.assign((istreambuf_iterator<char>(videoFile)), {});

    auto decodedFrame = decoder.decode(data, true);
    if(decodedFrame.status != Decoder::Status::OK) {
        cout << "Failed decoding, error code: " << static_cast<int>(decodedFrame.status) << "\n";
        return 4;
    }

    outputFile.write(reinterpret_cast<const char*>(decodedFrame.output.data()), decodedFrame.output.size());

    decoder.unload();
    return 0;
}