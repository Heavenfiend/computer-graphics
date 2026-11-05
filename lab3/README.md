# Лабораторная №3. Текстурирование

Вариант 10 — правильный додекаэдр. C++20, Vulkan, GLFW и ImGui.

На основе второй работы добавлены три текстурных материала с выбором для каждой
фигуры, анимированное сэмплирование, карты блика и свечения. Сохранены камера,
освещение, преобразования и анимация. Геометрия загружается в память GPU через
staging; свойства моделей хранятся в одном динамическом uniform-буфере.

## Запуск

Открыть `BUILD.cmd`, затем `RUN.cmd`. Нужны Visual Studio 2022 с C++, CMake
и Vulkan SDK (`VULKAN_SDK`). В CLion открыть `lab3`, выбрать toolchain
Visual Studio и цель `vulkan-starter-app`. PNG и шейдеры копируются рядом с exe;
Python для запуска не нужен.

Камера: WASD — движение, Q/E — вниз и вверх, правая кнопка мыши — поворот.
Остальные настройки камеры, света и анимации находятся в ImGui.

## Демонстрация

- `Basic texture mode (both objects)` — одна текстура без эффектов.
- Выбрать `Object`, затем его `Material` в `Textures`: Brick, Metal или Tiles.
- `Animated sampling` — движущиеся UV и модуляция цвета; доступны скорость,
  сила эффекта и `Pause sampling time`. Управление передаётся через push constants.
- Для Metal сравнить `Specular map` в обычном режиме. Выключить
  `Lighting (off: emission demo)` и переключать `Emissive map`: полосы светятся
  без освещения. У Brick и Tiles отсутствующие карты заменены чёрной текстурой 1×1.

Проверки: `powershell -ExecutionPolicy Bypass -File tests/verify.ps1`.
Сверка с заданием и лекциями — в [REQUIREMENTS.md](REQUIREMENTS.md).
Отчёт — [312_Колосов_Лаб3.pdf](../312_Колосов_Лаб3.pdf).
