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
git clone https://github.com/Qaterii/cowfs.git
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

В `kernel/tests/` есть два полуавтоматических скрипта, покрывающих все
заявленные сценарии. Их можно запускать сразу после `mount`, не трогая
модуль повторно.

### Автоматические тесты: write / unlink / setattr / rename

```bash
sudo bash kernel/tests/run_tests.sh
```

Покрывает:

| № | Операция | Что проверяется |
|---|---|---|
| 1 | `write`   | Файл изменён (`modified`), затем `cowctl rollback` возвращает исходное содержимое (`original`) |
| 2 | `unlink`  | Файл удалён (`rm`), затем `cowctl rollback` восстанавливает файл и его содержимое из теневой копии |
| 3 | `setattr` | `chmod 777` применяется, затем `cowctl rollback` возвращает исходные права доступа |
| 4 | `rename`  | `mv` переименовывает файл, затем `cowctl rollback` возвращает старое имя |

В конце скрипт выводит `cowctl list` для всех тестовых файлов и хвост
`dmesg`, а также итог `PASS=N FAIL=M`.

### Автоматический тест: сборщик мусора (GC) и истечение окна

Этот тест отдельный, так как требует **маленьких** значений
`window_seconds`/`gc_interval`, чтобы не ждать 5 минут. Перезагрузите
модуль с такими параметрами и запустите тест:

```bash
sudo umount /mnt/cow
sudo rmmod cowfs
sudo insmod cowfs.ko window_seconds=10 gc_interval=3
sudo mount -t cowfs -o lowerdir=/data/lower cowfs /mnt/cow

sudo bash kernel/tests/run_gc_test.sh
```

Покрывает:

| № | Сценарий | Что проверяется |
|---|---|---|
| 5 | Истечение окна хранения | После `write` создаётся версия с теневой копией; после ожидания `window_seconds + gc_interval` GC удаляет устаревшую версию и теневой файл, **модуль не падает**, `/mnt/cow` остаётся смонтированной, файл остаётся доступным, в `dmesg` нет `panic`/`oops`/`BUG: scheduling while atomic`/`WARNING` |

### Ручные тесты (вручную, по шагам)

Для каждого теста ниже описано: что он делает, в чём суть проверки и как
по выводу команд понять, что тест прошёл успешно.

#### Тест 1 — откат записи (WRITE rollback)

**Суть:** при каждой записи в файл cowfs сохраняет теневую копию его
предыдущего состояния. `cowctl rollback` должен вернуть файл к этому
состоянию.

```bash
F=/mnt/cow/t_write.txt
rm -f "$F"

echo "original" > "$F"
echo "modified" > "$F"
cat "$F"                    # modified

cowctl rollback "$F"
cat "$F"                    # original

cowctl list "$F"
```

**Как понять, что сработало:**
- после второй записи `cat` выводит `modified`;
- после `cowctl rollback` `cat` выводит `original` — содержимое
  восстановлено из теневой копии;
- `cowctl list` показывает хотя бы одну запись `WRITE` с путём в
  `.cowfs_shadow/`.

#### Тест 2 — откат удаления (UNLINK rollback), деструктивная операция

**Суть:** перед `unlink` (`rm`) cowfs копирует файл целиком в shadow-store
и запоминает исходное имя/каталог. `cowctl rollback` должен воссоздать
файл с тем же содержимым и именем.

```bash
F=/mnt/cow/t_unlink.txt
rm -f "$F"

echo "important" > "$F"
rm "$F"
ls "$F"                     # No such file or directory

cowctl rollback "$F"
ls "$F"                     # файл снова существует
cat "$F"                    # important
```

**Как понять, что сработало:**
- после `rm` файл реально пропал (`ls` — ошибка `No such file or
  directory`), т.е. деструктивная операция выполнилась;
- после `rollback` `ls` снова видит файл, а `cat` выводит исходное
  содержимое `important` — значит файл был восстановлен из теневой
  копии, а не просто остался на диске.

#### Тест 3 — откат изменения прав (SETATTR/chmod rollback)

