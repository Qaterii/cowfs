# cowfs — файловая система ядра Linux с откатом операций на базе COW

Курсовая работа. Прототип stackable-файловой системы уровня ядра Linux,
реализующей откат деструктивных операций (запись, удаление, переименование,
изменение метаданных) на основе механизма Copy-on-Write.

---

## Как это работает

cowfs монтируется поверх обычной директории (например, ext4) и перехватывает
все опасные VFS-операции. Перед каждой такой операцией система сохраняет
теневую копию файла или его метаданных в скрытую директорию `.cowfs_shadow/`.
Копии хранятся в течение настраиваемого временного окна, после чего
автоматически удаляются сборщиком мусора.

```
Пользователь: write / unlink / rename / setattr
                        │
              ┌─────────▼──────────┐
              │  cowfs (LKM)       │
              │  1. сохранить копию│
              │  2. выполнить ops  │
              └─────────┬──────────┘
                        │
              /data/lower/          ← нижняя ФС (ext4)
              .cowfs_shadow/        ← теневые копии
```

---

## Структура проекта

```
cowfs/
├── kernel/
│   ├── Makefile
│   ├── cowfs.h            # структуры, константы, прототипы
│   ├── cowfs_main.c       # точка входа модуля, параметры
│   ├── cowfs_super.c      # монтирование, суперблок
│   ├── cowfs_inode.c      # inode операции + COW для unlink/rename/setattr
│   ├── cowfs_file.c       # file операции + COW для write
│   ├── cowfs_dir.c        # directory операции, скрытие .cowfs_shadow
│   ├── cowfs_shadow.c     # копирование файлов в/из shadow store
│   ├── cowfs_versions.c   # хэш-таблица версий в памяти ядра, GC
│   └── cowfs_ctl.c        # /dev/cowfs_ctl, ioctl LIST/ROLLBACK
└── userspace/
    ├── Makefile
    └── cowctl.c           # CLI утилита для просмотра и отката версий
```

---

## Требования

- Linux kernel >= 6.3 (используется API `mnt_idmap`/`nop_mnt_idmap`)
- Тестировалось на Ubuntu 26.04 (kernel 6.14) и WSL2 с кастомным ядром 6.6
- `build-essential`, `linux-headers-$(uname -r)` (или собственноручно собранное дерево ядра — см. раздел про WSL2)

---

## Запуск в WSL2 (Ubuntu)

Ядро WSL2 по умолчанию (~5.15) не подходит — в нём нет `mnt_idmap`,
используемого в коде модуля (появился в ядре 6.3). Также для WSL2
по умолчанию нет заголовков для сборки внешних модулей. Поэтому нужно
один раз собрать кастомное ядро WSL2 (ветка 6.6.y) с подготовленным
деревом для модулей.

### 1. Собрать кастомное ядро WSL2

Внутри WSL2 (Ubuntu) выполнить:

```bash
chmod +x scripts/build-wsl2-kernel.sh
./scripts/build-wsl2-kernel.sh
```

Сборка займёт 20-40 минут. Скрипт:
- соберёт ядро 6.6 из исходников Microsoft (`Microsoft/config-wsl`);
- установит модули в `/lib/modules/<version>/`;
- создаст симлинк `/lib/modules/<version>/build`, чтобы `Makefile` модуля
  находил дерево заголовков так же, как в обычном дистрибутиве;
- скопирует `bzImage` в `C:\wsl-kernel\bzImage-<version>`.

### 2. Подключить новое ядро в Windows

В PowerShell:

```powershell
notepad $env:USERPROFILE\.wslconfig
```

Добавить (версия — та, что вывел скрипт):

```ini
[wsl2]
kernel=C:\\wsl-kernel\\bzImage-6.6.x
```

Перезапустить WSL:

```powershell
wsl --shutdown
```

Снова открыть Ubuntu и проверить:

```bash
uname -r                          # должно быть 6.6.x
ls /lib/modules/$(uname -r)/build # должна быть директория с заголовками
```

### 3. Дальше — как в обычной установке

После этого все шаги (сборка `cowfs.ko`, `insmod`, `mount`, тесты)
выполняются так же, как описано ниже для VM — переходи к разделу
"Установка на виртуальной машине", начиная с шага 2 (клонирование
репозитория уже сделано, если ты работаешь из этой директории).

