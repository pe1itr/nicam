# Environment profiles

`tools/nicam-run` loads one profile before starting receivers or transmitters.

Default profile lookup:

```sh
config/environments/$(hostname -s).env
```

Override profile lookup:

```sh
NICAM_ENV=websdr tools/nicam-run wbfm-rx ...
NICAM_ENV_FILE=/opt/nicam/local.env tools/nicam-run nicam-rx ...
```

Profiles are shell env files. Keep machine-specific device names, Python mode
and audio player choices here instead of putting them in systemd units or long
manual commands.

For `PYTHON_MODE=venv`, prefer `VENV_PATH=${REPO_DIR}/.venv`. If a profile still
contains an old absolute venv path and the checkout-local `.venv` exists,
`tools/nicam-run` falls back to that checkout-local venv automatically.
