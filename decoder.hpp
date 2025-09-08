// H.264 decoder stack for Raspberry Pi computers running Linux
// Written by ukicomputers

// Used information from the following sources

// Raspberry Pi V4L2 specification
// https://www.raspberrypi.com/documentation/computers/camera_software.html#v4l2-drivers

// Linux Kernel V4L2 specification (video for linux)
// https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/v4l2.html

// 6by9's V4L2 M2M C example
// https://github.com/6by9/v4l2_m2m/blob/master/m2m.c

#pragma once
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cerrno>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <iostream>
using namespace std;

// as per rpiv4l2
const string decoderDev = "/dev/video10";
const string converterDev = "/dev/video12";
const int bufferCount = 4;

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
        INVALID_DATA_PROVIDED,
        FAILED,
        FREEING_FAILED
    };

    struct DecodedFrame {
        // decode status
        Status status = Status::OK;

        // vector of decoded/converted colors
        vector<uint8_t> output;
    };
private:
    struct MemoryBuffer {
        vector<void *> start;
        vector<struct v4l2_plane> planes;
    };

    int decoder;
    bool decoderInitialized = false;
    bool decodeStreamStarted = false;
    vector<MemoryBuffer> decoderOutputBuffer;
    vector<MemoryBuffer> decoderInputBuffer;

    int converter;
    bool converterInitialized = false;

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
                // TODO: check for improper unallocated behaviour
                if(buffer.start[j] != MAP_FAILED) {
                    munmap(buffer.start[j], buffer.planes[j].length);
                }
            }
        }
    }

    InitStatus mmapBuffers(const int fd, const int type, const int planes, const int bufferCount, vector<MemoryBuffer> &output) {
        struct v4l2_requestbuffers reqBuffer = {};
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
            struct v4l2_buffer buffer = {};
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

                // output[i].length[j] = buffer.m.planes[j].length;
            }
        }

        return InitStatus::OK;
    }

    InitStatus queueBuffers(const int fd, const int type, const int planes, const vector<MemoryBuffer> &output) {
        for(int i = 0; i < output.size(); i++) {
            struct v4l2_buffer buffer = {};

            buffer.type = type;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = i;
            buffer.m.planes = decoderInputBuffer[i].planes.data();
            buffer.length = planes;

            if(xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
                return InitStatus::FAILED;
            }
        }

        return InitStatus::OK;
    }
