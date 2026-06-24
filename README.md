# Stellar Desktop Environment

Stellar is an X11 desktop environment focused on advanced multi-monitor setups, pixel-accurate UI rendering, and deep system-level customization.
Stellar uses the incredibly powerful and flexible AwesomeWM as its window manager.

It is designed for users who want a desktop that fully embraces the flexibility of Xorg, with support for everything from modern high-DPI and variable refresh rate displays to low-resolution CRT monitors and retro gaming setups.

## Status

Stellar is now in early alpha/testing phase, but is not yet ready for an official release.
The current state is that it works as a daily driver, though it still has a ways to go.  Implemented in the current version:

- Advanced multi-monitor support
- Mixed DPI display support
- Mixed refresh rate support, including VRR
- Support for all FreeType fonts, including true bitmap fonts
- Windows support tabs for any GUI Terminal app
- Pixel-accurate rendering of UI elements and fonts
- 96dpi and 120dpi bitmap UI elements
- Independent screensaver and power-saving behavior per monitor
- Easy Xorg configuration through the GUI
- Automatic detection of hardware and monitor capabilities
- Compositor support (based on Picom)

## Goals

Stellar is built around a few core ideas:

- Make full use of the flexibility and power of X11/Xorg
- Provide first-class support for complex multi-monitor configurations
- Support both productivity and gaming workflows
- Deliver pixel-accurate rendering across a wide range of display types
- Expose advanced system and display configuration through a GUI
- Create a desktop environment that values control, precision, and specialization over simplification

## Features

Planned and in-progress features include:

- Advanced window rules (beyond even what KDE Plasma offers)
- More levels of DPI-based bitmap UI scaling (48dpi all the way up to 192dpi)
- More advanced Xorg configuration options through the GUI
- Easy resolution/modesetting, with integrated CRT libswitchres
- Advanced support for very low-resolution displays, including CRT monitors
- Advanced window and application grouping
- Support for both modern and retro gaming use cases
- More first-class Stellar apps and applets, including an audio control panel and network configurator

## Philosophy

Stellar is based on the idea that Xorg is being abandoned too early, despite still offering a level of flexibility and capability that remains unmatched for many advanced desktop use cases.

This project aims to show that standard Xorg is not broken, obsolete, or inadequate for a modern desktop environment. In many areas, it still enables levels of customization and behavior that are difficult or impossible to achieve elsewhere.

Rather than treating X11 as legacy technology to be discarded, Stellar treats it as a powerful foundation for building a highly capable and specialized desktop experience.

While continued development or forking of the Xorg codebase may also be a worthwhile long-term goal, Stellar is focused on what can already be achieved today with standard Xorg.

## Focus Areas

Stellar is especially aimed at users who care about:

- Complex multi-monitor desktop setups
- Mixed-density display environments
- Pixel-perfect UI behavior
- Retro display and CRT support
- Deep control over window behavior
- High-performance desktop usage
- Gaming and emulator-friendly desktop workflows
- A desktop environment that exposes advanced capabilities instead of hiding them

## Why X11?

Stellar is intentionally being built for X11 because Xorg still provides a level of openness, configurability, and display flexibility that is essential to this project’s goals.

X11 is the enabling technology, not a compromise or a holdover.

## Roadmap

The project is still in the planning and early implementation stage.

Initial work is focused on:

- Core desktop architecture
- Display and monitor management
- DPI-aware rendering model
- Window management behavior
- GUI-driven Xorg configuration
- Font rendering and pixel-accurate UI foundations

## Contributing

Contributions, discussion, and feedback may be welcome in the future, but the project is not yet in a stable state for general use or outside development.

## License

Apache-2.0

# Installation

## Arch/Artix Linux (Recommended)

If you are using Arch Linux, Artix, or other Arch-variant (Garuda, EOS, etc) the easiest and most reliable way to install Stellar is via the AUR. This method automatically resolves all dependencies, including the necessary repository forks.

