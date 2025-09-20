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
#include <cerrno>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <algorithm> // just for std::min
using namespace std;

// as per rpiv4l2
const string decoderDev = "/dev/video10";
const string converterDev = "/dev/video12";

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
        FAILED
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
        vector<v4l2_plane> planes;
    };

    int decoder;
    bool decoderInitialized = false;
    bool decodeStreamStarted = false;
    vector<MemoryBuffer> decoderOutputBuffer;
    vector<MemoryBuffer> decoderInputBuffer;

    int converter;
    bool converterInitialized = false;

    bool waitEvent(int fd, short events, int timeout = 100) {
        pollfd descriptor;
        descriptor.fd = fd;
        descriptor.events = events;
        descriptor.revents = 0;

        int ret = poll(&descriptor, 1, timeout);
        if(ret <= 0) {
            // poll timeout or fail
            cout << "POLL TIMEOUT\n";
            return false;
        }

        // if requested events are successfully executed in done events
        return (descriptor.revents & events) != 0;
    }

    int xioctl(int fd, int request, void *arg) {
        int status;

        do {
            cout << "xioctl and again\n";
            status = ioctl(fd, request, arg);
        } while (status == -1 && errno == EINTR);

        if(status == -1) {
            cout << "xioctl errno " << errno << ": " << strerror(errno) << "\n";
        }

        return status;
    }

    void munmapBuffers(vector<MemoryBuffer> &output) {
        for(const auto &buffer : output) {
            for(int j = 0; j < buffer.start.size(); j++) {
                // TODO: check for improper unallocated behaviour of int of &output
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

        // ioctl sets (programs) devices with cetain options
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
        cout << "new decode call\n";
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

        cout << "BEGIN\n";

        // feeding input buffers
        {
            int remaining = data.size();
            const uint8_t *dataPtr = data.data();

            while(remaining > 0) {
                // input buffer handling
                v4l2_buffer inputBuffer = {};
                inputBuffer.type = inputType;
                inputBuffer.memory = V4L2_MEMORY_MMAP;

                vector<v4l2_plane> planeData(decoderInputBuffer[0].planes.size());
                inputBuffer.m.planes = planeData.data();
                inputBuffer.length = planeData.size();

                if(xioctl(decoder, VIDIOC_DQBUF, &inputBuffer) < 0) {
                    // TODO: poll
                    cout << "input buffer dequeue fail\n";

                    if(errno == EAGAIN) {
                        if(waitEvent(decoder, POLLOUT | POLLWRNORM)) {
                            continue;
                        }
                    }

                    returnedOutput.status = Status::FAILED;
                    return returnedOutput;
                }

                cout << "choosed buffer input index " << inputBuffer.index << "\n";

                // take maximal size currently from input buffer chunk
                const int copySize = min((int)decoderInputBuffer[inputBuffer.index].planes[0].length, remaining);
                cout << "copySize " << copySize << "\n";
                
                // set the decode data
                memcpy(decoderInputBuffer[inputBuffer.index].start[0], dataPtr, copySize);
                decoderInputBuffer[inputBuffer.index].planes[0].bytesused = copySize;

                cout << "flags before " << inputBuffer.flags << "\n";
                if(remaining - copySize == 0 && lastData) {
                    inputBuffer.flags |= V4L2_BUF_FLAG_LAST;
                    cout << "flags before " << inputBuffer.flags << "\n";
                }

                inputBuffer.m.planes = decoderInputBuffer[inputBuffer.index].planes.data();

                if(xioctl(decoder, VIDIOC_QBUF, &inputBuffer) < 0) {
                    cout << "BUFFER Q fail\n";
                    returnedOutput.status = Status::FAILED;
                    return returnedOutput;
                }

                cout << "BUFFER Q end\n";

                // move onto next item
                remaining -= copySize;
                dataPtr += copySize;
            }
        }

        // getting output buffers
        while(true) {
            cout << "new while decode loop\n";

            // get decoded output
            v4l2_buffer outputBuffer = {};
            outputBuffer.type = outputType;
            outputBuffer.memory = V4L2_MEMORY_MMAP;
            
            // temporary plane vector
            vector<v4l2_plane> planeData(decoderOutputBuffer[0].planes.size());
            outputBuffer.m.planes = planeData.data();
            outputBuffer.length = planeData.size();

            if(xioctl(decoder, VIDIOC_DQBUF, &outputBuffer) < 0) {
                cout << "fucking fail in DQBUF\n";
                if(errno == EAGAIN) {
                    // didn't process new incoming task yet
                    cout << "EAGAIN in getbuf\n";
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

            cout << "got output from buffer index " << outputBuffer.index << "\n";

            for(int j = 0; j < outputBuffer.length; j++) {
                decoderOutputBuffer[outputBuffer.index].planes[j].bytesused = planeData[j].bytesused;

                if(decoderOutputBuffer[outputBuffer.index].planes[j].bytesused > 0) {
                    cout << "COPYING DATA RN RN\n";
                    const uint8_t *decodedData = static_cast<const uint8_t *>(decoderOutputBuffer[outputBuffer.index].start[j]);
                    returnedOutput.output.insert(returnedOutput.output.end(), decodedData, decodedData + decoderOutputBuffer[outputBuffer.index].planes[j].bytesused);
                    decoderOutputBuffer[outputBuffer.index].planes[j].bytesused = 0;
                }
            }

            outputBuffer.m.planes = decoderOutputBuffer[outputBuffer.index].planes.data();

            if(xioctl(decoder, VIDIOC_QBUF, &outputBuffer) < 0) {
                // buffer memory reallocation failed
                cout << "OUTPUT FREE FAIL\n";
                returnedOutput.status = Status::FAILED;
                return returnedOutput;
            }

            if(outputBuffer.flags & V4L2_BUF_FLAG_LAST) {
                // TODO: check with documentation if we should queue even if last flag
                cout << "HIT LAST FLAG\n";
                break;
            }
        }

        return returnedOutput;
    }
};