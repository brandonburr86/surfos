"""The guiapps scenario's checks of track MEDIA (docs/superpowers/plans/2026-10-17-gui-suite.md); the Driver
is gui_apps_scenario.py's. A placeholder until the track's work lands: each window opens from the Apps menu and
closes."""
from gui_apps_scenario import window  # noqa: F401  (where the track's windows open, for its checks)


def checks(d):
    for name in ('Image Viewer', 'Hex',):
        d.open_app(name)
        d.visible(f'{name} opens from the Apps menu', d.title(name), f'guiapps-{name.lower().replace(" ", "-")}.png')
        d.close(name)
