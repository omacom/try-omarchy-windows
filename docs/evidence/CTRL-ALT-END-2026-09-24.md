# Ctrl+Alt+End diagnostic update, September 24, 2026

The earlier PowerShell `SendInput` harness never reached the launcher hook. Its
control test also failed through the shipped Print Screen path, so those runs
are not product evidence.

With the Go SendInput probe and QEMU focused at `08:47:08`, the branch hook saw
the injected chord and forwarded it to the guest:

```text
08:47:08 cad-debug: hook saw vk=0xa2 wParam=0x100
08:47:08 cad-debug: hook saw vk=0x23 wParam=0x100
08:47:08 cad-debug: wParam=0x100 down=true pid=4572 fg=4572 focused=true ctrl=0xffffffffffff8000 alt=0xffffffffffff8001 flags=0x11 forwarding=false
08:47:08 winkey: forwarded ctrl to the guest
08:47:08 winkey: forwarded alt to the guest
08:47:08 winkey: forwarded delete to the guest
08:47:10 cad-debug: hook saw vk=0xa4 wParam=0x100
08:47:10 cad-debug: wParam=0x100 down=true pid=4572 fg=4572 focused=true ctrl=0xffffffffffff8000 alt=0xffffffffffff8001 flags=0x11 forwarding=true
```

That run also showed another hook consuming the End key-up, leaving the
held-Delete design latched. The new handler queues one complete Ctrl+Alt+Delete
press and release when End goes down. This prevents a lost End release from
leaving keys held in the guest.

The one-shot version has local tests only. Guest window closing and physical
keyboard, RDP, and AltGr behavior are not yet verified on hardware.
