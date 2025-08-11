# Face Detection VLC Filter

This project uses C++ to build a face detection video filter plugin for **VLC 4.X** API. It builds two files, 
`libfacedetection.a` (static library) and `libfacedetection_plugin.so` (VLC video filter plugin). We use the
libfacedetection library by Shiqi Yu and the OpenCV library for image processing.

This project was sponsored by [VideoLAN](https://www.videolan.org/). Thank you to Jean-Baptiste Kempf and the VideoLAN
team for their guidance.

## Prerequisites

You need to have VLC 4.X headers installed, along with Meson and a C++ compiler. Make sure you have all VLC dependencies 
for building plugins installed as well.

## Quick start (Meson)

From the project root:

```bash
meson setup build -Dbuild_vlc_plugin=true -Dopenmp=true -Denable_avx2=true
meson compile -C build
meson install -C build
```

This will produce:
```
build/libfacedetection.a
build/vlc-plugins/video_filter/libfacedetection_plugin.so
```

## Licensing

This project is licensed under the GPLv3 license. See the [LICENSE](https://code.videolan.org/videolan/vlc#license) file for details. We follow the same rules
as VLC for licensing.