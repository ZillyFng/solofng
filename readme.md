# Zilly - solofng

Maintained by ChillerDragon. Who did not invent fng.
This is just yet another version of fng for teeworlds 0.7.
The original fng is apparently written by some "TOM".

For a more complete and maintained fng/solofng mod with stats and 0.6/0.7 support checkout ddnet-insta:

https://github.com/ddnet-insta/ddnet-insta

Based on the game teeworlds.
Please visit https://www.teeworlds.com/ for up-to-date information about the game.

Originally written by Magnus Auvinen.


Building on Linux or macOS
==========================

Installing dependencies
-----------------------

    # Debian/Ubuntu
    sudo apt install build-essential cmake git libfreetype6-dev libsdl2-dev libpnglite-dev libwavpack-dev python3

    # Fedora
    sudo dnf install @development-tools cmake gcc-c++ git freetype-devel pnglite-devel python3 SDL2-devel wavpack-devel

    # Arch Linux (doesn't have pnglite in its repositories)
    sudo pacman -S --needed base-devel cmake freetype2 git python sdl2 wavpack

    # macOS
    brew install cmake freetype sdl2


Downloading repository
----------------------

    git clone https://github.com/ZillyFng/solofng --recurse-submodules
    cd solofng

    # If you already cloned the repository before, use:
    # git submodule update --init


Building
--------

    mkdir -p build
    cd build
    cmake ..
    make

On subsequent builds, you only have to repeat the `make` step.

You can then run the client with `./teeworlds` and the server with
`./teeworlds_srv`.
