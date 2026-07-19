# Win31 X architecture

Win31 X is one Xlib process that owns the Xorg root window's window-manager
redirect. It reparents normal X11 clients into classic frames and owns a small
set of override-redirect desktop windows: Applications, Control Panel, Run,
Task Manager, the desktop session menu, and power confirmations.

The main event loop multiplexes X events, service-child completion, auto-lock
state, deferred layout saves, and timed Task Manager samples. There is no
toolkit event loop and no compositor.

## Task Manager

Task Manager is a singleton internal window with Windows 98-style chrome. The
global Ctrl+Shift+Esc binding maps and focuses it, or raises the existing copy
when it is already open. It participates in the same active-monitor placement,
maximize, edge-snap, rubber-band move, hotplug reflow, focus, and saved-geometry
rules as the other persistent internal windows.

The four views deliberately separate X11 tasks from Linux processes:

- **Applications** walks the window manager's live top-level client list,
  including minimized clients. Switch To restores, raises, and focuses the
  selected client. End Task uses that client's normal `WM_DELETE_WINDOW`
  lifecycle when supported.
- **Processes** presents the latest procfs snapshot with image name, PID, CPU,
  resident memory, and ownership. Selection is stored as `(PID, start time)` so
  refreshes and PID reuse cannot silently retarget an action.
- **Performance** draws one-second CPU and memory samples into fixed-length
  histories. CPU is derived from deltas between aggregate `/proc/stat` samples;
  memory use is derived from total and available memory in `/proc/meminfo`.
- **System** shows data read from procfs and `/etc/os-release`: OS, kernel,
  hostname, CPU model, logical processor count, RAM, uptime, load averages, and
  current process count.

`src/task_manager_data.c` owns sampling and process-control policy. It builds a
complete temporary snapshot, then swaps it into place only after the required
system files have parsed successfully. A failed refresh therefore leaves the
last usable view intact. Expected per-process exit, access, and malformed-file
errors omit only that row; resource or global I/O failures abort the entire
refresh instead of publishing a partial view. Per-process CPU history is
matched by both PID and kernel start time. The event-loop timer refreshes the UI
automatically once per second; F5 can request an immediate refresh. It does not
fork `ps`, run a shell, or poll in a blocking thread.

The hot process loop reads only `/proc/PID/stat` (including the displayed comm
name, CPU counters, state, start time, and resident pages) and
`/proc/PID/status` (real UID); it does not open `comm` or `cmdline`. Sampling
stops immediately after 16,384 accepted rows and marks the snapshot truncated,
bounding synchronous work and memory on unusually large hosts.

### Ending processes safely

Processes from every user remain visible, but Win31 X only signals a process
owned by the real user running the window manager. PID 1, non-positive PIDs,
the window manager itself, and another user's processes are refused. Just
before every signal, the backend rereads procfs and verifies the real UID and
kernel start time. It opens a Linux pidfd before validation and sends the signal
through that stable handle, closing the PID-reuse race between display and
action. It deliberately refuses process control when pidfds are unavailable or
when the sampler uses an alternate proc root; it never degrades to `kill()` by
numeric PID.

End Process requires a second click before sending `SIGTERM`. If that exact
process remains alive after the grace period, the UI exposes a separate Force
End action, which repeats all identity and ownership checks before `SIGKILL`.
No signal action invokes a shell or elevates privileges.

## Other boundaries

- `applications.c` discovers and parses freedesktop desktop entries, while
  `app_icons.c` resolves their application-provided PNG or XPM artwork.
- `icon_assets.c` decodes the supplied fallback artwork. Shipped icon pixels
  come only from the user-provided archive documented in
  `assets/icons/README.md`.
- `desktop_state.c` stores monitor-relative desktop and window geometry through
  private, versioned, atomic files. Task Manager geometry is part of schema 3;
  readers continue accepting schema 1 and 2 files and preserve unknown future
  schemas.
- `wifi_backend.c`, `auto_lock.c`, and `session_actions.c` isolate external
  service execution. Their arguments are fixed or parsed without a shell, and
  Wi-Fi passwords use a private inherited file descriptor.

## Platform assumptions

The process sampler requires Linux procfs at `/proc`; its system label normally
comes from `/etc/os-release`. Debian supplies both by default. The graphical
window manager requires X11, XRandR, Xinerama, libpng, and a TrueColor visual.
Process termination additionally requires Linux 5.3 or newer with usable
`pidfd_open` and `pidfd_send_signal` support; sampling remains available if a
kernel or sandbox does not provide those operations. NetworkManager and the
lock services remain optional runtime recommendations for their corresponding
Control Panel sections, not Task Manager dependencies.
