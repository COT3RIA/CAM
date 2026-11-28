#pragma once

#include <juce_core/juce_core.h>

// Название проекта DAW из заголовка её главного окна (только Windows, лучшая попытка).
// Если определить не удалось, возвращается пустая строка.
juce::String detectHostProjectName();
