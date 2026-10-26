// Video: the ffmpeg program decodes it and writes raw RGBA frames to a pipe.
// A reader thread holds back each frame until the playback clock reaches it,
// so when converting is slower than the video, frames are skipped.
#include "media.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

const char* OPEN_ERROR = "Could not open the file.\nVideos need ffmpeg and ffprobe installed (on PATH).";

struct Video
{
    std::string path;
    int w = 0, h = 0;
    double fps = 0;
    std::string rate;            // fps as ffprobe wrote it, e.g. "30000/1001"
    SDL_Process* proc = nullptr;
    std::thread reader;
    std::mutex m;
    std::condition_variable cv;
    std::vector<unsigned char> frame; // latest frame read, guarded by m
    int frameIdx = -1;                // its index, -1 = none yet
    int wanted = 0;                   // reader may hand over frames up to this index
    int first = 0;                    // index of the first frame ffmpeg sends (seek point)
    bool eof = false;
    std::atomic<bool> stop{ false };
};
static Video g_video;

static std::vector<unsigned char> g_videoFrame; // the video frame the main thread is converting

static void videoReader()
{
    Video& v = g_video;
    SDL_IOStream* out = SDL_GetProcessOutput(v.proc);
    size_t size = (size_t)v.w * v.h * 4;
    std::vector<unsigned char> buf(size);

    for (int idx = v.first; ; ++idx)
    {
        size_t got = 0;
        while (got < size && !v.stop)
        {
            size_t n = SDL_ReadIO(out, buf.data() + got, size - got);
            got += n;
            if (n > 0) continue;
            if (SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break; // end of video or error
            SDL_Delay(1);
        }

        std::unique_lock<std::mutex> lock(v.m);
        if (got < size)
        {
            v.eof = true;
            v.cv.notify_all(); // wakes showVideoFrame
            return;
        }
        v.cv.wait(lock, [&] { return v.stop || idx <= v.wanted; });
        if (v.stop) return;
        v.frame.swap(buf);
        v.frameIdx = idx;
        v.cv.notify_all();
    }
}

void stopVideo()
{
    Video& v = g_video;
    if (!v.proc) return;
    {
        std::lock_guard<std::mutex> lock(v.m);
        v.stop = true;
    }
    v.cv.notify_all();
    SDL_KillProcess(v.proc, true);
    if (v.reader.joinable()) v.reader.join();
    SDL_WaitProcess(v.proc, true, nullptr);
    SDL_DestroyProcess(v.proc);
    v.proc = nullptr;
}

// (Re)start ffmpeg at `t` seconds
bool startVideo(double t)
{
    stopVideo();
    Video& v = g_video;
    std::string ss = std::to_string(t);
    std::string vf = "fps=" + v.rate; // constant frame rate, so frame index = time * fps
    const char* args[] = { "ffmpeg", "-v", "error", "-nostdin", "-ss", ss.c_str(),
        "-i", v.path.c_str(), "-map", "0:v:0", "-vf", vf.c_str(),
        "-f", "rawvideo", "-pix_fmt", "rgba", "-", nullptr };
    v.proc = SDL_CreateProcess(args, true);
    if (!v.proc) return false;

    v.first = (int)std::lround(t * v.fps);
    v.wanted = v.first;
    v.frameIdx = -1;
    v.eof = false;
    v.stop = false;
    v.frame.assign((size_t)v.w * v.h * 4, 0);
    g_frameShown = -1;
    v.reader = std::thread(videoReader);
    return true;
}

static double parseRate(const char* s)
{
    int num = 0, den = 0;
    if (std::sscanf(s, "%d/%d", &num, &den) != 2 || num <= 0 || den <= 0) return 0;
    return (double)num / den;
}

// Size, frame rate and length of the first video stream, from ffprobe
static bool probeVideo(const std::string& path, Video& v, double& duration)
{
    const char* args[] = { "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=width,height,avg_frame_rate,r_frame_rate:stream_side_data=rotation:format=duration",
        "-of", "default=noprint_wrappers=1", path.c_str(), nullptr };
    SDL_Process* proc = SDL_CreateProcess(args, true);
    if (!proc) return false;
    size_t len = 0;
    int exitCode = -1;
    char* out = (char*)SDL_ReadProcess(proc, &len, &exitCode);
    SDL_DestroyProcess(proc);
    if (!out) return false;

    int w = 0, h = 0, rotation = 0;
    double avgRate = 0, baseRate = 0;
    std::string avgText, baseText;
    duration = 0;

    std::string text(out, len);
    SDL_free(out);
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (!value.empty() && value.back() == '\r') value.pop_back();

        if (key == "width") w = std::atoi(value.c_str());
        else if (key == "height") h = std::atoi(value.c_str());
        else if (key == "rotation") rotation = std::atoi(value.c_str());
        else if (key == "duration") duration = std::atof(value.c_str()); // "N/A" -> 0
        else if (key == "avg_frame_rate") { avgRate = parseRate(value.c_str()); avgText = value; }
        else if (key == "r_frame_rate") { baseRate = parseRate(value.c_str()); baseText = value; }
    }

    if (exitCode != 0 || w <= 0 || h <= 0) return false;
    if (avgRate <= 0 && baseRate <= 0) return false; // still image ffmpeg can read, not a video

    // ffmpeg turns rotated (phone) videos upright, so the frames come out with the sides swapped
    if (std::abs(rotation) % 180 == 90) std::swap(w, h);

    v.path = path;
    v.w = w;
    v.h = h;
    v.fps = avgRate > 0 ? avgRate : baseRate;
    v.rate = avgRate > 0 ? avgText : baseText;
    return true;
}

