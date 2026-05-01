# Environment profiles

`tools/nicam-run` loads one profile before starting receivers or transmitters.

Default profile lookup:

```sh
config/environments/$(hostname -s).env
```

Override profile lookup:

```sh
NICAM_ENV=odroid tools/nicam-run wbfm-rx ...
NICAM_ENV_FILE=/opt/nicam/local.env tools/nicam-run nicam-rx ...
```

Profiles are shell env files. Keep machine-specific device names, Python mode
and audio player choices here instead of putting them in systemd units or long
manual commands.
