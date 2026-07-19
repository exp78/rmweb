# rmweb Phase 6 Batch 2 — Reader Mode Improvements, In-Page Search, Memory Management & Tabs-Lite

**Дата:** 2026-07-09  
**Статус:** Ready for execution  
**Цель:** Сделать чтение длинных статей максимально комфортным и добавить базовые возможности "настоящего" браузера, не нарушая e-ink constraints.

**Что будет реализовано:**

### 1. Reader Mode v2 (главный приоритет)
- Улучшенная очистка страниц (Mozilla Readability + дополнительные правила)
- Автоопределение и предложение reader mode (более точное, чем сейчас)
- Настраиваемый шрифт, межстрочный интервал, ширина колонок, тёмная тема (через CSS filter + custom CSS)
- Прогресс чтения (scroll progress bar в chrome)
- "Читать позже" / save to local archive (простой HTML dump)

### 2. In-Page Search
- Поиск по странице (Ctrl+F style, но через chrome бар)
- Подсветка результатов (yellow background via JS)
- Навигация по результатам (Next/Prev)
- Работает как в reader mode, так и в обычном режиме

### 3. Memory Management & Stability
- Ограничение памяти WebProcess (WEBKIT_WEB_PROCESS_MEMORY_LIMIT_MB)
- Авто-очистка кэша при приближении к лимиту
- Улучшенный handling длинных страниц (lazy loading hints, reduce image quality)
- Leak detection / periodic GC calls

### 4. Tabs-Lite
- Простая система "закладок-сессий" (несколько одновременно открытых URL, переключение через long-press Home)
- Не полноценные tabs (чтобы не убивать память), а "quick switcher" между 3–4 страницами

## Архитектура решений

- Reader v2: расширить `Readability.js` + добавить `reader.css` + JS-инъекции через UserContentManager
- Search: использовать WebKit `findController` API (WebKitWebView `find` methods) + overlay в chrome
- Memory: environment variables + periodic `webkit_web_context_purge_cached_resources()` + monitoring
- Tabs-Lite: хранить в `profile.h` список активных сессий, переключение через `webkit_web_view_load_uri`

**Технические ограничения (напоминание):**
- Всё под `/home/root/.rmweb/`
- Chrome остаётся hand-painted B2
- Никаких тяжёлых UI-виджетов
- После каждой задачи — review + simplify + test

## Детальный план задач

**Task 0:** Обновить `CLAUDE.md` и этот план-файл.

**Task 1:** Reader Mode v2
- Улучшить `Readability.js` + добавить custom CSS/JS
- Добавить авто-предложение reader mode
- Добавить настройки (font, line-height, width, theme)

**Task 2:** In-Page Search
- Добавить UI в chrome (search bar on long-press address)
- Интегрировать WebKit FindController
- Подсветка + навигация

**Task 3:** Memory Management
- Добавить monitoring + env limits
- Периодическая очистка кэша
- Улучшенный GC hint для длинных страниц

**Task 4:** Tabs-Lite (quick session switcher)
- Расширить `profile.h` для хранения сессий
- Long-press Home → switcher UI
- Переключение между страницами

**Task 5–8:** Code Review, Simplification, full test suite, on-device verification (including long articles, memory pressure test).

**Task 9:** Полное обновление документации (research/reader.md, specs, README).

---

План готов. Я начинаю автономное выполнение Phase 6 Batch 2.

(Выполнение будет идти в фоне через subagent. По окончании дам краткий отчёт.)