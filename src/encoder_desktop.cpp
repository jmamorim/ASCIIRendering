// Video encoder: the ffmpeg program reads raw RGBA frames from a pipe and writes the file
#include "encoder.h"

#include <SDL3/SDL.h>

static SDL_Process* g_enc = nullptr;
static std::string g_path;    // the file being written, "" once it is complete
static bool g_closed = false;
static std::string g_error;

void encoderPrepare(VideoFormat)
{
    g_error.clear();
    g_closed = false;
}

bool encoderOpen(VideoFormat format, const std::string& path, int w, int h, const std::string& rate, double)
{
    std::string size = std::to_string(w) + "x" + std::to_string(h);
    const char* vf = format == V_GIF
        ? "vflip,split[a][b];[a]palettegen[p];[b][p]paletteuse"
        : "vflip,pad=ceil(iw/2)*2:ceil(ih/2)*2"; // yuv420p needs an even width and height
    std::vector<const char*> args = { "ffmpeg", "-v", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgba", "-s", size.c_str(), "-r", rate.c_str(), "-i", "-",
        "-vf", vf };
    if (format == V_MP4)
        args.insert(args.end(), { "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", "-movflags", "+faststart" });
    else
        args.insert(args.end(), { "-loop", "0" });
    args.push_back(path.c_str());
    args.push_back(nullptr);

    // Only stdin is a pipe; ffmpeg's errors go to our terminal
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)args.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    g_enc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);

    if (!g_enc)
    {
        g_error = "Could not start ffmpeg. It must be installed (on PATH).";
        return false;
    }
    g_path = path;
    return true;
}

bool encoderBusy()
{
    return false; // writes block until ffmpeg takes the frame
}

bool encoderWrite(const std::vector<unsigned char>& rgba)
{
    SDL_IOStream* io = SDL_GetProcessInput(g_enc);
    size_t done = 0;
    while (done < rgba.size())
    {
        size_t n = SDL_WriteIO(io, rgba.data() + done, rgba.size() - done);
        done += n;
        if (n > 0) continue;
        if (SDL_GetIOStatus(io) != SDL_IO_STATUS_NOT_READY)
        {
            g_error = "ffmpeg stopped encoding; see the terminal for its message.";
            return false;
        }
        SDL_Delay(1);
    }
    return true;
}

void encoderClose()
{
    SDL_CloseIO(SDL_GetProcessInput(g_enc)); // end of input: ffmpeg finishes the file and exits
    g_closed = true;
}

int encoderPoll()
{
    if (!g_error.empty()) return -1;
    if (!g_closed) return 0;

    int code = 0;
    if (!SDL_WaitProcess(g_enc, false, &code)) return 0;
    SDL_DestroyProcess(g_enc);
    g_enc = nullptr;
    if (code == 0)
    {
        g_path.clear();
        return 1;
    }
    g_error = "ffmpeg could not write the file; see the terminal for its message.";
    return -1;
}

std::string encoderError()
{
    return g_error;
}

void encoderAbort()
{
    if (g_enc)
    {
        SDL_KillProcess(g_enc, true);
        SDL_WaitProcess(g_enc, true, nullptr);
        SDL_DestroyProcess(g_enc);
        g_enc = nullptr;
    }
    if (!g_path.empty()) SDL_RemovePath(g_path.c_str()); // the file is incomplete
    g_path.clear();
}