**Суть:** перед `chmod`/`chown`/`truncate` cowfs сохраняет старые
атрибуты inode (метаданные, без копии содержимого). `cowctl rollback`
должен вернуть старые права.

```bash
F=/mnt/cow/t_chmod.txt
rm -f "$F"

echo "attrs" > "$F"
orig=$(stat -c '%a' "$F")
echo "было: $orig"

chmod 777 "$F"
stat -c '%a' "$F"           # 777

cowctl rollback "$F"
stat -c '%a' "$F"           # снова $orig
```

**Как понять, что сработало:**
- после `chmod 777` `stat` показывает `777` — деструктивное изменение
  метаданных применилось;
- после `rollback` `stat` показывает то же значение, что было до
  `chmod` (например `644`/`664`) — метаданные восстановлены из
  сохранённой версии, а не остались `777`.

#### Тест 4 — откат переименования (RENAME rollback), деструктивная операция

**Суть:** перед `rename` cowfs запоминает старое и новое имя/путь.
`cowctl rollback` должен вернуть файл под старым именем (с тем же
содержимым) и убрать его из-под нового.

```bash
OLD=/mnt/cow/t_rename_old.txt
NEW=/mnt/cow/t_rename_new.txt
rm -f "$OLD" "$NEW"

echo "rename-me" > "$OLD"
mv "$OLD" "$NEW"
ls "$OLD" "$NEW"             # OLD: No such file, NEW: существует

cowctl rollback "$NEW"
ls "$OLD" "$NEW"             # OLD: существует, NEW: No such file
cat "$OLD"                   # rename-me
```

**Как понять, что сработало:**
- после `mv` старого имени уже нет, новое — есть (нормальное поведение
  `mv`);
- после `rollback` ситуация обратная: новое имя пропало, старое —
  вернулось, и содержимое файла (`rename-me`) сохранилось — значит
  откат не просто переименовал «как было», а корректно восстановил
  состояние через версии.

#### Тест 5 — истечение окна хранения и работа GC, деструктивная операция (для модуля)

**Суть:** проверка фонового сборщика мусора (`gc_worker`), который
работает в workqueue, удаляет устаревшие версии из хеш-таблицы в памяти
и физически стирает теневые файлы (`vfs_unlink` из контекста ядра без
пользовательского mount namespace). Главная цель — убедиться, что это
**не роняет ядро** и не оставляет мусор в `.cowfs_shadow/`.

```bash
# модуль должен быть загружен с маленькими параметрами,
# например window_seconds=10 gc_interval=3
sudo umount /mnt/cow
sudo rmmod cowfs
sudo dmesg -C
sudo insmod cowfs.ko window_seconds=10 gc_interval=3
sudo mount -t cowfs -o lowerdir=/data/lower cowfs /mnt/cow

sudo find /data/lower/.cowfs_shadow -type f -delete   # убрать "хвосты" прошлых тестов

echo "v1" > /mnt/cow/t_gc.txt
echo "v2" > /mnt/cow/t_gc.txt
ls /data/lower/.cowfs_shadow/          # теневая копия 'v1' присутствует

sleep 15                                # window_seconds + gc_interval с запасом

ls /data/lower/.cowfs_shadow/          # теневых файлов быть не должно
mount | grep cowfs                     # /mnt/cow всё ещё смонтирована
lsmod | grep cowfs                     # модуль всё ещё загружен
cat /mnt/cow/t_gc.txt                  # v2 — сам файл не пострадал

sudo dmesg | grep -iE 'cowfs: (gc:|removed shadow|failed to remove|panic|oops|BUG: scheduling while atomic|WARNING)'
```

**Как понять, что сработало:**
- до `sleep` теневой файл от записи `v1` есть в `.cowfs_shadow/`;
- после `sleep` он исчез — GC отработал и удалил устаревшую версию;
- `/mnt/cow` остаётся смонтирована, модуль остаётся загружен, `cat`
  файла работает — GC не сломал ФС;
