#!/bin/bash
# This boot runs after interrupt.sh powered the VM off with a real pacman
# SIGKILL having left /var/lib/pacman/db.lck behind. The early-boot recovery
# must remove that orphaned lock on its own, without disturbing the database or
# the preserved user files, so the next normal transaction just works.
set -euxo pipefail
work="$HOME/package-recovery-fixture"
grep -qx controlled-pretransaction-sigkill "$work/interrupted"
# The orphaned lock predates this boot, so try-omarchy-pacman-lock.service runs
# and removes it before this phase starts.
[[ ! -e /var/lib/pacman/db.lck && ! -L /var/lib/pacman/db.lck ]]
sudo journalctl -b -u try-omarchy-pacman-lock.service --no-pager
sudo journalctl -b -u try-omarchy-pacman-lock.service --no-pager | grep -q 'removed stale pacman lock'
! pgrep -x pacman
sha256sum -c "$HOME/package-recovery.sha256"
db_status=0
sudo pacman -Dk > /tmp/package-db-current.log 2>&1 || db_status=$?
[[ $db_status == "$(cat "$HOME/package-db-baseline.status")" ]]
cmp "$HOME/package-db-baseline.log" /tmp/package-db-current.log
cat /tmp/package-db-current.log
# Removing the pause hook lets the fixture finish normally. No lock file is
# deleted by hand: the boot recovery already did that.
sudo rm -f /etc/pacman.d/hooks/00-try-omarchy-recovery-test.hook
sudo pacman -U --noconfirm "$work/fixture.pkg.tar.zst"
pacman -Q try-omarchy-recovery-test
sudo pacman -Qk try-omarchy-recovery-test
[[ $(cat /usr/share/try-omarchy-recovery-test/data) == 'fixture package data' ]]
db_status=0
sudo pacman -Dk > /tmp/package-db-current.log 2>&1 || db_status=$?
[[ $db_status == "$(cat "$HOME/package-db-baseline.status")" ]]
cmp "$HOME/package-db-baseline.log" /tmp/package-db-current.log
cat /tmp/package-db-current.log
sha256sum -c "$HOME/package-recovery.sha256"
[[ ! -e /var/lib/pacman/db.lck ]]
sync
