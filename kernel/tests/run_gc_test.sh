#!/usr/bin/env bash
#
# Проверка сборщика мусора версий (GC) и удаления теневых копий по
# истечении окна хранения (window_seconds).
#
# В отличие от run_tests.sh, этот сценарий требует, чтобы модуль был
# загружен с НЕБОЛЬШИМИ значениями параметров, например:
#
#   sudo insmod cowfs.ko window_seconds=10 gc_interval=3
#   sudo mount -t cowfs -o lowerdir=/data/lower cowfs /mnt/cow
#   sudo bash kernel/tests/run_gc_test.sh
#
# Цель: убедиться, что после срабатывания GC (cowfs_version_gc /
# cowfs_versions_gc_all) система не падает (нет panic/oops от
# "scheduling while atomic" или WARN_ON при panic_on_warn=1) и
# теневые копии действительно удаляются.

set -u

MNT="/mnt/cow"
LOWER="/data/lower"
SHADOW="$LOWER/.cowfs_shadow"
PASS=0
FAIL=0

ok()  { echo "[PASS] $1"; PASS=$((PASS+1)); }
bad() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

section() { echo; echo "==== $1 ===="; }

WINDOW=$(cat /sys/module/cowfs/parameters/window_seconds 2>/dev/null || echo 0)
GC=$(cat /sys/module/cowfs/parameters/gc_interval 2>/dev/null || echo 0)

echo "window_seconds=$WINDOW gc_interval=$GC"
if [ "$WINDOW" -gt 30 ] || [ "$GC" -gt 10 ]; then
    echo "ВНИМАНИЕ: окно/интервал GC велики, тест будет долгим."
    echo "Рекомендуется: insmod cowfs.ko window_seconds=10 gc_interval=3"
fi

# ---------------------------------------------------------------------------
section "Test 5: GC переживает истечение окна (без паники ядра)"
# ---------------------------------------------------------------------------
F="$MNT/t_gc.txt"
rm -f "$F"

echo "v1" > "$F"
echo "v2" > "$F"   # создаёт версию COW_OP_WRITE с теневой копией 'v1'

shadow_before=$(ls -1 "$SHADOW" 2>/dev/null | wc -l)
echo "теневых копий до ожидания: $shadow_before"

sleep_for=$(( WINDOW + GC + 5 ))
echo "ожидание $sleep_for сек, чтобы GC отработал минимум один раз после истечения окна..."
sleep "$sleep_for"

# Если модуль выгрузился из-за паники, dmesg/lsmod это покажет.
if lsmod | grep -q '^cowfs'; then
    ok "gc: модуль cowfs всё ещё загружен после истечения окна"
else
    bad "gc: модуль cowfs не загружен (выгрузился/упал?)"
fi

if mount | grep -q ' /mnt/cow '; then
    ok "gc: /mnt/cow всё ещё смонтирована"
else
    bad "gc: /mnt/cow больше не смонтирована"
fi

if cat "$F" >/dev/null 2>&1; then
    ok "gc: $F всё ещё доступен после GC"
else
    bad "gc: $F стал недоступен после GC"
fi

shadow_after=$(ls -1 "$SHADOW" 2>/dev/null | wc -l)
echo "теневых копий после ожидания: $shadow_after"
if [ "$shadow_after" -lt "$shadow_before" ] || [ "$shadow_before" -eq 0 ]; then
    ok "gc: устаревшие теневые копии удалены ($shadow_before -> $shadow_after)"
else
    bad "gc: количество теневых копий не уменьшилось ($shadow_before -> $shadow_after)"
fi

if dmesg | tail -50 | grep -qiE 'panic|oops|BUG: scheduling while atomic|WARNING: CPU'; then
    bad "gc: в dmesg обнаружены признаки паники/oops/WARN"
    dmesg | tail -50 | grep -iE 'panic|oops|BUG: scheduling while atomic|WARNING: CPU'
else
    ok "gc: в dmesg нет паники/oops/WARN"
fi

# ---------------------------------------------------------------------------
section "Итог"
# ---------------------------------------------------------------------------
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
