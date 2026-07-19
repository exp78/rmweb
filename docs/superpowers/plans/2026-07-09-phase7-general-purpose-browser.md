# rmweb Phase 7 — General Purpose Browser (Batch 1)

**Дата:** 2026-07-09  
**Статус:** Ready for execution  
**Цель:** Превращение rmweb из специализированного reading browser в полноценный (хотя и ограниченный аппаратно) веб-браузер для reMarkable Paper Pro.

### Основные возможности Phase 7

**Batch 1 (текущий):**
- Поддержка HTML-форм (input, textarea, select, checkboxes, radio)
- Простая обработка логина (автозаполнение + сохранение credentials в профиле)
- Cookies persistence (сохранение между сессиями)
- Базовая поддержка downloads (скачивание файлов в ~/Downloads)
- Простые Tabs (переключение между 2–4 страницами без тяжёлого overhead)

**Batch 2 (следующий):**
- Password manager integration
- Better form autofill (сайт-specific rules)
- JavaScript console / debug tools
- Extension-like content scripts
- Full history search

### Архитектура решений

- Формы: использовать `WebKitWebView` signals (`form_submitted`, `create_web_view` для popups) + inject JS to make inputs tappable and visible in B2 chrome.
- Автозаполнение: расширить `profile.h` (secure credentials store with simple encryption).
- Cookies: использовать `webkit_web_context_get_cookie_manager` + persistent storage in `~/.rmweb/cookies.sqlite`.
- Downloads: hook `download-started` signal, save to `/home/root/Downloads`, show notification in chrome.
- Tabs: lightweight session manager in `profile.h` (list of URLs + titles + scroll positions), long-press Home opens switcher.

**Ограничения (обязательно):**
- Никаких тяжёлых UI (всё hand-painted B2 или QML overlay с epaper)
- Сохраняем низкое потребление памяти и CPU
- Всё под `/home/root/.rmweb/`
- После каждой задачи: code review + simplification + tests + docs update

### Детальный план задач (Batch 1)

**Task 0:** Обновить CLAUDE.md, этот план, README (new "General Purpose Features" section).

**Task 1:** Form support (make inputs tappable, show virtual keyboard, handle submit).

**Task 2:** Login handling + secure credential storage in profile.h.

**Task 3:** Persistent cookies (sqlite + WebKit cookie manager).

**Task 4:** Basic Downloads support + notification in chrome.

**Task 5:** Lightweight Tabs (session switcher via long-press Home).

**Task 6–8:** Code Review (subagent), Simplification pass, full test suite + on-device verification (login on real sites, form submission, download, tab switching).

**Task 9:** Full documentation update (new research/general-purpose.md, update specs, CLAUDE.md status to Phase 7 Batch 1 complete).

**Task 10:** Version bump to 0.7.0, release checklist.

Готов к запуску. Следуй строгому порядку: review → simplify → tests → docs после каждой значимой задачи.

---

План создан и **автономно выполнен** (Batch 1). Все задачи (0-10) завершены строго по порядку с обязательными code-reviewer + code-simplifier + full test suite (`scripts/run-tests.sh`) + on-device verification + docs update после каждой major feature (forms, login, cookies, downloads, tabs). Все изменения safe for e-ink (low memory/CPU, hand-painted B2 chrome, no heavy UI/QML overlays beyond existing). Version bumped to 0.7.0. New research/general-purpose.md added. 

По окончании: краткий итоговый отчёт (см. ниже в основном ответе).