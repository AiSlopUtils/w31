# Supplied icon provenance

Every application icon in this directory comes from the user-supplied archive
`win98_icons.zip`. No icon artwork was drawn or synthesized for Win31 X.
`SHA256SUMS` locks the exact 30 PNG files and retained source ICO so automated
tests detect any replacement or pixel-level change.

The PNG files below are byte-for-byte copies of the named archive members:

| Win31 X files | Archive source |
| --- | --- |
| `executable-32.png`, `executable-16.png` | `windows98-icons/png/executable-0.png`, `executable-1.png` |
| `terminal-32.png`, `terminal-16.png` | `windows98-icons/png/console_prompt-0.png`, `console_prompt-1.png` |
| `clock-32.png`, `clock-16.png` | `windows98-icons/png/clock-1.png`, `clock-0.png` |
| `editor-48.png`, `editor-16.png` | `windows98-icons/png/notepad-5.png`, `notepad-3.png` |
| `calculator-32.png`, `calculator-16.png` | `windows98-icons/png/calculator-0.png`, `calculator-1.png` |
| `files-48.png`, `files-16.png` | `windows98-icons/png/directory_closed-4.png`, `directory_closed-1.png` |
| `web-48.png`, `web-16.png` | `windows98-icons/png/world-4.png`, `world-5.png` |
| `mail-48.png`, `mail-16.png` | `windows98-icons/png/mailbox_world-2.png`, `mailbox_world-1.png` |
| `graphics-48.png`, `graphics-16.png` | `windows98-icons/png/paint_file-5.png`, `paint_file-1.png` |
| `media-48.png`, `media-16.png` | `windows98-icons/png/media_player_file-2.png`, `media_player_file-1.png` |
| `task-manager-32.png`, `task-manager-16.png` | `windows98-icons/png/computer_taskmgr-0.png`, `computer_taskmgr-1.png` |
| `settings-48.png`, `settings-16.png` | `windows98-icons/png/settings_gear-4.png`, `settings_gear-5.png` |
| `network-48.png`, `network-16.png` | `windows98-icons/png/network_normal_two_pcs-4.png`, `network_normal_two_pcs-3.png` |
| `games-48.png`, `games-16.png` | `windows98-icons/png/joystick-5.png`, `joystick-1.png` |
| `help-32.png`, `help-16.png` | `windows98-icons/png/help_book_big-0.png`, `help_book_big-1.png` |

The Applications artwork comes from frames 0 (48x48, 8-bit) and 2 (16x16,
8-bit) of `windows98-icons/ico/directory_program_group.ico`. The frames were
decoded to PNG without scaling or pixel changes; ImageMagick's absolute-error
comparison against the ICO frames returned zero. The original ICO is retained
at `original/directory_program_group.ico` for traceability.

The archive contained no README, license, copyright statement, credits, source
URL, or redistribution terms. These files are not covered by Win31 X's MIT
license; establish permission before redistributing them.
