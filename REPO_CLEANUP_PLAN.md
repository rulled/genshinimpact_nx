# REPO_CLEANUP_PLAN.md — аудит репозитория Genshin-on-Switch (2026-09-07)

Аудит строго READ-ONLY: ничего не удалено, ветки не менялись, команды ниже — только рекомендации.
Единственное исключение: выполнен `git fetch fork` (безопасная загрузка ссылок) — из-за него локально
появились `fork/experiment/nvk-26.1.4-loaderless` и тег `crash-baseline`, которые раньше существовали
только на форке. Рабочее дерево и ветки не затронуты.

---

## 1. Карта веток

### Локальные ветки (11)

| Ветка | Tip | Дата | vs fork/client-1234 | Вердикт |
|---|---|---|---|---|
| `client-1234` (HEAD) | d190891 | 09-07 | **behind 6, ahead 0** (fast-forward возможен, проверено `merge-base --is-ancestor`) | Рабочая. Синхронизировать |
| `client-1224` | 380d8ec | 09-04 | Полностью влита (`git log client-1234..client-1224` пуст) | Удалить (тег + бандлы + форк покрывают) |
| `login-stable-6.7.0` | 540998a | 09-01 | Полностью влита; тот же коммит, что тег `stable-login-6.7.0` | Удалить (тег остаётся) |
| `main` | 2eab8bf | 09-04 | **НЕ влита**: 2 уникальных коммита | **Не трогать** (см. ниже) |
| `worktree-oc-managed-256m` | 9177ebd | 09-07 | = fork/client-1234, 0 уникальных | Удалить ПОСЛЕ завершения OC-сессии |
| `agent-pipeline-throttle` | 29d0efe | 09-07 | Патч байт-в-байт = 0f48f28 на форке | Удалить |
| `agent-unmap-hardening` | 67e520f | 09-07 | Поглощена 04c5487 (черри-пик — надмножество) | Удалить |
| `agent-pipeline-cache` | 6babb56 | 09-07 | Поглощена 0961c55 (разница только в контексте/офсетах) | Удалить |
| `worktree-agent-a629b3c5013596cba` | 6406b23 | 08-28 | = origin/main «switch port», полностью влита | Удалить |
| `worktree-agent-a7d92c89227c0f00e` | 6406b23 | 08-28 | то же | Удалить |
| `worktree-agent-aa12cdb0fab6017fe` | 6406b23 | 08-28 | то же | Удалить |

### Проверка «влитости» агентских веток (главный риск)

`git cherry` показывает `+` для 67e520f и 6babb56 (patch-id не совпадает из-за промежуточных
коммитов 0f50137/770615f), поэтому проверялось содержимое патчей напрямую:

- `29d0efe` vs `0f48f28`: `diff` патчей — **IDENTICAL**. Полностью влита.
- `67e520f` vs `04c5487`: cherry-пик применяет тот же фикс **дважды** (в `log_crash_exit` и в `abort`),
  т.е. является **строгим надмножеством** агентского коммита (stat: agent main.c `5 +--`, cherry-pick `10 +++---`).
  Потери — ноль.
- `6babb56` vs `0961c55`: diff 30 строк — только `index`-хэши, заголовки `@@` и контекстные строки
  (соседние хунки сместились из-за 0f48f28). Содержимое vulkan_bridge.c (+277) и main.c (+6) идентично.
  Потери — ноль.

### Ветка `main` — единственная настоящая развилка

`main` (2eab8bf) = merge(80d273a, 7600d31):
- `80d273a` «Upgrade client to Genshin 7.0.0 — hardware-verified at login screen» — **альтернативная
  реализация** апгрейда 1224, параллельная `adf2a55` из client-1224 (расхождение ~120 строк в
  `source/main.c`, `source/unity_entrypoints.h`, README/TESTING).
- Остальное содержимое main (через 7600d31) полностью входит в client-1234.
- `main` — default-ветка форка (`fork/HEAD -> fork/main`), база PR.
- **80d273a существует только локально и на форке** — бандл от 2026-09-02 его ещё не содержит.

### Удалённые (fork = rulled/genshinimpact_nx, origin = NaGaa95 — upstream)

