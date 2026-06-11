#!/bin/bash
#
# Сборка кастомного ядра WSL2 (ветка 6.6.y) с поддержкой модулей,
# необходимая для компиляции и загрузки cowfs.ko.
#
# Запускать ВНУТРИ WSL2 (Ubuntu), из любой директории.
#
# По завершении:
#   1. скрипт скопирует bzImage в /mnt/c/wsl-kernel/bzImage-6.6
#   2. нужно прописать путь к нему в %USERPROFILE%\.wslconfig (на Windows)
#   3. выполнить `wsl --shutdown` в PowerShell и перезапустить WSL
#
set -euo pipefail

KERNEL_BRANCH="linux-msft-wsl-6.6.y"
BUILD_DIR="$HOME/wsl2-kernel-build"
OUT_DIR="/mnt/c/wsl-kernel"

echo "==> Установка зависимостей сборки ядра"
sudo apt update
sudo apt install -y build-essential flex bison libssl-dev libelf-dev \
    bc dwarves git libncurses-dev rsync

echo "==> Клонирование исходников ядра WSL2 (${KERNEL_BRANCH})"
if [ ! -d "$BUILD_DIR" ]; then
    git clone --depth=1 -b "$KERNEL_BRANCH" \
        https://github.com/microsoft/WSL2-Linux-Kernel.git "$BUILD_DIR"
fi
cd "$BUILD_DIR"

echo "==> Конфигурация (Microsoft/config-wsl)"
cp Microsoft/config-wsl .config
make olddefconfig

echo "==> Сборка ядра (это займёт 20-40 минут)"
make -j"$(nproc)"

echo "==> Подготовка дерева для сборки внешних модулей (cowfs)"
make modules_prepare

echo "==> Установка модулей в /lib/modules/<version>/"
sudo make modules_install

KVER=$(make kernelrelease)
echo "==> Версия собранного ядра: $KVER"

echo "==> Создание симлинка /lib/modules/$KVER/build -> $BUILD_DIR"
sudo ln -sfn "$BUILD_DIR" "/lib/modules/$KVER/build"

echo "==> Копирование bzImage в $OUT_DIR (доступно из Windows)"
mkdir -p "$OUT_DIR"
cp arch/x86/boot/bzImage "$OUT_DIR/bzImage-$KVER"

echo ""
echo "================================================================"
echo "Готово. Версия ядра: $KVER"
echo "Образ ядра скопирован в: C:\\wsl-kernel\\bzImage-$KVER"
echo ""
echo "Дальнейшие шаги (на стороне Windows, в PowerShell):"
echo ""
echo "  1. Откройте/создайте файл %USERPROFILE%\\.wslconfig и добавьте:"
echo ""
echo "     [wsl2]"
echo "     kernel=C:\\\\wsl-kernel\\\\bzImage-$KVER"
echo ""
echo "  2. Перезапустите WSL:"
echo ""
echo "     wsl --shutdown"
echo ""
echo "  3. Снова откройте Ubuntu (WSL) и проверьте:"
echo ""
echo "     uname -r        # должно показать $KVER"
echo "     ls /lib/modules/\$(uname -r)/build"
echo ""
echo "После этого можно собирать cowfs обычным способом (cd kernel && make)."
echo "================================================================"