public:
    ~Decoder() { unload(); }

    void unload() {
        if(decodeStreamStarted) {
            int inputType = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            int outputType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            xioctl(decoder, VIDIOC_STREAMOFF, &inputType);
            xioctl(decoder, VIDIOC_STREAMOFF, &outputType);
            decodeStreamStarted = false;
        }

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
    }

    // requires fixed video width and height
    InitStatus initializeDecoder(const int width, const int height, const string videoDevice = decoderDev) {
        if(decoderInitialized) {
            return InitStatus::OK;
        }

        // open video devices
        // note: blocking device is used even if v4l docs says it should be used with O_NONBLOCK
        decoder = open(videoDevice.c_str(), O_RDWR | O_NONBLOCK);
        if(decoder < 0) {
            return InitStatus::DEVICE_NOT_FOUND;
        }

        // encoder input specification (H264)
        struct v4l2_format inputFmt = {};
        inputFmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        inputFmt.fmt.pix_mp.width = width;
        inputFmt.fmt.pix_mp.height = height;
        inputFmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
        inputFmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
        inputFmt.fmt.pix_mp.num_planes = 1;

        // ioctl sets (programs) devices with cetain options
        if(xioctl(decoder, VIDIOC_S_FMT, &inputFmt) < 0) {
            close(decoder);
            if(errno == EINVAL) {
                return InitStatus::INCOMPATIBLE_HARDWARE;
            }
            return InitStatus::FAILED;
        }

        // encoder output specification (H264 -> YU12)
        struct v4l2_format outputFmt = {};
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

        // sadly, we need to use mmap for input buffer also
        {
            // decoding input buffer request
            InitStatus status = mmapBuffers(decoder, inputFmt.type, inputFmt.fmt.pix_mp.num_planes, 1, decoderInputBuffer);
            if(status != InitStatus::OK) {
                return status;
            }
            
            if(queueBuffers(decoder, inputFmt.type, inputFmt.fmt.pix_mp.num_planes, decoderInputBuffer) != InitStatus::OK) {
                return InitStatus::FAILED;
            }
        }

        {
            // decoding output buffer request
            InitStatus status = mmapBuffers(decoder, outputFmt.type, outputFmt.fmt.pix_mp.num_planes, 4, decoderOutputBuffer);
            if(status != InitStatus::OK) {
                return status;
            }   

            if(queueBuffers(decoder, outputFmt.type, outputFmt.fmt.pix_mp.num_planes, decoderOutputBuffer) != InitStatus::OK) {
                return InitStatus::FAILED;
            }
        }

        decoderInitialized = true;
        return InitStatus::OK;
    }

    // note: once first frame is decoded, it is expected that you will decode from now continously (don't worry if there is some delay between)
    // to avoid any GPU blocking, or similar, once you are finished with decoding completely, unload() the Decoder struct
    // it's also assumed that provided decoding data has the fixed resolution (specified when initializeDecoder was called)
    // if end of the H264 content/file is reached (when passing the last chunk), you should set lastData flag to true
    // this also means that if you provide whole H264 content at once, lastData needs to be set to true
    // input format also must be in Annex-B form
    DecodedFrame decode(const vector<uint8_t> &data, bool lastData) {
        DecodedFrame returnedOutput;

        if(!decoderInitialized) {
            returnedOutput.status = Status::NOT_INITIALIZED;
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

        // start decoding
        int remaining = data.size();
        const uint8_t *dataPtr = data.data();

        while(remaining > 0) {
            for(int i = 0; i < decoderInputBuffer.size() && remaining > 0; i++) {
                // input buffer handling
                struct v4l2_buffer inputBuffer = {};
                // take note that H264 input format only has one plane

                inputBuffer.type = inputType;
                inputBuffer.memory = V4L2_MEMORY_MMAP;
                inputBuffer.m.planes = decoderInputBuffer[i].planes.data();
                inputBuffer.length = 1;
                inputBuffer.index = i;

                if(xioctl(decoder, VIDIOC_DQBUF, &inputBuffer) < 0) {
                    returnedOutput.status = Status::FAILED;
                    return returnedOutput;
                }

                // take maximal size currently from input buffer chunk
                cout << "inputBuffer.index " << inputBuffer.index << "\n";
                cout << "planeData[0].length " << decoderInputBuffer[i].planes[0].length << "\n";

                const int copySize = min((int)decoderInputBuffer[i].planes[0].length, remaining);
                if(copySize == 0 && remaining > 0) {
                    returnedOutput.status = Status::INVALID_DATA_PROVIDED;
                    return returnedOutput;
                }

                // set the decode data
                memcpy(decoderInputBuffer[i].start[0], dataPtr, copySize);
                decoderInputBuffer[i].planes[0].bytesused = copySize;

                if(remaining - copySize == 0 && lastData) {
                    inputBuffer.flags = V4L2_BUF_FLAG_LAST;
                } else {
                    inputBuffer.flags = 0; // as per QBUF kernel spec
                }

                if(xioctl(decoder, VIDIOC_QBUF, &inputBuffer) < 0) {
                    returnedOutput.status = Status::FAILED;
                    return returnedOutput;
                }

                // move onto next item
                remaining -= copySize;
                dataPtr += copySize;
            }

            for(int i = 0; i < decoderOutputBuffer.size(); i++) {
                // get decoded output
                struct v4l2_buffer outputBuffer = {};
                outputBuffer.type = outputType;
                outputBuffer.memory = V4L2_MEMORY_MMAP;
                outputBuffer.m.planes = decoderOutputBuffer[i].planes.data();
                outputBuffer.length = decoderOutputBuffer[i].planes.size();
                outputBuffer.index = i;

                if(xioctl(decoder, VIDIOC_DQBUF, &outputBuffer) < 0) {
                    if(errno == EAGAIN) {
                        continue;
                    }
                    returnedOutput.status = Status::FAILED;
                    return returnedOutput;
                }

                cout << "outputBuffer.index " << outputBuffer.index << "\n";

                for(int j = 0; j < decoderOutputBuffer[i].planes.size(); j++) {
                    const uint8_t *decodedData = static_cast<const uint8_t *>(decoderOutputBuffer[outputBuffer.index].start[j]);
                    returnedOutput.output.insert(returnedOutput.output.end(), decodedData, decodedData + decoderOutputBuffer[i].planes[j].bytesused);
                }

                if(xioctl(decoder, VIDIOC_QBUF, &outputBuffer) < 0) {
                    returnedOutput.status = Status::FREEING_FAILED;
                    return returnedOutput;
                }
            }
        }

        return returnedOutput;
    }
};