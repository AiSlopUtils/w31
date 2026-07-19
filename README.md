# Win31 X

Win31 X is a small X11 window manager styled after the useful parts of Windows
3.1. It is written in C against core Xlib and is intended to run as the sole
window manager in a Debian Xorg session.

The current implementation includes:

- Windows 3.1-inspired beveled frames, title bars, move/resize, click-to-focus
  and raise, close, minimize, and maximize/restore controls. Dragging a title
  bar to the left or right edge of a monitor snaps the window to that half of
  the monitor. A thin, classic rubber-band outline follows title-bar moves and
  the actual window moves only on button release, avoiding repeated full-window
  repaints while dragging under Xorg or QEMU. Transient dialogs stay above
  their owning application.
- RandR 1.5 logical-monitor discovery with an Xinerama fallback. Maximize,
  snap, Applications, Control Panel, Run, the desktop menu, and power
  confirmations use the active monitor. Display hotplug and layout changes
  reflow arranged windows, internal windows, dialogs, and desktop icons.
- A real desktop icon for every minimized client. When the client publishes
  `_NET_WM_ICON`, that program artwork is used in its frame and minimized
  icon; one click maps, raises, and focuses the application again.
- A private, versioned desktop-layout store remembers manually positioned
  Applications and Control Panel icons, Applications, Control Panel, and Run
  geometry, application geometry and snap/maximize state by stable X identity,
  and the last Control Panel section. Monitor-relative positions safely fall
  back when a display is disconnected.
- A permanent **Applications** desktop icon. It opens a keyboard- and
  mouse-driven window containing the visible applications installed through
  freedesktop `.desktop` entries. Applications and Control Panel are persistent
  windows: they can coexist, remain open when focus moves or a program starts,
  maximize or snap like application windows, and close explicitly with their
  `X` button or Alt+F4. Launcher entries use the actual PNG or XPM named by
  each desktop file's `Icon=` field.
- A Windows+R / Super+R **Run** dialog. It accepts an executable and literal
  arguments, reports invalid commands in place, and never evaluates shell
  syntax.
- A classic right-click desktop menu with **Lock**, **Log Out**, **Restart**,
  and **Shut Down**. It leaves the active application focused, dismisses on an
  outside click, and appears only for the unused desktop rather than over an
  application or desktop icon. Log Out, Restart, and Shut Down each require
  confirmation in a classic dialog before taking effect; Lock remains
  immediate.
- A permanent **Control Panel** desktop icon with three sections:
  **Wi-Fi** scans, connects, and disconnects through NetworkManager;
  **Colors** applies and remembers one of five desktop/title-bar schemes; and
  **Auto Lock** enables an idle timeout or locks the screen immediately through
  `xss-lock` and `xsecurelock`, with one visible asterisk for each password
  character entered.
- High-resolution artwork selected from the user-supplied Windows 98 icon
  archive for the desktop UI and as a safe fallback whenever a program does
  not publish a usable icon.
- ICCCM client lifecycle, focus, `WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`,
  `WM_CHANGE_STATE`, and `WM_STATE` handling.
- A deliberately small EWMH subset for client lists, activation, close
  requests, frame extents, one desktop, work area, and hidden state.
- Alt+Tab, Alt+F2, Alt+F4, and Windows+R / Super+R shortcuts.

![Win31 X running on Debian 13 ARM64 in QEMU](docs/win31x-desktop.png)

## Build on Debian

Install the build packages and compile:

```sh
sudo apt update
sudo apt install build-essential pkg-config libx11-dev libxrandr-dev libxinerama-dev libpng-dev
make prefix=/usr
make prefix=/usr check
```

The Makefile verifies `pkg-config`, X11, XRandR, Xinerama, and libpng before
compiling and prints the required Debian/Ubuntu packages if anything is
missing. You can run only that preflight with `make check-build-deps`.

For every Control Panel feature, install the recommended runtime services too:

```sh
sudo apt install network-manager xss-lock xsecurelock
```

The binary is `build/win31x`. Win31 X does not need a compositor, GTK, Qt, or
an existing desktop environment. Its supplied-color icon renderer requires the
normal TrueColor visual provided by contemporary Xorg configurations.

To install the binary and make it available in a display manager's session
list:

