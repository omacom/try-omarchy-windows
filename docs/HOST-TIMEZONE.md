# Windows time zone

New guest images can follow the Windows time zone while Omarchy runs. The
launcher sends a current zone every five seconds over a dedicated guest port.
The guest applies installed IANA zones and refreshes the desktop clock. The
personal setup form selects the Windows zone when a valid host hint is available.

Choosing a time zone inside Omarchy stops automatic following. That choice
survives guest restarts and later Windows zone changes. **Follow Windows Time
Zone** in the application launcher returns to host following and asks for the
guest password when sudo requires one.

The launcher only adds the live channel when the guest image declares it.
Older images keep their startup-only behavior. The diagnostic
`-timezone keep` option omits both the startup hint and live channel;
`-timezone Area/City` supplies a fixed zone instead of reading Windows.

The integration changes the time-zone selection, not the system clock, NTP,
RTC settings, or password policy. Unknown zones and failed updates leave the
guest selection intact. A manual choice made on an older Windows image is
preserved when its recorded host zone differs from the current guest zone.

On existing disks, **Update > Omarchy** installs the runtime clock refresh
backport. The zone itself follows without that package update, but an older
shell can keep displaying its previous zone until it is restarted.

This behavior is implemented for a future guest payload. It is not part of
the published `v0.6.2` image.