Using your preferred AUR helper (e.g., yay or paru):
```
yay -S stellar-desktop-git
```
The AUR package will automatically pull the required forks:

- awesome-stellar-git: Fixes Arch's current awesome-git Lua 5.5 breakage by strictly enforcing the Lua 5.4 requirement. It also provides enhanced EHMW_MOVERESIZE behavior compared to stock AwesomeWM (using the currently unmerged PR #3859).
- atch-stellar-git: Pulls the necessary timed-drain-filter branch of the Atch terminal multiplexer to fix input draining issues with apps like fish and vim.

## Manual Installation (From Source)

If you are building from source on another distribution, you will need git, gcc, make, and pkgconf installed to compile the environment.  

### Install System Dependencies

Install the following packages using your distribution's package manager:

    Core: lua54, lua54-socket, lua54-dkjson, glib2, polkit, systemd-libs.
    X11 / Graphics: libxcb, xcb-util, xcb-util-keysyms, libxkbcommon, libxkbcommon-x11, cairo, freetype2, libdrm, libpciaccess, libx11, libxrandr, libxcursor, libxi, libxext, fontconfig, fonttosfnt, picom.  
    Desktop Utilities: stalonetray. Stellar uses stalonetray rather than the tray built into awesome.
    Screensaver (Optional): Xscreensaver. Stellar uses the hacks from xscreensaver, but uses its own stellar-saver daemon to run them.  

    Note: Stellar will run without picom or fonttosfnt, but will not have compositor effects or bitmap font support, respectively. Stalonetray is also technically optional, if you don't want a system tray, or you want to use the tray built into awesome instead.

    Stellar has not yet been tested with Xlibre, it is currently being developed and tested using standard Xorg only. Any reports of issues with Xlibre would be helpful and appreciated.
###  Install Required Custom Components

Stellar relies on a couple of specific builds for its window manager and terminal multiplexer:

    AwesomeWM: You need awesome-git at a minimum. Stock awesome-git works perfectly fine with Stellar. However, compiling the awesome-stellar-git fork is recommended if you want better support of windows that use client-side decorations.
    Atch: The integrated terminal multiplexer requires atch to function, but this is optional if you don't care about the Stellar terminal widget. Stock atch will work, but there are bugs in atch that manifest if you move terminal windows between screens.  Cloning and building the timed-drain-filter branch of the atch fork here will eliminate this compatibility issue.
```
git clone -b timed-drain-filter https://github.com/miranda/atch.git
# Follow the Atch build instructions
```

### Build and Install Stellar

Once your dependencies are satisfied, clone the Stellar repository and install:
```
git clone https://github.com/miranda/stellar.git
cd stellar
make
sudo make install
```

## Usage

Once installed, you can launch the Stellar Desktop Environment in two main ways:

### Via a Display Manager
If you use a display manager (ly, lightdm, sddm, etc.), simply log out and select Stellar from your desktop session menu before logging back in.

### Via .xinitrc
If you prefer starting your X11 session manually via startx, add the following line to your ~/.xinitrc file:
```
exec stellar
```

### Note on Xorg config
Stellar will generate and install a compatible Xorg config file.  Stellar uses Zaphodheads with isolated screens in the ServerLayout section, which will interfere with the normal operation of other DEs or bare window managers in a multi-monitor setup.  Switching back and forth is handled automatically, just follow the dialog prompt instructions.
If you get stuck in a Stellar config and it's not giving you the option to revert to a non-Stellar configuration, you can manually delete the file:
```
/etc/X11/xorg.conf.d/10-stellar-video.conf
```
Deleting this file will have no consequences as Stellar keeps a cached file for restoration the next time you log into Stellar.
If you also use Wayland sessions, switching between Stellar and a Wayland session will not require any Xorg config-swapping.  Ironically, this may be the most convenient way to switch between Stellar and an alternative DE, if you need to do this often.
