# hyprfm

A fast terminal file manager for Linux, designed for keyboard-driven workflows and seamless Vim integration.

Built for users who live inside the terminal and want a lightweight, efficient, and predictable file explorer for environments like Hyprland, Wayland, TTY sessions, and minimal Linux setups.

---

## Features

### Navigation
- Arrow key navigation
- Enter directories
- Open files directly in `$EDITOR` (defaults to `vim`)
- Backspace to go to parent directory

### File Operations
- Create files
- Create directories
- Rename entries
- Clone files/folders
- Move multiple selected items
- Delete to trash with undo support

### Vim-style Commands
- `yy` copy selected file/folder path
- `yc` copy file content
- `pp` paste copied content as new file
- `dd` move to trash
- `u` undo last delete

### Productivity
- Prefix search
- Sorting by:
  - Name
  - Size
  - Date
- Multi-selection mode
- Clipboard integration (`wl-copy` / `xclip`)

### Designed For
- Hyprland
- Wayland users
- Vim / Neovim workflows
- Keyboard-first Linux environments

---

## Keybindings

| Key | Action |
|-----|--------|
| Enter | Open file / enter folder |
| Backspace | Go back |
| a | Create file |
| A | Create folder |
| r | Rename |
| C | Clone |
| Space | Select item |
| M | Move selected |
| ESC | Clear selection |
| / | Search |
| s | Change sorting |
| P | Copy current path |
| q | Quit |

### Vim-style Sequences

| Command | Action |
|--------|--------|
| yy | Copy selected path |
| yc | Copy file content |
| pp | Paste content |
| dd | Delete to trash |
| u | Undo delete |

---

## Installation

```bash
git clone https://github.com/yourname/hyprfm.git
cd hyprfm
make
