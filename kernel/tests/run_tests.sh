#!/usr/bin/env bash
#
# Ручные/полуавтоматические проверки заявленных операций cowfs:
#   - write    : откат содержимого файла
#   - unlink   : откат удаления (восстановление файла)
#   - rename   : откат переименования
#   - setattr  : откат изменения прав (chmod)
#
# Запускать на WSL после insmod/mount:
#   sudo bash kernel/tests/run_tests.sh
#
# Скрипт не трогает модуль (insmod/mount) — это делается отдельно.

set -u

MNT="/mnt/cow"
PASS=0
FAIL=0

ok()   { echo "[PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

section() {
    echo
    echo "==== $1 ===="
}

# ---------------------------------------------------------------------------
section "Test 1: WRITE rollback"
# ---------------------------------------------------------------------------
F="$MNT/t_write.txt"
rm -f "$F"

echo "original" > "$F"
echo "modified" > "$F"

content=$(cat "$F")
[ "$content" = "modified" ] && ok "write: файл изменён до 'modified'" \
                              || bad "write: ожидалось 'modified', получено '$content'"

cowctl rollback "$F" >/dev/null
content=$(cat "$F")
[ "$content" = "original" ] && ok "write: rollback вернул 'original'" \
                              || bad "write: после rollback ожидалось 'original', получено '$content'"

# ---------------------------------------------------------------------------
section "Test 2: UNLINK rollback"
# ---------------------------------------------------------------------------
F="$MNT/t_unlink.txt"
rm -f "$F"

echo "important" > "$F"
rm "$F"

if [ -e "$F" ]; then
    bad "unlink: файл всё ещё существует после rm"
else
    ok "unlink: файл удалён"
fi

cowctl rollback "$F" >/dev/null

if [ -e "$F" ]; then
    content=$(cat "$F")
    [ "$content" = "important" ] && ok "unlink: rollback восстановил содержимое 'important'" \
                                   || bad "unlink: rollback восстановил файл, но содержимое='$content'"
else
    bad "unlink: rollback не восстановил файл"
fi

# ---------------------------------------------------------------------------
section "Test 3: SETATTR (chmod) rollback"
# ---------------------------------------------------------------------------
F="$MNT/t_chmod.txt"
rm -f "$F"

echo "attrs" > "$F"
orig_mode=$(stat -c '%a' "$F")

chmod 777 "$F"
new_mode=$(stat -c '%a' "$F")
[ "$new_mode" = "777" ] && ok "setattr: chmod 777 применён" \
                         || bad "setattr: ожидался режим 777, получено $new_mode"

cowctl rollback "$F" >/dev/null
restored_mode=$(stat -c '%a' "$F")
[ "$restored_mode" = "$orig_mode" ] && ok "setattr: rollback восстановил режим $orig_mode" \
                                      || bad "setattr: ожидался режим $orig_mode, получено $restored_mode"

# ---------------------------------------------------------------------------
section "Test 4: RENAME rollback"
# ---------------------------------------------------------------------------
F_OLD="$MNT/t_rename_old.txt"
F_NEW="$MNT/t_rename_new.txt"
rm -f "$F_OLD" "$F_NEW"

echo "rename-me" > "$F_OLD"
mv "$F_OLD" "$F_NEW"

if [ -e "$F_NEW" ] && [ ! -e "$F_OLD" ]; then
    ok "rename: файл переименован в $(basename "$F_NEW")"
else
    bad "rename: переименование не выполнено как ожидалось"
fi

cowctl rollback "$F_NEW" >/dev/null

if [ -e "$F_OLD" ] && [ ! -e "$F_NEW" ]; then
    ok "rename: rollback вернул старое имя $(basename "$F_OLD")"
else
    bad "rename: rollback НЕ вернул старое имя (файл всё ещё '$F_NEW'?)"
fi

# ---------------------------------------------------------------------------
section "Версии и dmesg"
# ---------------------------------------------------------------------------
for f in t_write.txt t_unlink.txt t_chmod.txt t_rename_old.txt t_rename_new.txt; do
    p="$MNT/$f"
    [ -e "$p" ] && cowctl list "$p"
done

echo
dmesg | tail -20

# ---------------------------------------------------------------------------
section "Итог"
# ---------------------------------------------------------------------------
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
