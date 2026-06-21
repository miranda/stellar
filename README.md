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
