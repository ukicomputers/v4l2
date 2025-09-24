// H.264 decoder stack for Raspberry Pi computers running Linux
// Written by ukicomputers

// Used information from the following sources

// Raspberry Pi V4L2 specification
// https://www.raspberrypi.com/documentation/computers/camera_software.html#v4l2-drivers

// Linux Kernel V4L2 specification (video for linux)
// https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/v4l2.html

#pragma once
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <cerrno>
#include <string>
#include <vector>
#include <cstring>
#include <poll.h>
#include <algorithm> // just for std::min
#include <fstream>
using namespace std;

const string decoderDev = "/dev/video10";
const string converterDev = "/dev/video12";
const int eventTimeout = 500;
const int memoryThreshold = 51200;

struct Decoder {
    enum class InitStatus {
        OK,
        DEVICE_NOT_FOUND,
        INCOMPATIBLE_HARDWARE,
        INSUFFICIENT_MEMORY,
        FAILED
    };

    enum class Status {
        OK,
        NOT_INITIALIZED,
        INSUFFICIENT_MEMORY,
        FAILED
    };

    struct DecodedFrame {
        // decode status
        Status status = Status::OK;

        // vector of decoded/converted colors
        vector<unsigned int> output;

        // provided output size
        pair<int, int> imageSize;
    };
private:
    struct MemoryBuffer {
        vector<void *> start;
        vector<v4l2_plane> planes;
    };

    int decoder;
    bool decoderInitialized = false;
    bool decodeStreamStarted = false;
    vector<MemoryBuffer> decoderOutputBuffer;
    vector<MemoryBuffer> decoderInputBuffer;
    pair<int, int> decoderOutputSize;

    int converter;
    bool converterInitialized = false;

    int memoryLimit; // in KiB

    bool waitEvent(int fd, short events) {
        pollfd descriptor;
        descriptor.fd = fd;
        descriptor.events = events;
        descriptor.revents = 0;

        int ret = poll(&descriptor, 1, eventTimeout);
        if(ret <= 0) {
            // poll timeout or fail
            return false;
        }

        // if requested events are successfully executed in done events
        return (descriptor.revents & events) != 0;
    }

    int xioctl(int fd, int request, void *arg) {
        int status;

        do {
            status = ioctl(fd, request, arg);
        } while (status == -1 && errno == EINTR);

        return status;
    }

    void munmapBuffers(vector<MemoryBuffer> &output) {
        for(const auto &buffer : output) {
            for(int j = 0; j < buffer.start.size(); j++) {
                // TODO: check for improper unallocated behaviour of int in &output
                if(buffer.start[j] != MAP_FAILED) {
                    munmap(buffer.start[j], buffer.planes[j].length);
                }
            }
        }
    }

    InitStatus mmapBuffers(const int fd, const int type, const int planes, const int bufferCount, vector<MemoryBuffer> &output) {
        v4l2_requestbuffers reqBuffer = {};
        reqBuffer.count = bufferCount;
        reqBuffer.type = type;
        reqBuffer.memory = V4L2_MEMORY_MMAP;

        if(xioctl(fd, VIDIOC_REQBUFS, &reqBuffer) < 0) {
            if(errno == EINVAL) {
                return InitStatus::INCOMPATIBLE_HARDWARE;
            }

            return InitStatus::FAILED;
        }

        if(reqBuffer.count < 1) {
            return InitStatus::INSUFFICIENT_MEMORY;
        }

        output.resize(reqBuffer.count);

        for(int i = 0; i < reqBuffer.count; i++) {
            v4l2_buffer buffer = {};
            output[i].planes.resize(planes);            

            buffer.type = type;
            buffer.index = i;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.m.planes = output[i].planes.data();
            buffer.length = planes;
            
            if(xioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
                return InitStatus::FAILED;
            }

            output[i].start.resize(planes);

            for(int j = 0; j < planes; j++) {
                output[i].start[j] = mmap(nullptr, buffer.m.planes[j].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.planes[j].m.mem_offset);
                if(output[i].start[j] == MAP_FAILED) {
                    munmapBuffers(output);
                    return InitStatus::FAILED;
                }
            }

            if(xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
                return InitStatus::FAILED;
            }
        }

        return InitStatus::OK;
    }

    int getMemoryUsage() {
        ifstream status("/proc/self/status");
        if(!status) return -1;

        string data;
        while(getline(status, data)) {
            if(data.find("VmRSS") != string::npos) {
                int usage = -1;
                sscanf(data.c_str(), "VmRSS: %d kB", &usage);
                return usage;
            }
        }

        return -1;
    }

    int getFreeMemory() {
        struct sysinfo info;
        if(sysinfo(&info) != 0) {
            return -1;
        }

        return ((info.freeram + info.freeswap) * info.mem_unit) / 1024;
    }

