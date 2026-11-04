# Muntin data and privacy

Muntin collects no data and sends nothing over the network. The diagnostic events of PowerToys are left out of it.

It keeps these files on your computer, in `%LOCALAPPDATA%\Muntin`:

| File | What it holds |
|---|---|
| `settings.json` | Your settings |
| `dock-layouts.json` | Your layouts: monitor names, app paths, and window positions |
| `desktop-scroll.txt` | While the desktop is scrolled: where each window belongs |
| `Logs` | A log of what Muntin did, for troubleshooting |

A named layout also adds a shortcut to your Start menu, in a `Muntin` folder. **Start at sign-in** adds a shortcut to your Startup folder.

Delete the folder and those shortcuts to remove everything Muntin keeps.