| Ref на форке | Коммит | Примечание |
|---|---|---|
| `main` | 2eab8bf | default-ветка, содержит 80d273a |
| `client-1234` | 9177ebd | впереди локальной на 6 |
| `client-1224` | 380d8ec | дубликат локальной, можно удалить на форке после нового бандла |
| `login-stable-6.7.0` | 540998a | то же |
| `experiment/nvk-26.1.4-loaderless` | 7e661d5 | **уникальная работа**: 1 коммит «ci: build against Mesa 26.1.4 SDK» поверх fc0ce43, меняет только `.github/workflows/build.yml`. Единственная копия — **не удалять** |
| тег `crash-baseline` | 770615f | не был закоммичен локально до fetch |

## 2. Теги и restore points

| Объект | Значение | Статус |
|---|---|---|
| `stable-login-6.7.0` | 540998a (аннотированный) | ОК, локально + форк |
| `stable-login-7.0.0-1224` | dc2cf9f (аннотированный, подробное сообщение с SHA артефакта CI) | ОК, локально + форк |
| `crash-baseline` (форк) | 770615f | теперь загружен локально |
| `_restore_points/genshinimpact_nx-stable-login-7.0.0-1224-2026-09-02.bundle` (523 K) | 5 ссылок: client-1224, login-stable-6.7.0, main@6406b23, 2 тега | `bundle verify` — **okay, полная история**. УСТАРЕЛ: не содержит 9177ebd и 2eab8bf/80d273a |
| `backups/login-stable-6.7.0.bundle` (510 K) | 3 ссылки (6.7.0-эра) | `bundle verify` — **okay, полная история** |

Три слоя restore point подтверждены: локальные коммиты/теги, 2 валидных бандла, публичный форк.
**Пробел:** свежее состояние (client-1234 @ 9177ebd + main @ 2eab8bf) есть только на GitHub — нужен новый бандл.

## 3. Worktrees (5)

| Worktree | Ветка | Статус | Вердикт |
|---|---|---|---|
| корень репо | client-1234 | чистый, только `?? .claude/` | оставить |
| `.claude/worktrees/agent-a629b3c5013596cba` | agent-pipeline-throttle | чистый | удалить |
| `.claude/worktrees/agent-a7d92c89227c0f00e` | agent-unmap-hardening | чистый | удалить |
| `.claude/worktrees/agent-aa12cdb0fab6017fe` | agent-pipeline-cache | чистый | удалить |
| `.claude/worktrees/oc-managed-256m` | worktree-oc-managed-256m | чистый, **LOCKED: «claude session oc-managed-256m (pid 24392)»** | не трогать до завершения сессии |

`git stash list` — пусто. `git fsck --unreachable` — 19 blob + 6 tree, **0 недостижимых коммитов**
(мусор от add/stash, безвредно). `.git` — 2.1 MB, чистка объектов не нужна.

## 4. Мусор в самом репо

- `?? .claude/` (7.8 MB — 4 worktree-каталога) → добавить `.claude/` в `.gitignore`.

## 5. Родительская директория `D:\Programing_shit\genshin\`

