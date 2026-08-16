# Upstream provenance

This repository is an independent modified fork of VideoLAN VLC.

- Upstream: <https://code.videolan.org/videolan/vlc.git>
- Upstream base commit: `3458be162f476ff64b639140b684efa1143ddeea`
- XRVLC branch: `init`
- Initial XRVLC release tag: `v0.0.1`

The XRVLC changes add or adapt LibVLC media-player APIs, MP4 elementary-stream
handling, Android and OpenGL video output, video-output invalidation, and the
SoXR Android build configuration required by the XRVLC playback pipeline.

Files modified from the upstream base are:

- `contrib/src/soxr/rules.mak`
- `include/vlc/libvlc_media_player.h`
- `lib/libvlc.sym`
- `lib/video.c`
- `modules/demux/mp4/essetup.c`
- `modules/demux/mp4/mp4.c`
- `modules/video_output/android/display.c`
- `modules/video_output/android/utils.c`
- `modules/video_output/opengl/display.c`
- `src/video_output/video_output.c`

Comment-capable modified files carry an in-file change notice dated
2026-08-16. `lib/libvlc.sym` cannot safely contain comments because it is used
directly as a libtool export-symbol list, so its modification is recorded here.

This repository is not affiliated with, sponsored by, or endorsed by
VideoLAN. VLC and VLC media player are trademarks of VideoLAN.