```sh
sudo make prefix=/usr install
```

Log out, choose **Win31 X** in LightDM/GDM/SDDM, and log back in. Do not start it
inside a session that already has a window manager unless you use a nested X
server.

## Try it without replacing your desktop

On Debian, install a nested X server and a couple of test programs:

```sh
sudo apt install xserver-xephyr xterm x11-apps
Xephyr :2 -screen 1024x768 &
DISPLAY=:2 ./build/win31x &
DISPLAY=:2 xterm &
DISPLAY=:2 xclock &
```

The desktop starts with Classic Teal unless another saved scheme exists. Click
**Applications** once, then double-click a program to start it. Minimize a
program with the underscore button; its icon appears along the bottom of the
desktop and restores it with one click. Click **Control Panel** once to manage
Wi-Fi, colors, and screen locking. Applications and Control Panel remain open
when you focus another window or start a program, and they can be open at the
same time; close either one with its `X` button or Alt+F4. Press Windows+R to
run an executable by name or path. Right-click unused desktop space for the
session menu.

Drag the Applications or Control Panel icon to arrange the desktop. Their
positions, Applications, Control Panel, and Run geometry, application window
geometry, color scheme, and the selected Control Panel section are restored at
the next login.

Application geometry is keyed by GTK application ID, desktop-file ID, or
WM_CLASS, plus WM_WINDOW_ROLE when an application supplies one. Concurrent
windows with an otherwise identical identity receive separate ordinal slots;
because X11 exposes no stronger distinction for those windows, matching them
after a full logout is best effort and can follow their next launch order.

Controls:

| Action | Control |
| --- | --- |
| Move / resize | Drag a title bar (release the outline to commit) / outer frame edge |
| Snap left / right | Drag a title bar to the left / right edge of a monitor |
| Minimize / maximize or restore / close | `_` / maximize-restore / `X` title-bar button |
| Restore minimized app | Click its desktop icon |
| Arrange desktop icons | Drag an icon; click without moving to open it |
| Applications | Click its desktop icon or press Alt+F2 |
| Control Panel | Click its desktop icon |
| Run a command | Windows+R / Super+R, then type an executable and arguments |
| Lock | Right-click unused desktop space, then click Lock (immediate) |
| Log out / restart / shut down | Right-click unused desktop space, click the action, then confirm it |
| Launcher navigation | Arrows, Page Up/Down, Enter, mouse wheel |
| Wi-Fi | Refresh, select a network, enter its password if required, then Connect |
| Change colors | Select Colors, then click a color scheme |
| Auto lock | Select Auto Lock, toggle it, choose a timeout, or click Lock Now |
| Cycle / close active window | Alt+Tab / Alt+F4 |

## Tests

`make check` tests `.desktop` parsing, safe `Exec` and Run argument parsing,
XDG application-icon resolution and raster decoding, settings and desktop
layout validation, private atomic persistence, NetworkManager output and command handling,
auto-lock process supervision, fixed session-action argument vectors, all
supplied PNG dimensions and decoding, exact supplied-asset checksums, and every
icon category. The core tests do not require X; the auto-lock test exercises X
saver policy as well when a display is available. For the X11 regression suite,
install `libxtst-dev`, `xvfb`, `xauth`, and `xfonts-base`; `make smoke-xvfb` then
uses real XTEST pointer input to exercise the actual window-manager state
machine:

- manage a new client and reach `NormalState`;
- adopt a client that was mapped before the window manager started;
- focus and raise overlapping clients, including one with its own conflicting
  passive button grab, while replaying the activating click to that client;
- reject stale focus events from an already minimized client;
- keep transient dialogs above their owners and minimize/restore the family
  through one desktop icon;
- honor client Above/Below stack requests without breaking transient order;
- maximize and restore a client without losing its previous geometry, and snap
  title-bar drags to the left and right halves of the selected monitor;
- keep the real window stationary while a title-bar drag is held, move its
  rubber-band outline with the pointer, and commit the move on button release;
- enforce minimum sizes even with pathological resize increments;
- validate default, centered, southeast, static, and dynamically changed
  window gravity, bordered synthetic geometry, and shift-free withdrawal;
