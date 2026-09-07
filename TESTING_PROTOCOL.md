# Тестовый протокол: Eden (эмулятор) vs Switch (железо)

Выработан 2026-09-07 на фазе Eden-feasibility (см. коммиты 4bc49e9..8fa1f4a на client-1234).

## Вывод

Eden — рабочий стенд для **бут-логики wrapper'а** (userspace-код против syscall-интерфейса):
за один вечер найдено и починено 5 багов, которые по FTP-циклу на консоли заняли бы недели
и несли риск фриза консоли. Стена Eden — GPU-слой: его HLE nvdrv/Maxwell не переваривает
сырой командный поток Mesa NVK (assert-шторм MacroJIT/maxwell_3d, вечное ожидание fence).

## Что тестировать в EDEN (быстро, итерация минуты)

1. **Self-test'ы бута**: syscall-чеки, memory broker, sparse-арена, heap-донор, спилл-аллокации
   (`sparse_guest_spill_self_test` и соседи — ловят ошибки вычисления потолков/геометрии).
2. **Загрузка модуля**: so_load/so_relocate/so_finalize, алиасинг libyuanshen.so, символы.
3. **Код-патчи**: verify + install всех четырёх exact-code патчей (JNI resolver, transfer guard,
   mmoron path, unity slab) — сейчас через pre-finalize memcpy, без svc.
4. **Плагины/библиотеки**: dlopen 23 .so из lib/arm64-v8a, JNI-регистрации, combo crypto.
5. **Unity init**: il2cpp, метадата, шейдер-компиляция, создание swapchain'а, первый
   vkAllocateMemory — в Eden это всё реально отрабатывает.
6. **Файловый слой**: staging файлов, версии, каталоги no_backup/files/cache.

## Что тестировать ТОЛЬКО на ЖЕЛЕЗЕ

1. **Рендер**: NVK command stream на реальном nvdrv — device-lost, push_sync, fence-throttle,
   тайминги кадра (в Eden nvdrv HLE несовместим с NVK по определению).
2. **Реальные сервисы**: nifm-сеть (скорость HTTP-пути, реальный CDN), ввод, аккаунт-паспорт.
3. **Стресс/долгие сессии**: пайплайн-бурсты ShaderWarmUp, утечки памяти, watchdog'и.
4. **Финальная валидация**: логин → геймплей.

## Фиксы Eden-фазы (все железо-нейтральны, client-1234 8fa1f4a)

| Коммит | Фикс | Железо |
|---|---|---|
| b1a4870 | env-хэндл → CUR_PROCESS fallback | overridden-ветка не тронута |
| 5612fb1 | потолок донора из бюджета процесса (не-overridden путь) | overridden-ветка не тронута |
| 8fa1f4a | код-патчи pre-finalize memcpy вместо so_patch_code | **изменён путь** — требует смоук-теста логотипов |
| d5a7852 | диф. диагностика патчей + perm-swap фоллбэк | только failure-пути |
| 83ad714/062afd6/cc837b3 | диагностика арен/мапов | только failure-пути |

## Деплой

- Консоль: `sdmc:/switch/genshinimpact_nx/` по FTP (192.168.31.39:5000, ~36 Mbit/s).
  Рабочий билд не перезаписывать: `genshinimpact_nx_ownfix.nro` (52e0cf25) — откат.
  Eden-фазовый: `genshinimpact_nx_edenfix.nro` (34612c52 = tip 8fa1f4a).
- Eden: `D:\Programing_shit\eden\genshin_testN.nro` (файл лочится запущенным гостем —
  писать под новым именем или останавливать игру); GDB stub 6543 стартует гостя
  suspended — продолжить скриптом /tmp/gdbcont.py.
- sdmc Eden: `C:\Users\rulled\AppData\Roaming\eden\sdmc\switch\genshinimpact_nx\`
  (полная стейджировка: lib/arm64-v8a 24 .so, assets.nxpack 484M byte-exact и пр.).

## Артефакты

- NRO-бэкапы: `D:\Programing_shit\genshin\nro-backup-2026-09-07\nro-eden-*.nro`.
- CI: `gh workflow run build.yml --repo rulled/genshinimpact_nx --ref client-1234`.
