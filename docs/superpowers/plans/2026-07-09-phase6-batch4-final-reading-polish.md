# rmweb Phase 6 Batch 4 — Final Reading Browser Polish & Transition to Phase 7

**Дата:** 2026-07-09  
**Статус:** Ready for execution  
**Цель:** Завершить Phase 6 (Reading Browser) на высоком уровне качества и плавно перейти к Phase 7 (General Purpose Browser).

### Что входит в Batch 4

1. **Финальная полировка Reader Experience**
   - Улучшенный прогресс-бар чтения (с процентами и оценкой времени)
   - "Night Mode" с автоматическим переключением по времени/освещённости
   - Пресеты стилей ("News", "Book", "Academic", "Minimal") с одним тапом
   - Улучшенная обработка таблиц, кода и изображений в reader mode
   - "Focus Mode" (скрытие chrome при чтении)

2. **Качество и стабильность**
   - Финальный pass по производительности (render time < 200ms, memory usage)
   - Улучшенная обработка ошибок рендеринга ("Couldn't render this page" → suggestions)
   - Авто-сохранение позиции чтения (scroll position per URL)

3. **Подготовка к Phase 7**
   - Рефакторинг chrome для будущего добавления tabs/forms
   - Добавление hook'ов для будущих фич (login detection, form filling, downloads)
   - Обновление архитектурного документа
   - Подготовка к публикации (GitHub README, screenshots, demo video plan)

### План задач

**Task 0:** Обновить CLAUDE.md и создать этот план.

**Task 1:** Reader Polish (progress bar, presets, focus mode, night mode, table/code handling).

**Task 2:** Auto-save reading position + improved error page.

**Task 3:** Performance & stability final pass (render budget, memory profiling).

**Task 4:** Architecture hooks for Phase 7 (tabs, forms, downloads).

**Task 5–7:** Mandatory Code Review (subagent), Simplification pass, full test suite (`run-tests.sh`).

**Task 8:** On-device verification (long articles, night reading, focus mode, error cases).

**Task 9:** Full documentation update (all specs, research/reader.md, README with new features, screenshots section).

**Task 10:** Prepare release checklist (version bump, LICENSE review, GitHub structure).

**Рабочие правила (обязательно соблюдать):**
- После каждой крупной задачи — code-reviewer + code-simplifier
- Все тесты должны проходить
- Никаких новых TODO
- Изменения только через инструменты (search_replace, write и т.д.)
- Финальный отчёт — краткий

---

План создан. Я начинаю **автономное выполнение Phase 6 Batch 4**.

Как и раньше — по окончании дам короткий итоговый отчёт.