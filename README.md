# framecopy

Mirror the Steam Frame headset display over SSH, like scrcpy.

    framecopy [-e both|left|right] [-s WxH] [-f fps] [-b bitrate] [user@host]

## Configuration

framecopy defaults to both eyes, 1920x960, 30 fps, 6M, `steamos@frame`. 

`ssh` and `mpv` must be available on your local system. `gcc` should be available on your Frame

The size is limited to 3840x1920. Larger sizes reset the Frame when the encoder starts.

The first run will build the helper applications on the Frame, and requires a one-time `sudo setcap cap_sys_admin+ep` command for `framecopy-kms`. When the helper is rebuilt, you will need to reapply the capability. framecopy will ask you to do this when it is necessary.

## Nix
`nix run . -- [options]` runs it with its dependencies, and `nix flake check` builds both programs for the current system and checks the C formatting.

## Contributing
See the [contribution guidelines](CONTRIBUTING.md).

## License
This project is MIT licensed.