| Объект | Размер | Что это | Рекомендация |
|---|---|---|---|
| `client-6.7.0-analysis/` | **1.9 GB** | XAPK/APK 1206 + распакованный `staged/` | **Архив №1**: 7z на внешний диск, затем удалить. Эра 1206; целевая версия теперь 1224/7.0.1 |
| `ida_data.jsonl` | 109 MB | IDA-данные 1224 | Архив (jsonl сжимается в 3–5x) |
| `dump.cs` (корень) | 104 MB | il2cpp dump 7.0.x | **Оставить** (живой справочник) |
| `dump-7.0.0/` | 104 MB | dump.cs **байт-идентичен** корневому (md5 `ef55338f…` у обоих) | **Удалить каталог** — чистый дубликат |
| `evidence/` | 48 MB | 6 прогонов 2026-08-31 (эра 1206) | Архив |
| `nro-backup-2026-09-07/` | 41 MB | 3 NRO (в т.ч. mesa26_test) | Оставить до следующего деплоя, затем архив |
| `Vulkan-Headers-1.4.305/` | 32 MB | Зависимость фазы Mesa/NVK | **Оставить на месте** |
| `nvk-sdk-25.0.7/` | 28 MB | SDK Mesa 25.0.7 (текущий CI-пин) | **Оставить на месте** |
| `nvk-switch/` + `nvk-switch-25.0.7.tar.xz` | 3.2 MB | Исходники/патчи NVK | Оставить |
| `_restore_points/`, `backups/` | ~1 MB | Бандлы | Оставить |
| `session-ses_f93e_FINAL.md` | 1.6 MB | Итоговый лог сессии 1224 | Оставить |
| `2session-ses_f93e_FINAL - Copy.txt` | 1.6 MB | md5 **идентичен** `.md` выше | **Удалить** (дубликат) |
| `session-ses_f93e_FINAL - Copy.txt` | 505 KB | Более старая копия | Архив |
| `session-ses_fa80.md`, `session-ses_fac6.md` | 560/548 KB | Промежуточные логи | Архив |
| `session_switch_fix_anticheat_fix_internet.md` | 305 KB | Эра 6.1 | Архив |
| 64 python-скрипта (`find_*`, `verify_*`, `disasm_*`, `resolve_*`, `check_*`, ~5–9 KB каждый, <0.5 MB суммарно) | — | Одноразовые recon-скрипты сессии 1224 | Переложить в `_archive/scripts-1224-recon/`. **НЕ удалять**: fp4-hardening отложен, пригодятся `resolve_fp4_*.py`, `find_fingerprints.py`, `analyze_1224.py` |
| `inject.cpp`, `Injector.vcxproj` | 24 KB | Windows-инжектор, к Switch-порту отношения не имеет | Архив |
| `XAPK_1224_UPGRADE_PLAN.md` | 14 KB | План (выполнен) | В `_archive/docs/` |
| `NATIVE_NRO_ANALYSIS.md`, `session-state-2026-09-07.md`, `error codes.txt`, `Запускаем локальный сервер…6.1.md`, `opencode.json`, `.claude/`, `.opencode/` | — | Актуальные гайды/конфиги | Оставить |

Ожидаемый эффект: ~2.1 GB освобождается (1.9 GB архив 1206 + 104 MB дубль dump.cs + 109 MB ida_data в архив),
рабочая директория сжимается до репо + тулчейн NVK/Mesa + актуальные гайды.

## 6. Целевая структура веток

```
origin/main (NaGaa95, upstream)          — только чтение, не трогать
fork/main (2eab8bf)                      — default-ветка форка, база PR; несёт уникальный 80d273a
fork/client-1234 (9177ebd) = client-1234 — ЕДИНСТВЕННАЯ рабочая линия
Теги-вехи: stable-login-6.7.0, stable-login-7.0.0-1224, crash-baseline (+ новые вехи = новые теги, не ветки)
experiment/nvk-26.1.4-loaderless         — хранится на форке (Mesa-26 CI, единственная копия)
Локально остаётся: client-1234, main. Всё.
```

Правило на будущее: веха → аннотированный тег (`git tag -a … -m …` + push), эксперимент →
`experiment/*` ветка с коротким сроком жизни, агентская работа → сразу cherry-pick в client-1234
и удаление ветки (как и произошло — просто не убраны хвосты).

## 7. План действий (команды НЕ выполнены)

