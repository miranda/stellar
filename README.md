# Stellar Desktop Environment

Stellar is an X11 desktop environment focused on advanced multi-monitor setups, pixel-accurate UI rendering, and deep system-level customization.

It is designed for users who want a desktop that fully embraces the flexibility of Xorg, with support for everything from modern high-DPI and variable refresh rate displays to low-resolution CRT monitors and retro gaming setups.

## Status

Stellar is currently in early development and is not ready for release yet.

This repository exists to document the project, track progress, and serve as the foundation for development.

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

- Advanced multi-monitor support
- Mixed DPI display support
- Mixed refresh rate support, including VRR
- Support for modern high-resolution HiDPI displays
- Support for very low-resolution displays, including CRT monitors
- DPI-based UI scaling without bitmap scaling or blur
- Pixel-accurate rendering of UI elements and fonts
- Support for all FreeType fonts, including true bitmap fonts
- Independent screensaver and power-saving behavior per monitor
- Advanced window rules
- Advanced window and application grouping
- Real-time Xorg configuration through the GUI
- Automatic detection of hardware and monitor capabilities
- Compositor support
- Support for both modern and retro gaming use cases

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
