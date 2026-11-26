SwanShellDX @TAG@: an extended build of [swanshell](https://github.com/asiekierka/swanshell), the menu of the [nileswan](https://49bitcat.com/docs/nileswan/) cartridge for the WonderSwan, WonderSwan Color and SwanCrystal. It is **not** the official swanshell: please do not report its problems to the swanshell project.

The plugins show their patch number (`PATCH` at the top right of the start screen, and in their menu's ABOUT page): it is the number in this release's name.

@CHANGES@
## Files

- **SwanShellDX-@TAG@.zip**: copy its contents to the root of the microSD card (FAT32). It holds the menu (`/NILESWAN/MENU.WS`), the BIOS files, the fonts, the licences and the plugins.
- **SwanShellDX-extras-@TAG@.zip** (optional): copy its `NILESWAN` folder over the card's.
  - `PATCHES`: the START+Y1 patch files of [SwanShell_RebootPatcher](https://github.com/mariovirgili/SwanShell_RebootPatcher);
  - `ICONS`: icon sets made with [SwanShell_IconEditor](https://github.com/mariovirgili/SwanShell_IconEditor) (Theme > Icons);
  - `WALLPAPERS`: wallpapers (Theme > Wallpaper).

## What it adds to swanshell

- Emulator plugins: Game Gear (`.gg`), Master System (`.sms`), SG-1000 (`.sg`), SC-3000 with the SF-7000 disk unit (`.sf`), Game Boy (`.gb`), ColecoVision (`.col`, with an on-screen keypad: START + Y2 (Y right)). Battery saves, save states (START + A / START + B held), an on-screen menu (START + Y3 (Y down)), back to the menu with START + Y1 (Y up).
- START + Y1 (Y up) back to the menu from WonderSwan games (Settings > System > Advanced).
- Game pictures (viewer and thumbnails), icon sets, wallpapers, menu music, VGM player improvements, favorites, recent and most played lists, IPS/BPS patches applied at launch, ROM info, save backups, a Help page.

See the [README](https://github.com/mariovirgili/swanshellDX/blob/SwanShellDX/README.md) and the plugins' [README](https://github.com/mariovirgili/swanshellDX/blob/SwanShellDX/src/ggswan/README.md).

## Known limits

- The SF-7000 plugin needs a disk with Disk BASIC on it, copied as `/NILESWAN/SF7000.SF` (not included).
- The ColecoVision plugin needs the ColecoVision BIOS (8 KB), copied as `/NILESWAN/COLECO.ROM` (not included). No MegaCart (over 32 KB) or Super Game Module games yet.
- Master System: while the plugin's numbers (FPS, profiler) are shown, every sprite is drawn in front of the background; the FPS can be read in the on-screen menu's VIDEO page instead (GAME FPS).
- Some games run below full speed in their heaviest scenes.

## Tools

- [SwanShell_ImageBuilder](https://github.com/mariovirgili/SwanShell_ImageBuilder): the game pictures.
- [SwanShell_RebootPatcher](https://github.com/mariovirgili/SwanShell_RebootPatcher): the START+Y1 patches.
- [SwanShell_IconEditor](https://github.com/mariovirgili/SwanShell_IconEditor): the icon sets.
- To try it on a PC: [nileswan-medem](https://github.com/49bitcat/nileswan-medem), the nileswan's emulator.

## Credits

swanshell, AthenaOS, libnile and the Wonderful toolchain: Adrian "asie" Siekierka and 49bitcat. SwanShellDX, the plugins and the tools: Gio-74, made with Claude (Anthropic). Full credits and licences in the README and in `/NILESWAN/license`.