```bash
cd /d/Programing_shit/genshin/genshinimpact_nx

# --- Шаг 0. Точка невозврата: свежий бандл (делать ПЕРВЫМ) ---
git bundle create ../_restore_points/genshinimpact_nx-client-1234-2026-09-07.bundle --all
git bundle verify  ../_restore_points/genshinimpact_nx-client-1234-2026-09-07.bundle

# --- Шаг 1. Синхронизировать рабочую ветку (fast-forward, ahead=0, риск ноль) ---
git merge --ff-only fork/client-1234        # d190891 -> 9177ebd

# --- Шаг 2. Скрыть служебный мусор из git status ---
printf '\n# Claude Code agent worktrees and local settings\n.claude/\n' >> .gitignore

# --- Шаг 3. Удалить агентские worktrees и ветки (все чистые, содержимое доказано на форке) ---
git worktree remove .claude/worktrees/agent-a629b3c5013596cba
git worktree remove .claude/worktrees/agent-a7d92c89227c0f00e
git worktree remove .claude/worktrees/agent-aa12cdb0fab6017fe
git branch -D agent-pipeline-throttle agent-unmap-hardening agent-pipeline-cache
git branch -D worktree-agent-a629b3c5013596cba worktree-agent-a7d92c89227c0f00e worktree-agent-aa12cdb0fab6017fe

# --- Шаг 4. Удалить поглощённые исторические ветки (-d безопасен: merged) ---
git branch -d client-1224 login-stable-6.7.0

# --- Шаг 5. oc-managed-256m — ТОЛЬКО после завершения сессии OC (pid 24392) ---
git worktree unlock .claude/worktrees/oc-managed-256m
git worktree remove .claude/worktrees/oc-managed-256m
git branch -D worktree-oc-managed-256m      # = fork/client-1234, 0 уникальных

# --- Шаг 6. (опционально) прибрать ветки на форке — только после Шага 0 ---
git push fork --delete client-1224 login-stable-6.7.0
# experiment/nvk-26.1.4-loaderless НЕ трогать!

# --- Шаг 7. Родительская директория: архив и дубликаты ---
cd /d/Programing_shit/genshin
mkdir -p _archive/scripts-1224-recon _archive/docs _archive/sessions
# 7a. Дубликаты — безопасно, md5 проверены:
rm -rf dump-7.0.0                                        # dump.cs = корневому
rm "2session-ses_f93e_FINAL - Copy.txt"                  # = session-ses_f93e_FINAL.md
# 7b. Одноразовые скрипты 1224:
mv find_*.py verify_*.py disasm_*.py resolve_fp*.py check_*.py debug_*.py \
   analyze_1224.py map_helper.py sections.py final_fp_check.py reverify_string_new_len.py \
   capstone_cluster.py decode_patch.py extract_1224_assets.py filter_snl_candidates.py \
   ida_script.py find_snl_*.py _archive/scripts-1224-recon/
# 7c. Архив тяжёлой истории 1206 (НЕ удалять без подтверждения архива):
7z a -mx=9 _archive/client-6.7.0-analysis-1206.7z client-6.7.0-analysis
mv evidence _archive/evidence-1206-2026-08-31
mv session-ses_fa80.md session-ses_fac6.md "session-ses_f93e_FINAL - Copy.txt" \
   session_switch_fix_anticheat_fix_internet.md _archive/sessions/
mv inject.cpp Injector.vcxproj _archive/
mv XAPK_1224_UPGRADE_PLAN.md _archive/docs/
# 7d. После проверки архива: rm -rf client-6.7.0-analysis; архив _archive/ скопировать на внешний диск
# 7e. Оставить на месте: genshinimpact_nx/, dump.cs, ida_data.jsonl (или в архив при желании),
#     nvk-sdk-25.0.7/, nvk-switch/, Vulkan-Headers-1.4.305/, nro-backup-2026-09-07/,
#     _restore_points/, backups/, NATIVE_NRO_ANALYSIS.md, session-state-2026-09-07.md,
#     error codes.txt, Запускаем локальный сервер….md, .claude/, .opencode/, opencode.json
```

## 8. Оценка рисков

| Действие | Риск | Обоснование |
|---|---|---|
| ff client-1234 → 9177ebd | Нулевой | ahead=0, проверено merge-base; рабочее дерево чистое |
| Удаление 6 агентских веток + 3 worktrees | Нулевой | Все чистые; содержимое на форке доказано патч-диффами (1 идентичен, 2 поглощены надмножеством/идентичны по содержимому); объекты ещё ~30 дней в reflog даже при ошибке |
| Удаление client-1224, login-stable-6.7.0 | Нулевой | Полностью влиты; покрыты 2 тегами, 2 валидными бандлами и форком; 380d8ec остаётся в истории client-1234 |
| main | НЕ УДАЛЯТЬ | Единственный носитель 80d273a (альтернативный апгрейд 1224) вместе с форком; в бандле от 09-02 его нет |
| oc-managed-256m | Не трогать сейчас | Заблокирован живой сессией claude (pid 24392) |
| Удаление веток на форке | Низкий | Только после нового бандла; experiment/* сохранить |
| Родительская директория | Низкий при порядке 7a→7d | Дубликаты md5-подтверждены; 1.9 GB APK — удалять только после проверки архива; скрипты перемещать, не удалять |

## 9. Итог после чистки

- Локально: **2 ветки** (`client-1234` = `main`-состояние форка… точнее client-1234 и main), **3 тега**, 2+1 бандлов, ноль лишних worktrees.
- Форк: main + client-1234 + experiment/nvk-26.1.4-loaderless + 3 тега.
- Диск: −~2.1 GB в родительской директории, репо остаётся ~2 MB + 7.8 MB worktrees (до их удаления).
