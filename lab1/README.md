# Лабораторная №1. Основы 3D-графики

Вариант 10 — правильный додекаэдр. C++20, Vulkan, GLFW и ImGui.

На сцене две фигуры. Для каждой можно менять положение, поворот, масштаб
и цвет, включать цвета вершин и анимацию с паузой. Есть переключение
перспективной и ортографической проекций. У каждой фигуры свой VkDescriptorSet.

## Запуск

Нужны Vulkan SDK, CMake и компилятор C++20. Путь к SDK должен быть в VULKAN_SDK.
Библиотеки скачиваются при первой настройке проекта.

В CLion: открыть папку lab1, выбрать Visual Studio в Toolchain и собрать
цель vulkan-starter-app. Working directory — папка lab1.

Под Windows с Build Tools 2022 можно также использовать BUILD.cmd,
затем RUN.cmd. После изменений в коде нужна повторная сборка.

## Файлы

- source/application.cpp — матрицы, интерфейс, анимация и рисование.
- source/dodecahedron.hpp — вершины и грани фигуры.
- shaders/dodecahedron.vert и .frag — преобразование вершин, цвет и свет.
- [Отчёт](../312_Колосов_Лаб1.pdf).

Основа — [стартовый проект преподавателя](https://github.com/vladeemerr/vulkan-starter-app).
Его код работы с Vulkan находится в source/graphics_internal.cpp и source/main.cpp.
Лицензия шаблона сохранена в LICENSE и NOTICE.
