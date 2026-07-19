# rmweb Phase 6 Batch 1 — Start Page, Persistent Bookmarks, History & Settings

**Дата:** 2026-07-09  
**Статус:** Draft → Ready for implementation  
**Цель:** Превратить rmweb из "просто читалки" в полноценный удобный reading browser с персонализацией.

**Ключевые возможности, которые будут добавлены:**
- Красивая стартовая страница (`file://` home.html) с плитками закладок и недавней истории
- Постоянные закладки (★) и история (сохраняются между запусками)
- Настройки по умолчанию (zoom, reader font size, User-Agent), которые сохраняются
- Кнопки **Home** и **Bookmark** в B2 chrome
- `rmweb:clear-history` scheme для очистки недавнего

## Архитектура и ключевые решения

Все новые модули будут **чисто std::-only** (без Qt/WebKit), чтобы их можно было полноценно тестировать на хосте через `clang++`.

**Новые файлы:**
- `engine/wpeqt/profile.h` — хранение, загрузка/сохранение, бизнес-логика (bookmarks, history, settings)
- `engine/wpeqt/startpage.h` — генерация HTML стартовой страницы + htmlEscape
- `tests/profile_test.cpp`
- `tests/startpage_test.cpp`

**Изменяемые файлы:**
- `engine/wpeqt/main.cpp` (WpeEngine + WpeView + main)
- `engine/wpeqt/keyboard.h` / `gesture.h` (незначительно, если понадобится)
- `CLAUDE.md`, `README.md`, docs

**Хранение данных:**
- Директория: `~/.rmweb/` (по умолчанию) или `$RMWEB_PROFILE`
- Формат: простой текстовый (чтобы легко читать/править руками)
  - `bookmarks.txt` — `url\ttitle` (новые сверху)
  - `history.txt` — `timestamp\turl\ttitle` (самые новые сверху, дедупликация, лимит 300)
  - `settings.txt` — `key=value`
- Атомарные записи (`.tmp` + `rename`)
- Повреждённый файл → fallback на defaults, приложение не падает

**Ограничения (из CLAUDE.md):**
- Всё, что пишет приложение — только под `/home`
- Chrome рисуется вручную в кадр (hand-painted B2)
- Тапы обрабатываются через `TouchReader` + hit-test в C++
- Никаких TODO в финальном коде
- После каждой значимой задачи: code-review → simplify → on-device verify

## Детальный план выполнения (10 задач)

### Task 0: Подготовка документации
- [ ] Обновить `CLAUDE.md` — добавить раздел Phase 6, обновить статус
- [ ] Создать этот файл (`2026-07-09-phase6-batch1-start-page.md`) как основной план
- [ ] Обновить `README.md` — добавить раздел Features и ссылки

### Task 1: profile.h + unit-тесты
- [ ] Создать `engine/wpeqt/profile.h` (структуры `Bookmark`, `HistoryEntry`, `Settings`; функции load/save, toggleBookmark, addHistory, sanitizeField и т.д.)
- [ ] Создать `tests/profile_test.cpp` (round-trip, edge cases, limits, sanitization)
- [ ] Добавить тест в `run-tests.sh`

### Task 2: startpage.h + unit-тесты
- [ ] Создать `engine/wpeqt/startpage.h` (`buildStartPage(const vector<Bookmark>&, const vector<HistoryEntry>&)`)
- [ ] Создать `tests/startpage_test.cpp` (проверка наличия ссылок, escaping, empty state, clear-history link)
- [ ] Добавить тест в `run-tests.sh`

### Task 3: Интеграция в WpeEngine (main.cpp)
- [ ] Добавить загрузку профиля в конструкторе/start()
- [ ] Реализовать `goHome()`, `toggleBookmark(currentUrl)`, `addToHistory()`
- [ ] Обработать `rmweb:clear-history` в policy decision
- [ ] Сохранять настройки при изменении (zoom, readerFont, ua)

### Task 4: Обновление Chrome (WpeView)
- [ ] Добавить кнопки Home и Bookmark star в `drawChromeBar()`
- [ ] Расширить `enum Hit` и `hitChrome()`
- [ ] Добавить сигналы `bookmarkedChanged(bool)` и обработку в paint
- [ ] Нарисовать иконки (vector paths, как существующие)

### Task 5: Настройки по умолчанию и применение
- [ ] Применять zoom, reader font и UA при загрузке страницы
- [ ] Сохранять изменения из reader mode (A-/A+)
- [ ] Добавить fallback значения

### Task 6: Code Review
- [ ] Запустить `feature-dev:code-reviewer` на все изменённые файлы (main.cpp, profile.h, startpage.h, тесты)

### Task 7: Simplification Pass
- [ ] Запустить `code-simplifier:code-simplifier` на новые и изменённые модули

### Task 8: On-device Verification
- [ ] Собрать, забандлить, задеплоить
- [ ] Проверить сценарии:
  - Запуск без URL → стартовая страница
  - Добавление/удаление закладок (звёздочка обновляется)
  - История заполняется и очищается
  - Настройки сохраняются после перезапуска
  - Тапы по плиткам работают

### Task 9: Финализация документации и релиза
- [ ] Обновить все research/specs документы
- [ ] Добавить раздел "Features" в README.md
- [ ] Подготовить commit + push (если нужно — создать PR)

## Рабочие соглашения (напоминание)
- После каждой крупной задачи — code review + simplify
- Все host-тесты должны проходить (`./scripts/run-tests.sh`)
- Никаких новых TODO в коде
- Коммиты только конкретных файлов (`.env` никогда не коммитится)
- Финальный commit trailer: `Co-Authored-By: Grok <grok@x.ai>`

**Готов к запуску.**

Этот план можно выполнять через `subagent-driven-development` или `executing-plans`. 

Хотите, чтобы я **немедленно запустил** его (автономно, шаг за шагом, с использованием subagents)? Напишите «запускай план».