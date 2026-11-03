# framecopy

Mirror the Steam Frame headset display over SSH, like scrcpy.

    framecopy [-e both|left|right] [-s WxH] [-f fps] [-b bitrate] [user@host]

Defaults: both eyes, 1920x960, 30 fps, 6M, `steamos@frame`. Needs `ssh` and `mpv` locally, `gcc`, `ffmpeg` and Vulkan on the Frame.

The first run builds on the Frame and prints a one-time `sudo setcap cap_sys_admin+ep` command for `framecopy-kms`. Rebuilding `framecopy-kms` clears the capability.

With nix, `nix run . -- [options]` runs it with its dependencies, and `nix flake check` builds both programs for the current system and checks the C formatting.