- map unmanaged top-level InputOnly windows and maintain oldest-first
  `_NET_CLIENT_LIST` ordering;
- minimize through `WM_CHANGE_STATE`, find the mapped desktop icon, and restore
  by clicking, while using the client's real `_NET_WM_ICON` pixels;
- open the Applications window and actually launch a test desktop entry while
  keeping the launcher open, and verify its real desktop-entry icon is drawn;
- open Run with a real XTEST Windows+R chord (also with Caps Lock and Num Lock),
  edit and execute a command, and keep the other internal windows open;
- open the desktop menu only from unused root space, clamp it at monitor edges,
  preserve the active client, dismiss it on an outside click, and exercise
  immediate Lock plus confirmed Restart, Shut Down, and Log Out through
  injected test executables, while cancellation leaves the session running;
- keep Applications and Control Panel mapped together across focus changes and
  newly mapped clients, ignore Escape, and close only the selected internal
  window through its `X` button;
- change and persist a color scheme, scan and connect to a simulated secured
  Wi-Fi network, and invoke Lock Now;
- verify that both idle and Lock Now locker processes receive asterisk password
  feedback without changing the window manager's environment;
- verify that Wi-Fi passwords are masked, passed outside the process argument
  list, and absent from window-manager logs;
- activate and raise a newly mapped client without dismissing Applications or
  Control Panel;
- withdraw and correctly unmanage a client; and
- reject a second window manager on the same display.

The same target also restarts the WM against one- and two-monitor layouts to
verify exact icon, Applications, Control Panel, Run, and application geometry;
duplicate application identities; Control Panel section and color restoration;
disconnected-monitor fallback/return; corrupt-coordinate clamping; and safe
client exit while a minimized icon is being dragged.

Run the memory-sanitized build with:

```sh
make clean
make SANITIZE=1 check all
```

## QEMU test VM on Apple Silicon

The scripts in `tools/qemu` create a project-local Debian 13 ARM64 VM. ARM64 is
important on an Apple Silicon Mac because it uses QEMU's HVF acceleration;
an x86_64 guest would use much slower CPU emulation.

```sh
./tools/qemu/prepare-debian-vm.sh
./tools/qemu/run-debian-vm.sh
```

Preparation downloads Debian's generic-cloud ARM64 image, pins and revalidates
its published checksum on later runs, checks the qcow2 images, creates a
disposable overlay, and packages the current source tree into a fresh NoCloud
seed. During provisioning Debian installs Xorg and the build tools, builds the
real `.deb` (including the Xvfb/XTEST suite), installs it, reruns the suite
against `/usr/bin/win31x` and its installed icon directory, checks the
`x-window-manager` alternative and X session entry, and starts LightDM. The
Cocoa display then logs into Win31 X automatically. The VM account is `win31x`
with password `win31x`; SSH is exposed only on host loopback port 2222.

Generated VM data lives in `.vm/` and is ignored by Git. Delete that directory
to get a completely fresh guest.

## Design notes and current scope

![Win31 X architecture](docs/architecture.svg)

Win31 X is a focused single-workspace window manager. It prefers the logical
monitor layout reported by RandR 1.5, falls back to Xinerama heads when RandR
does not provide a usable multi-monitor layout, and treats the root window as
one monitor if neither source supplies one. It does not reparent client-owned
override-redirect menus or tooltips, preserves clients through the X save set,
and never sends `.desktop` `Exec` text through a shell. The launcher tokenizes
commands and expands the field codes that make sense when no files or URLs were
supplied. Run similarly builds an exact argument vector, but treats `%` and
shell-looking characters literally; pipes, redirection, substitutions, and
other shell syntax are not evaluated.

The active monitor follows the interaction: a desktop or icon click and a
title-bar release use the monitor under the pointer, while keyboard actions use
the monitor containing the focused Applications, Control Panel, Run, or
managed-client window. If no window supplies that context, Win31 X retains the
last active monitor and falls back to the pointer and then the primary monitor.
The permanent Applications and Control Panel icons start on the primary
monitor, but can be dragged to any monitor. Their preferred monitor and
monitor-relative position persist across sessions; each minimized application
icon stays on the monitor where its client was minimized.

