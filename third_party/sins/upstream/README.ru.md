# SINS - SINS Is Not Systemd

**SINS** — это модульный и легковесный слой совместимости, который устраняет разрыв между `runit` и `systemd`. Он позволяет запускать программное обеспечение, зависящее от `systemd` (например, Nginx, Docker или сервисы GNOME), в среде `runit`, предоставляя прослойку `systemctl` и фоновые демоны для D-Bus, уведомлений Notify и управления ресурсами Cgroups.

---

[English](README.md) | [Русский](README.ru.md)

---

## Что такое SINS

Это **прослойка совместимости для runit**, а не замена systemd. Много софта заработает «как есть», но функции **настоящего systemd** (user@, portable-сервисы, полные пространства journal, cgroup API systemd и т.п.) могут не поддерживаться — см. таблицы.

### Матрица совместимости

**systemctl** (частичный паритет, упор на скрипты и установщики):

| Команда / поведение | Уровень |
|---------------------|---------|
| start, stop, restart, reload, status, enable, disable | Поддерживается (`sv` и ссылки enable) |
| daemon-reload, show, cat, list-units, list-unit-files, is-system-running | Поддерживается (упрощённые семантики) |
| mask, unmask | Поддерживается (`/etc/sins/masked`) |
| try-restart, reload-or-restart, try-reload-or-restart, kill | Поддерживается (best-effort) |
| preset, preset-all | Явный no-op (нет базы preset) |
| Несколько юнитов за раз; `--user`; `--quiet` | Частично |
| Остальное | Вне плана без вклада сообщества |

**Юнит → run-скрипт**

| Фича | Уровень |
|------|---------|
| ExecStart, ExecStartPre (`sh -c` с экранированием) | Поддерживается |
| Type=simple по умолчанию; notify (`NOTIFY_SOCKET`); forking (ожидание `PIDFile`, если задан) | Частично |
| Type=oneshot | Частично: старт и логи есть, но точной семантики состояний systemd oneshot нет |
| Environment, EnvironmentFile (строки KEY=value), WorkingDirectory | Частично (`pkg/runit/manager.go`) |
| User через `chpst -u` | Частично |
| Group, ambient caps, drop-in systemd, slices | Не планируется / игнор |

**D-Bus** (тег `dbus`):

| Область | Уровень |
|---------|---------|
| `org.freedesktop.systemd1` Manager и заглушки под introspect | Частично |
| Установка hostname/timedate/locale | **Явные ошибки**, без «тихого успеха» |

**libsystemd / journal**

| Область | Уровень |
|---------|---------|
| Трамплины в elogind | По карте символов |
| NULL после `dlsym` | **abort с сообщением** |
| Файловый `sd_journal_*` | Поддерживается; wait/get_fd через **inotify** каталога лога, если возможно |

### Сообщество и упаковка

