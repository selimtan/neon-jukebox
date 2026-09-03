# Runtime assets

Neon Jukebox first looks for the optional OFL-licensed fonts below:

- `assets/fonts/NotoSans-Regular.ttf` for regular UI text
- `assets/fonts/NotoSans-Bold.ttf` for headings
- `assets/fonts/NotoSansMono-Regular.ttf` for meter and visualizer labels

When they are not present, the Windows Segoe UI and Consolas fonts are used.
No music is distributed with the application.

## Windows application icon

- `assets/windows/neon_jukebox.png` is the high-resolution transparent master.
- `assets/windows/neon_jukebox.ico` contains the Windows icon sizes from 16 to
  256 pixels.
- `assets/windows/neon_jukebox.rc` embeds that icon into `neon_jukebox.exe` so
  Explorer, desktop shortcuts, the taskbar, and the window use the branded icon.