Maximize fills only the monitor containing the window. Releasing a title bar at
either edge of any monitor snaps it to that monitor's left or right half, and
restore returns the exact pre-arranged geometry. Applications, Control Panel,
and Run are centered on the active monitor the first time they open; their
saved geometry takes precedence on later logins. The right-click desktop menu is
constrained to the monitor under the pointer, and a power confirmation is
centered on that same monitor. RandR hotplug and screen-layout notifications
refresh the monitor list: maximized and snapped windows are reapplied to their
remembered monitor, ordinary windows are kept reachable, and icons, internal
windows, Run, and an open confirmation are reflowed. If a remembered monitor
has disappeared, its nearest surviving monitor is used, with the primary
monitor as the final fallback. Automatic rescue does not overwrite the saved
preferred monitor, so reconnecting it restores the original layout.

Title-bar moves use a thin rubber-band outline in the Windows 3.1 style. The
real window remains stationary until the pointer button is released, reducing
the large repaints that can otherwise produce visible tearing in Xorg guests
under QEMU. Border resizing remains live.

The WM-owned desktop menu is itself override-redirect, so opening it does not
replace the active application or `_NET_ACTIVE_WINDOW`. Lock uses the same
supervised locker as Control Panel's Lock Now and starts immediately. Choosing
Log Out, Restart, or Shut Down first opens a classic confirmation dialog. The
safe default is No. Click Yes or press Y to confirm; Left, Right, or Tab moves
the selection and Enter activates it. No, the dialog's X button, Escape, or
Alt+F4 returns to the desktop without acting. The modal input shield does not
hold X keyboard or pointer grabs, and automatically yields to a full-screen
locker so auto lock cannot be blocked by an unattended confirmation. Confirmed
Log Out ends the window
manager process so a normal display-manager session returns to its login
screen. Confirmed Restart and Shut Down invoke a validated `systemctl`
executable with the single fixed argument `reboot` or `poweroff`; they do not
use a shell or `sudo`, and normal systemd/logind authorization policy still
applies. Win31 X searches `/usr/bin/systemctl` and `/bin/systemctl`. The
`WIN31X_SYSTEMCTL` environment variable can instead name an absolute executable
for tests or nonstandard installations; an invalid override disables those two
menu actions. The regression suite injects a harmless recorder through that
override and therefore never restarts or powers off the test system.

The Control Panel also avoids a shell. Wi-Fi operations use exact `nmcli`
argument vectors and deliver a WPA/SAE password through a private inherited
file descriptor; the password is never placed in `argv`, an environment
variable, or a log message, and transient copies are cleared. Auto Lock uses
the established X saver/`xss-lock` path so suspend and login-session locking
remain compatible with Debian instead of implementing password authentication
inside the window manager. Win31 X launches its owned lock paths with
`XSECURELOCK_PASSWORD_PROMPT=asterisks`: this confirms each received character
but intentionally reveals password length. Authentication remains entirely in
`xsecurelock` and PAM; Win31 X never handles the lock-screen password. Color and
auto-lock preferences are stored privately in
`$XDG_CONFIG_HOME/win31x/settings.conf` (normally
`~/.config/win31x/settings.conf`).

Reliable core-X click replay uses a transparent input child over inactive
client content. Consequently, an inactive application's hover cursor and
pointer-motion feedback begin after its first activating click; the click
itself is still delivered to the application.

The shipped icon pixels come only from the user-provided `win98_icons.zip`.
Their exact archive paths, chosen resolutions, checksums, and licensing caveat
are recorded in [`assets/icons/README.md`](assets/icons/README.md). Installed
applications keep their own artwork: the launcher resolves PNG/XPM `Icon=`
names through the XDG hicolor and pixmaps paths, and managed clients may publish
`_NET_WM_ICON`. The supplied archive remains the fallback; Win31 X does not
synthesize replacement artwork.

This first version provides per-monitor maximize/restore, left/right edge snap,
and monitor-aware window, icon, and dialog placement, but intentionally does
not implement virtual desktops, general-purpose tiling or layout automation,
compositing, a taskbar, file-manager desktop items, or restoration of client
application processes across login sessions. Window geometry is restored when
an application starts again, but Win31 X does not relaunch it automatically.
Those features can be layered on without changing the core minimized-icon
model.
