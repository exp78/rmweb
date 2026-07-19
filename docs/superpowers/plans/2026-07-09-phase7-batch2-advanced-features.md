# rmweb Phase 7 Batch 2 — Advanced General Purpose Features

**Дата:** 2026-07-09  
**Статус:** Ready for execution  
**Цель:** Завершить Phase 7 и сделать rmweb полноценным (насколько позволяет железо) браузером для reMarkable Paper Pro.

### Возможности Batch 2

1. **Password Manager** — безопасное хранение и автозаполнение логинов/паролей (расширение profile.h с простым шифрованием).
2. **Advanced Autofill** — контекстное заполнение форм (email, address, payment hints), сайт-specific rules.
3. **JavaScript Console / Debug Tools** — простой on-device console (tap on address bar with long-press → debug panel).
4. **Lightweight Extensions** — content scripts / user scripts (через UserContentManager).
5. **Full History Search** — поиск по истории и закладкам с фильтрами.
6. **Final Polish** — gesture improvements, better error pages, performance dashboard.

### План задач

**Task 0:** Обновить все документы и CLAUDE.md (Phase 7 status).

**Task 1:** Password Manager + secure storage.

**Task 2:** Advanced form autofill and rules engine.

**Task 3:** JS Console + debug overlay in B2 chrome.

**Task 4:** Lightweight user scripts / content blocking extensions.

**Task 5:** Full History Search + improved bookmark manager.

**Task 6–8:** Code Review, Simplification, comprehensive tests (including real-site login, form submission, console usage).

**Task 9:** On-device verification (multiple real websites: login, forms, downloads, console, long sessions).

**Task 10:** Final documentation, version 0.8.0, full release preparation (GitHub repo structure, screenshots, demo flow, user guide).

**Обязательные правила (строго соблюдать):**
- После каждой фичи: code-reviewer + code-simplifier + `./scripts/run-tests.sh` + docs update
- Никаких TODO в коде
- Всё под `/home`, низкое потребление ресурсов
- Финальный релиз-ready state

---

План создан. Я начинаю **автономное выполнение Phase 7 Batch 2 + финализацию проекта**.

Это будет последний большой этап. По окончании дам полный итоговый отчёт о завершении проекта.