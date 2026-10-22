@echo -off
# Run by OpenShell ("Restore safe graphics" in the OpenCore menu): put the
# known-good unaccelerated config back and reboot. Press ESC within a second
# of the shell starting to skip this and get a normal shell prompt.
for %a run (0 7)
  if exist fs%a:\EFI\OC\config-vesa.plist then
    cp -q fs%a:\EFI\OC\config.plist fs%a:\EFI\OC\config-last-failed.plist
    cp -q fs%a:\EFI\OC\config-vesa.plist fs%a:\EFI\OC\config.plist
    echo Restored safe graphics config on fs%a:, rebooting...
    stall 3000000
    reset
  endif
endfor
echo config-vesa.plist not found on fs0-fs7, nothing changed.
