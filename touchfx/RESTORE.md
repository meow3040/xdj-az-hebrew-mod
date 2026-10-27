# Restore the previous version

Power off the player and connect the USB to your computer. Restore the entire
backed-up MODS folder, or restore the saved core/audio/panel together from the
same working version. Preserve sample/data folders when restoring selectively.
Reconnect the USB and perform a full restart. Approve the restored module if asked.
Restore saved player settings only if necessary and through your established
backup procedure. Do not copy libraries into the player's running cache or edit
approval files. For a return to official AZ-MODS, follow upstream installation
and signing instructions; the developer override is a separate player setting.

After restoring the complete official installation, remove the developer override
if you enabled it for this test: `rm /home/root/settings/azmods-unsigned.on` over
SSH as root, then fully restart. Removing it while custom unsigned libraries remain
installed will prevent those libraries from loading.