- **Artix + runit**: целевой сценарий; честная матрица важнее заявлений о полном systemd.
- **Форум / wiki**: завести тему на [форуме Artix](https://forum.artixlinux.org/) или дополнить wiki — это помогает собирать списки «ломучих» пакетов.

### Модули (go build tags)

- **dbus**, **notify**, **timers**, **sockets**, **cgroups** — см. `build.sh`.

### Чеклист десктопа (KDE / Hyprland + AUR)

SINS закрывает слой **`systemctl` + `libsystemd` + шина systemd1**. Остальное — как на Artix без systemd:

1. **Сборка**: для полноценного DE — `SINS_PROFILE=de` или `full`. **minimal** — без `sins-daemon`.
2. **Сессия**: **elogind** + DM / Wayland.
3. **Права**: **polkit** и группы (`storage`, `network`…).
4. **Порталы**: **xdg-desktop-portal** + бэкенд (**-kde** / **-gtk** / **-wlr**).
5. **Звук / BT**: **PipeWire** и т.д. — отдельно от SINS.
6. **Диски**: **udisks2** + **udiskie**.
7. **Pacman**: `IgnorePkg = systemd` и т.д.

**`systemctl --user`**: чтение unit’ов из пользовательских путей для `status`/`show`/`cat`; **start/enable** с `--user` не поддерживаются — см. stderr.

AUR: [contrib/desktop/AUR-checklist.md](contrib/desktop/AUR-checklist.md), смоук: `test/smoke_aur.sh` после `build.sh --profile minimal`.

### CLI test matrix (база перед релизом)

Релизный минимум по CLI закреплён smoke-набором в `test/`:

- `test/smoke.sh` — journal shim, базовый поток `systemctl`, опциональный D-Bus smoke.
- `test/smoke_aur.sh` — linker/`pkg-config` sanity для AUR.
- `test/smoke_template_units.sh` — `@`-шаблоны и регенерация через `daemon-reload`.
- `test/smoke_oneshot_forking.sh` — проверки генерации run-скриптов для `Type=oneshot` и `Type=forking`.
- `test/smoke_reload.sh` — путь выполнения `ExecReload`.
- `test/smoke_user_mode.sh` — read-only поведение `--user` и блокировка мутаций.
- `test/smoke_dbus_unit_props.sh` — свойства `org.freedesktop.systemd1.Unit` и ошибки Manager-методов.

Go/no-go перед тегом: [contrib/desktop/release-checklist.md](contrib/desktop/release-checklist.md).

### Заметки по безопасности

- Имена сервисов для `sv` и путей **валидируются** (`pkg/safeunit`): без `/`, `..` и опасных символов.
- В генерируемых run-скриптах **экранируются** рабочий каталог, `PIDFile`, каталог `chpst -e`; **ExecReload** выполняется через то же quoting, что и `ExecStart`.
- Ключи **Environment** должны быть безопасными идентификаторами, чтобы не выйти за пределы каталога `env/` сервиса.
- **Политика system D-Bus** (`org.freedesktop.systemd1.conf`): у не-root ограничены вызовы `Manager` (без **StartUnit/StopUnit/RestartUnit** на system bus); при особых сценариях правьте конфиг локально.
- Права unix-сокетов по умолчанию **0666** (как у типичного `docker.sock`). Ужесточение: **`SINS_UNIX_SOCKET_MODE`** и **`SINS_NOTIFY_SOCKET_MODE`** (восьмеричная строка, например `0660`).

---

## 🐧 Поддерживаемые дистрибутивы

SINS разработан для дистрибутивов **Linux**, использующих **runit** в качестве системы инициализации или менеджера сервисов:
- **Artix Linux** (версия runit)
- **Void Linux**
- **Arch Linux** (с кастомной настройкой runit)
- **Devuan** (версия runit)

---

## 🛠️ Сборка и установка

### Зависимости
- **Go** (рекомендуется 1.25+)
- **GCC** и GNU `ld` — для сборки шима `libsystemd.so.0` (только Linux **x86_64**). **Systemd и libsystemd для сборки не нужны**; реальные символы подтягиваются в рантайме через elogind.
- **Runit** и **D-Bus** — зависимости на целевой системе, а не на чистой машине для компиляции.

На архитектурах, отличных от x86_64, `build.sh` соберёт Go-бинарники, но пропустит сборку `.so`.

### Обновление экспортируемого ABI
Правьте `pkg/libsystemd/libsystemd.map`, затем пересоберите список `JUMP_TO`:

```bash
python3 scripts/sync_stub_jumps.py
```

Либо `SINS_SYNC_STUB=1 ./build.sh` перед сборкой шима (нужен Python 3).

### Пользовательский журнал (sd_journal)
Реализованы **`sd_journal_*`** поверх одного append-only файла (без journald). Запись и чтение — обычным API libsystemd; путь к файлу:

- `$SINS_JOURNAL_FILE`, иначе
- `/var/log/sins-journal/journal.sins` (если доступен на запись), иначе
- `/tmp/sins-journal/journal.sins`

Просмотр для отладки:

```bash
python3 scripts/sins-journal-cat.py
python3 scripts/sins-journal-cat.py /путь/к/journal.sins
```

В пакете ставится `/etc/logrotate.d/sins-journal` (из `contrib/logrotate/sins-journal`). Скрипт `sins.install` создаёт `/var/log/sins-journal` и при наличии группы `adm` выставляет права для чтения лога администраторами.

### 1. Профили (Artix / Void — только нужное)

```bash
./build.sh --list-profiles
./build.sh --profile minimal     # только systemctl + libsystemd.so
./build.sh --profile de          # dbus + notify (типичный DE)
./build.sh --profile server      # dbus + timers + cgroups
./build.sh --full

SINS_TAGS=dbus ./build.sh
./build.sh --tags dbus,notify
SINS_STRIP=1 ./build.sh --profile minimal
./build.sh --print-tags --profile de
```

В неинтерактивном режиме по умолчанию — **`minimal`**, пока не заданы `SINS_PROFILE`, `SINS_TAGS` или `SINS_CHOICE`.

Void: [contrib/void/README.md](contrib/void/README.md).

### 2. Интерактивная сборка

```bash
chmod +x build.sh
./build.sh
```

Буквы профиля (`m`/`d`/`e`/`s`/`f`) или старый числовой `SINS_CHOICE`.

### 3. Legacy / CI

```bash
SINS_CHOICE=0 ./build.sh
./build.sh --verify
```

В CI: verify, `go test ./...`, матрица тегов, профили minimal/de и полный smoke-набор `test/smoke*.sh`.

### 4. Установка (Arch/Artix)

По умолчанию в `PKGBUILD` — **`SINS_PROFILE=full`**. Минимальный пакет:

```bash
SINS_PROFILE=minimal makepkg -si
```

Для стека DE без socket-активации: `SINS_PROFILE=de makepkg -si`.

### 5. Управление пакетами (Arch Linux)
Чтобы обновления `systemd` не перезаписывали прослойку SINS, рекомендуется добавить `systemd` в `IgnorePkg` в вашем `/etc/pacman.conf`:
```ini
[options]
IgnorePkg = systemd
```

---

## 🧬 Использование

### Управление сервисами
Используйте бинарный файл `systemctl` как обычно:
```bash
systemctl start nginx
systemctl status nginx -f
```

### Демон SINS (sins-daemon)
Если вы включили поддержку D-Bus или Notify, убедитесь, что `sins-daemon` запущен в фоновом режиме. Вы можете настроить его как сервис runit:
```bash
# Создание сервиса runit для sins-daemon
mkdir -p /etc/runit/sv/sins-daemon
echo -e "#!/bin/sh\nexec sins-daemon" > /etc/runit/sv/sins-daemon/run
chmod +x /etc/runit/sv/sins-daemon/run
ln -s /etc/runit/sv/sins-daemon /var/service/
```

---

## 📜 Лицензия
MIT