> **Примечание:** WSL2 — полноценная Linux VM (Hyper-V), поэтому `insmod`,
> `mount -t cowfs` и работа с `/dev/cowfs_ctl` работают штатно и не влияют
> на хост Windows.

---

## Установка на виртуальной машине (Ubuntu 26.04)

### 1. Установить зависимости

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) git
```

### 2. Клонировать репозиторий

```bash
git clone https://github.com/<ВАШ_ЛОГИН>/cowfs.git
cd cowfs
```

### 3. Собрать модуль ядра

```bash
cd kernel
make
```

После успешной сборки появится файл `cowfs.ko`.

### 4. Собрать userspace утилиту

```bash
cd ../userspace
make
sudo cp cowctl /usr/local/bin/
```

### 5. Подготовить директории

```bash
sudo mkdir -p /data/lower
sudo chmod 777 /data/lower
sudo mkdir -p /mnt/cow
```

### 6. Загрузить модуль

```bash
cd ../kernel

# Стандартные параметры (окно 5 минут, GC каждую минуту)
sudo insmod cowfs.ko

# Или с явными параметрами:
sudo insmod cowfs.ko window_seconds=120 gc_interval=30

# Проверить загрузку
lsmod | grep cowfs
dmesg | tail -20
```

### 7. Смонтировать файловую систему

```bash
sudo mount -t cowfs -o lowerdir=/data/lower cowfs /mnt/cow

# Проверить
mount | grep cowfs
ls /mnt/cow
```

---

## Использование

### Просмотр версий файла

```bash
cowctl list /mnt/cow/myfile.txt
```

Вывод:
```
Versions for: /mnt/cow/myfile.txt
TIMESTAMP              OPERATION  SHADOW
---------              ---------  ------
2026-06-11 14:00:01    WRITE      /data/lower/.cowfs_shadow/42_1718107201
2026-06-11 13:59:10    WRITE      /data/lower/.cowfs_shadow/42_1718107150
```

### Откат к последней сохранённой версии

```bash
cowctl rollback /mnt/cow/myfile.txt
```

### Откат к конкретной версии по timestamp

```bash
cowctl rollback /mnt/cow/myfile.txt 1718107150
```

### Изменение параметров на лету (без перезагрузки модуля)

```bash
# Изменить размер окна хранения снимков
echo 600 | sudo tee /sys/module/cowfs/parameters/window_seconds

# Изменить интервал GC
echo 60 | sudo tee /sys/module/cowfs/parameters/gc_interval
```

---

## Тестирование

### Тест 1 — откат записи

```bash
echo "original" > /mnt/cow/test.txt
echo "modified" > /mnt/cow/test.txt
cat /mnt/cow/test.txt          # modified

cowctl rollback /mnt/cow/test.txt
cat /mnt/cow/test.txt          # original
```

### Тест 2 — откат удаления

```bash
echo "important" > /mnt/cow/important.txt
rm /mnt/cow/important.txt
ls /mnt/cow/important.txt      # No such file

cowctl rollback /mnt/cow/important.txt
cat /mnt/cow/important.txt     # important
```

### Тест 3 — откат изменения прав

```bash
chmod 777 /mnt/cow/test.txt
ls -la /mnt/cow/test.txt       # rwxrwxrwx

cowctl rollback /mnt/cow/test.txt
ls -la /mnt/cow/test.txt       # исходные права восстановлены
```

---

## Остановка

```bash
sudo umount /mnt/cow
sudo rmmod cowfs
dmesg | tail -10               # убедиться в чистом завершении
```

---

## Отладка

```bash
# Логи ядра в реальном времени
sudo dmesg -w | grep cowfs

# Если umount не работает — найти процессы, держащие точку монтирования
fuser -m /mnt/cow
lsof | grep /mnt/cow
```

---

## Параметры модуля

| Параметр | По умолчанию | Описание |
|---|---|---|
| `window_seconds` | 300 | Время хранения снимков (секунды) |
| `gc_interval` | 60 | Интервал запуска сборщика мусора (секунды) |

---

## Перехватываемые операции

| Операция | Системный вызов | Что сохраняется |
|---|---|---|
| Запись | `write()` | Содержимое файла до первой записи |
| Удаление | `unlink()` | Полная копия файла + имя |
| Переименование | `rename()` | Старое имя + метаданные |
| Атрибуты | `chmod`, `chown`, `touch` | uid, gid, mode, mtime |