    bool decodeMemoryAvailable() {
        if(memoryLimit == -1) {
            if(getFreeMemory() >= memoryThreshold) {
                return true;
            } else {
                return false;
            }
        } else {
            if(getMemoryUsage() >= memoryLimit) {
                return false;
            } else {
                return true;
            }
        }
    }
public:
    ~Decoder() { unload(); }

    void stopDecoder() {
        if(decodeStreamStarted) {
            int inputType = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            int outputType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            xioctl(decoder, VIDIOC_STREAMOFF, &inputType);
            xioctl(decoder, VIDIOC_STREAMOFF, &outputType);
            decodeStreamStarted = false;
        }
    }

    void unload() {
        stopDecoder();

        if(decoderInitialized) {
            munmapBuffers(decoderInputBuffer);
            munmapBuffers(decoderOutputBuffer);
            close(decoder);
            decoderInitialized = false;
        }

        if(converterInitialized) {
            close(converter);
            converterInitialized = false;
        }

        decoderInputBuffer.clear();
        decoderOutputBuffer.clear();
        decoderOutputSize = {};
    }

    /*
        note for memory management:

        if decoding produces enormous amount of data for a big input file
        it is guaranteed that it will exceed your memory and OOM killer will act

        because of that, you can set maximum containing memory for this process
        or just leave managing automatically by passing maxMemory = -1 (default)

        if you set maximum containing memory, and if it exceeds global system memory
        OOM will not be handled

        INSUFFICIENT_MEMORY as Status from decoding function will be set in this case

        also, width and height of input image must be constant (does not change through decoding)
    */

    InitStatus initializeDecoder(const int width, const int height, const int maxMemory = -1, const string videoDevice = decoderDev) {
        if(decoderInitialized) {
            return InitStatus::OK;
        }

        // open video devices
        decoder = open(videoDevice.c_str(), O_RDWR | O_NONBLOCK);
        if(decoder < 0) {
            return InitStatus::DEVICE_NOT_FOUND;
        }

        // encoder input specification (H264)
        v4l2_format inputFmt = {};
        inputFmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        inputFmt.fmt.pix_mp.width = width;
        inputFmt.fmt.pix_mp.height = height;
        inputFmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
        inputFmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        inputFmt.fmt.pix_mp.num_planes = 1;

        // ioctl sets (programs) devices with certain options
        if(xioctl(decoder, VIDIOC_S_FMT, &inputFmt) < 0) {
            close(decoder);
            if(errno == EINVAL) {
                return InitStatus::INCOMPATIBLE_HARDWARE;
            }
            return InitStatus::FAILED;
        }

        // encoder output specification (H264 -> YU12)
        v4l2_format outputFmt = {};
        outputFmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        outputFmt.fmt.pix_mp.width = width;
        outputFmt.fmt.pix_mp.height = height;
        outputFmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
        outputFmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        outputFmt.fmt.pix_mp.num_planes = 1;

        if(xioctl(decoder, VIDIOC_S_FMT, &outputFmt) < 0) {
            close(decoder);
            if(errno == EINVAL) {
                return InitStatus::INCOMPATIBLE_HARDWARE;
            }
            return InitStatus::FAILED;
        }

        // get actual size
        if(xioctl(decoder, VIDIOC_G_FMT, &outputFmt) < 0) {
            close(decoder);
            return InitStatus::FAILED;
        }

        decoderOutputSize = {(int)outputFmt.fmt.pix_mp.width, (int)outputFmt.fmt.pix_mp.height};

        // decoding input buffer request
        InitStatus outputStatus = mmapBuffers(decoder, inputFmt.type, inputFmt.fmt.pix_mp.num_planes, 4, decoderInputBuffer);
        if(outputStatus != InitStatus::OK) {
            return outputStatus;
        }

        // decoding output buffer request
        InitStatus inputStatus = mmapBuffers(decoder, outputFmt.type, outputFmt.fmt.pix_mp.num_planes, 4, decoderOutputBuffer);
        if(inputStatus != InitStatus::OK) {
            return inputStatus;
        }

        memoryLimit = maxMemory;
        decoderInitialized = true;
        return InitStatus::OK;
    }

    /* 
       notes for decoding:
    
       once first frame is decoded, it is expected that you will decode more frames
       to avoid any GPU blocking externally, once you are finished with constant decoding, destructure, or call stopDecoder

       if last part of the H264 content is passed, you should set lastData bool to true

       input must be progressive, and in Annex-B form

       you don't need to send full packets
       for example, if reading from a file, you can read chunks, and send them here
       when chunk is complete, the frame will be decoded and returned

       outcome image size may be different since size needs to be divisible by decoder stepsize
       image size will be increased to point of image size divisibility with stepsize
    */
    
