GdkPixBuf module for the KTX image format
=========================================

# PLEASE USE THE BELOW COMMAND TO CLONE THIS REPO:
```sh
git clone git@github.com:juliusikkala/pixbufloader-ktx.git --recursive
```
This repo uses submodules, `--recursive` fetches them automatically.

Ever wanted to look at your KTX textures in Gnome's image viewer? Rejoice,
all two of you now have a module for that.

This module is not very well tested, and only supports loading, not saving.
It is also not very fast with some formats, due to the kinds of type
conversions needed. Only 2D (non-array, non-cubemap) textures are
supported, with pixel formats that can be sensibly shown (without scale
parameters). Floats are clamped to [0, 1]. 1-4 channel images are
supported.

The code is pretty messy, I just hacked this together quickly because I
wanted to look at KTX images and couldn't find any simple viewer utilities.

## Installation:
```sh
meson build
sudo ninja -C build install
```
