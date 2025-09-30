# v4l2
An simple, minimal, lightweight and fast, standalone H264 decode library for bare Linux environments meant to be used on very low power *Raspberry Pi*s.

Tested (working) on Raspberry Pi Zero 2W (or any other CPU that contains VideoCore IV graphics).

You can see the demo, usage, and full walkthrough [here](https://hc-cdn.hel1.your-objectstorage.com/s/v3/d7ee1fde7ffda6f82d29750d239dd4e9b58d3aa6_2025-09-30_22-50-19.mp4).

## Using
This library is made as a single file - drop [decoder.hpp](https://github.com/ukicomputers/v4l2/blob/main/decoder.hpp) in your project, include it, and interface with it using `Decoder` class.

Firstly, you need to initialize Decoder by calling `Decoder::initializeDecoder` function. It requires video width, height, and some other parameters like setting maximal usable memory by the whole program, and changing the decoder path (where *Video 4 Linux* device is located, by default cases, for *Raspberry Pi*s with video processing unit, it's `/dev/video10`).

Video device needs to support "single-planar" H264 input with also "single-planar" YU12 (YUV 4:2:0) 8-bit output.

To decode, just call `Decoder::decode` function, and pass required arguments (input/chunk content and is it EOF). Note that you can pass chunks of any size and it doesn't need to be full file or be some important content of file (you can read chunks of file - and pass chunk by chunk to the decode function). Code handles any inconsistencies. Input **must be** in Annex-B form (standard).

You will get for output as `vector<uint8_t>` (decoded YUV for each bit stored in vector). See [this video](https://www.youtube.com/watch?v=q_mhF_Ys6nw) for more information about the YUV format. You can later preview the output with any *raw pixel preview software*, such as *ffplay*.

After everything (when you are finished decoding), just call `Decoder::unload` function, or simply, destucture.

## Building
`decoder.hpp` requires headers for V4L2 API - `linux-headers` need to be installed. Everything else used are standard C++/C libaries. Linking anything to library isn't required.

## Running example
This includes building example provided with this project ([main.cpp](https://github.com/ukicomputers/v4l2/blob/main/main.cpp)), or just get already compiled executable from Release page.
```bash
g++ -O3 main.cpp -o v4l2
```
After that, you can simply run the executable with `./v4l2`. You may need to change some variables in header of `main.cpp` depending on your video file specification. 

**Note** that on some systems downloaded/compiled executable needs to have permission to execute.
```bash
chmod +x ./v4l2
```