    DecodedFrame decode(const vector<char> &data, bool lastData) {
        DecodedFrame returnedOutput;

        if(!decoderInitialized) {
            returnedOutput.status = Status::NOT_INITIALIZED;
            return returnedOutput;
        }

        if(!decodeMemoryAvailable()) {
            returnedOutput.status = Status::INSUFFICIENT_MEMORY;
            return returnedOutput;
        }

        int inputType = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        int outputType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

        // turn on stream
        if(!decodeStreamStarted) {
            if(
                xioctl(decoder, VIDIOC_STREAMON, &inputType) < 0 ||
                xioctl(decoder, VIDIOC_STREAMON, &outputType) < 0
            ) {
                returnedOutput.status = Status::FAILED;
                return returnedOutput;
            }

            decodeStreamStarted = true;
        }

        // TODO: subscribe to resolution change events
        // feeding input buffers
        {
            int remaining = data.size();
            const char *dataPtr = data.data();

            while(remaining > 0) {
                // input buffer handling
                v4l2_buffer inputBuffer = {};
                inputBuffer.type = inputType;
                inputBuffer.memory = V4L2_MEMORY_MMAP;

                vector<v4l2_plane> planeData(decoderInputBuffer[0].planes.size());
                inputBuffer.m.planes = planeData.data();
                inputBuffer.length = planeData.size();

                if(xioctl(decoder, VIDIOC_DQBUF, &inputBuffer) < 0) {
                    if(errno == EAGAIN) {
                        if(waitEvent(decoder, POLLOUT | POLLWRNORM)) {
                            continue;
                        }
                    }

                    returnedOutput.status = Status::FAILED;
                    return returnedOutput;
                }

                // take maximal size currently from input buffer chunk
                const int copySize = min((int)decoderInputBuffer[inputBuffer.index].planes[0].length, remaining);

                // set the decode data
                memcpy(decoderInputBuffer[inputBuffer.index].start[0], dataPtr, copySize);
                decoderInputBuffer[inputBuffer.index].planes[0].bytesused = copySize;

                if(remaining - copySize == 0 && lastData) {
                    inputBuffer.flags |= V4L2_BUF_FLAG_LAST;
                }

                inputBuffer.m.planes = decoderInputBuffer[inputBuffer.index].planes.data();

                if(xioctl(decoder, VIDIOC_QBUF, &inputBuffer) < 0) {
                    returnedOutput.status = Status::FAILED;
                    return returnedOutput;
                }

                // move onto next item
                remaining -= copySize;
                dataPtr += copySize;
            }
        }

        // getting output buffers
        while(true) {
            if(!decodeMemoryAvailable()) {
                returnedOutput.status = Status::INSUFFICIENT_MEMORY;
                return returnedOutput;
            }

            // get decoded output
            v4l2_buffer outputBuffer = {};
            outputBuffer.type = outputType;
            outputBuffer.memory = V4L2_MEMORY_MMAP;
            
            // temporary plane vector
            vector<v4l2_plane> planeData(decoderOutputBuffer[0].planes.size());
            outputBuffer.m.planes = planeData.data();
            outputBuffer.length = planeData.size();

            if(xioctl(decoder, VIDIOC_DQBUF, &outputBuffer) < 0) {
                if(errno == EAGAIN) {
                    // didn't process new incoming task yet
                    if(waitEvent(decoder, POLLIN | POLLRDNORM)) {
                        continue;
                    }
                }

                if(errno == EPIPE) {
                    // no more data to decode
                    break;
                }

                returnedOutput.status = Status::FAILED;
                return returnedOutput;
            }

            for(int j = 0; j < outputBuffer.length; j++) {
                decoderOutputBuffer[outputBuffer.index].planes[j].bytesused = planeData[j].bytesused;

                if(decoderOutputBuffer[outputBuffer.index].planes[j].bytesused > 0) {
                    const unsigned int *decodedData = static_cast<const unsigned int *>(decoderOutputBuffer[outputBuffer.index].start[j]);
                    returnedOutput.output.insert(returnedOutput.output.end(), decodedData, decodedData + decoderOutputBuffer[outputBuffer.index].planes[j].bytesused);
                    decoderOutputBuffer[outputBuffer.index].planes[j].bytesused = 0;
                }
            }

            outputBuffer.m.planes = decoderOutputBuffer[outputBuffer.index].planes.data();

            if(xioctl(decoder, VIDIOC_QBUF, &outputBuffer) < 0) {
                returnedOutput.status = Status::FAILED;
                return returnedOutput;
            }
        }

        returnedOutput.imageSize = decoderOutputSize;
        return returnedOutput;
    }
};