bool loadVideo(const std::string& path)
{
    Video probed;
    double duration;
    if (!probeVideo(path, probed, duration)) return false;

    stopVideo();
    Video& v = g_video;
    v.path = probed.path;
    v.w = probed.w;
    v.h = probed.h;
    v.fps = probed.fps;
    v.rate = probed.rate;
    startMedia(M_VIDEO, duration);
    createPreview(v.w, v.h);
    g_videoFrame.assign((size_t)v.w * v.h * 4, 0);
    return startVideo(0);
}

void updateVideo(double dt)
{
    Video& v = g_video;
    if (!v.proc) return;
    if (g_playing) g_time += dt;

    bool fresh = false, ended = false, empty = false;
    {
        std::lock_guard<std::mutex> lock(v.m);
        v.wanted = std::max(v.first, (int)(g_time * v.fps));
        if (v.frameIdx >= 0 && v.frameIdx != g_frameShown)
        {
            v.frame.swap(g_videoFrame);
            g_frameShown = v.frameIdx;
            fresh = true;
        }
        // Ended once the clock is past the last frame ffmpeg sent
        ended = v.eof && v.frameIdx == g_frameShown && g_time * v.fps >= v.frameIdx + 1;
        empty = v.eof && v.frameIdx < 0;
    }
    v.cv.notify_all();

    if (fresh) showFrame(g_videoFrame.data());
    if (ended || empty)
    {
        if (empty && v.first == 0) g_playing = false; // ffmpeg could not decode anything
        else if (g_playing)
        {
            g_time = 0;
            startVideo(0); // loop
        }
        else
        {
            g_time = std::min(g_time, (double)(v.frameIdx + 1) / v.fps);
        }
    }
}

double videoFps() { return g_video.fps; }
const char* videoRate() { return g_video.rate.c_str(); }

// Waits for ffmpeg to decode frame idx, so it never answers "not ready"
int showVideoFrame(int idx)
{
    Video& v = g_video;
    if (!v.proc) return -1;
    {
        std::unique_lock<std::mutex> lock(v.m);
        v.wanted = idx;
        v.cv.notify_all();
        v.cv.wait(lock, [&] { return v.frameIdx >= idx || v.eof; });
        if (v.frameIdx < idx) return -1;
        v.frame.swap(g_videoFrame);
        g_frameShown = v.frameIdx;
    }
    v.cv.notify_all();
    showFrame(g_videoFrame.data());
    return 1;
}
