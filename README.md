Vita Recovery Menu

A diagnostic and recovery environment for the PlayStation Vita designed to help repair common system issues such as plugin conflicts, storage mount failures, and configuration corruption.

This project aims to provide a safe troubleshooting toolkit for Vita users and developers by combining system diagnostics, plugin management tools, and filesystem utilities in a single environment.

Features

Current features include:

System compatibility detection

Firmware detection with spoof protection

Model and motherboard identification

Plugin configuration discovery

Partition and filesystem inspection

Developer mode for advanced system access

Plugin repair and recovery tools

Planned features include:

Boot-time recovery access using the R trigger

Automated plugin conflict detection

Storage mount diagnostics

Advanced recovery tools for misconfigured systems


Project Goals

The goal of this project is to create a recovery environment similar to PC recovery consoles, tailored specifically for the PlayStation Vita.

Many common Vita problems are caused by: plugin conflicts
incorrect storage configuration
missing tai configuration
corrupted system files
This project provides tools that help diagnose and repair those issues safely.

Architecture Overview

The recovery system is divided into two main components.
Recovery Application
Boot-Time Plugin (planned)

Recovery Application

The main recovery environment runs as a standard Vita application.

It provides a graphical interface for diagnostics and repair tools.

Core capabilities include: system information detection 
plugin configuration management
filesystem browsing
storage troubleshooting
The system compatibility layer collects hardware and firmware information using Vita kernel APIs and filesystem inspection.

Firmware detection uses:sceKernelGetSystemSwVersion()
with a fallback method that reads the firmware version directly from the active OS partition:sdstor0:int-lp-act-os
This prevents firmware spoofing by plugins.

Compatibility Detection

The system detects several important hardware and software attributes.

These include:console model
motherboard revision
firmware version
taiHEN configuration
Enso installation status
The compatibility system is designed to help the recovery environment adapt to different Vita hardware revisions.

Boot-Time Recovery (Planned Feature)

A future component of the project is a minimal kernel plugin that enables boot-time access to the recovery environment.

This plugin will allow users to enter recovery mode by holding the R trigger while powering on the system.

Conceptual boot flow:Power On
↓
Enso Bootloader
↓
taiHEN loads kernel plugins
↓
Recovery plugin loads
↓
Check controller input
↓
If R trigger pressed → Launch recovery menu
Else → Continue normal boot

The boot plugin will remain intentionally minimal to reduce the risk of boot instability.

Its responsibilities will be limited to:controller input detection
conditional recovery launch
safe exit if recovery not requested
The full recovery environment will remain in the userland application.

Development Status

This project is currently experimental and under active development.

Some features are still incomplete, including:boot-time recovery
automated plugin repair
advanced storage diagnostics

Contributions and feedback from the Vita homebrew community are welcome.

Roadmap

Planned future improvements include:boot-time recovery access
plugin conflict detection
storage mount visualization
automatic recovery tools
boot diagnostics
safe plugin sandbox mode

Safety Notice

This tool interacts with low-level system components on the PlayStation Vita.

Users should ensure they understand the risks associated with modifying system configurations.

Boot-time functionality should only be used on systems with Enso installed.