- в `dmesg` есть `cowfs: gc: removed N of N version(s)` и
  `cowfs: removed shadow ...` для каждого удалённого файла, и **нет**
  строк `panic`/`oops`/`BUG: scheduling while atomic`/`WARNING` — это
  подтверждает, что фоновое удаление из workqueue прошло безопасно (без
  сна под спинлоком, без обращения к чужому mount namespace и т.п.).

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

Если `rmmod cowfs` выдаёт `ERROR: Module cowfs is in use`, а
`lsmod | grep cowfs` показывает refcount > 0 даже после успешного
`umount` — модуль завис (остался лишний `module_get` от предыдущих
попыток монтирования в этой же сессии). В WSL2 проще всего сбросить
состояние ядра целиком:

```powershell
wsl --shutdown
```

После повторного запуска Ubuntu `lsmod | grep cowfs` и
`mount | grep cowfs` должны быть пустыми, можно грузить модуль заново.

---

## Полное удаление и переустановка обновлённой версии

```bash
# 1. Размонтировать и выгрузить текущий модуль
sudo umount /mnt/cow
sudo rmmod cowfs

# 2. Проверить, что ничего не осталось
lsmod | grep cowfs
mount | grep cowfs

# 3. Получить свежую версию исходников
cd ~/cowfs
git fetch origin
git reset --hard origin/main

# 4. Полностью пересобрать (без старых .o/.ko/.symvers)
cd kernel
make clean
make

# 5. Загрузить и смонтировать обновлённый модуль
sudo insmod cowfs.ko window_seconds=120 gc_interval=30
sudo mount -t cowfs -o lowerdir=/data/lower cowfs /mnt/cow
mount | grep cowfs
ls -la /mnt/cow

# 6. (опционально) пересобрать userspace-утилиту
cd ../userspace
make
sudo cp cowctl /usr/local/bin/
```

> Если `rmmod` на шаге 1 ругается "Module cowfs is in use" —
> см. раздел "Отладка" выше (`wsl --shutdown` для WSL2).

---

## Полная очистка системы от следов установки

Если нужно убрать cowfs из системы целиком (например, перед сдачей VM
или переносом на другую машину):

```bash
# 1. Размонтировать точку монтирования cowfs
sudo umount /mnt/cow

# 2. Выгрузить модуль ядра
sudo rmmod cowfs

# 3. Убедиться, что модуль выгружен и точка монтирования не висит
lsmod | grep cowfs        # пусто
mount | grep cowfs        # пусто

# 4. Удалить теневое хранилище версий (.cowfs_shadow создаётся
#    внутри lowerdir и НЕ удаляется автоматически при umount)
sudo rm -rf /data/lower/.cowfs_shadow

# 5. Удалить установленную userspace-утилиту
sudo rm -f /usr/local/bin/cowctl

# 6. Удалить рабочие/тестовые директории (если использовались только для cowfs)
sudo rm -rf /data/lower /mnt/cow

# 7. Удалить собранные артефакты модуля и утилиты в исходниках
cd ~/cowfs
git clean -xdf kernel userspace   # уберёт *.ko, *.o, *.mod*, Module.symvers, modules.order и т.п.

# 8. (опционально) удалить сам репозиторий с исходниками
cd ~
rm -rf ~/cowfs
```

**Как проверить, что система чистая:**
- `lsmod | grep cowfs` и `mount | grep cowfs` — пустой вывод;
- `dmesg | grep cowfs` — больше не появляется новых строк после перезагрузки/повторной проверки;
- `ls /data/lower` — директории `.cowfs_shadow` нет (если сама `/data/lower` не удалена);
- `which cowctl` — не находит утилиту.

> Кастомное ядро WSL2 (если собиралось по разделу "Запуск в WSL2") и
> запись `kernel=...` в `.wslconfig` — часть окружения сборки, а не
> следы cowfs; их трогать не обязательно. Если всё же нужно вернуть
> ядро WSL2 по умолчанию — удалите/закомментируйте строку `kernel=`
> в `%USERPROFILE%\.wslconfig` и выполните `wsl --shutdown`.

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
