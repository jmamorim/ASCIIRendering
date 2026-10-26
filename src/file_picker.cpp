#include "file_picker.h"
#include "app.h"

#include <mutex>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static std::mutex g_pickedMutex;
static std::string g_picked;

#ifdef __EMSCRIPTEN__
// Browser: an <input type="file"> copies the file into Emscripten's in-memory
// file system, then webFilePicked hands its path to the main loop.
EM_JS(void, PickImageFile, (), {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = 'image/*,video/*';
    input.onchange = async () => {
        const file = input.files[0];
        if (!file) return;
        const data = new Uint8Array(await file.arrayBuffer());
        try { FS.mkdir('/picked'); } catch (e) {}
        FS.writeFile('/picked/file', data, { canOwn: true });
        _webFilePicked();
    };
    input.click();
});

extern "C" EMSCRIPTEN_KEEPALIVE void webFilePicked()
{
    std::lock_guard<std::mutex> lock(g_pickedMutex);
    g_picked = "/picked/file";
}
#else
static void SDLCALL onFilePicked(void*, const char* const* files, int)
{
    if (!files || !files[0]) return; // error or cancelled
    std::lock_guard<std::mutex> lock(g_pickedMutex);
    g_picked = files[0];
}

void PickImageFile()
{
    static const SDL_DialogFileFilter filters[] = {
        { "Images, GIFs and videos", "bmp;jpg;jpeg;png;gif;tga;psd;mp4;mkv;webm;mov;avi;m4v" },
        { "All Files", "*" },
    };
    SDL_ShowOpenFileDialog(onFilePicked, nullptr, g_window, filters, 2, nullptr, false);
}
#endif

std::string takePickedFile()
{
    std::string picked;
    std::lock_guard<std::mutex> lock(g_pickedMutex);
    picked.swap(g_picked);
    return picked;
}
