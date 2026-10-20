# ASCIIRendering

Open an image, GIF or video, then drag the sliders to see the ASCII output
change live.
Runs on Windows, macOS and Linux (SDL3 + Dear ImGui).

The build needs CMake 3.24+, a C++20 compiler and internet access the first
time: Dear ImGui, stb and (when it isn't installed) SDL3 are downloaded while
configuring.

## Windows

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) (Community
   is fine) with the **Desktop development with C++** workload. It includes
   CMake and Git.
2. Build, either way:
   - **In Visual Studio**: File > Open > Folder, pick this folder, wait for
     CMake to finish, choose `ASCIIRendering.exe` as the startup item and
     press F5.
   - **From the command line**: open the *Developer Command Prompt for VS
     2022* in this folder and run

         cmake -S . -B build
         cmake --build build --config Release

3. Run it:

       build\Release\ASCIIRendering.exe
       build\Release\ASCIIRendering.exe C:\path\to\image.png

SDL3 is built from source and linked statically, so the .exe runs on any
Windows 10/11 machine without extra DLLs.

## macOS

1. Install the compiler and the dependencies:

       xcode-select --install
       brew install cmake sdl3

2. Build:

       cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
       cmake --build build

3. Run it:

       ./build/ASCIIRendering
       ./build/ASCIIRendering ~/Pictures/image.png

## Linux

1. Install a compiler, CMake and SDL3:

       # Arch
       sudo pacman -S base-devel cmake sdl3
       # Debian / Ubuntu (Ubuntu 25.04+, Debian 13+)
       sudo apt install build-essential cmake libsdl3-dev
       # Fedora
       sudo dnf install gcc-c++ cmake SDL3-devel

   If your distro has no SDL3 package, leave it out: CMake builds SDL3 from
   source. It then needs the X11/Wayland development headers, see
   [SDL's Linux notes](https://github.com/libsdl-org/SDL/blob/main/docs/README-linux.md).

2. Build:

       cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
       cmake --build build

3. Run it:

       ./build/ASCIIRendering
       ./build/ASCIIRendering ~/Pictures/image.png

The **Open file...** button uses the desktop's file chooser (the XDG portal,
or `zenity` as a fallback). If neither is installed, pass the image on the
command line or drop it on the window instead.

## Videos

GIFs work out of the box. Videos (mp4, mkv, webm, mov, ...) are decoded by
the `ffmpeg` and `ffprobe` programs, which must be on PATH when you open one:

    # Windows
    winget install Gyan.FFmpeg
    # macOS
    brew install ffmpeg
    # Arch / Debian / Fedora
    sudo pacman -S ffmpeg / sudo apt install ffmpeg / sudo dnf install ffmpeg

## Using it

- Open an image, GIF or video with **Open file...**, by dropping it on the
  window, or by passing its path when starting the program.
- GIFs and videos play in a loop at their real speed; when converting a frame
  takes longer than the frame lasts, frames are skipped. **Pause**/**Play**
  and the slider under it pause and seek.
- Drag the sliders; the ASCII output on the right updates as you go.
- **Copy text** puts the ASCII output (the current frame) on the clipboard.

The glyphs are matched against DejaVu Sans Mono, which is compiled into the
program (`assets/`, see `assets/DejaVu-LICENSE.txt`).
