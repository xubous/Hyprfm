# hyprfm

A fast terminal file manager for Linux, designed for keyboard-driven workflows and seamless Vim integration.
Built for users who live inside the terminal and want a lightweight, efficient, and predictable file explorer for environments like Hyprland, Wayland, TTY sessions, and minimal Linux setups.

---

## Features

### Navigation
- Arrow key navigation
- Enter directories and open files directly in `$EDITOR` (defaults to `vim`)
- Backspace to go to parent directory
- **Auto-refresh**: directory listing updates automatically when external processes create or delete files (powered by `inotify`)
- **cd on exit**: terminal changes to the last visited directory when you quit (`q`)

### File Operations
- Create files (`a`) and directories (`A`)
- Rename entries (`r`)
- Clone files/folders with auto-suffix (`C`)
- Move multiple selected items to any destination (`M`)
- Delete to trash with undo support (`dd` / `u`)

### Vim-style Commands
- `yy` — copy selected file/folder path to system clipboard
- `yc` — copy file content to system clipboard and internal clipboard
- `pp` — paste copied content as a new file
- `dd` — move to trash (recoverable)
- `u` — undo last delete

### Sorting
- By name (default)
- By size
- By modification date (newest first)
- Cycle through modes with `s`

### Search
- Prefix-based search with `/`
- Searches directories first, then files
- Jumps cursor to first match and highlights it

### Multi-selection
- Select/deselect with `Space` (cursor advances automatically)
- Move all selected items with `M`
- Clear selection with `ESC`

### Clipboard Integration
- Wayland: `wl-copy`
- X11 fallback: `xclip`

### Designed For
- Hyprland / Wayland
- Vim / Neovim workflows
- Keyboard-first Linux environments
- TTY and minimal setups

---

## Keybindings

| Key | Action |
|-----|--------|
| `↑` / `↓` | Navigate |
| `Enter` | Open file in `$EDITOR` / enter folder |
| `Backspace` | Go to parent directory |
| `a` | Create file |
| `A` | Create folder |
| `r` | Rename |
| `C` | Clone |
| `Space` | Select / deselect item |
| `M` | Move selected items |
| `ESC` | Clear selection |
| `/` | Search by prefix |
| `s` | Cycle sort mode (name → size → date) |
| `P` | Copy current directory path |
| `q` | Quit (and cd to last directory) |

### Vim-style Sequences

| Command | Action |
|---------|--------|
| `yy` | Copy selected path to system clipboard |
| `yc` | Copy file content to system clipboard |
| `pp` | Paste copied content as new file |
| `dd` | Move to trash (ask confirmation) |
| `u` | Undo last delete |

> Sequences have a 400 ms timeout between keys, matching Vim behavior.

---

## Installation

### Dependencies

- `g++` with C++17 support
- `libncurses` (`libncurses-dev` on Debian/Ubuntu, `ncurses` on Arch)
- `wl-copy` (Wayland) or `xclip` (X11) for clipboard support

### Build and install

```bash
git clone https://github.com/xubous/Hyprfm.git
cd Hyprfm
make
sudo make install
```

### Enable cd-on-exit (recommended)

After installing, add the shell wrapper to your shell config. This makes your terminal automatically change to the last directory you visited in hyprfm when you quit.

**bash:**
```bash
echo 'source /usr/local/bin/fm-shell.sh' >> ~/.bashrc && source ~/.bashrc
```

**zsh:**
```bash
echo 'source /usr/local/bin/fm-shell.sh' >> ~/.zshrc && source ~/.zshrc
```

Then just run:

```bash
fm
```

### Uninstall

```bash
sudo make uninstall
```

---

## How it works

hyprfm uses a **binary search tree (BST)** internally to store and sort directory entries, giving O(log n) search and O(n) rendering. The directory watcher uses Linux's `inotify` API to detect filesystem changes in real time without polling. Deleted files are moved to `~/.fm_trash/` instead of being permanently removed, enabling the undo feature.

---

## License

MIT
